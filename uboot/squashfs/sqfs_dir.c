// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 */

#include <errno.h>
#include <linux/types.h>
#include <linux/byteorder/little_endian.h>
#include <linux/byteorder/generic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "sqfs_filesystem.h"
#include "sqfs_utils.h"

bool sqfs_is_dir(__le16 type)
{
	return (le16_to_cpu(type) == SQFS_DIR_TYPE) ||
		(le16_to_cpu(type) == SQFS_LDIR_TYPE);
}

/*
 * Receives a pointer (void *) to a position in the inode table containing the
 * directory's inode. Returns directory inode offset into the directory table.
 * m_list contains each metadata block's position, and m_count is the number of
 * elements of m_list. Those metadata blocks come from the compressed directory
 * table.
 */
int sqfs_dir_offset(void *dir_i, uint32_t *m_list, int m_count)
{
	struct squashfs_base_inode base;
	struct squashfs_ldir_inode ldir;
	struct squashfs_dir_inode dir;
	uint32_t start_block;
	uint16_t offset;
	int j;

	memcpy(&base, dir_i, sizeof(base));

//	printf("%s: %d debug\n", __func__, __LINE__);
	switch (base.inode_type) {
	case SQFS_DIR_TYPE:
//	printf("%s: %d debug\n", __func__, __LINE__);
		memcpy(&dir, dir_i, sizeof(dir));
//	printf("%s: %d debug\n", __func__, __LINE__);
		start_block = le32_to_cpu(dir.start_block);
//	printf("%s: %d debug\n", __func__, __LINE__);
		offset = le16_to_cpu(dir.offset);
//	printf("%s: %d debug\n", __func__, __LINE__);
		break;
	case SQFS_LDIR_TYPE:
//	printf("%s: %d debug\n", __func__, __LINE__);
		memcpy(&ldir, dir_i, sizeof(ldir));
//	printf("%s: %d debug\n", __func__, __LINE__);
		start_block = le32_to_cpu(ldir.start_block);
//	printf("%s: %d debug\n", __func__, __LINE__);
		offset = le16_to_cpu(ldir.offset);
//	printf("%s: %d debug\n", __func__, __LINE__);
		break;
	default:
		printf("Error: this is not a directory.\n");
		return -EINVAL;
	}

//	printf("Start block: %d\n", *start_block);
//	printf("Offset: %d\n", *offset);

//	printf("%s: %d debug\n", __func__, __LINE__);
	for (j = 0; j < m_count; j++) {
//		printf("m_list[%d]: %d\n", j, m_list[j]);
		if (m_list[j] == start_block)
			return (++j * SQFS_METADATA_BLOCK_SIZE) + offset;
	}

//	printf("%s: %d debug\n", __func__, __LINE__);
	if (start_block == 0)
		return offset;

	printf("Error: invalid inode refence to directory table.\n");

	return -EINVAL;
}

bool sqfs_is_empty_dir(void *dir_i)
{
	struct squashfs_base_inode *base;
	struct squashfs_ldir_inode *ldir;
	struct squashfs_dir_inode *dir;
	uint32_t file_size;


//	printf("%s: %d debug\n", __func__, __LINE__);
	base = malloc(sizeof(*base));
	if (!base)
		return errno;

//	printf("%s: %d debug\n", __func__, __LINE__);
	memcpy(base, dir_i, sizeof(*base));

//	printf("%s: %d debug\n", __func__, __LINE__);
	switch (le16_to_cpu(base->inode_type)) {
	case SQFS_DIR_TYPE:
//	printf("%s: %d debug\n", __func__, __LINE__);
		dir = malloc(sizeof(*dir));
		memcpy(dir, dir_i, sizeof(*dir));
		file_size = le16_to_cpu(dir->file_size);
		free(dir);
		break;
	case SQFS_LDIR_TYPE:
//	printf("%s: %d debug\n", __func__, __LINE__);
		ldir = malloc(sizeof(*ldir));
		memcpy(ldir, dir_i, sizeof(*ldir));
		file_size = le32_to_cpu(ldir->file_size);
		free(ldir);
		break;
	default:
		printf("Error: this is not a directory.\n");
		free(base);
		return false;
	}

//	printf("%s: %d debug\n", __func__, __LINE__);
	free(base);

	return file_size == SQFS_EMPTY_FILE_SIZE;
}
