#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <limits.h>
#include <time.h>
#include "fs_layout.h"

static uint8_t *kai_disk_base = NULL;
static struct superblock *kai_sb = NULL;
static uint8_t *kai_ino_bm = NULL;
static uint8_t *kai_data_bm = NULL;
static struct inode *kai_ino_table = NULL;
static char resolved_img_path[PATH_MAX];

static int test_bmap_bit(uint8_t *bmap, int bit_idx) {
    return (bmap[bit_idx / 8] >> (bit_idx % 8)) & 1;
}

static void write_bmap_bit(uint8_t *bmap, int bit_idx, int state) {
    if (state)
        bmap[bit_idx / 8] |= (1 << (bit_idx % 8));
    else
        bmap[bit_idx / 8] &= ~(1 << (bit_idx % 8));
}

static int claim_free_bit(uint8_t *bmap, int max_bits, uint32_t *free_tally) {
    for (int k = 0; k < max_bits; k++) {
        if (!test_bmap_bit(bmap, k)) {
            write_bmap_bit(bmap, k, 1);
            if (free_tally && *free_tally > 0) (*free_tally)--;
            return k;
        }
    }
    return -ENOSPC;
}

static void release_bmap_bit(uint8_t *bmap, int bit_idx, uint32_t *free_tally) {
    if (test_bmap_bit(bmap, bit_idx)) {
        write_bmap_bit(bmap, bit_idx, 0);
        if (free_tally) (*free_tally)++;
    }
}

static uint8_t *blk_mem_addr(uint32_t phys_blk) {
    return kai_disk_base + ((size_t)phys_blk * BLOCK_SIZE);
}

/* Resolve logical block index [0..1034] -> physical block number (0 if sparse) */
static uint32_t lookup_logical_block(struct inode *node, int logical_idx) {
    if (logical_idx < DIRECT_DATA_PTRS) {
        return node->blocks[logical_idx];
    }
    int ind_off = logical_idx - DIRECT_DATA_PTRS;
    if (ind_off >= (int)PTRS_PER_BLOCK || node->blocks[INDIRECT_IDX] == 0) {
        return 0;
    }
    uint32_t *ind_table = (uint32_t *)blk_mem_addr(node->blocks[INDIRECT_IDX]);
    return ind_table[ind_off];
}

/* Resolve or lazily allocate logical block index [0..1034] */
static int alloc_logical_block(struct inode *node, int logical_idx, uint32_t *out_phys) {
    if (logical_idx < DIRECT_DATA_PTRS) {
        if (node->blocks[logical_idx] == 0) {
            int bit = claim_free_bit(kai_data_bm, DATA_BLOCKS_COUNT, &kai_sb->free_blocks);
            if (bit < 0) return -ENOSPC;
            node->blocks[logical_idx] = (uint32_t)(DATA_BLOCK_START + bit);
            memset(blk_mem_addr(node->blocks[logical_idx]), 0, BLOCK_SIZE);
        }
        *out_phys = node->blocks[logical_idx];
        return 0;
    }

    int ind_off = logical_idx - DIRECT_DATA_PTRS;
    if (ind_off >= (int)PTRS_PER_BLOCK) return -EFBIG;

    /* Lazily allocate the 4 KB indirect pointer table block at blocks[11] */
    if (node->blocks[INDIRECT_IDX] == 0) {
        int ind_bit = claim_free_bit(kai_data_bm, DATA_BLOCKS_COUNT, &kai_sb->free_blocks);
        if (ind_bit < 0) return -ENOSPC;
        node->blocks[INDIRECT_IDX] = (uint32_t)(DATA_BLOCK_START + ind_bit);
        memset(blk_mem_addr(node->blocks[INDIRECT_IDX]), 0, BLOCK_SIZE);
    }

    uint32_t *ind_table = (uint32_t *)blk_mem_addr(node->blocks[INDIRECT_IDX]);
    if (ind_table[ind_off] == 0) {
        int data_bit = claim_free_bit(kai_data_bm, DATA_BLOCKS_COUNT, &kai_sb->free_blocks);
        if (data_bit < 0) return -ENOSPC;
        ind_table[ind_off] = (uint32_t)(DATA_BLOCK_START + data_bit);
        memset(blk_mem_addr(ind_table[ind_off]), 0, BLOCK_SIZE);
    }

    *out_phys = ind_table[ind_off];
    return 0;
}

