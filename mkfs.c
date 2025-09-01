// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, Hongzhen Luo
 */

#include "internal.h"
#include "numbfs_config.h"
#include "disk.h"
#include <getopt.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#define NUMBFS_DEFAULT_INODES 4096

static struct numbfs_superblock_info sbi;

static struct option log_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"num_inodes", required_argument, NULL, 2},
        {"size", required_argument, NULL, 's'},
        {0, 0, 0, 0}
};

static void numbfs_help_info(void)
{
        printf(
                "Usage: [OPTIONS] TARGET\n"
                "Create a NumbFS filesystem image.\n"
                "\n"
                "Gerneral options:\n"
                " --help                display this help information and exit\n"
                " --num_inodes=#        specify the number of inodes (default: 4096)\n"
                " --size=#{M,K,G}       spacify the filesystem image size\n"
        );
}

#ifdef HAVE_NUMBFS_DEBUG
static void numbfs_show_config(void)
{
        printf(
                "All configs:\n"
                "    num_inodes: %d\n", sbi.total_inodes
        );
}
#endif

static int numbfs_open_dev(char *dev)
{
        int fd, ret;
        struct stat st;

        fd = open(dev, O_RDWR | O_CREAT, 0644);
        if (fd < 0)
                return -errno;

        ret = fstat(fd, &st);
        if (ret) {
                fprintf(stderr, "fail to fstat %s\n", dev);
                close(fd);
                return -errno;
        }

        sbi.fd = fd;
        return 0;
}

static int numbfs_parse_args(int argc, char **argv)
{
        int opt, val, ret;
        char *img_path, unit;
        long long size;

        while ((opt = getopt_long(argc, argv, "s:h", log_options, NULL)) != -1) {
                switch(opt) {
                        case 'h':
                                numbfs_help_info();
                                exit(0);
                        case 2:
                                val = atoi(optarg);
                                if (val <= 0 || val & 0x7) {
                                        fprintf(stderr, "Error: invalid num_inodes: %d, should be positive and multiple of 8\n", val);
                                        return -EINVAL;
                                }
                                sbi.total_inodes = val;
                                sbi.free_inodes = sbi.total_inodes - NUMBFS_ROOT_NID;
                                break;
                        case 's':
                                if (sscanf(optarg, "%lld%c", &size, &unit) < 1)  {
                                        fprintf(stderr, "invalid size format: %s ,should be xxx K, xxx M, xxx G\n", optarg);
                                        exit(1);
                                }
                                if (unit == 'k' || unit == 'K')
                                        sbi.size = size * 1024LL;
                                else if (unit == 'm' || unit == 'M')
                                        sbi.size = size * 1024LL * 1024LL;
                                else if (unit == 'g' || unit == 'G')
                                        sbi.size = size * 1024LL * 1024LL * 1024LL;
                                else
                                        sbi.size = size;
                                break;
                        default:
                                fprintf(stderr, "Unknown option: %s\n\n", argv[optind - 1]);
                                numbfs_help_info();
                                exit(1);
                }
        }

        if (optind >= argc) {
                fprintf(stderr, "miss block device path!\n");
                exit(1);
        }

        img_path = strdup(argv[optind++]);
        if (!img_path) {
                fprintf(stderr, "failed to get block device path\n");
                return -ENOMEM;
        }

        ret = numbfs_open_dev(img_path);
        free(img_path);
        return ret;
}

static void numbfs_init_config(void)
{
        sbi.fd = -1;
        sbi.total_inodes = NUMBFS_DEFAULT_INODES;
        sbi.free_inodes = sbi.total_inodes - NUMBFS_ROOT_NID;
        sbi.size = -1;
}

static int numbfs_mkdir_lostfound(void)
{
#define LOSTFOUND       "lost+found"
#define LOSTFOUNDLEN    strlen(LOSTFOUND)
        struct numbfs_inode_info ni;
        struct numbfs_dirent *dir;
        char buf[BYTES_PER_BLOCK];
        int nid;
        int err;

        nid = numbfs_empty_dir(&sbi, NUMBFS_ROOT_NID);
        if (nid <= NUMBFS_ROOT_NID)
                return -EINVAL;

        ni.sbi = &sbi;
        ni.nid = NUMBFS_ROOT_NID;
        err = numbfs_get_inode(&sbi, &ni);
        if (err)
                return err;

        dir = (struct numbfs_dirent*)buf;
        memcpy(dir->name, LOSTFOUND, LOSTFOUNDLEN);
        dir->name[LOSTFOUNDLEN] = '\0';
        dir->name_len = LOSTFOUNDLEN;
        dir->ino = nid;
        dir->type = DT_DIR;
        err = numbfs_pwrite_inode(&ni, buf, ni.size, sizeof(*dir));
        if (err)
                return err;
#undef  LOSTFOUND
#undef  LOSTFOUNDLEN
        return 0;
}

/*
 * The disk layout:
 * | reserved | superblock | inode bitmap | inodes | block bitmap | data |
 */
