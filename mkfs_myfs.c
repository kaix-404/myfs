#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include "fs_layout.h"

static void kai_set_bit(uint8_t *bmap, int bit_pos) {
    bmap[bit_pos / 8] |= (1 << (bit_pos % 8));
}

int main(void) {
    int img_fd = open("disk.img", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (img_fd < 0) {
        perror("open disk.img");
        return 1;
    }

    size_t img_bytes = (size_t)TOTAL_BLOCKS * BLOCK_SIZE;
    if (ftruncate(img_fd, img_bytes) < 0) {
        perror("ftruncate");
        close(img_fd);
        return 1;
    }

    uint8_t *raw_map = mmap(NULL, img_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, img_fd, 0);
    if (raw_map == MAP_FAILED) {
        perror("mmap");
        close(img_fd);
        return 1;
    }

    memset(raw_map, 0, img_bytes);

    struct superblock *kai_sb = (struct superblock *)(raw_map + SUPERBLOCK_BLK * BLOCK_SIZE);
    kai_sb->magic = FS_MAGIC;
    kai_sb->total_blocks = TOTAL_BLOCKS;
    kai_sb->total_inodes = MAX_INODES;
    kai_sb->free_blocks = DATA_BLOCKS_COUNT - 1; /* 1004 free after root dir allocation */
    kai_sb->free_inodes = MAX_INODES - 1;        /* 127 free after Inode 0 allocation */

    uint8_t *ino_bmap  = raw_map + INODE_BM_BLK * BLOCK_SIZE;
    uint8_t *data_bmap = raw_map + DATA_BM_BLK * BLOCK_SIZE;

    /* Claim Inode 0 and Data Block 0 (Block 19) for '/' */
    kai_set_bit(ino_bmap, 0);
    kai_set_bit(data_bmap, 0);

    struct inode *ino_arr = (struct inode *)(raw_map + INODE_TABLE_START * BLOCK_SIZE);
    time_t ts_now = time(NULL);

    ino_arr[0].mode = S_IFDIR | 0755;
    ino_arr[0].size = BLOCK_SIZE;
    ino_arr[0].blocks[0] = DATA_BLOCK_START;
    ino_arr[0].atime = ts_now;
    ino_arr[0].mtime = ts_now;
    ino_arr[0].ctime = ts_now;
    ino_arr[0].links_count = 2;

    struct dir_entry *root_slots = (struct dir_entry *)(raw_map + DATA_BLOCK_START * BLOCK_SIZE);
    strcpy(root_slots[0].name, ".");
    root_slots[0].inode_no = 0;
    root_slots[0].valid = 1;

    strcpy(root_slots[1].name, "..");
    root_slots[1].inode_no = 0;
    root_slots[1].valid = 1;

    msync(raw_map, img_bytes, MS_SYNC);
    munmap(raw_map, img_bytes);
    close(img_fd);

    printf("Successfully initialized disk.img (4MB, %d blocks)\n", TOTAL_BLOCKS);
    return 0;
}