static void free_inode_data_blocks(struct inode *node) {
    /* 1. Free direct data blocks [0..10] */
    for (int idx = 0; idx < DIRECT_DATA_PTRS; idx++) {
        if (node->blocks[idx] != 0) {
            int data_bit = (int)node->blocks[idx] - DATA_BLOCK_START;
            memset(blk_mem_addr(node->blocks[idx]), 0, BLOCK_SIZE);
            release_bmap_bit(kai_data_bm, data_bit, &kai_sb->free_blocks);
            node->blocks[idx] = 0;
        }
    }
    /* 2. Free single-indirect data blocks and the indirect table block itself */
    if (node->blocks[INDIRECT_IDX] != 0) {
        uint32_t *ind_table = (uint32_t *)blk_mem_addr(node->blocks[INDIRECT_IDX]);
        for (int j = 0; j < (int)PTRS_PER_BLOCK; j++) {
            if (ind_table[j] != 0) {
                int data_bit = (int)ind_table[j] - DATA_BLOCK_START;
                memset(blk_mem_addr(ind_table[j]), 0, BLOCK_SIZE);
                release_bmap_bit(kai_data_bm, data_bit, &kai_sb->free_blocks);
                ind_table[j] = 0;
            }
        }
        int ind_bit = (int)node->blocks[INDIRECT_IDX] - DATA_BLOCK_START;
        memset(blk_mem_addr(node->blocks[INDIRECT_IDX]), 0, BLOCK_SIZE);
        release_bmap_bit(kai_data_bm, ind_bit, &kai_sb->free_blocks);
        node->blocks[INDIRECT_IDX] = 0;
    }
    node->size = 0;
}

static void reclaim_trashed_inode(uint32_t ino_num) {
    if (ino_num == 0 || ino_num >= MAX_INODES) return;
    struct inode *node = &kai_ino_table[ino_num];
    free_inode_data_blocks(node);
    memset(node, 0, sizeof(struct inode));
    release_bmap_bit(kai_ino_bm, (int)ino_num, &kai_sb->free_inodes);
}

