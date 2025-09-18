// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, Hongzhen Luo
 */

#ifndef __NUMBFS_INTERNAL_H
#define __NUMBFS_INTERNAL_H

#include "disk.h"
#include <stdbool.h>

struct numbfs_superblock_info {
        int fd;
        int feature;
        int total_inodes;
        int free_inodes;
        int data_blocks;
        int free_blocks;
        int ibitmap_start;
        int inode_start;
        int bbitmap_start;
        int data_start;

        long long size;
};

/* TODO: xattr support */
struct numbfs_inode_info {
        /* in */
        struct numbfs_superblock_info *sbi;
        int nid;

        /* out */
        int mode;
        int nlink;
        int uid;
        int gid;
        int size;
        int data[NUMBFS_NUM_DATA_ENTRY];
        int xattr_start;
        int xattr_count;
};

#define NUMBFS_BLOCKS_PER_BLOCK (BYTES_PER_BLOCK * BITS_PER_BYTE)
#define NUMBFS_NODES_PER_BLOCK  (BYTES_PER_BLOCK / sizeof(struct numbfs_inode))

/* calculate the block number of the bitmap related to @blkno */
static inline int numbfs_bmap_blk(int startblk, int blkno)
{
        return startblk + blkno / NUMBFS_BLOCKS_PER_BLOCK;
}

/* calculate the byte number in the block related to @blkno */
static inline int numbfs_bmap_byte(int blkno)
{
        return  (blkno % NUMBFS_BLOCKS_PER_BLOCK) / BITS_PER_BYTE;
}

/* calculate the bit number in the byte related to @blkno */
static inline int numbfs_bmap_bit(int blkno)
{
        return (blkno % NUMBFS_BLOCKS_PER_BLOCK) % BITS_PER_BYTE;
}

static inline int numbfs_inode_blk(struct numbfs_superblock_info *sbi,
                                   int nid)
{
        return sbi->inode_start + nid / NUMBFS_NODES_PER_BLOCK;
}

static inline int numbfs_data_blk(struct numbfs_superblock_info *sbi,
                                  int blk)
{
        return sbi->data_start + blk;
}

/* read/write the blkno-th block in the device */
int numbfs_read_block(struct numbfs_superblock_info *sbi,
                      char buf[BYTES_PER_BLOCK], int blkno);
int numbfs_write_block(struct numbfs_superblock_info *sbi,
                       char buf[BYTES_PER_BLOCK], int blkno);

/* read the on=disk superblock */
int numbfs_get_superblock(struct numbfs_superblock_info *sbi, int fd);

/* data block management */
int numbfs_alloc_block(struct numbfs_superblock_info *sbi, int *blkno);
int numbfs_free_block(struct numbfs_superblock_info *sbi, int blkno);

/* get inode information according inode number*/
int numbfs_get_inode(struct numbfs_superblock_info *sbi,
                     struct numbfs_inode_info *ni);
/* logical block number to physical block address translation */
int numbfs_inode_blkaddr(struct numbfs_inode_info *ni,
                         int pos, bool alloc, bool extent);

/* read/write the logical block in inode's address space */
int numbfs_pwrite_inode(struct numbfs_inode_info *ni,
                        char buf[BYTES_PER_BLOCK], int offset, int len);
int numbfs_pread_inode(struct numbfs_inode_info *ni,
                       char buf[BYTES_PER_BLOCK], int offset, int len);

int numbfs_alloc_inode(struct numbfs_superblock_info *sbi, int *nid);
int numbfs_free_inode(struct numbfs_superblock_info *sbi, int nid);

/* make an empty dir */
int numbfs_empty_dir(struct numbfs_superblock_info *sbi, int pnid);

#endif
