#ifndef FS_LAYOUT_H
#define FS_LAYOUT_H

#include <stdint.h>

#define FS_MAGIC 0x5553464D /* "MYFS" in little-endian hex */
#define BLOCK_SIZE 4096
#define TOTAL_BLOCKS 1024
#define MAX_INODES 128
#define MAX_FILE_NAME 56

/* 11 Direct Pointers (blocks[0..10]) + 1 Single-Indirect Pointer (blocks[11]) */
#define DIRECT_BLOCKS 12
#define DIRECT_DATA_PTRS 11
#define INDIRECT_IDX 11
#define PTRS_PER_BLOCK (BLOCK_SIZE / sizeof(uint32_t)) /* 1024 pointers */
#define MAX_FILE_BLOCKS (DIRECT_DATA_PTRS + PTRS_PER_BLOCK) /* 1035 blocks = 4.04 MB */

/* Partition Block Offsets on disk.img */
#define SUPERBLOCK_BLK 0
#define INODE_BM_BLK 1
#define DATA_BM_BLK 2
#define INODE_TABLE_START 3
#define DATA_BLOCK_START 19
#define DATA_BLOCKS_COUNT (TOTAL_BLOCKS - DATA_BLOCK_START) /* 1005 usable blocks */

struct superblock {
    uint32_t magic;
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint8_t padding[4076]; /* Align structure to 4096 bytes */
};

struct inode {
    uint32_t mode;                  /* POSIX file mode and permissions */
    uint32_t size;                  /* Logical payload size in bytes */
    uint32_t blocks[DIRECT_BLOCKS]; /* [0..10] direct, [11] single-indirect */
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t links_count;
    uint8_t padding[428];           /* Align structure to 512 bytes (8 per block) */
};

struct dir_entry {
    char name[MAX_FILE_NAME];       /* 56-byte null-terminated filename */
    uint32_t inode_no;              /* Target inode number [0..127] */
    uint8_t valid;                  /* FSM state: 0=free, 1=active, 2=trash */
    uint8_t padding[3];             /* Align structure to 64-byte cache line */
};

_Static_assert(sizeof(struct superblock) == 4096, "superblock must be 4096 bytes");
_Static_assert(sizeof(struct inode) == 512, "inode must be 512 bytes");
_Static_assert(sizeof(struct dir_entry) == 64, "dir_entry must be 64 bytes");

#endif