static int lookup_path_inode(const char *vfs_path) {
    if (strcmp(vfs_path, "/") == 0) return 0;

    char path_copy[PATH_MAX];
    strncpy(path_copy, vfs_path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    int walk_ino = 0;
    char *tok_ctx = NULL;
    char *comp = strtok_r(path_copy, "/", &tok_ctx);

    while (comp != NULL) {
        struct inode *cur_node = &kai_ino_table[walk_ino];
        if (!S_ISDIR(cur_node->mode)) return -ENOTDIR;

        struct dir_entry *entries = (struct dir_entry *)blk_mem_addr(cur_node->blocks[0]);
        int found_ino = -ENOENT;

        for (int s = 0; s < 64; s++) {
            if (entries[s].valid == 1 && strcmp(entries[s].name, comp) == 0) {
                found_ino = (int)entries[s].inode_no;
                break;
            }
        }
        if (found_ino < 0) return -ENOENT;
        walk_ino = found_ino;
        comp = strtok_r(NULL, "/", &tok_ctx);
    }
    return walk_ino;
}

static int separate_parent_leaf(const char *vfs_path, int *dir_ino, char *base_name) {
    char buf_copy[PATH_MAX];
    strncpy(buf_copy, vfs_path, sizeof(buf_copy) - 1);
    buf_copy[sizeof(buf_copy) - 1] = '\0';

    char *last_sep = strrchr(buf_copy, '/');
    if (!last_sep) return -EINVAL;

    if (last_sep == buf_copy) {
        *dir_ino = 0;
        strncpy(base_name, last_sep + 1, MAX_FILE_NAME - 1);
        base_name[MAX_FILE_NAME - 1] = '\0';
    } else {
        *last_sep = '\0';
        *dir_ino = lookup_path_inode(buf_copy);
        if (*dir_ino < 0) return *dir_ino;
        strncpy(base_name, last_sep + 1, MAX_FILE_NAME - 1);
        base_name[MAX_FILE_NAME - 1] = '\0';
    }
    if (strlen(base_name) == 0) return -EINVAL;
    return 0;
}

static void *myfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    if (conn) {
        conn->want &= ~FUSE_CAP_ATOMIC_O_TRUNC;
    }
    if (cfg) {
        cfg->kernel_cache = 0;
    }

    int disk_fd = open(resolved_img_path, O_RDWR);
    if (disk_fd < 0) {
        perror("open disk.img in myfs_init");
        exit(1);
    }

    size_t map_len = (size_t)TOTAL_BLOCKS * BLOCK_SIZE;
    kai_disk_base = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, disk_fd, 0);
    close(disk_fd);

    if (kai_disk_base == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }

    kai_sb = (struct superblock *)(kai_disk_base + SUPERBLOCK_BLK * BLOCK_SIZE);
    kai_ino_bm = kai_disk_base + INODE_BM_BLK * BLOCK_SIZE;
    kai_data_bm = kai_disk_base + DATA_BM_BLK * BLOCK_SIZE;
    kai_ino_table = (struct inode *)(kai_disk_base + INODE_TABLE_START * BLOCK_SIZE);

    return NULL;
}

static int myfs_getattr(const char *path, struct stat *stbuf,
                        struct fuse_file_info *fi) {
    (void)fi;
    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/.diskstat") == 0 ||
        strcmp(path, "/.recover") == 0 ||
        strcmp(path, "/.purge") == 0) {
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = 2048;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        return 0;
    }

    int ino = lookup_path_inode(path);
    if (ino < 0) return ino;

    struct inode *node = &kai_ino_table[ino];
    stbuf->st_mode = node->mode;
    stbuf->st_nlink = node->links_count;
    stbuf->st_size = node->size;
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    stbuf->st_atime = node->atime;
    stbuf->st_mtime = node->mtime;
    stbuf->st_ctime = node->ctime;
    return 0;
}

static int myfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                        off_t offset, struct fuse_file_info *fi,
                        enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;

    int ino = lookup_path_inode(path);
    if (ino < 0) return ino;

    struct inode *node = &kai_ino_table[ino];
    if (!S_ISDIR(node->mode)) return -ENOTDIR;

    struct dir_entry *entries = (struct dir_entry *)blk_mem_addr(node->blocks[0]);
    for (int s = 0; s < 64; s++) {
        if (entries[s].valid == 1) {
            filler(buf, entries[s].name, NULL, 0, 0);
        }
    }
    return 0;
}

static int myfs_create(const char *path, mode_t mode,
                       struct fuse_file_info *fi) {
    (void)fi;
    int parent_ino;
    char leaf[MAX_FILE_NAME];
    int rc = separate_parent_leaf(path, &parent_ino, leaf);
    if (rc < 0) return rc;

    struct inode *p_node = &kai_ino_table[parent_ino];
    struct dir_entry *entries = (struct dir_entry *)blk_mem_addr(p_node->blocks[0]);

    int open_slot = -1;
    for (int s = 0; s < 64; s++) {
        if (entries[s].valid == 1 && strcmp(entries[s].name, leaf) == 0)
            return -EEXIST;
        if (entries[s].valid == 0 && open_slot < 0)
            open_slot = s;
    }
    if (open_slot < 0) return -ENOSPC;

    int allocated_ino = claim_free_bit(kai_ino_bm, MAX_INODES, &kai_sb->free_inodes);
    if (allocated_ino < 0) return allocated_ino;

    struct inode *new_node = &kai_ino_table[allocated_ino];
    memset(new_node, 0, sizeof(struct inode));
    time_t now_ts = time(NULL);
    new_node->mode = S_IFREG | (mode & 0777);
    new_node->size = 0;
    new_node->atime = now_ts;
    new_node->mtime = now_ts;
    new_node->ctime = now_ts;
    new_node->links_count = 1;

    strncpy(entries[open_slot].name, leaf, MAX_FILE_NAME - 1);
    entries[open_slot].name[MAX_FILE_NAME - 1] = '\0';
    entries[open_slot].inode_no = (uint32_t)allocated_ino;
    entries[open_slot].valid = 1;

    p_node->mtime = now_ts;
    return 0;
}

