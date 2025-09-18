// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025, Hongzhen Luo
 */

#ifndef __NUMBFS_DISK_H
#define __NUMBFS_DISK_H

#include "utils.h"
#include <linux/types.h>
#include <dirent.h>

#define NUMBFS_MAGIC    0x4E554D42 /* "NUMB" */

/* the first block is reserved */
#define NUMBFS_SUPER_OFFSET BYTES_PER_BLOCK

#define NUMBFS_HOLE	(-32)

/* root inode number */
#define NUMBFS_ROOT_NID	0

#define NUMBFS_NUM_DATA_ENTRY	10
#define NUMBFS_MAX_PATH_LEN	60
#define NUMBFS_MAX_ATTR 32

/* 128-byte on-disk numbfs superblock, 64 bytes should be enough, but... */
struct numbfs_super_block {
	__le32 s_magic;
	/* feature bits */
	__le32 s_feature;
	/* block addr of inode bitmap */
	__le32 s_ibitmap_start;
	/* block addr of inode zone */
	__le32 s_inode_start;
	/* block addr of block bitmap */
	__le32 s_bbitmap_start;
	/* block addr of data start */
	__le32 s_data_start;
	/* total num of inodes */
	__le32 s_total_inodes;
	/* num of free inodes */
	__le32 s_free_inodes;
	/* num of total data blocks */
	__le32 s_data_blocks;
	/* num of free data blocks */
	__le32 s_free_blocks;
	/* reserved */
	__u8 s_reserved[88];
};

/* 64-byte on-disk numbfs inode */
struct numbfs_inode {
	__le16 i_ino;
	__le16 i_nlink;
	__le16 i_uid;
	__le16 i_gid;
	__le32 i_mode;
	__le32 i_size;
	/* start block addr of xattrs */
	__le32 i_xattr_start;
	/* number of xattrs */
	__u8 i_xattr_count;
	__u8 reserved2[3]; /* padding */
	/* block addr of data blocks */
	__le32 i_data[10];
};

/* 64-byte on-disk numbfs dirent */
struct numbfs_dirent {
	__u8 name_len;
	__u8 type;
	char name[NUMBFS_MAX_PATH_LEN];
	__le16 ino;
};

struct numbfs_timestamps {
	__le64 t_atime;
	__le64 t_mtime;
	__le64 t_ctime;
	__u8 reserved[8];
};

/* xattr name indexes */
#define NUMBFS_XATTR_INDEX_USER              1
#define NUMBFS_XATTR_INDEX_TRUSTED           2

#define NUMBFS_XATTR_MAXNAME	16
#define NUMBFS_XATTR_MAXVALUE	32

/* on-disk xattr entry */
struct numbfs_xattr_entry {
	__u8 e_valid;
	__u8 e_type;
	__u8 e_nlen;
	__u8 e_vlen;
	__u8 e_name[NUMBFS_XATTR_MAXNAME];
	__u8 e_value[NUMBFS_XATTR_MAXVALUE];
};

#define NUMBFS_XATTR_MAX_ENTRY \
	((BYTES_PER_BLOCK - sizeof(struct numbfs_timestamps)) / sizeof(struct numbfs_xattr_entry))
#define NUMBFS_XATTR_ENTRY_START	(sizeof(struct numbfs_timestamps))

/* check the on-disk layout at compile time */
static inline void numbfs_check_ondisk(void)
{
	NUMBFS_BUILD_BUG_ON(sizeof(struct numbfs_super_block) != 128);
	NUMBFS_BUILD_BUG_ON(sizeof(struct numbfs_inode) != 64);
	NUMBFS_BUILD_BUG_ON(sizeof(struct numbfs_dirent) != 64);
	NUMBFS_BUILD_BUG_ON(sizeof(struct numbfs_timestamps) != 32);
}

#endif
