/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 *
 * sqfs_utils.h: function prototypes and struct definitions, included at main.c
 */

#ifndef SQFS_UTILS_H
#define SQFS_UTILS_H

#include <stdbool.h>

#define __bitwise
#define CHECK_FLAG(flag, bit) (((flag) >> (bit)) & 1)
#define __aligned_u64 __u64 __aligned(8)
#define __aligned_be64 __be64 __aligned(8)
#define __aligned_le64 __le64 __aligned(8)
#define BIT(x) (1 << (x))
#define BITS_PER_LONG 64
#define GENMASK(h, l) \
	(((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))

/* Metadata blocks start by a 2-byte length header */
#define HEADER_SIZE 2

/*
 * These two macros work as getters for a metada block header, retrieving the
 * data size and if it is compressed/uncompressed
 */
#define IS_COMPRESSED(A) (!((A) & BIT(15)))
#define DATA_SIZE(A) ((A) & GENMASK(14, 0))

#define SUPER_BLOCK_SIZE 96

//#define DEBUG
#ifdef DEBUG
#define DEBUG_TEST 1
#else
#define DEBUG_TEST 0
#endif

#define printd(...) \
	do { if (DEBUG_TEST) {\
		fprintf(stderr, "\n%s:%d:%s()", __FILE__, __LINE__, __func__);\
		fprintf(stderr, "\n\t" __VA_ARGS__);\
	} \
	} while (0)\

#define METADATA_BLOCK_SIZE 8192

typedef __signed__ char __s8;
typedef unsigned char __u8;
typedef __signed__ short __s16;
typedef unsigned short __u16;
typedef __signed__ int __s32;
typedef unsigned int __u32;
typedef __signed__ long __s64;
typedef unsigned long __u64;
typedef __u16 __bitwise __le16;
typedef __u16 __bitwise __be16;
typedef __u32 __bitwise __le32;
typedef __u32 __bitwise __be32;
typedef __u64 __bitwise __le64;
typedef __u64 __bitwise __be64;

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
	__le64 lookup_table_start;
};

struct super_block_flags {
	/* check: unused
	 * uncompressed_ids: not supported
	 */
	bool uncompressed_inodes;
	bool uncompressed_data;
	bool check;
	bool uncompressed_frags;
	bool no_frags;
	bool always_frags;
	bool duplicates;
	bool exportable;
	bool uncompressed_xattrs;
	bool no_xattrs;
	bool compressor_options;
	bool uncompressed_ids;
};

enum squashfs_compression_type {
	ZLIB = 1,
	LZMA,
	LZO,
	XZ,
	LZ4,
	ZSTD,
};

int sqfs_dump_sblk(void *file_mapping);
int sqfs_fill_sblk_flags(struct super_block_flags *sblkf, unsigned short flags);
int sqfs_dump_sblk_flags(struct super_block_flags *sblkf);

#endif /* SQFS_UTILS_H  */