static int myfs_open(const char *path, struct fuse_file_info *fi) {
    if (strcmp(path, "/.diskstat") == 0 ||
        strcmp(path, "/.recover") == 0 ||
        strcmp(path, "/.purge") == 0) {
        return 0;
    }

    int ino = lookup_path_inode(path);
    if (ino < 0) return ino;

    if (fi->flags & O_TRUNC) {
        struct inode *node = &kai_ino_table[ino];
        free_inode_data_blocks(node);
        node->mtime = time(NULL);
    }
    return 0;
}

static int myfs_read(const char *path, char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi) {
    (void)fi;
    char vfs_msg[2048];

    if (strcmp(path, "/.diskstat") == 0) {
        int soft_deleted_count = 0;
        for (int i = 0; i < MAX_INODES; i++) {
            if (test_bmap_bit(kai_ino_bm, i) && S_ISDIR(kai_ino_table[i].mode)) {
                struct dir_entry *entries =
                    (struct dir_entry *)blk_mem_addr(kai_ino_table[i].blocks[0]);
                for (int s = 0; s < 64; s++) {
                    if (entries[s].valid == 2) soft_deleted_count++;
                }
            }
        }
        char line_a[33], line_b[33];
        for (int b = 0; b < 32; b++)
            line_a[b] = test_bmap_bit(kai_data_bm, b) ? '#' : '.';
        line_a[32] = '\0';
        for (int b = 32; b < 64; b++)
            line_b[b - 32] = test_bmap_bit(kai_data_bm, b) ? '#' : '.';
        line_b[32] = '\0';

        int msg_len = snprintf(vfs_msg, sizeof(vfs_msg),
            "========================================\n"
            "       MYFS LIVE STORAGE MONITOR        \n"
            "========================================\n"
            "Magic Number : 0x%08X\n"
            "Block Size   : %d Bytes (Max File: 4.04 MB)\n"
            "Data Blocks  : %u Used / %u Free (Total: %d)\n"
            "Inodes       : %u Used / %u Free (Total: %d)\n"
            "Trash Bin    : %d file(s) awaiting recovery/purge\n"
            "----------------------------------------\n"
            "Live Data Block Map (First 64 Blocks):\n"
            "[%s]\n[%s]\n"
            "Legend: '#' = Allocated | '.' = Free\n"
            "========================================\n",
            kai_sb->magic, BLOCK_SIZE,
            DATA_BLOCKS_COUNT - kai_sb->free_blocks, kai_sb->free_blocks, TOTAL_BLOCKS,
            MAX_INODES - kai_sb->free_inodes, kai_sb->free_inodes, MAX_INODES,
            soft_deleted_count, line_a, line_b);

        if (offset >= msg_len) return 0;
        if (offset + (off_t)size > msg_len) size = msg_len - offset;
        memcpy(buf, vfs_msg + offset, size);
        return (int)size;
    }

    if (strcmp(path, "/.recover") == 0) {
        int recovered_files = 0;
        for (int i = 0; i < MAX_INODES; i++) {
            if (test_bmap_bit(kai_ino_bm, i) && S_ISDIR(kai_ino_table[i].mode)) {
                struct dir_entry *entries =
                    (struct dir_entry *)blk_mem_addr(kai_ino_table[i].blocks[0]);
                for (int s = 0; s < 64; s++) {
                    if (entries[s].valid == 2) {
                        entries[s].valid = 1;
                        recovered_files++;
                    }
                }
            }
        }
        int msg_len = snprintf(vfs_msg, sizeof(vfs_msg),
            "[MYFS RECOVERY] Restored %d deleted file(s) from Trash!\n",
            recovered_files);
        if (offset >= msg_len) return 0;
        if (offset + (off_t)size > msg_len) size = msg_len - offset;
        memcpy(buf, vfs_msg + offset, size);
        return (int)size;
    }

    if (strcmp(path, "/.purge") == 0) {
        int purged_files = 0;
        for (int i = 0; i < MAX_INODES; i++) {
            if (test_bmap_bit(kai_ino_bm, i) && S_ISDIR(kai_ino_table[i].mode)) {
                struct dir_entry *entries =
                    (struct dir_entry *)blk_mem_addr(kai_ino_table[i].blocks[0]);
                for (int s = 0; s < 64; s++) {
                    if (entries[s].valid == 2) {
                        reclaim_trashed_inode(entries[s].inode_no);
                        memset(&entries[s], 0, sizeof(struct dir_entry));
                        purged_files++;
                    }
                }
            }
        }
        int msg_len = snprintf(vfs_msg, sizeof(vfs_msg),
            "[MYFS PURGE] Permanently freed %d file(s) from Trash.\n",
            purged_files);
        if (offset >= msg_len) return 0;
        if (offset + (off_t)size > msg_len) size = msg_len - offset;
        memcpy(buf, vfs_msg + offset, size);
        return (int)size;
    }

    int ino = lookup_path_inode(path);
    if (ino < 0) return ino;

    struct inode *node = &kai_ino_table[ino];
    if (S_ISDIR(node->mode)) return -EISDIR;
    if (offset >= (off_t)node->size) return 0;
    if (offset + (off_t)size > (off_t)node->size)
        size = node->size - offset;

    size_t copied_total = 0;
    while (copied_total < size) {
        size_t cur_byte = (size_t)offset + copied_total;
        int blk_slot = (int)(cur_byte / BLOCK_SIZE);
        size_t in_blk_off = cur_byte % BLOCK_SIZE;
        size_t slice_len = BLOCK_SIZE - in_blk_off;
        if (slice_len > size - copied_total) slice_len = size - copied_total;

        uint32_t phys_blk = lookup_logical_block(node, blk_slot);
        if (phys_blk == 0) {
            memset(buf + copied_total, 0, slice_len);
        } else {
            memcpy(buf + copied_total, blk_mem_addr(phys_blk) + in_blk_off, slice_len);
        }
        copied_total += slice_len;
    }
    node->atime = time(NULL);
    return (int)copied_total;
}

