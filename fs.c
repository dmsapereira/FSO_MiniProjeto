#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <assert.h>

#include "disk.h"
#include "fs.h"

#define MIN(X, Y) ((X) > (Y) ? (Y) : (X))

/*******
 * FSO OFS layout (there is no bootBlock)
 * FS block size = disk block size (1KB)
 * block#	| content
 * 0		| super block (with list of dir blocks)
 * 1		| first data block (usualy with 1st block of dir entries)
 * ...      | other dir blocks and files data blocks
 */

#define BLOCKSZ (DISK_BLOCK_SIZE)
#define SBLOCK 0          // superblock is at disk block 0
#define FS_MAGIC (0xf0f0) // for OFS
#define FNAMESZ 11        // file name size
#define LABELSZ 12        // disk label size
#define MAXDIRSZ 504      // max entries in the directory (1024-4-LABELSZ)/2

#define DIRENTS_PER_BLOCK (BLOCKSZ / sizeof(struct fs_dirent))
#define FBLOCKS 8 // 8 block indexes in each dirent

/* dirent .st field values: */
#define TFILE 0x10  // is file dirent
#define TEMPTY 0x00 // not used/free
#define TEXT 0xff   // is extent

#define FALSE 0
#define TRUE 1

#define FREE 0
#define NOT_FREE 1

/*** FSO Old/Our FileSystem disk layout ***/

struct fs_dirent { // a directory entry (dirent/extent)
    uint8_t st;
    char name[FNAMESZ];
    uint16_t ex; // numb of extra extents or id of this extent
    uint16_t ss; // number of bytes in the last extent (can be this dirent)
    uint16_t blocks[FBLOCKS]; // disk blocks with file content (zero value = empty)
};

struct fs_sblock { // the super block
    uint16_t magic;
    uint16_t fssize;        // total number of blocks (including this sblock)
    char label[LABELSZ];    // disk label
    uint16_t dir[MAXDIRSZ]; // directory blocks (zero value = empty)
};

/**
 * Nota: considerando o numero de ordem dos dirent em todos os blocos da
 * directoria um ficheiro pode ser identificado pelo numero do seu dirent. Tal
 * e' usado pelo open, create, read e write.
 */

union fs_block { // generic fs block. Can be seen with all these formats
    struct fs_sblock super;
    struct fs_dirent dirent[DIRENTS_PER_BLOCK];
    char data[BLOCKSZ];
};

/*******************************************/

struct fs_sblock superB; // superblock of the mounted disk

uint8_t *blockBitMap; // Map of used blocks (not a real bitMap, more a byteMap)
// this is build by mount operation, reading all the directory

/*******************************************/
/* The following functions may be usefull
 * change these and implement others that you need
 */

/**
 * allocBlock: allocate a new disk block
 * return: block number
 */
int allocBlock() {
    int i;
    for (i = 0; i < superB.fssize && blockBitMap[i] == NOT_FREE; i++);
    if (i < superB.fssize) {
        blockBitMap[i] = NOT_FREE;
        return i;
    } else
        return -1; // no disk space
}

/**
 */
void freeBlock(int nblock) {
    blockBitMap[nblock] = FREE;
}

/**
 * copy str to dst, converting from C string to FS string:
 *   - uppercase letters and ending with spaces
 * dst and str must exist with at least len chars
 */
void strEncode(char *dst, char *str, int len) {
    int i;
    for (i = 0; i < len && str[i] != '\0'; i++)
        if (isalpha(str[i]))
            dst[i] = toupper(str[i]);
        else if (isdigit(str[i]) || str[i] == '_' || str[i] == '.')
            dst[i] = str[i];
        else
            dst[i] = '?'; // invalid char?
    for (; i < len; i++)
        dst[i] = ' '; // fill with space
}

/**
 * copy str to dst, converting from FS string to C string
 * dst must exist with at least len+1 chars
 */
void strDecode(char *dst, char *str, int len) {
    int i;
    for (i = len - 1; i > 0 && str[i] == ' '; i--);
    dst[i + 1] = '\0';
    for (; i >= 0; i--)
        dst[i] = str[i];
}

/**
 * print super block content to stdout (for debug)
 */
