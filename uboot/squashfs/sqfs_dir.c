// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "sqfs_filesystem.h"
#include "sqfs_utils.h"

bool sqfs_is_dir(union squashfs_inode *i)
{
	return (i->base->inode_type == SQFS_DIR_TYPE) ||
		(i->base->inode_type == SQFS_LDIR_TYPE);
}

/*
 * Returns directory inode offset into the directory table. m_list contains each
 * metadata block's position, and m_count is the number of elements of m_list.
 * Those metadata blocks come from the compressed directory table.
 */
int sqfs_dir_offset(union squashfs_inode *i, uint32_t *m_list, int m_count)
{
	uint32_t *start_block;
	uint16_t *offset;
	int j;

	switch (i->base->inode_type) {
	case SQFS_DIR_TYPE:
		start_block = &i->dir->start_block;
		offset = &i->dir->offset;
		break;
	case SQFS_LDIR_TYPE:
		start_block = &i->ldir->start_block;
		offset = &i->ldir->offset;
		break;
	default:
		printf("Error: this is not a directory.\n");
		return -EINVAL;
	}

//	printf("Start block: %d\n", *start_block);
//	printf("Offset: %d\n", *offset);

	for (j = 0; j < m_count; j++) {
//		printf("m_list[%d]: %d\n", j, m_list[j]);
		if (m_list[j] == *start_block)
			return (++j * SQFS_METADATA_BLOCK_SIZE) + *offset;
	}

	if (*start_block == 0)
		return *offset;

	printf("Error: invalid inode refence to directory table.\n");

	return -EINVAL;
}

bool sqfs_is_empty_dir(union squashfs_inode *i)
{
	switch (i->base->inode_type) {
	case SQFS_DIR_TYPE:
		return i->dir->file_size == SQFS_EMPTY_FILE_SIZE;
	case SQFS_LDIR_TYPE:
		return i->ldir->file_size == SQFS_EMPTY_FILE_SIZE;
	default:
		printf("Error: this is not a directory.\n");
		return false;
	}
}