static int myfs_write(const char *path, const char *buf, size_t size,
                      off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    int ino = lookup_path_inode(path);
    if (ino < 0) return ino;

    struct inode *node = &kai_ino_table[ino];
    if (S_ISDIR(node->mode)) return -EISDIR;

    size_t cap_limit = (size_t)MAX_FILE_BLOCKS * BLOCK_SIZE;
    if ((size_t)offset >= cap_limit) return -EFBIG;
    if ((size_t)offset + size > cap_limit)
        size = cap_limit - (size_t)offset;

    size_t done_bytes = 0;
    while (done_bytes < size) {
        size_t cur_byte = (size_t)offset + done_bytes;
        int blk_slot = (int)(cur_byte / BLOCK_SIZE);
        size_t in_blk_off = cur_byte % BLOCK_SIZE;
        size_t slice_len = BLOCK_SIZE - in_blk_off;
        if (slice_len > size - done_bytes) slice_len = size - done_bytes;

        uint32_t phys_blk = 0;
        int rc = alloc_logical_block(node, blk_slot, &phys_blk);
        if (rc < 0) return (done_bytes > 0) ? (int)done_bytes : rc;

        memcpy(blk_mem_addr(phys_blk) + in_blk_off, buf + done_bytes, slice_len);
        done_bytes += slice_len;
    }

    if ((size_t)offset + done_bytes > node->size)
        node->size = (uint32_t)((size_t)offset + done_bytes);

    node->mtime = time(NULL);
    return (int)done_bytes;
}