static int numbfs_mkfs(void)
{
        int i, err, total_blocks, remain;
        struct numbfs_super_block *sb;
        char buf[BYTES_PER_BLOCK];
        off_t start, end;
        struct stat st;
        long long dev_size;

        err = fstat(sbi.fd, &st);
        if (err) {
                fprintf(stderr, "fail to fstat block dev\n");
                return -errno;
        }

        /* get the block device size */
        if (S_ISBLK(st.st_mode)) {
                if (ioctl(sbi.fd, BLKGETSIZE64, &dev_size) == -1) {
                        perror("fail to get block device's size\n");
                        close(sbi.fd);
                        return -EINVAL;
                }
        } else {
                dev_size = st.st_size;
        }


        if (sbi.size == -1) {
                sbi.size = dev_size;
        } else {
                if (dev_size < sbi.size) {
                        fprintf(stderr, "error: the device size (%lld) is smaller than required size (%lld)\n",
                                        dev_size,
                                        sbi.size);
                        return -EINVAL;
                } else if (dev_size > sbi.size) {
                        fprintf(stderr, "warning: the device size (%lld) is larger than required size (%lld), truncate it\n",
                                        dev_size,
                                        sbi.size);
                }
        }

        if (sbi.size <=  2 * BYTES_PER_BLOCK + round_up(sbi.total_inodes * 64, BYTES_PER_BLOCK) + 3) {
                fprintf(stderr, "device too small, should be at least %d Bytes\n",
                                2 * BYTES_PER_BLOCK + round_up(sbi.total_inodes * 64, BYTES_PER_BLOCK) + 3);
                close(sbi.fd);
                return -EINVAL;
        }

        total_blocks = sbi.size / BYTES_PER_BLOCK;

        /* inode bitmap start block addr */
        sbi.ibitmap_start = 2;
        /* inodes start block add */
        sbi.inode_start = sbi.ibitmap_start +
                        DIV_ROUND_UP(DIV_ROUND_UP(sbi.total_inodes, BITS_PER_BYTE), BYTES_PER_BLOCK);
        /* block bitmap start block addr */
        sbi.bbitmap_start = sbi.inode_start +
                        DIV_ROUND_UP(sbi.total_inodes * sizeof(struct numbfs_inode), BYTES_PER_BLOCK);

        remain = total_blocks - sbi.bbitmap_start - 1;
        /* nr total data blocks */
        sbi.data_blocks = remain -
                        DIV_ROUND_UP(DIV_ROUND_UP(remain, BITS_PER_BYTE), BYTES_PER_BLOCK);
        sbi.free_blocks = sbi.data_blocks;

        start = 2;
        end = sbi.bbitmap_start +
                        DIV_ROUND_UP(DIV_ROUND_UP(sbi.data_blocks, BITS_PER_BYTE), BYTES_PER_BLOCK);
        memset(buf, 0, sizeof(buf));
        /* clear all the bits in range [start, end] */
        for (i = start; i < end; i++) {
                err = pwrite(sbi.fd, buf, BYTES_PER_BLOCK, i * BYTES_PER_BLOCK);
                if (err != BYTES_PER_BLOCK) {
                        fprintf(stderr, "failed to write to block@[%d, %d]\n", i, i + 1);
                        return -EIO;
                }
        }

        /* set all the data array to NUMBFS_HOLE */
        for (i = sbi.inode_start; i < sbi.bbitmap_start; i++) {
                int j, k;
                struct numbfs_inode *inode;

                err = numbfs_read_block(&sbi, buf, i);
                if (err)
                        return err;

                for (j = 0; j < (int)NUMBFS_NODES_PER_BLOCK; j++) {
                        inode = ((struct numbfs_inode*)buf) + j;
                        for (k = 0; k < NUMBFS_NUM_DATA_ENTRY; k++)
                                inode->i_data[k] = cpu_to_le32(NUMBFS_HOLE);
                }

                err = numbfs_write_block(&sbi, buf, i);
                if (err)
                        return err;
        }

        /* data zone start block addr */
        sbi.data_start = end;

#ifdef HAVE_NUMBFS_DEBUG
        printf("Superblock information:\n");
        printf("    num_inodes: %d\n", sbi.total_inodes);
        printf("    ibitmap_start: %d\n", sbi.ibitmap_start);
        printf("    inodes_start: %d\n", sbi.inode_start);
        printf("    bbitmap_start: %d\n", sbi.bbitmap_start);
        printf("    num_free_blocks: %d\n", sbi.free_blocks);
#endif

        /* create the root inode */
        err = numbfs_empty_dir(&sbi, NUMBFS_ROOT_NID);
        if (err != NUMBFS_ROOT_NID) {
                fprintf(stderr, "failed to prepare root inode, err: %d\n", err);
                return err;
        }

        err = numbfs_mkdir_lostfound();
        if (err)
                return err;

        memset(buf, 0, BYTES_PER_BLOCK);
        sb                      = (struct numbfs_super_block*)buf;
        sb->s_magic             = NUMBFS_MAGIC;
        sb->s_feature           = cpu_to_le32(0);
        sb->s_ibitmap_start     = cpu_to_le32(sbi.ibitmap_start);
        sb->s_inode_start       = cpu_to_le32(sbi.inode_start);
        sb->s_bbitmap_start     = cpu_to_le32(sbi.bbitmap_start);
        sb->s_data_start        = cpu_to_le32(sbi.data_start);
        sb->s_total_inodes      = cpu_to_le32(sbi.total_inodes);
        sb->s_free_inodes       = cpu_to_le32(sbi.free_inodes);
        sb->s_data_blocks       = cpu_to_le32(sbi.data_blocks);
        sb->s_free_blocks       = cpu_to_le32(sbi.free_blocks);

        return numbfs_write_block(&sbi, buf, NUMBFS_SUPER_OFFSET / BYTES_PER_BLOCK);
}

static void numbfs_cleanup(void)
{
        if (sbi.fd >= 0)
                close(sbi.fd);
}

int main(int argc, char **argv)
{
        int err;

        numbfs_init_config();

        err = numbfs_parse_args(argc, argv);
        if (err) {
                fprintf(stderr, "Error occurred while parsing arguments.\n");
                goto exit;
        }

#ifdef HAVE_NUMBFS_DEBUG
        numbfs_show_config();
#endif

        err = numbfs_mkfs();
        if (err)
                fprintf(stderr, "Error: failed to mkfs\n");

exit:
        numbfs_cleanup();
        return 0;
}
