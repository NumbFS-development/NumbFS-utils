// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, Hongzhen Luo
 */

#include "internal.h"
#include "utils.h"
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DOT             "."
#define DOTDOT          ".."
#define DOTLEN          strlen(DOT)
#define DOTDOTLEN       strlen(DOTDOT)

int numbfs_read_block(struct numbfs_superblock_info *sbi,
                      char buf[BYTES_PER_BLOCK], int blkno)
{
        int ret;

        ret = pread(sbi->fd, buf, BYTES_PER_BLOCK, blkno * BYTES_PER_BLOCK);
        if (ret != BYTES_PER_BLOCK) {
                fprintf(stderr, "failed to read block@%d\n", blkno);
                return -EIO;
        }
        return 0;
}

int numbfs_write_block(struct numbfs_superblock_info *sbi,
                       char buf[BYTES_PER_BLOCK], int blkno)
{
        int ret;

        ret = pwrite(sbi->fd, buf, BYTES_PER_BLOCK, blkno * BYTES_PER_BLOCK);
        if (ret != BYTES_PER_BLOCK) {
                fprintf(stderr, "failed to write block@%d\n", blkno);
                return -EIO;
        }
        return 0;
}

/* get the superblock info from device@fd */
int numbfs_get_superblock(struct numbfs_superblock_info *sbi, int fd)
{
        struct numbfs_super_block *sb;
        char buf[BYTES_PER_BLOCK];
        int err;

        sbi->fd = fd;

        err = numbfs_read_block(sbi, buf, NUMBFS_SUPER_OFFSET / BYTES_PER_BLOCK);
        if (err)
                return err;

        sb = (struct numbfs_super_block*)buf;
        if (le32_to_cpu(sb->s_magic) != NUMBFS_MAGIC) {
                fprintf(stderr, "[corrupted] invalid superblock, magic: %X\n", le32_to_cpu(sb->s_magic));
                return -EINVAL;
        }

        sbi->ibitmap_start      = le32_to_cpu(sb->s_ibitmap_start);
        sbi->inode_start        = le32_to_cpu(sb->s_inode_start);
        sbi->bbitmap_start      = le32_to_cpu(sb->s_bbitmap_start);
        sbi->data_start         = le32_to_cpu(sb->s_data_start);
        sbi->total_inodes       = le32_to_cpu(sb->s_total_inodes);
        sbi->free_inodes        = le32_to_cpu(sb->s_free_inodes);
        sbi->data_blocks        = le32_to_cpu(sb->s_data_blocks);
        sbi->free_blocks        = le32_to_cpu(sb->s_free_blocks);
        sbi->feature            = le32_to_cpu(sb->s_feature);
        return 0;
}

static int numbfs_bitmap_alloc(struct numbfs_superblock_info *sbi, int startblk,
                               int total, int *res, int *status)
{
        int err, i, byte, bit;
        char buf[BYTES_PER_BLOCK];

        for (i = 0; i < total; i++) {
                /* read a new block */
                if (i % NUMBFS_BLOCKS_PER_BLOCK == 0) {
                        err = numbfs_read_block(sbi, buf, numbfs_bmap_blk(startblk, i));
                        if (err)
                                return err;
                }

                byte = numbfs_bmap_byte(i);
                bit = numbfs_bmap_bit(i);
                if (!(buf[byte] & (1 << bit))) {
                        *res = i;
                        /* set this bit to 1 */
                        buf[byte] |= (1 << bit);
                        err = numbfs_write_block(sbi, buf, numbfs_bmap_blk(startblk, i));
                        if (err)
                                return err;
                        break;
                }

        }
        *status -= 1;
        return 0;
}

/* alloc a free data block */
int numbfs_alloc_block(struct numbfs_superblock_info *sbi, int *blkno)
{
        if (!sbi->free_blocks)
                return -ENOMEM;

        return numbfs_bitmap_alloc(sbi, sbi->bbitmap_start, sbi->data_blocks,
                                   blkno, &sbi->free_blocks);
}

static int numbfs_bitmap_free(struct numbfs_superblock_info *sbi, int startblk,
                              int free, int *status)
{
        char buf[BYTES_PER_BLOCK];
        int err, byte, bit;

        err = numbfs_read_block(sbi, buf, numbfs_bmap_blk(startblk, free));
        if (err)
                return err;

        byte = numbfs_bmap_byte(free);
        bit = numbfs_bmap_bit(free);
        BUG_ON(!(buf[byte] & (1 << bit)));
        buf[byte] &= ~(1 << bit);

        err = numbfs_write_block(sbi, buf, numbfs_bmap_blk(startblk, free));
        *status += 1;
        return 0;
}

/* free a data block */
int numbfs_free_block(struct numbfs_superblock_info *sbi, int blkno)
{
        if (blkno >= sbi->data_blocks)
                return -EINVAL;

        return numbfs_bitmap_free(sbi, sbi->bbitmap_start,
                                  blkno, &sbi->free_blocks);
}