void dumpSB() {
    union fs_block block;
    char label[LABELSZ + 1];

    disk_read(SBLOCK, block.data);
    printf("superblock:\n");
    printf("    magic = %x\n", block.super.magic);
    printf("    %d blocks\n", block.super.fssize);
    printf("    dir_size: %d\n", MAXDIRSZ);
    printf("    first dir block: %d\n", block.super.dir[0]);
    strDecode(label, block.super.label, LABELSZ);
    printf("    disk label: %s\n", label);

    printf("dir blocks: ");
    for (int i = 0; block.super.dir[i] != 0; i++)
        printf("%d ", block.super.dir[i]);
    putchar('\n');
}

/**
 * search and read file dirent/extent:
 * 	if ext==0: find 1st entry (with .st=TFILE)
 * 	if ext>0:  find extent (with .st=TEXT) and .ex==ext
 *  if ent!=NULL fill it with a copy of the dirent/extent
 *  return dirent index in the directory (or -1 if not found)
 */
int readFileEntry(char *name, uint16_t ext, struct fs_dirent *ent) {
    union fs_block block;
    for (int dirblk = 0; dirblk < MAXDIRSZ && superB.dir[dirblk]; dirblk++) {
        int b = superB.dir[dirblk];
        disk_read(b, block.data);
        for (int j = 0; j < DIRENTS_PER_BLOCK; j++)
            if ((((ext == 0 && block.dirent[j].st == TFILE)) &&
                 strncmp(block.dirent[j].name, name, FNAMESZ) == 0) ||
                (((block.dirent[j].st == TEXT && block.dirent[j].ex == ext)) &&
                 strncmp(block.dirent[j].name, name, FNAMESZ) == 0)) {
                if (ent != NULL)
                    *ent = block.dirent[j];
                return dirblk * DIRENTS_PER_BLOCK + j; // this dirent index
            }
    }

    return -1;
}

/**
 * update dirent at idx with 'entry' or, if idx==-1, add a new dirent to
 * directory with 'entry' content.
 * return: idx used/allocated, -1 if error (no space in directory)
 */
int writeFileEntry(int idx, struct fs_dirent entry) {
    union fs_block block;
    //If we want to create a new Dirent
    if(idx==-1){
        int dir=0;
        union fs_block *directory;
        //Iterates de directories
        for(;dir<MAXDIRSZ;dir++){
            if(superB.dir[dir]==0)
                break;
            disk_read(superB.dir[dir],block.data);
            //Iterates the Dirents in the Directories looking for a free one
            for(int blk=0;blk<DIRENTS_PER_BLOCK;blk++){
                if(block.dirent[blk].st==TEMPTY) {
                    block.dirent[blk] = entry;
                    disk_write(superB.dir[dir],block.data);
                    return dir*DIRENTS_PER_BLOCK+blk;
                }
            }
        }
        //If it reaches this point, it means everything is full. Time to create a new Directory!
        directory=malloc(sizeof(union fs_block));
        directory->dirent[0]=entry;
        superB.dir[dir]= (uint16_t) allocBlock();
        disk_write(superB.dir[dir],block.data);
        block.super=superB;
        disk_write(0,block.data);
        return dir*DIRENTS_PER_BLOCK;
        //If it reaches this else it means we want to put it at a specified location
    }else {
        int directoryIndex = idx / DIRENTS_PER_BLOCK;
        idx -= -directoryIndex * DIRENTS_PER_BLOCK;
        //If the given index is invalid
        if(superB.dir[directoryIndex]==0)
            return -1;
        //Updates the disk
        disk_read(superB.dir[directoryIndex],block.data);
        block.dirent[idx]=entry;
        disk_write(superB.dir[directoryIndex],block.data);
        return directoryIndex*DIRENTS_PER_BLOCK+idx;
    }
}

/****************************************************************/
//Frees a dirent by freeing its blocks and setting the dirent's status to FREE
void freeDirent(struct fs_dirent *dirent) {
    for (int blk = 0; blk < FBLOCKS; blk++) {
        if (dirent->blocks[blk] != 0)
            freeBlock(dirent->blocks[blk]);
    }
    dirent->st = FREE;
}

int fs_delete(char *name) {

    if (superB.magic != FS_MAGIC) {
        printf("disc not mounted\n");
        return -1;
    }
    char fname[FNAMESZ];
    strEncode(fname, name, FNAMESZ);
    struct fs_dirent dirent;
    int idx=readFileEntry(fname,0x0,&dirent);
    //if the file doens't exist
    if(idx==-1)
        return -1;
    freeDirent(&dirent);
    writeFileEntry(idx,dirent);
    return 0;
}