static int myfs_truncate(const char *path, off_t size,
                         struct fuse_file_info *fi) {
    (void)fi;
    int ino = lookup_path_inode(path);
    if (ino < 0) return ino;

    struct inode *node = &kai_ino_table[ino];
    if (size == 0) {
        free_inode_data_blocks(node);
    } else if (size < (off_t)node->size) {
        int keep_blocks = (int)((size + BLOCK_SIZE - 1) / BLOCK_SIZE);
        size_t tail_off = (size_t)size % BLOCK_SIZE;
        if (tail_off > 0) {
            uint32_t last_phys = lookup_logical_block(node, keep_blocks - 1);
            if (last_phys != 0) {
                memset(blk_mem_addr(last_phys) + tail_off, 0, BLOCK_SIZE - tail_off);
            }
        }
        for (int idx = keep_blocks; idx < (int)MAX_FILE_BLOCKS; idx++) {
            if (idx < DIRECT_DATA_PTRS) {
                if (node->blocks[idx] != 0) {
                    int data_bit = (int)node->blocks[idx] - DATA_BLOCK_START;
                    memset(blk_mem_addr(node->blocks[idx]), 0, BLOCK_SIZE);
                    release_bmap_bit(kai_data_bm, data_bit, &kai_sb->free_blocks);
                    node->blocks[idx] = 0;
                }
            } else if (node->blocks[INDIRECT_IDX] != 0) {
                uint32_t *ind_table = (uint32_t *)blk_mem_addr(node->blocks[INDIRECT_IDX]);
                int ind_off = idx - DIRECT_DATA_PTRS;
                if (ind_table[ind_off] != 0) {
                    int data_bit = (int)ind_table[ind_off] - DATA_BLOCK_START;
                    memset(blk_mem_addr(ind_table[ind_off]), 0, BLOCK_SIZE);
                    release_bmap_bit(kai_data_bm, data_bit, &kai_sb->free_blocks);
                    ind_table[ind_off] = 0;
                }
            }
        }
        if (keep_blocks <= DIRECT_DATA_PTRS && node->blocks[INDIRECT_IDX] != 0) {
            int ind_bit = (int)node->blocks[INDIRECT_IDX] - DATA_BLOCK_START;
            memset(blk_mem_addr(node->blocks[INDIRECT_IDX]), 0, BLOCK_SIZE);
            release_bmap_bit(kai_data_bm, ind_bit, &kai_sb->free_blocks);
            node->blocks[INDIRECT_IDX] = 0;
        }
        node->size = (uint32_t)size;
    }
    node->mtime = time(NULL);
    return 0;
}

static int myfs_unlink(const char *path) {
    int parent_ino;
    char leaf[MAX_FILE_NAME];
    int rc = separate_parent_leaf(path, &parent_ino, leaf);
    if (rc < 0) return rc;

    struct inode *p_node = &kai_ino_table[parent_ino];
    struct dir_entry *entries = (struct dir_entry *)blk_mem_addr(p_node->blocks[0]);

    for (int s = 0; s < 64; s++) {
        if (entries[s].valid == 1 && strcmp(entries[s].name, leaf) == 0) {
            if (S_ISDIR(kai_ino_table[entries[s].inode_no].mode))
                return -EISDIR;
            entries[s].valid = 2;
            p_node->mtime = time(NULL);
            return 0;
        }
    }
    return -ENOENT;
}

