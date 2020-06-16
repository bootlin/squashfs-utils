/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 */

#ifndef SQFS_FILESYSTEM_H
#define SQFS_FILESYSTEM_H

#include <stdint.h>
#include <fs.h>

#define SQFS_UNCOMPRESSED_DATA 0x0002
#define SQFS_MAGIC_NUMBER 0x73717368
/* The three first members of squashfs_dir_index make a total of 12 bytes */
#define SQFS_DIR_INDEX_BASE_LENGTH 12
/* size of metadata (inode and directory) blocks */
#define SQFS_METADATA_BLOCK_SIZE 8192
/* Max. number of fragment entries in a metadata block is 512 */
#define SQFS_MAX_ENTRIES 512
/* Metadata blocks start by a 2-byte length header */
#define SQFS_HEADER_SIZE 2
#define SQFS_LREG_INODE_MIN_SIZE 56
#define SQFS_DIR_HEADER_SIZE 12
#define SQFS_MISC_ENTRY_TYPE -1
#define SQFS_EMPTY_FILE_SIZE 3
#define SQFS_STOP_READDIR 0
#define SQFS_EMPTY_DIR -1
/*
 * A directory entry object has a fixed length of 8 bytes, corresponding to its
 * first four members, plus the size of the entry name, which is equal to
 * 'entry_name' + 1 bytes.
 */
#define SQFS_ENTRY_BASE_LENGTH 8
/* Inode types */
#define SQFS_DIR_TYPE 1
#define SQFS_REG_TYPE 2
#define SQFS_SYMLINK_TYPE 3
#define SQFS_BLKDEV_TYPE 4
#define SQFS_CHRDEV_TYPE 5
#define SQFS_FIFO_TYPE 6
#define SQFS_SOCKET_TYPE 7
#define SQFS_LDIR_TYPE 8
#define SQFS_LREG_TYPE 9
#define SQFS_LSYMLINK_TYPE 10
#define SQFS_LBLKDEV_TYPE 11
#define SQFS_LCHRDEV_TYPE 12
#define SQFS_LFIFO_TYPE 13
#define SQFS_LSOCKET_TYPE 14

struct squashfs_super_block {
	__le32 s_magic;
	__le32 inodes;
	__le32 mkfs_time;
	__le32 block_size;
	__le32 fragments;
	__le16 compression;
	__le16 block_log;
	__le16 flags;
	__le16 no_ids;
	__le16 s_major;
	__le16 s_minor;
	__le64 root_inode;
	__le64 bytes_used;
	__le64 id_table_start;
	__le64 xattr_id_table_start;
	__le64 inode_table_start;
	__le64 directory_table_start;
	__le64 fragment_table_start;
	__le64 export_table_start;
};

struct squashfs_directory_index {
	uint32_t index;
	uint32_t start;
	uint32_t size;
	char name[0];
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
	struct squashfs_directory_index index[0];
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

struct squashfs_directory_entry {
	uint16_t offset;
	uint16_t inode_offset;
	uint16_t type;
	uint16_t name_size;
	char name[0];
};

struct squashfs_directory_header {
	uint32_t count;
	uint32_t start;
	uint32_t inode_number;
};

struct squashfs_fragment_block_entry {
	uint64_t start;
	uint32_t size;
	uint32_t _unused;
};


struct squashfs_dir_stream {
	struct fs_dir_stream *fs_dirs;
	struct fs_dirent *dentp;
	/* table points to a position into the directory table */
	void *table;
	/* uncompressed size of the entire listing, including headers */
	int size;
	int entry_count;
	/* SquashFS structures */
	struct squashfs_directory_header *dir_header;
	struct squashfs_directory_entry *entry;
	union squashfs_inode i;
	/* References to the tables' beginnings */
	char *inode_table;
	char *dir_table;

};

struct squashfs_file_info {
	/* File size in bytes (uncompressed) */
	size_t size;
	/* Reference to list of data blocks's sizes */
	uint32_t *blk_sizes;
	/* Offset into the fragment block */
	uint32_t *offset;
	/* Offset in which the data blocks begin */
	uint64_t start;
	/* Is file fragmented? */
	bool frag;
	/* Compressed fragment */
	bool comp;
};

void *sqfs_find_inode(void *inode_table, int inode_number, int inode_count,
		      uint32_t block_size);

int sqfs_search_entry(union squashfs_inode *i, char **token_list,
		      int token_count, void *inode_table, void *dir_table,
		      int inode_count, int block_size);


int sqfs_dir_offset(union squashfs_inode *i, uint32_t *m_list, int m_count);

int sqfs_read_metablock(void *file_mapping, int offset, bool *compressed,
			uint32_t *data_size);

bool sqfs_is_empty_dir(union squashfs_inode *i);

bool sqfs_is_dir(union squashfs_inode *i);

#endif /* SQFS_FILESYSTEM_H */