/* get the inode info at @ni->nid */
int numbfs_get_inode(struct numbfs_superblock_info *sbi,
                     struct numbfs_inode_info *ni)
{
        struct numbfs_inode *inode;
        char buf[BYTES_PER_BLOCK];
        int err, i;

        err = numbfs_read_block(sbi, buf, numbfs_inode_blk(sbi, ni->nid));
        if (err)
                return err;

        inode = ((struct numbfs_inode*)buf) + (ni->nid % NUMBFS_NODES_PER_BLOCK);
        ni->sbi = sbi;
        ni->mode   = le32_to_cpu(inode->i_mode);
        ni->nlink  = le16_to_cpu(inode->i_nlink);
        ni->uid    = le16_to_cpu(inode->i_uid);
        ni->gid    = le16_to_cpu(inode->i_gid);
        ni->size   = le32_to_cpu(inode->i_size);
        for (i = 0; i < NUMBFS_NUM_DATA_ENTRY; i++)
                ni->data[i] = le32_to_cpu(inode->i_data[i]);
        ni->xattr_start = le32_to_cpu(inode->i_xattr_start);
        ni->xattr_count = inode->i_xattr_count;

        return 0;
}

/*
 * get the block that contains pos-th byte in the address space;
 * if there is a hole, then alloc a block
 */
int numbfs_inode_blkaddr(struct numbfs_inode_info *inode, int pos, bool alloc, bool extent)
{
        int blkno, err;

        if (extent) {
                fprintf(stderr, "error: extent feature currently is not supported!\n");
                return -ENOTSUP;
        }

        if ((pos / BYTES_PER_BLOCK) >= NUMBFS_NUM_DATA_ENTRY) {
                fprintf(stderr, "error: pos@%d is out of range!\n", pos);
                return -E2BIG;
        }

        if (alloc && inode->data[pos / BYTES_PER_BLOCK] == NUMBFS_HOLE) {
                char buf[BYTES_PER_BLOCK];

                err = numbfs_alloc_block(inode->sbi, &blkno);
                if (err) {
                        fprintf(stderr, "failed to alloc data block\n");
                        return err;
                }

                memset(buf, 0, BYTES_PER_BLOCK);
                err = numbfs_write_block(inode->sbi, buf,
                                numbfs_data_blk(inode->sbi, blkno));
                if (err)
                        return err;

                inode->data[pos / BYTES_PER_BLOCK] = blkno;
        }

        return inode->data[pos / BYTES_PER_BLOCK];
}

static int numbfs_dump_inode(struct numbfs_inode_info *ni)
{
        struct numbfs_superblock_info *sbi = ni->sbi;
        struct numbfs_inode *inode;
        char meta[BYTES_PER_BLOCK];
        int nid = ni->nid;
        int i, err;

        err = numbfs_read_block(sbi, meta, numbfs_inode_blk(sbi, nid));
        if (err)
                return err;

        inode = ((struct numbfs_inode*)meta) + (nid % NUMBFS_NODES_PER_BLOCK);
        inode->i_ino    = cpu_to_le16(ni->nid);
        inode->i_mode   = cpu_to_le32(ni->mode);
        inode->i_nlink  = cpu_to_le16(ni->nlink);
        inode->i_uid    = cpu_to_le16(ni->uid);
        inode->i_gid    = cpu_to_le16(ni->gid);
        inode->i_size   = cpu_to_le32(ni->size);
        for (i = 0; i < NUMBFS_NUM_DATA_ENTRY; i++)
                inode->i_data[i] = cpu_to_le32(ni->data[i]);
        inode->i_xattr_start = cpu_to_le32(ni->xattr_start);

        err = numbfs_write_block(sbi, meta, numbfs_inode_blk(sbi, nid));
        if (err) {
                fprintf(stderr, "error: failed to dump inode@%d\n", nid);
                return -EIO;
        }

#ifdef HAVE_NUMBFS_DEBUG
        err = numbfs_read_block(sbi, meta, numbfs_inode_blk(sbi, nid));
        if (err) {
                fprintf(stderr, "error: failed to read inode meta block@%d\n",
                        numbfs_inode_blk(sbi, nid));
                return -EIO;
        }

        inode = ((struct numbfs_inode*)meta) + (nid % NUMBFS_NODES_PER_BLOCK);
        assert(inode->i_nlink == cpu_to_le16(ni->nlink));
        assert(inode->i_size == cpu_to_le32(ni->size));
        for (i = 0; i < NUMBFS_NUM_DATA_ENTRY; i++)
                assert(inode->i_data[i] == cpu_to_le32(ni->data[i]));
#endif

        return 0;
}

/**
 * write the buffer to the blkaddr-th block in the address space
 * @buf: the content
 * @offset: the position in the file's address space
 * @len: write length
 *
 * Note that this helper does not support cross-block write
 */
