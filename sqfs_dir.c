// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 *
 * sqfs_dir.c:		support directory table parsing
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

#include "sqfs_filesystem.h"
#include "sqfs_utils.h"
#include "sqfs_decompressor.h"

#define MAJOR_NUMBER_BITMASK GENMASK(15, 8)
#define MINOR_NUMBER_BITMASK GENMASK(7, 0)
#define LREG_INODE_MIN_SIZE 56
/*
 * A directory entry object has a fixed length of 8 bytes, corresponding to its
 * first four members, plus the size of the entry name, which is equal to
 * 'entry_name' + 1 bytes.
 */
#define ENTRY_BASE_LENGTH 8
#define EMPTY_FILE_SIZE 3

/* Returns directory inode offset into the directory table */
int sqfs_get_dir_offset(union squashfs_inode *i)
{
	switch (i->base->inode_type) {
	case SQUASHFS_DIR_TYPE:
		return (i->dir->start_block * METADATA_BLOCK_SIZE) +
			i->dir->offset;
	case SQUASHFS_LDIR_TYPE:
		return (i->ldir->start_block * METADATA_BLOCK_SIZE) +
			i->ldir->offset;
	default:
		printf("Error: this is not a directory.\n");
		return -EINVAL;
	}
}

bool sqfs_is_empty_dir(union squashfs_inode *i)
{
	switch (i->base->inode_type) {
	case SQUASHFS_DIR_TYPE:
		return i->dir->file_size == EMPTY_FILE_SIZE;
	case SQUASHFS_LDIR_TYPE:
		return i->ldir->file_size == EMPTY_FILE_SIZE;
	default:
		printf("Error: this is not a directory.\n");
		return false;
	}
}

void sqfs_print_dir_name(union squashfs_inode *dir,
			 union squashfs_inode *parent,
			 void *inode_table, void *dir_table)
{
	int k, l, total_length = 0, inode_number, name_length;
	struct directory_header *parent_header;
	struct directory_entry *entry;

	/*
	 * Retrieve the parent inode in the directory table,
	 * since only the parent holds this directory's name within its entries.
	 */
	parent_header = dir_table +
		(parent->dir->start_block * METADATA_BLOCK_SIZE) +
		parent->dir->offset;

	for (k = 0; k <= parent_header->count; k++) {
		entry = (void *)parent_header + sizeof(*parent_header) +
			total_length;

		name_length = entry->name_size + 1;
		inode_number = parent_header->inode_number +
			entry->inode_offset;

		if (inode_number == dir->dir->inode_number) {
			printf("Name: ");
			for (l = 0; l < name_length; l++)
				printf("%c", entry->name[l]);
			printf("\n");
		}

		total_length += ENTRY_BASE_LENGTH + name_length;
	}
}

int sqfs_dump_dir(union squashfs_inode *dir, union squashfs_inode *parent,
		  void *inode_table, void *dir_table)
{
	int l, k, total_length = 0, name_length;
	struct directory_header *dir_header;
	struct directory_entry *entry;

	if (dir->base->inode_type == SQUASHFS_DIR_TYPE) {
		dir_header = dir_table +
			(dir->dir->start_block * METADATA_BLOCK_SIZE) +
			dir->dir->offset;
	} else if (dir->base->inode_type == SQUASHFS_LDIR_TYPE) {
		dir_header = dir_table +
			(dir->ldir->start_block * METADATA_BLOCK_SIZE) +
			dir->ldir->offset;
	} else {
		return -EINVAL;
	}

	sqfs_print_dir_name(dir, parent, inode_table, dir_table);
	printd("--- --- --- ---\n");

	/*
	 * For each directory inode, the directory table stores a list of all
	 * entries stored inside, with references back to the inodes that
	 * describe those entries. 'count' is the number of entries, which
	 * matches with the number of contents - 1.
	 */
	printd("Number of contents: %u\n", dir_header->count + 1);

	/* The block's index in the Inode Table where the inode is stored. */
	printd("Inode table offset: 0x%08x\n", dir_header->start);

	/*
	 * An arbitrary inode number. The entries that follow store their inode
	 * number as a difference to this.
	 */
	printd("Inode number: 0x%08x\n", dir_header->inode_number);

	printd("Directory entries:\n");
	total_length = 0;
	for (k = 0; k <= dir_header->count ; k++) {
		entry = (void *)dir_header + sizeof(*dir_header) + total_length;

		/* Entry name */
		printf("%d) ", k + 1);
		name_length = entry->name_size + 1;
		for (l = 0; l < name_length; l++)
			printf("%c", entry->name[l]);
		printf(":\n");

		/*
		 * Inode type: for extended inodes, the corresponding basic type
		 * is stored here instead.
		 */
		switch (entry->type) {
		case SQUASHFS_DIR_TYPE:
		case SQUASHFS_LDIR_TYPE:
			printd("Directory\n");
			break;
		case SQUASHFS_REG_TYPE:
		case SQUASHFS_LREG_TYPE:
			printd("File\n");
			break;
		case SQUASHFS_SYMLINK_TYPE:
		case SQUASHFS_LSYMLINK_TYPE:
			printd("Basic Symlink\n");
			break;
		case SQUASHFS_BLKDEV_TYPE:
		case SQUASHFS_CHRDEV_TYPE:
		case SQUASHFS_LBLKDEV_TYPE:
		case SQUASHFS_LCHRDEV_TYPE:
			printd("Block | Char. device\n");
			break;
		case SQUASHFS_FIFO_TYPE:
		case SQUASHFS_SOCKET_TYPE:
		case SQUASHFS_LFIFO_TYPE:
		case SQUASHFS_LSOCKET_TYPE:
			printd("Fifo | Socket\n");
			break;
		default:
			printd("Unknown inode type\n");
			return -EINVAL;
		}

		/* Offset into the uncompressed inode metadata block */
		printd("Entry offset: 0x%04x\n", entry->offset);

		/*
		 * The difference of this inode's number to the reference
		 * stored in the header
		 */
		printd("Inode offset: 0x%04x\n", entry->inode_offset);

		printf("\n");
		total_length += ENTRY_BASE_LENGTH + name_length;
	}

	printd("--- --- --- ---\n\n");

	return 0;
}