static int myfs_mkdir(const char *path, mode_t mode) {
    int parent_ino;
    char leaf[MAX_FILE_NAME];
    int rc = separate_parent_leaf(path, &parent_ino, leaf);
    if (rc < 0) return rc;

    struct inode *p_node = &kai_ino_table[parent_ino];
    struct dir_entry *p_entries = (struct dir_entry *)blk_mem_addr(p_node->blocks[0]);

    int open_slot = -1;
    for (int s = 0; s < 64; s++) {
        if (p_entries[s].valid == 1 && strcmp(p_entries[s].name, leaf) == 0)
            return -EEXIST;
        if (p_entries[s].valid == 0 && open_slot < 0)
            open_slot = s;
    }
    if (open_slot < 0) return -ENOSPC;

    int dir_ino = claim_free_bit(kai_ino_bm, MAX_INODES, &kai_sb->free_inodes);
    if (dir_ino < 0) return dir_ino;

    int dir_blk_bit = claim_free_bit(kai_data_bm, DATA_BLOCKS_COUNT, &kai_sb->free_blocks);
    if (dir_blk_bit < 0) {
        release_bmap_bit(kai_ino_bm, dir_ino, &kai_sb->free_inodes);
        return dir_blk_bit;
    }

    struct inode *new_dir = &kai_ino_table[dir_ino];
    memset(new_dir, 0, sizeof(struct inode));
    time_t now_ts = time(NULL);
    new_dir->mode = S_IFDIR | (mode & 0777);
    new_dir->size = BLOCK_SIZE;
    new_dir->blocks[0] = (uint32_t)(DATA_BLOCK_START + dir_blk_bit);
    new_dir->atime = now_ts;
    new_dir->mtime = now_ts;
    new_dir->ctime = now_ts;
    new_dir->links_count = 2;

    struct dir_entry *d_entries = (struct dir_entry *)blk_mem_addr(new_dir->blocks[0]);
    memset(d_entries, 0, BLOCK_SIZE);
    strcpy(d_entries[0].name, ".");
    d_entries[0].inode_no = (uint32_t)dir_ino;
    d_entries[0].valid = 1;

    strcpy(d_entries[1].name, "..");
    d_entries[1].inode_no = (uint32_t)parent_ino;
    d_entries[1].valid = 1;

    strncpy(p_entries[open_slot].name, leaf, MAX_FILE_NAME - 1);
    p_entries[open_slot].name[MAX_FILE_NAME - 1] = '\0';
    p_entries[open_slot].inode_no = (uint32_t)dir_ino;
    p_entries[open_slot].valid = 1;

    p_node->links_count++;
    p_node->mtime = now_ts;
    return 0;
}

static int myfs_rmdir(const char *path) {
    if (strcmp(path, "/") == 0) return -EBUSY;

    int parent_ino;
    char leaf[MAX_FILE_NAME];
    int rc = separate_parent_leaf(path, &parent_ino, leaf);
    if (rc < 0) return rc;

    struct inode *p_node = &kai_ino_table[parent_ino];
    struct dir_entry *p_entries = (struct dir_entry *)blk_mem_addr(p_node->blocks[0]);

    for (int s = 0; s < 64; s++) {
        if (p_entries[s].valid == 1 && strcmp(p_entries[s].name, leaf) == 0) {
            uint32_t target_ino = p_entries[s].inode_no;
            struct inode *t_node = &kai_ino_table[target_ino];
            if (!S_ISDIR(t_node->mode)) return -ENOTDIR;

            struct dir_entry *t_entries =
                (struct dir_entry *)blk_mem_addr(t_node->blocks[0]);
            for (int k = 2; k < 64; k++) {
                if (t_entries[k].valid == 1) return -ENOTEMPTY;
            }

            reclaim_trashed_inode(target_ino);
            memset(&p_entries[s], 0, sizeof(struct dir_entry));
            p_node->links_count--;
            p_node->mtime = time(NULL);
            return 0;
        }
    }
    return -ENOENT;
}