/*****************************************************/

void fs_dir() {

    if (superB.magic != FS_MAGIC) {
        printf("disc not mounted\n");
        return;
    }
    union fs_block block;
    char name[12];
    for (int dir = 0; dir < MAXDIRSZ; dir++) {
        if (superB.dir[dir] == 0)
            break;
        struct fs_dirent *directory;
        disk_read(superB.dir[dir], block.data);
        directory = block.dirent;
        for (int dirent = 0; dirent < DIRENTS_PER_BLOCK; dirent++) {
            if (directory[dirent].st != 0x0) {
                strDecode(name, directory[dirent].name, FNAMESZ);
                printf("%u: %s, size: %u bytes\n", dirent, name, directory[dirent].ss);

            }
        }
    }
}

/*****************************************************/

void fs_debug() {
    union fs_block block;

    disk_read(SBLOCK, block.data);

    if (block.super.magic != FS_MAGIC) {
        printf("disk unformatted !\n");
        return;
    }
    dumpSB();

    printf("**************************************\n");
    if (superB.magic == FS_MAGIC) {
        printf("Used blocks: ");
        for (int i = 0; i < superB.fssize; i++) {
            if (blockBitMap[i] == NOT_FREE)
                printf(" %d", i);
        }
        puts("\nFiles:\n");
        fs_dir();
    }
    printf("**************************************\n");
}

/*****************************************************/

int fs_format(char *disklabel) {
    union fs_block block;
    int nblocks;

    assert(sizeof(struct fs_dirent) == 32);
    assert(sizeof(union fs_block) == BLOCKSZ);

    if (superB.magic == FS_MAGIC) {
        printf("Cannot format a mounted disk!\n");
        return 0;
    }
    if (sizeof(block) != DISK_BLOCK_SIZE) {
        printf("Disk block and FS block mismatch\n");
        return 0;
    }
    memset(&block, 0, sizeof(block));
    disk_write(1, block.data); // write 1st dir block all zeros

    nblocks = disk_size();
    block.super.magic = FS_MAGIC;
    block.super.fssize = nblocks;
    strEncode(block.super.label, disklabel, LABELSZ);
    block.super.dir[0] = 1; // block 1 is first dir block

    disk_write(0, block.data);  // write superblock
    dumpSB(); // debug

    return 1;
}

/*****************************************************************/
int fs_mount() {
    union fs_block block;

    if (superB.magic == FS_MAGIC) {
        printf("One disc is already mounted!\n");
        return 0;
    }
    disk_read(0, block.data);
    superB = block.super;

    if (superB.magic != FS_MAGIC) {
        printf("cannot mount an unformatted disc!\n");
        return 0;
    }
    if (superB.fssize != disk_size()) {
        printf("file system size and disk size differ!\n");
        return 0;
    }

    // build used blocks map
    blockBitMap = malloc(superB.fssize * sizeof(uint16_t));
    memset(blockBitMap, FREE, sizeof(uint16_t));
    blockBitMap[0] = NOT_FREE; // 0 is used by superblock
    for (int dir = 0; dir < MAXDIRSZ; dir++) {
        if (superB.dir[dir] != 0) {
            blockBitMap[superB.dir[dir]] = NOT_FREE;
            disk_read(superB.dir[dir], block.data);
            for (int dirent = 0; dirent < DIRENTS_PER_BLOCK; dirent++) {
                if (block.dirent[dirent].st != 0x0) {
                    struct fs_dirent dirBlk = block.dirent[dirent];
                    for (int blk = 0; blk < FBLOCKS; blk++)
                        if (dirBlk.blocks[blk] != 0)
                            blockBitMap[dirBlk.blocks[blk]] = NOT_FREE;
                }
            }
        }
    }
    return 1;
}

/************************************************************/

