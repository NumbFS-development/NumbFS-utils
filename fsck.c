// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, Hongzhen Luo
 */

#include "internal.h"
#include "disk.h"
#include <getopt.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

static struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"inodes", no_argument, NULL, 'i'},
        {"blocks", no_argument, NULL, 'b'},
        {"nid", required_argument, NULL, 'n'},
        {0, 0, 0, 0}
};

struct numbfs_fsck_cfg {
        bool show_inodes;
        bool show_blocks;
        int nid;
        char *dev;
};

static void numbfs_fsck_help(void)
{
        printf(
                "Usage: [OPTIONS] TARGET\n"
                "Get disk statistics.\n"
                "\n"
                "Gerneral options:\n"
                " --help                display this help information and exit\n"
                " --inodes|-i           display inode usage\n"
                " --blocks|-b           display block usage\n"
                " --nid=X               display the inode information of inode@nid\n"
        );
}

static void numbfs_fsck_parse_args(int argc, char **argv, struct numbfs_fsck_cfg *cfg)
{
        int opt;

        while ((opt = getopt_long(argc, argv, "n:hib", long_options, NULL)) != -1) {
                switch(opt) {
                        case 'h':
                                numbfs_fsck_help();
                                exit(0);
                        case 'i':
                                cfg->show_inodes = true;
                                break;
                        case 'b':
                                cfg->show_blocks = true;
                                break;
                        case 'n':
                                cfg->nid = atoi(optarg);
                                break;
                        default:
                                fprintf(stderr, "Unknown option: %s\n\n", argv[optind - 1]);
                                numbfs_fsck_help();
                                exit(1);
                }
        }

        if (optind >= argc) {
                fprintf(stderr, "missing block device!\n");
                exit(1);
        }

        cfg->dev = strdup(argv[optind++]);
        if (!cfg->dev) {
                fprintf(stderr, "failed to get block device path\n");
                exit(1);
        }
}

static int count_bit(__u8 byte)
{
        int ret = 0, i;

        for (i = 0; i < 8; i ++) {
                ret += (byte & 1);
                byte >>= 1;
        }
        return ret;
}

static int numbfs_fsck_used(char *buf)
{
        int i, ret = 0;

        for (i = 0; i < BYTES_PER_BLOCK; i++)
                ret += count_bit(buf[i]);
        return ret;
}

static inline char *numbfs_dir_type(int type)
{
        if (type == DT_DIR)
                return "DIR    ";
        else if (type == DT_LNK)
                return "SYMLINK";
        else
                return "REGULAR";
}

static void numbfs_time_to_date(char buf[BYTES_PER_BLOCK], long time)
{
        struct tm *timeinfo = localtime((time_t*)&time);

        memset(buf, 0, BYTES_PER_BLOCK);
        strftime(buf, BYTES_PER_BLOCK, "%Y-%m-%d %H:%M:%S", timeinfo);
}

static void numbfs_dump_xattrs(struct numbfs_inode_info *ni)
{
        char buf[BYTES_PER_BLOCK];
        struct numbfs_xattr_entry *xe;
        char name[NUMBFS_XATTR_MAXNAME], value[NUMBFS_XATTR_MAXVALUE];
        int err, i, j;


        if (!ni->xattr_count)
                return;

        err = numbfs_read_block(ni->sbi, buf, numbfs_data_blk(ni->sbi, ni->xattr_start));
        if (err) {
                fprintf(stderr, "error: failed to read xattr block\n");
                return;
        }

        printf("    -------\n");
        printf("    xattrs (count: %d)\n", ni->xattr_count);
        xe = (struct numbfs_xattr_entry*)(buf + NUMBFS_XATTR_ENTRY_START);
        for (i = 0; i < (int)NUMBFS_XATTR_MAX_ENTRY; i++, xe++) {
                int front;

                if (!xe->e_valid)
                        continue;

                memset(name, 0, sizeof(name));
                memset(value, 0, sizeof(value));

                front = NUMBFS_XATTR_MAXNAME - xe->e_nlen - 1;
                for (j = 0; j < front; j++)
                        name[j + xe->e_nlen] = ' ';
                memcpy(name, xe->e_name, xe->e_nlen);

                front = NUMBFS_XATTR_MAXVALUE - xe->e_vlen - 1;
                for (j = 0; j < front; j++)
                        value[j + xe->e_vlen] =  ' ';
                memcpy(value, xe->e_value, xe->e_vlen);
                printf("        type: %02d, name: %s, value: %s\n", xe->e_type, name, value);
        }
        printf("    -------\n");
}