int sqfs_dump_directory_table(void *file_mapping)
{
	int ret = 0, k, l, block_list_size, inode_sizes = 0, dir_count = 0;
	int index_list_size = 0;
	unsigned char *dest_inode_table = NULL, *dest_dir_table = NULL;
	void *dir_table, *inode_table;
	struct squashfs_super_block *sblk;
	union squashfs_inode i, parent;
	unsigned long dest_len;
	size_t data_size;
	bool compressed;

	sblk = file_mapping;

	/* Parse inode table (metadata block) header */
	ret = sqfs_read_metablock(file_mapping, sblk->inode_table_start,
				  &compressed, &data_size);
	if (ret) {
		printf("Null pointers passed to sqfs_read_metablock.\n");
		return ret;
	}

	inode_table = file_mapping + sblk->inode_table_start + HEADER_SIZE;

	/* Extract compressed Inode table */
	if (compressed) {
		dest_len = METADATA_BLOCK_SIZE;
		dest_inode_table = malloc(dest_len);
		if (!dest_inode_table) {
			printf("%s: Memory allocation error.\n", __func__);
			free(dest_inode_table);
			return errno;
		}

		ret = sqfs_decompress(dest_inode_table, &dest_len, inode_table,
				      data_size);
		if (ret != Z_OK) {
			printf("Error while uncompressing metadata.\n");
			return ret;
		}

		printd("Uncompressed table size: %lu bytes\n", dest_len);
		inode_table = dest_inode_table;
	}

	/* Parse directory table (metadata block) header */
	ret = sqfs_read_metablock(file_mapping, sblk->directory_table_start,
				  &compressed, &data_size);
	if (ret) {
		printf("Null pointers passed to sqfs_read_metablock.\n");
		if (compressed)
			free(dest_inode_table);
	}

	dir_table = file_mapping + sblk->directory_table_start + HEADER_SIZE;

	/* Extract compressed Directory table */
	if (compressed) {
		dest_len = METADATA_BLOCK_SIZE;
		dest_dir_table = malloc(dest_len);
		if (!dest_dir_table) {
			printf("%s: Memory allocation error.\n", __func__);
			free(dest_dir_table);
			return errno;
		}

		ret = sqfs_decompress(dest_dir_table, &dest_len, dir_table,
				      data_size);
		if (ret != Z_OK) {
			printf("Error while uncompressing metadata.\n");
			return ret;
		}

		printd("Uncompressed table size: %lu bytes\n", dest_len);
		dir_table = dest_dir_table;
	}

	printd("\nDIRECTORY TABLE:\n\n");

	/*
	 * First 'parent' inode must be the root inode itself, and its inode
	 * number is equal to the number of inodes
	 */
	parent.base = sqfs_find_inode(inode_table, sblk->inodes, sblk->inodes,
				      sblk->block_size);

	/*
	 * Find directory/extended directory inodes in Inode table and then
	 * retrieve their positions in the uncompressed Directory table.
	 */
	for (k = 0; k < sblk->inodes; k++) {
		i.base = inode_table + inode_sizes;
		switch (i.base->inode_type) {
		case SQUASHFS_DIR_TYPE:
			i.dir = inode_table + inode_sizes;

			/* Look for a parent when the inode is not the root */
			if (k != sblk->inodes - 1) {
				printf("Directory %d\n", ++dir_count);
				parent.base = sqfs_find_inode(inode_table,
							      i.dir->parent_inode,
							      sblk->inodes,
							      sblk->block_size);
			} else {
				printf("Root directory\n");
			}

			if (sqfs_is_empty_dir(&i)) {
				sqfs_print_dir_name(&i, &parent, inode_table,
						    dir_table);
				printf("Empty directory.\n\n");
			} else {
				sqfs_dump_dir(&i, &parent, inode_table,
					      dir_table);
			}

			inode_sizes += sizeof(struct squashfs_dir_inode);
			break;
		case SQUASHFS_LDIR_TYPE:
			i.ldir = inode_table + inode_sizes;

			/* Look for a parent when the inode is not the root */
			if (k != sblk->inodes - 1) {
				printf("(extended) Directory %d\n",
				       ++dir_count);
				parent.base = sqfs_find_inode(inode_table,
							      i.ldir->parent_inode,
							      sblk->inodes,
							      sblk->block_size);
			} else {
				printf("Root (extended) directory\n");
			}

			if (sqfs_is_empty_dir(&i)) {
				sqfs_print_dir_name(&i, &parent, inode_table,
						    dir_table);
				printf("Empty directory.\n\n");
			} else {
				sqfs_dump_dir(&i, &parent, inode_table,
					      dir_table);
			}

			/*
			 * Calculate inode size: if there are no indexes, no
			 * need to calculate the index list size.
			 */
			if (i.ldir->i_count == 0) {
				inode_sizes +=
					sizeof(struct squashfs_ldir_inode);
				break;
			}

			for (l = 0; l < i.ldir->i_count + 1; l++)
				index_list_size += i.ldir->index[l].size + 1;

			inode_sizes += sizeof(struct squashfs_ldir_inode) +
				((i.ldir->i_count + 1) * DIR_INDEX_BASE_LENGTH)
				+ index_list_size;

			index_list_size = 0;
			break;
		case SQUASHFS_REG_TYPE:
			i.reg = inode_table + inode_sizes;
			if (i.reg->fragment == 0xFFFFFFFF) {
				block_list_size = ceil((float)i.reg->file_size
						       / sblk->block_size);
			} else {
				block_list_size = (i.reg->file_size /
						   sblk->block_size);
			}

			inode_sizes += sizeof(struct squashfs_reg_inode) +
				(block_list_size * sizeof(uint32_t));

			break;
		case SQUASHFS_LREG_TYPE:
			i.lreg = inode_table + inode_sizes;
			if (i.lreg->fragment == 0xFFFFFFFF) {
				block_list_size = ceil((float)i.lreg->file_size
						       / sblk->block_size);
			} else {
				block_list_size = (i.lreg->file_size /
						   sblk->block_size);
			}

			inode_sizes += sizeof(struct squashfs_lreg_inode) +
				(block_list_size * sizeof(uint32_t));

			break;
		case SQUASHFS_SYMLINK_TYPE:
		case SQUASHFS_LSYMLINK_TYPE:
			i.symlink = inode_table + inode_sizes;
			inode_sizes += sizeof(struct squashfs_symlink_inode) +
				i.symlink->symlink_size;

			break;
		case SQUASHFS_BLKDEV_TYPE:
		case SQUASHFS_CHRDEV_TYPE:
			inode_sizes += sizeof(struct squashfs_dev_inode);
			break;
		case SQUASHFS_LBLKDEV_TYPE:
		case SQUASHFS_LCHRDEV_TYPE:
			inode_sizes += sizeof(struct squashfs_ldev_inode);
			break;
		case SQUASHFS_FIFO_TYPE:
		case SQUASHFS_SOCKET_TYPE:
			inode_sizes += sizeof(struct squashfs_ipc_inode);
			break;
		case SQUASHFS_LFIFO_TYPE:
		case SQUASHFS_LSOCKET_TYPE:
			inode_sizes += sizeof(struct squashfs_lipc_inode);
			break;
		default:
			printf("Unknown inode type\n");
			ret = -EINVAL;
		}
	}

	if (!dest_inode_table)
		free(dest_inode_table);
	if (!dest_dir_table)
		free(dest_dir_table);

	return ret;
}