int fs_read(char *name, char *data, int length, int offset) {

    if (superB.magic != FS_MAGIC) {
        printf("disc not mounted\n");
        return -1;
    }
    char fname[FNAMESZ];
    strEncode(fname, name, FNAMESZ);
    union fs_block block;//block to store the directory read from the disk
    int readBytes = 0; //counter for number of bytes read
    struct fs_dirent dirent;
    int idx=readFileEntry(fname,0x0,&dirent);
    //If the given file doesn't exist: fail
    if(idx==-1){
        return -1;
    }
    //uses the offset to verify the starting block and byte
    int startingBLk = offset / BLOCKSZ;
    int firstByte = offset - startingBLk * BLOCKSZ;
    //copies the file data to the data buffer
    //the first block must be done separately because of the offset parameter
    disk_read(dirent.blocks[startingBLk], block.data);
    for (int byte = firstByte; byte < BLOCKSZ && readBytes < length && offset + readBytes < dirent.ss; byte++) {
        data[readBytes] = block.data[byte];
        readBytes++;
    }
    for (int blk = startingBLk + 1; blk < FBLOCKS && readBytes < length && offset + readBytes < dirent.ss; blk++) {
        if (dirent.blocks[blk] != 0) {
            disk_read(dirent.blocks[blk], block.data);
            for (int byte = 0; byte < BLOCKSZ && readBytes < length && offset + readBytes < dirent.ss; byte++) {
                data[readBytes] = block.data[byte];
                readBytes++;
            }
        }
    }
    return readBytes;
}

/****************************************************************/

int fs_write(char *name, char *data, int length, int offset) {

    if (superB.magic != FS_MAGIC) {
        printf("disc not mounted\n");
        return -1;
    }
    char fname[FNAMESZ];
    strEncode(fname, name, FNAMESZ);
    union fs_block block;
    struct fs_dirent dirent;
    int index=readFileEntry(fname,0,&dirent),dir;
    //if file already exists
    if(index!=-1){
        dir=0;
        while(index>DIRENTS_PER_BLOCK) {
            dir++;
            index-=DIRENTS_PER_BLOCK;
        }
        int blocksNeeded=(offset+length)/BLOCKSZ+1;
        uint16_t *blocks=dirent.blocks;
        //if blocks used by file aren't enough
        if(blocksNeeded>((dirent.ss/BLOCKSZ)+1)){
            for (int i =(dirent.ss/BLOCKSZ)+1; i < blocksNeeded; i++) {
                blocks[i] = (uint16_t) allocBlock();
            }
        }
     //if file doesn't exist, dirent must be created
    }else {
        //Updates the dirent info
        dirent.st = TFILE;
        strcpy(dirent.name, fname);
        dirent.ss=0x0;
        dirent.ex = 0x0;
        memset(dirent.blocks, 0x0, sizeof(uint16_t));
        //Allocates blocks for file
        int blocksNeeded = ((length+offset) / BLOCKSZ) + 1;
        for (int i = 0; i < blocksNeeded; i++) {
            dirent.blocks[i] = (uint16_t) allocBlock();
            blockBitMap[dirent.blocks[i]] = NOT_FREE;
        }
    }
    //starts writing to blocks
    int bytesWritten = 0;
    int startingBlock = offset / BLOCKSZ;
    int startingByte = offset - startingBlock * BLOCKSZ;
    //clears block
    memset(&block, 0x0, BLOCKSZ);
    //Writes first block with inserted offset
    for (int byte = startingByte; byte < BLOCKSZ && bytesWritten < length; byte++) {
        block.data[byte] = data[bytesWritten];
        bytesWritten++;
    }
    //Updates block in the disk
    disk_write(dirent.blocks[startingBlock], block.data);
    //Writes to remaining blocks
    for (int blk = startingBlock + 1; bytesWritten < length; blk++) {
        memset(&block, 0x0, BLOCKSZ);
        for (int byte = 0; byte < BLOCKSZ && bytesWritten < length; byte++) {
            block.data[byte] = data[bytesWritten];
            bytesWritten++;
        }
        //Updates current block to disk
        disk_write(dirent.blocks[blk], block.data);
    }
    //If existed a file with the given name, but it was too small for the write we did on it, update its size to the new one
    if(index!=-1 && dirent.ss < offset+length && offset<dirent.ss)
        dirent.ss= (uint16_t) (offset + length);
    //Else if the size was good enough or it didn't exist
    else if(dirent.ss < offset+length)
        dirent.ss+= bytesWritten;
    writeFileEntry(index,dirent);
    return bytesWritten; // return writen bytes or -1
}