/* show the inode information at @nid */
static int numbfs_fsck_show_inode(struct numbfs_superblock_info *sbi,
                                  int nid)
{
        struct numbfs_inode_info *ni;
        struct numbfs_dirent *dir;
        char buf[BYTES_PER_BLOCK];
        struct numbfs_timestamps nt;
        int err, i;


        ni = malloc(sizeof(*ni));
        if (!ni)
                return -ENOMEM;

        ni->nid = nid;
        ni->sbi = sbi;
        err = numbfs_get_inode(sbi, ni);
        if (err) {
                fprintf(stderr, "error: failed to get inode information\n");
                goto exit;
        }

        err = numbfs_read_block(sbi, buf, numbfs_data_blk(sbi, ni->xattr_start));
        if (err) {
                fprintf(stderr, "error: failed to read xattr block\n");
                goto exit;
        }

        nt = *(struct numbfs_timestamps*)buf;

        printf("================================\n");
        printf("Inode Information\n");
        printf("    inode number:               %d\n", nid);
        if (S_ISDIR(ni->mode))
                printf("    inode type:                 DIR\n");
        else if (S_ISLNK(ni->mode))
                printf("    inode type:                 SYMLINK\n");
        else
                printf("    inode type:                 REGULAR FILE\n");
        printf("    link count:                 %d\n", ni->nlink);
        printf("    inode uid:                  %d\n", ni->uid);
        printf("    inode gid:                  %d\n", ni->gid);
        numbfs_time_to_date(buf, le64_to_cpu(nt.t_atime));
        printf("    inode atime:                %s\n", buf);
        numbfs_time_to_date(buf, le64_to_cpu(nt.t_mtime));
        printf("    inode mtime:                %s\n", buf);
        numbfs_time_to_date(buf, le64_to_cpu(nt.t_ctime));
        printf("    inode ctime:                %s\n", buf);
        printf("    inode size:                 %d\n", ni->size);
        numbfs_dump_xattrs(ni);
        printf("\n");

        if (S_ISDIR(ni->mode)) {
                printf("    DIR CONTENT\n");
                for (i = 0; i < ni->size; i += sizeof(struct numbfs_dirent)) {
                        if (i % BYTES_PER_BLOCK == 0) {
                                err = numbfs_pread_inode(ni, buf, i, BYTES_PER_BLOCK);
                                if (err) {
                                        fprintf(stderr, "error: failed to read block@%d of inode@%d\n",
                                                i / BYTES_PER_BLOCK, nid);
                                        goto exit;
                                }
                        }
                        dir = (struct numbfs_dirent*)&buf[i % BYTES_PER_BLOCK];
                        printf("       INODE: %05d, TYPE: %s, NAMELEN: %02d NAME: %s\n",
                                le16_to_cpu(dir->ino), numbfs_dir_type(dir->type),dir->name_len, dir->name);
                }
        }

exit:
        free(ni);
        return err;
}

static int numbfs_fsck(int argc, char **argv)
{
        struct numbfs_fsck_cfg cfg = {
                .show_inodes = 0,
                .show_blocks = 0,
                .nid = -1,
                .dev = NULL
        };
        struct numbfs_superblock_info sbi;
        char buf[BYTES_PER_BLOCK];
        int fd, err, cnt, i;

        numbfs_fsck_parse_args(argc, argv, &cfg);

        fd = open(cfg.dev, O_RDWR, 0644);
        if (fd < 0)
                return -errno;

        err = numbfs_get_superblock(&sbi, fd);
        if (err) {
                fprintf(stderr, "failed to read superblock\n");
                goto exit;
        }

        printf("Superblock Information\n");
        printf("    inode bitmap start:         %d\n", sbi.ibitmap_start);
        printf("    inode zone start:           %d\n", sbi.inode_start);
        printf("    block bitmap start:         %d\n", sbi.bbitmap_start);
        printf("    data zone start:            %d\n", sbi.data_start);
        printf("    free inodes:                %d\n", sbi.free_inodes);
        printf("    total inodes:               %d\n", sbi.total_inodes);
        printf("    total free blocks:          %d\n", sbi.free_blocks);
        printf("    total data blocks:          %d\n", sbi.data_blocks);

        if (cfg.show_inodes) {
                cnt = 0;
                for (i = sbi.ibitmap_start; i < sbi.inode_start; i++) {
                        err = pread(fd, buf, BYTES_PER_BLOCK, i * BYTES_PER_BLOCK);
                        if (err != BYTES_PER_BLOCK) {
                                fprintf(stderr, "failed to read block@%d\n", i);
                                err = -EIO;
                                goto exit;
                        }

                        cnt += numbfs_fsck_used(buf);
                }
                BUG_ON(cnt != sbi.total_inodes - sbi.free_inodes);
                printf("    inodes usage:               %.2f%%\n", 100.0 * cnt / sbi.total_inodes);
        }

        if (cfg.show_blocks) {
                cnt = 0;
                for (i = sbi.bbitmap_start; i < sbi.data_start; i++) {
                        err = pread(fd, buf, BYTES_PER_BLOCK, i * BYTES_PER_BLOCK);
                        if (err != BYTES_PER_BLOCK) {
                                fprintf(stderr, "failed to read block@%d\n", i);
                                err = -EIO;
                                goto exit;
                        }

                        cnt += numbfs_fsck_used(buf);
                }
                BUG_ON(cnt != sbi.data_blocks - sbi.free_blocks);
                printf("    blocks usage:               %.2f%%\n", 100.0 * cnt / sbi.data_blocks);
        }

        if (cfg.nid >= 0) {
                err = numbfs_fsck_show_inode(&sbi, cfg.nid);
                if (err) {
                        fprintf(stderr, "error: failed to show inode information\n");
                        goto exit;
                }
        }

        err = 0;
exit:
        close(fd);
        free(cfg.dev);
        return err;
}

int main(int argc, char **argv)
{
        int err;

        err = numbfs_fsck(argc, argv);
        if (err) {
                fprintf(stderr, "Error occured in fsck, err: %d\n", err);
                exit(1);
        }
        return 0;
}
