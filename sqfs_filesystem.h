/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 *
 * sqfs_filesystem.h: inode, directory, fragment, export, u/gid and xattr tables
 */

#ifndef SQFS_FILESYSTEM_H
#define SQFS_FILESYSTEM_H

#include <stdint.h>

#include "sqfs_utils.h"

/* Inode table */

#define SQUASHFS_DIR_TYPE		1
#define SQUASHFS_REG_TYPE		2
#define SQUASHFS_SYMLINK_TYPE		3
#define SQUASHFS_BLKDEV_TYPE		4
#define SQUASHFS_CHRDEV_TYPE		5
#define SQUASHFS_FIFO_TYPE		6
#define SQUASHFS_SOCKET_TYPE		7
#define SQUASHFS_LDIR_TYPE		8
#define SQUASHFS_LREG_TYPE		9
#define SQUASHFS_LSYMLINK_TYPE		10
#define SQUASHFS_LBLKDEV_TYPE		11
#define SQUASHFS_LCHRDEV_TYPE		12
#define SQUASHFS_LFIFO_TYPE		13
#define SQUASHFS_LSOCKET_TYPE		14
/* The three first members of squashfs_dir_index make a total of 12 bytes */
#define DIR_INDEX_BASE_LENGTH 12
#define IS_FRAGMENTED(A) ((A) != 0xFFFFFFFF)

struct squashfs_dir_index {
	__le32 index;
	__le32 start_block;
	__le32 size;
	unsigned char name[0];
};

struct squashfs_base_inode {
	__le16 inode_type;
	__le16 mode;
	__le16 uid;
	__le16 guid;
	__le32 mtime;
	__le32 inode_number;
};

struct squashfs_ipc_inode {
	__le16 inode_type;
	__le16 mode;
	__le16 uid;
	__le16 guid;
	__le32 mtime;
	__le32 inode_number;
	__le32 nlink;
};

struct squashfs_lipc_inode {
	__le16 inode_type;
	__le16 mode;
	__le16 uid;
	__le16 guid;
	__le32 mtime;
	__le32 inode_number;
	__le32 nlink;
	__le32 xattr;
};

struct squashfs_dev_inode {
	__le16 inode_type;
	__le16 mode;
	__le16 uid;
	__le16 guid;
	__le32 mtime;
	__le32 inode_number;
	__le32 nlink;
	__le32 rdev;
};

struct squashfs_ldev_inode {
	__le16 inode_type;
	__le16 mode;
	__le16 uid;
	__le16 guid;
	__le32 mtime;
	__le32 inode_number;
	__le32 nlink;
	__le32 rdev;
	__le32 xattr;
};
struct squashfs_symlink_inode {
	__le16 inode_type;
	__le16 mode;
	__le16 uid;
	__le16 guid;
	__le32 mtime;
	__le32 inode_number;
	__le32 nlink;
	__le32 symlink_size;
	char symlink[0];
};

struct squashfs_reg_inode {
	__le16 inode_type;
	__le16 mode;
	__le16 uid;
	__le16 guid;
	__le32 mtime;
	__le32 inode_number;
	__le32 start_block;
	__le32 fragment;
	__le32 offset;
	__le32 file_size;
	__le32 block_list[0];
};

struct squashfs_lreg_inode {
	__le16 inode_type;
	__le16 mode;
	__le16 uid;
	__le16 guid;
	__le32 mtime;
	__le32 inode_number;
	__le64 start_block;
	__le64 file_size;
	__le64 sparse;
	__le32 nlink;
	__le32 fragment;
	__le32 offset;
	__le32 xattr;
	__le32 block_list[0];
};

struct squashfs_dir_inode {
	__le16 inode_type;
	__le16 mode;
	__le16 uid;
	__le16 guid;
	__le32 mtime;
	__le32 inode_number;
	__le32 start_block;
	__le32 nlink;
	__le16 file_size;
	__le16 offset;
	__le32 parent_inode;
};

struct squashfs_ldir_inode {
	__le16 inode_type;
	__le16 mode;
	__le16 uid;
	__le16 guid;
	__le32 mtime;
	__le32 inode_number;
	__le32 nlink;
	__le32 file_size;
	__le32 start_block;
	__le32 parent_inode;
	__le16 i_count;
	__le16 offset;
	__le32 xattr;
	struct squashfs_dir_index index[0];
};

union squashfs_inode {
	struct squashfs_base_inode *base;
	struct squashfs_dev_inode *dev;
	struct squashfs_ldev_inode *ldev;
	struct squashfs_symlink_inode *symlink;
	struct squashfs_reg_inode *reg;
	struct squashfs_lreg_inode *lreg;
	struct squashfs_dir_inode *dir;
	struct squashfs_ldir_inode *ldir;
	struct squashfs_ipc_inode *ipc;
	struct squashfs_lipc_inode *lipc;
};

/* Position in directory/inode table */
struct inode_reference {
	uint32_t number;
	__le32 start_block;
	__le32 offset;
};

int sqfs_dump_inode_table(void *file_mapping);

void *sqfs_find_inode(void *inode_table, int inode_number, int inode_count,
		      uint32_t block_size);

/* Directory table */

struct directory_index {
	uint32_t index;
	uint32_t start;
	uint32_t name_size;
	char name[0];
};

struct directory_entry {
	uint16_t offset;
	uint16_t inode_offset;
	uint16_t type;
	uint16_t name_size;
	char name[0];
};

struct directory_header {
	uint32_t count;
	uint32_t start;
	uint32_t inode_number;
};

int sqfs_dump_directory_table(void *file_mapping);
int sqfs_dump_entry(void *file_mapping, char *path);
int sqfs_get_dir_offset(union squashfs_inode *i);
int sqfs_dump_dir(union squashfs_inode *dir, union squashfs_inode *parent,
		  void *inode_table, void *dir_table);
void sqfs_print_dir_name(union squashfs_inode *dir,
			 union squashfs_inode *parent,
			 void *inode_table, void *dir_table);
bool sqfs_is_empty_dir(union squashfs_inode *i);

/* Fragment table */

struct fragment_block_entry {
	uint64_t start;
	uint32_t size;
	uint32_t _unused;
};

/* Export table */

/* uid/gid lookup table */

/* xattr table */

/* Metadata blocks */

int sqfs_read_metablock(void *file_mapping, int offset, bool *compressed,
			size_t *data_size);

#endif /* SQFS_FILESYSTEM_H */