static int myfs_rename(const char *from, const char *to, unsigned int flags) {
    (void)flags;
    int src_dir, dst_dir;
    char src_leaf[MAX_FILE_NAME], dst_leaf[MAX_FILE_NAME];

    if (separate_parent_leaf(from, &src_dir, src_leaf) < 0) return -ENOENT;
    if (separate_parent_leaf(to, &dst_dir, dst_leaf) < 0) return -ENOENT;

    struct dir_entry *s_entries =
        (struct dir_entry *)blk_mem_addr(kai_ino_table[src_dir].blocks[0]);
    int found_slot = -1;
    for (int s = 0; s < 64; s++) {
        if (s_entries[s].valid == 1 && strcmp(s_entries[s].name, src_leaf) == 0) {
            found_slot = s;
            break;
        }
    }
    if (found_slot < 0) return -ENOENT;
    uint32_t moving_ino = s_entries[found_slot].inode_no;

    struct dir_entry *d_entries =
        (struct dir_entry *)blk_mem_addr(kai_ino_table[dst_dir].blocks[0]);
    int dest_slot = -1;
    for (int s = 0; s < 64; s++) {
        if (d_entries[s].valid == 1 && strcmp(d_entries[s].name, dst_leaf) == 0) {
            reclaim_trashed_inode(d_entries[s].inode_no);
            dest_slot = s;
            break;
        }
        if (d_entries[s].valid == 0 && dest_slot < 0)
            dest_slot = s;
    }
    if (dest_slot < 0) return -ENOSPC;

    memset(&s_entries[found_slot], 0, sizeof(struct dir_entry));
    strncpy(d_entries[dest_slot].name, dst_leaf, MAX_FILE_NAME - 1);
    d_entries[dest_slot].name[MAX_FILE_NAME - 1] = '\0';
    d_entries[dest_slot].inode_no = moving_ino;
    d_entries[dest_slot].valid = 1;

    return 0;
}

static int myfs_utimens(const char *path, const struct timespec tv[2],
                        struct fuse_file_info *fi) {
    (void)fi;
    int ino = lookup_path_inode(path);
    if (ino < 0) return ino;
    struct inode *node = &kai_ino_table[ino];
    if (tv) {
        node->atime = tv[0].tv_sec;
        node->mtime = tv[1].tv_sec;
    } else {
        time_t now_ts = time(NULL);
        node->atime = now_ts;
        node->mtime = now_ts;
    }
    return 0;
}

static int myfs_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    memset(stbuf, 0, sizeof(struct statvfs));
    stbuf->f_bsize   = BLOCK_SIZE;
    stbuf->f_frsize  = BLOCK_SIZE;
    stbuf->f_blocks  = kai_sb->total_blocks;
    stbuf->f_bfree   = kai_sb->free_blocks;
    stbuf->f_bavail  = kai_sb->free_blocks;
    stbuf->f_files   = kai_sb->total_inodes;
    stbuf->f_ffree   = kai_sb->free_inodes;
    stbuf->f_favail  = kai_sb->free_inodes;
    stbuf->f_namemax = MAX_FILE_NAME - 1;
    return 0;
}

static const struct fuse_operations my_ops = {
    .init     = myfs_init,
    .getattr  = myfs_getattr,
    .readdir  = myfs_readdir,
    .create   = myfs_create,
    .open     = myfs_open,
    .read     = myfs_read,
    .write    = myfs_write,
    .truncate = myfs_truncate,
    .unlink   = myfs_unlink,
    .mkdir    = myfs_mkdir,
    .rmdir    = myfs_rmdir,
    .rename   = myfs_rename,
    .utimens  = myfs_utimens,
    .statfs   = myfs_statfs,
};

int main(int argc, char *argv[]) {
    if (!realpath("disk.img", resolved_img_path)) {
        perror("realpath(disk.img) failed; run ./mkfs_myfs first");
        return 1;
    }
    return fuse_main(argc, argv, &my_ops, NULL);
}