int numbfs_pwrite_inode(struct numbfs_inode_info *ni,
                        char buf[BYTES_PER_BLOCK], int offset, int len)
{
        int target, err;
        char tmp[BYTES_PER_BLOCK];
        int off = offset % BYTES_PER_BLOCK;

        if (off + len > BYTES_PER_BLOCK)
                return -E2BIG;

        /* extend the inode size with holes */
        ni->size = max(ni->size, offset + len);

        target = numbfs_inode_blkaddr(ni, offset,
                                      true, false);
        if (target < 0)
                return target;

        err = numbfs_read_block(ni->sbi, tmp,
                        numbfs_data_blk(ni->sbi, target));
        if (err)
                return err;

        memcpy(tmp + off, buf, len);

        err = numbfs_write_block(ni->sbi, tmp,
                        numbfs_data_blk(ni->sbi, target));
        if (err)
                return err;

        return numbfs_dump_inode(ni);
}

/* read the blkaddr-th block in the address space */
int numbfs_pread_inode(struct numbfs_inode_info *ni,
                       char buf[BYTES_PER_BLOCK], int offset, int len)
{
        int target, err;
        char tmp[BYTES_PER_BLOCK];
        int off = offset % BYTES_PER_BLOCK;

        if (off + len > BYTES_PER_BLOCK)
                return -E2BIG;

        target = numbfs_inode_blkaddr(ni, offset, false, false);
        if (target < 0 && target != NUMBFS_HOLE)
                return target;

        /* read a hole */
        if (offset >= ni->size || target == NUMBFS_HOLE) {
                memset(buf, len, BYTES_PER_BLOCK);
                return 0;
        }

        err = numbfs_read_block(ni->sbi, tmp,
                        numbfs_data_blk(ni->sbi, target));
        if (err)
                return err;

        memcpy(buf, tmp + off, len);
        return 0;
}

/* get a empty inode */
int numbfs_alloc_inode(struct numbfs_superblock_info *sbi, int *nid)
{
        if (!sbi->free_inodes)
                return -ENOMEM;

        return numbfs_bitmap_alloc(sbi, sbi->ibitmap_start,
                                   sbi->total_inodes, nid, &sbi->free_inodes);
}

int numbfs_free_inode(struct numbfs_superblock_info *sbi, int nid)
{
        if (nid >= sbi->total_inodes)
                return -EINVAL;

        return numbfs_bitmap_free(sbi, sbi->ibitmap_start, nid,
                                  &sbi->free_inodes);
}

static int numbfs_update_timestaps(struct numbfs_inode_info *inode,
                                   long time)
{
        struct numbfs_superblock_info *sbi = inode->sbi;
        struct numbfs_timestamps *nt;
        char buf[BYTES_PER_BLOCK];
        int err, blk;

        err = numbfs_alloc_block(inode->sbi, &blk);
        if (err)
                return err;

        memset(buf, 0, sizeof(buf));
        nt = (struct numbfs_timestamps*)buf;
        nt->t_atime = time;
        nt->t_mtime = time;
        nt->t_ctime = time;

        err = numbfs_write_block(sbi, buf, numbfs_data_blk(sbi, blk));
        if (err)
                return err;

        inode->xattr_start = blk;
        return 0;
}

int numbfs_empty_dir(struct numbfs_superblock_info *sbi, int pnid)
{
        struct numbfs_inode_info inode;
        struct numbfs_dirent *dir;
        char buf[BYTES_PER_BLOCK];
        int nid, err, i;

        err = numbfs_alloc_inode(sbi, &nid);
        if (err)
                return err;

        memset(&inode, 0, sizeof(struct numbfs_inode_info));
        inode.nid = nid;
        inode.sbi = sbi;
        inode.mode = S_IFDIR | 0755;
        inode.nlink = 2;
        inode.uid = (__uint16_t)getuid();
        inode.gid = (__uint16_t)getgid();
        inode.size = 0;
        for (i = 0; i < NUMBFS_NUM_DATA_ENTRY; i++)
                inode.data[i] = NUMBFS_HOLE;

        dir = (struct numbfs_dirent*)buf;
        memcpy(dir->name, DOT, DOTLEN);
        dir->name[DOTLEN] = '\0';
        dir->name_len = DOTLEN;
        dir->ino = cpu_to_le16(nid);
        dir->type = DT_DIR;
        inode.size += sizeof(struct numbfs_dirent);

        dir++;
        memcpy(dir->name, DOTDOT, DOTDOTLEN);
        dir->name[DOTDOTLEN] = '\0';
        dir->name_len = DOTDOTLEN;
        dir->ino = cpu_to_le16(pnid);
        dir->type = DT_DIR;
        inode.size += sizeof(struct numbfs_dirent);

        /* update timestaps */
        err = numbfs_update_timestaps(&inode, (long)time(NULL));
        if (err)
                return err;

        err = numbfs_pwrite_inode(&inode, buf, 0, inode.size);
        if (err)
                return err;
        return nid;
}
