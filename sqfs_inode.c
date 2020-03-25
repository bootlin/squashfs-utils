// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 *
 * sqfs_inode.c: support inode table parsing
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

#define SIZE(obj) printf("%d\n", sizeof(struct obj))
#define MAJOR_NUMBER_BITMASK GENMASK(15, 8)
#define MINOR_NUMBER_BITMASK GENMASK(7, 0)
#define LREG_INODE_MIN_SIZE 56
#define ENTRY_BASE_LENGTH 8
/* size of metadata (inode and directory) blocks */
#define SQUASHFS_METADATA_SIZE	8192
/* Max. number of fragment entries in a metadata block is 512 */
#define MAX_ENTRIES 512
#define SQUASHFS_FRAGMENT_BYTES(A) ((A) * sizeof(struct fragment_block_entry))
#define SQUASHFS_FRAGMENT_INDEX(A) ((A) / MAX_ENTRIES)
#define SQUASHFS_FRAGMENT_INDEX_OFFSET(A) ((A) % MAX_ENTRIES)
#define SQUASHFS_UNCOMPRESSED_DATA 0x0002
#define COMPRESSED_FRAGMENT_BLOCK(A) (!((A) & BIT(24)))
#define FRAGMENT_BLOCK_SIZE(A) ((A) & GENMASK(23, 0))
#define DIR_HEADER_SIZE 12

static bool sqfs_is_dir(union squashfs_inode *i)
{
	return (i->base->inode_type == SQUASHFS_DIR_TYPE) ||
		(i->base->inode_type == SQUASHFS_LDIR_TYPE);
}

/*
 * Retrieves fragment block entry and returns true if the fragment block is
 * compressed
 */
static bool sqfs_frag_lookup(void *file_mapping, uint32_t inode_fragment,
			     struct fragment_block_entry *e)
{
	uint64_t *fragment_index_table, start_block;
	uint16_t *header, src_len;
	struct fragment_block_entry *entries;
	struct squashfs_super_block *sblk;
	int block, offset, ret;
	size_t dest_len;
	void *metadata;

	sblk = file_mapping;
	block = SQUASHFS_FRAGMENT_INDEX(inode_fragment);
	offset = SQUASHFS_FRAGMENT_INDEX_OFFSET(inode_fragment);

	/* Start of the fragment index table in memory */
	fragment_index_table = file_mapping + sblk->fragment_table_start;

	/*
	 * Get the start offset of the metadata block that contains the right
	 * fragment_block_entry
	 */
	start_block = fragment_index_table[block];

	/* Every metadata block starts with a 16-bit header */
	header = file_mapping + start_block;
	metadata = file_mapping + start_block + HEADER_SIZE;
	printd("Header 0x%04x\n", *header);
	src_len = DATA_SIZE(*header);

	printd("Compressed data size: %u bytes\n", src_len);

	if (IS_COMPRESSED(*header)) {
		entries = malloc(METADATA_BLOCK_SIZE);
		if (!entries) {
			printf("%s: Memory allocation error.\n", __func__);
			free(entries);
			return errno;
		}

		printd("Compressed metadata block\n");
		ret = sqfs_decompress(entries, &dest_len, metadata, src_len);
		if (ret != Z_OK) {
			free(entries);
			return ret;
		}

		printd("Uncompressed size: %ld\n", dest_len);
	} else {
		entries = file_mapping + start_block + HEADER_SIZE;
		printd("Uncompressed metadata block\n");
	}

	*e = *(struct fragment_block_entry *)(entries + offset);

	if (IS_COMPRESSED(*header))
		free(entries);

	printd("Fragment entry:\n");
	printd("Start: 0x%016lx\n", e->start);

	if (COMPRESSED_FRAGMENT_BLOCK(e->size))
		printd("Compressed fragment block\n");
	else
		printd("Uncompressed fragment block\n");

	printd("Fragment block on-disk size: %lu\n",
	       FRAGMENT_BLOCK_SIZE(e->size));

	return COMPRESSED_FRAGMENT_BLOCK(e->size);
}

static int sqfs_parse_path(char *path, bool *is_a_dir)
{
	int token_count = 0, l;

	printd("Path: %s\n\n", path);

	if (path[0] != '/') {
		printf("Starting '/' in path is missing\n");
		return -EINVAL;
	}

	for (l = 0; l < strlen(path); l++)
		if (path[l] == '/')
			token_count++;

	/* A directory's path ends by '/' */
	if (path[strlen(path) - 1] == '/') {
		*is_a_dir = true;

		/*
		 * The number of tokens must be the same for both files and
		 * directories
		 */
		if (token_count > 1)
			token_count--;
	} else {
		*is_a_dir = false;
	}

	return token_count;
}

static int sqfs_search_entry(union squashfs_inode *i, char **token_list,
			     int token_count, void *inode_table,
			     void *dir_table, int inode_count, int block_size)
{
	int j, k, l, name_length, total_length = 0, new_inode_number;
	struct directory_header *parent_header;
	struct directory_entry *dir_entry;
	bool equals = false;

	i->base = sqfs_find_inode(inode_table, inode_count, inode_count,
				  block_size);

	parent_header = dir_table + sqfs_get_dir_offset(i);

	for (j = 0; j < token_count; j++) {
		printd("Searching for %s...\n", token_list[j]);
		printd("Current inode %d\n", i->base->inode_number);
		if (sqfs_is_dir(i)) {
			parent_header = dir_table + sqfs_get_dir_offset(i);
		} else {
			printf("Entry not found\n");
			return -EINVAL;
		}

		for (k = 0; k <= parent_header->count; k++) {
			printd("%d)\n", k);

			dir_entry = (void *)parent_header + DIR_HEADER_SIZE
				+ total_length;

			/* Skip comparison between different length strings */
			name_length = dir_entry->name_size + 1;
			if (strlen(token_list[j]) != name_length) {
				total_length += ENTRY_BASE_LENGTH + name_length;
				continue;
			}

			equals = true;
			for (l = 0; l < strlen(token_list[j]); l++)
				equals &= (dir_entry->name[l] ==
					   token_list[j][l]);

			printd("Token: %s\n", token_list[j]);

			if (equals) {
				printd("%s found\n", token_list[j]);

				/* Redefine inode as the found token */
				new_inode_number = dir_entry->inode_offset
					+ parent_header->inode_number;

				i->base = sqfs_find_inode(inode_table,
							  new_inode_number,
							  inode_count,
							  block_size);
				if (sqfs_is_dir(i)) {
					parent_header = dir_table +
						sqfs_get_dir_offset(i);
				}

				break;
			}

			total_length += ENTRY_BASE_LENGTH + name_length;
		}

		if (!equals) {
			printf("Entry not found.\n");
			return -EINVAL;
		}

		total_length = 0;
	}

	return 0;
}

static int sqfs_display_entry_content(union squashfs_inode *i, void
				      *file_mapping, void *dir_table,
				      void *inode_table, bool is_a_file)
{
	int k, l, j = 0, ret = 0, datablk_count = 0;
	char *fragment_block, **datablocks;
	bool compressed, frag;
	uint32_t *frag_block_offset, *block_sizes, compressed_size;
	struct fragment_block_entry frag_entry;
	size_t dest_len, file_size;
	struct squashfs_super_block *sblk;
	union squashfs_inode parent;
	unsigned long blocks_start;

	sblk = file_mapping;

	/* If is root, do not look for parent */
	if (i->base->inode_number == sblk->inodes) {
		parent.base = i->base;
	} else {
		parent.base = sqfs_find_inode(inode_table,
					      i->base->inode_number + 1,
					      sblk->inodes, sblk->block_size);
	}

	if (is_a_file) {
		switch (i->base->inode_type) {
		case SQUASHFS_REG_TYPE:
			printd("Basic File\n");
			printd("Start block: 0x%08x\n",
			       i->reg->start_block);
			printd("Fragment block index: 0x%08x\n",
			       i->reg->fragment);
			printd("Fragment block offset: 0x%08x\n",
			       i->reg->offset);
			printd("(Uncompressed) File size: %u\n",
			       i->reg->file_size);
			file_size = i->reg->file_size;
			frag_block_offset = &i->reg->offset;
			blocks_start = i->reg->start_block;
			block_sizes = i->reg->block_list;
			frag = IS_FRAGMENTED(i->reg->fragment);

			/* Count number of data blocks used to store the file */
			if (frag) {
				printd("Fragmented file.\n");
				datablk_count = (i->reg->file_size
						 / sblk->block_size);

				compressed = sqfs_frag_lookup(file_mapping,
							      i->reg->fragment,
							      &frag_entry);
			} else {
				printd("File not fragmented.\n");
				datablk_count = ceil((float)i->reg->file_size
						     / sblk->block_size);
			}

			break;
		case SQUASHFS_LREG_TYPE:
			printd("Extended File\n");
			printd("Start block: 0x%lx\n",
			       i->lreg->start_block);
			printd("Fragment block index: 0x%08x\n",
			       i->lreg->fragment);
			printd("Fragment block offset: 0x%08x\n",
			       i->lreg->offset);
			printd("(Uncompressed) File size: %lu\n",
			       i->lreg->file_size);
			file_size = i->lreg->file_size;
			frag_block_offset = &i->lreg->offset;
			blocks_start = i->lreg->start_block;
			block_sizes = i->lreg->block_list;
			frag = IS_FRAGMENTED(i->lreg->fragment);

			if (frag) {
				printd("Fragmented file.\n");
				datablk_count = (i->lreg->file_size
						 / sblk->block_size);

				compressed = sqfs_frag_lookup(file_mapping,
							      i->lreg->fragment,
							      &frag_entry);
			} else {
				printd("File not fragmented.\n");
				datablk_count = ceil((float)i->lreg->file_size
						     / sblk->block_size);
			}

			break;
		case SQUASHFS_SYMLINK_TYPE:
		case SQUASHFS_LSYMLINK_TYPE:
			printd("Basic Symlink\n");
			printd("Target path: ");
			for (l = 0; l < i->symlink->symlink_size; l++)
				printf("%c", i->symlink->symlink[l]);
			printf("\n");
		case SQUASHFS_BLKDEV_TYPE:
		case SQUASHFS_CHRDEV_TYPE:
			printd("Basic Block | Char. device\n");
			break;
		case SQUASHFS_LBLKDEV_TYPE:
		case SQUASHFS_LCHRDEV_TYPE:
			printd("Extended Block | Char. device\n");
			break;
		case SQUASHFS_FIFO_TYPE:
		case SQUASHFS_SOCKET_TYPE:
			printd("Basic Fifo | Socket\n");
			break;
		case SQUASHFS_LFIFO_TYPE:
		case SQUASHFS_LSOCKET_TYPE:
			printd("Extended Fifo | Socket\n");
			break;
		default:
			printf("Unknown entry type\n");
			return -EINVAL;
		}
	/* It's a directory */
	} else {
		if (sqfs_is_empty_dir(i)) {
			sqfs_print_dir_name(i, &parent, inode_table,
					    dir_table);

			printf("Empty directory.\n");
		} else {
			sqfs_dump_dir(i, &parent, inode_table, dir_table);
		}

		return 0;
	}

	printd("Display file content:\n\n");
	if (datablk_count > 0) {
		printd("Number of data blocks %d\n", datablk_count);
		datablocks = malloc(datablk_count * sizeof(char *));
		if (!datablocks) {
			printf("%s: Memory allocation error.\n", __func__);
			free(datablocks);
			return errno;
		}

		for (j = 0; j < datablk_count; j++) {
			datablocks[j] = malloc(sblk->block_size);
			if (!datablocks[j]) {
				printf("%s: Memory allocation error.\n",
				       __func__);
				free(datablocks[j]);
				ret = errno;
				goto free_memory;
			}
		}

	} else {
		printd("Completely fragmented file (no data blocks)\n");
	}

	if (sblk->flags & SQUASHFS_UNCOMPRESSED_DATA) {
		printd("Data blocks are uncompressed.\n");
		datablocks = file_mapping + blocks_start;
		for (j = 0; j < datablk_count; j++)
			for (k = 0; k < sblk->block_size; k++)
				printf("%c", datablocks[j][k]);
	} else {
		printd("Data blocks are compressed.\n");
		compressed_size = 0;
		for (j = 0; j < datablk_count; j++) {
			dest_len = sblk->block_size;
			ret = sqfs_decompress(datablocks[j], &dest_len,
					      file_mapping + blocks_start +
					      compressed_size,
					      block_sizes[j]);
			if (ret != Z_OK) {
				printf("Error while decompressing data blk.\n");
				return ret;
			}

			compressed_size += block_sizes[j];
		}
	}

	/* Display data block(s) content */
	for (j = 0; j < datablk_count; j++)
		for (k = 0; k < sblk->block_size; k++) {
			printf("%c", datablocks[j][k]);
			file_size--;
		}

	/* File compressed and fragmented */
	if (frag && compressed) {
		dest_len = sblk->block_size;
		fragment_block = malloc(sblk->block_size);
		if (!fragment_block) {
			printf("%s: Memory allocation error.\n", __func__);
			free(fragment_block);
			ret = errno;
			goto free_memory;
		}

		ret = sqfs_decompress(fragment_block, &dest_len, file_mapping +
				      frag_entry.start, frag_entry.size);
		if (ret != Z_OK) {
			printf("Error while decompressing fragment block.\n");
			goto free_memory;
		}

		printd("Uncompressed fragment block size: %ld\n", dest_len);
		for (j = 0; j < file_size; j++)
			printf("%c", fragment_block[*frag_block_offset + j]);

		free(fragment_block);
	} else if (frag && !compressed) {
		fragment_block = file_mapping + frag_entry.start;
		for (j = 0; j < file_size; j++)
			printf("%c", fragment_block[*frag_block_offset + j]);
	}

free_memory:

	for (j = 0; j < datablk_count; j++)
		free(datablocks[j]);
	if (datablk_count > 0)
		free(datablocks);

	return ret;
}

/* Given a path to a file or directory, return its content */
int sqfs_dump_entry(void *file_mapping, char *path)
{
	int j = 0, token_count = 0, ret = 0;
	char *dest_inode_table, *dest_dir_table;
	char **token_list, *aux;
	bool compressed, is_a_file, is_a_dir;
	void *dir_table, *inode_table;
	size_t src_len, dest_len;
	struct squashfs_super_block *sblk;
	union squashfs_inode i;

	/* Parsing path to file/directory */
	token_count = sqfs_parse_path(path, &is_a_dir);
	if (token_count < 0)
		return -EINVAL;
	is_a_file = !is_a_dir;

	/* Tokenize path string */
	token_list = malloc(token_count * sizeof(char *));
	if (!token_list) {
		printf("%s: Memory allocation error.\n", __func__);
		free(token_list);
		return errno;
	}

	if (path[0] == '/' && strlen(path) == 1) {
		token_list[0] = malloc(strlen(path) + 1);
		strcpy(token_list[0], path);
	} else {
		for (j = 0; j < token_count; j++) {
			aux = strtok(!j ? path : NULL, "/");
			token_list[j] = malloc(strlen(aux) + 1);
			if (!token_list[j]) {
				printf("%s: Memory allocation error.\n",
				       __func__);
				free(token_list[j]);
				return errno;
			}

			strcpy(token_list[j], aux);
		}
	}

	sblk = file_mapping;

	/* Parse inode table (metadata block) header */
	ret = sqfs_read_metablock(file_mapping, sblk->inode_table_start,
				  &compressed, &src_len);
	if (ret) {
		printf("Null pointers passed to sqfs_read_metablock.\n");
		goto free_memory;
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
				      src_len);
		if (ret != Z_OK)
			return ret;

		printd("Uncompressed table size: %lu bytes\n", dest_len);
		inode_table = dest_inode_table;
	}

	/* Parse directory table (metadata block) header */
	ret = sqfs_read_metablock(file_mapping, sblk->directory_table_start,
				  &compressed, &src_len);
	if (ret) {
		printf("Null pointers passed to sqfs_read_metablock.\n");
		goto free_memory;
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
				      src_len);
		if (ret != Z_OK) {
			printf("%s: Error while uncompressing metadata.\n",
			       __func__);
			return ret;
		}

		printd("Uncompressed table size: %lu bytes\n", dest_len);
		dir_table = dest_dir_table;
	}

	/*
	 * Look for file or directory name in Directory table, starting by root
	 * inode
	 */
	i.base = sqfs_find_inode(inode_table, sblk->inodes, sblk->inodes,
				 sblk->block_size);

	/* If the path is equal to '/', display root content */
	if (!strcmp(path, "/"))
		goto dump_entry;

	ret = sqfs_search_entry(&i, token_list, token_count, inode_table,
				dir_table, sblk->inodes, sblk->block_size);

	if (ret) {
		printf("Error while searching for entry\n");
		goto free_memory;
	}

dump_entry:

	ret = sqfs_display_entry_content(&i, file_mapping, dir_table,
					 inode_table, is_a_file);
	if (ret)
		printf("Error while displaying entry content.\n");

free_memory:

	for (j = 0; j < token_count; j++)
		free(token_list[j]);
	free(token_list);

	free(dest_dir_table);
	free(dest_inode_table);

	return ret;
}

/*
 * Given the uncompressed inode table, the inode to be found and the number of
 * inodes in the table, return inode position in case of success.
 */
void *sqfs_find_inode(void *inode_table, int inode_number, int inode_count,
		      uint32_t block_size)
{
	int k, l, block_list_size = 0, offset = 0, index_list_size = 0;
	union squashfs_inode i;

	if (!inode_table) {
		printf("%s: Invalid pointer to inode table.\n", __func__);
		return NULL;
	}

	for (k = 0; k < inode_count; k++) {
		i.base = inode_table + offset;
		if (i.base->inode_number == inode_number)
			return (void *)(inode_table + offset);

		switch (i.base->inode_type) {
		case SQUASHFS_DIR_TYPE:
			i.dir = inode_table + offset;
			offset += sizeof(struct squashfs_dir_inode);
			break;
		case SQUASHFS_REG_TYPE:
			i.reg = inode_table + offset;

			if (IS_FRAGMENTED(i.reg->fragment)) {
				block_list_size = (i.reg->file_size /
						    block_size);
			} else {
				block_list_size = ceil((float)i.reg->file_size
						       / block_size);
			}

			offset += sizeof(struct squashfs_reg_inode) +
				block_list_size * sizeof(uint32_t);
			break;
		case SQUASHFS_LDIR_TYPE:
			i.ldir = inode_table + offset;
			if (i.ldir->i_count == 0) {
				offset += sizeof(struct squashfs_ldir_inode);
				break;
			}

			for (l = 0; l < i.ldir->i_count + 1; l++)
				index_list_size += i.ldir->index[l].size + 1;

			offset += sizeof(struct squashfs_ldir_inode)
				+ (i.ldir->i_count + 1) * DIR_INDEX_BASE_LENGTH
				+ index_list_size;

			index_list_size = 0;
			break;
		case SQUASHFS_LREG_TYPE:
			i.lreg = inode_table + offset;
			if (i.lreg->fragment == 0xFFFFFFFF) {
				block_list_size = ceil((float)i.lreg->file_size
						       / block_size);
			} else {
				block_list_size = (i.lreg->file_size /
						    block_size);
			}

			offset += sizeof(struct squashfs_lreg_inode)
				+ block_list_size * sizeof(uint32_t);

			break;
		case SQUASHFS_SYMLINK_TYPE:
		case SQUASHFS_LSYMLINK_TYPE:
			i.symlink = inode_table + offset;
			offset += sizeof(struct squashfs_symlink_inode)
				+ i.symlink->symlink_size;

			break;
		case SQUASHFS_BLKDEV_TYPE:
		case SQUASHFS_CHRDEV_TYPE:
			i.dev = inode_table + offset;
			offset += sizeof(struct squashfs_dev_inode);
			break;
		case SQUASHFS_LBLKDEV_TYPE:
		case SQUASHFS_LCHRDEV_TYPE:
			i.ldev = inode_table + offset;
			offset += sizeof(struct squashfs_ldev_inode);
			break;
		case SQUASHFS_FIFO_TYPE:
		case SQUASHFS_SOCKET_TYPE:
			i.ipc = inode_table + offset;
			offset += sizeof(struct squashfs_ipc_inode);
			break;
		case SQUASHFS_LFIFO_TYPE:
		case SQUASHFS_LSOCKET_TYPE:
			i.lipc = inode_table + offset;
			offset += sizeof(struct squashfs_lipc_inode);
			break;
		default:
			printf("Error while searching inode: unknown type.\n");
			return NULL;
		}
	}

	return NULL;
}

int sqfs_dump_inode_table(void *file_mapping)
{
	int k, l, ret, inode_sizes = 0, block_list_size = 1,
	    index_list_size = 0;
	struct squashfs_super_block *sblk;
	unsigned char *dest_table;
	unsigned long dest_len;
	union squashfs_inode i;
	char modified_time[80];
	struct tm timestamp;
	void *inode_table;
	size_t data_size;
	time_t rawtime;
	bool compressed;

	sblk = file_mapping;
	printd("Inode table size: %ld bytes\n",
	       sblk->directory_table_start - sblk->inode_table_start);

	ret = sqfs_read_metablock(file_mapping, sblk->inode_table_start,
				  &compressed, &data_size);
	if (ret) {
		printf("Null pointers passed to sqfs_read_metablock.\n");
		return ret;
	}

	inode_table = file_mapping + sblk->inode_table_start + HEADER_SIZE;

	if (compressed) {
		/*
		 * The largest inode is squashfs_lreg_inode, with a minimal size
		 * of 56 bytes, supposing an empty 'block_list' member.
		 */
		dest_len = sblk->inodes * LREG_INODE_MIN_SIZE;
		dest_table = malloc(dest_len);
		if (!dest_table) {
			printf("%s: Memory allocation error.\n", __func__);
			free(dest_table);
			return errno;
		}

		ret = sqfs_decompress(dest_table, &dest_len, inode_table,
				      data_size);
		if (ret != Z_OK) {
			printf("Error while uncompressing metadata.\n");
			return ret;
		}

		printd("Uncompressed table size: %lu bytes\n", dest_len);
		inode_table = dest_table;
	}

	printf("--- --- ---\n");
	for (k = 0; k < sblk->inodes; k++) {
		printf("{Inode %d/%d}\n", k + 1, sblk->inodes);
		i.base = inode_table + inode_sizes;
		printf("--- --- ---\n");

		/* Display inode header, type-independent */
		printf("Permissions: 0x%04x\n", i.base->mode);
		printf("UID index: 0x%04x\n", i.base->uid);
		printf("GID index: 0x%04x\n", i.base->guid);

		/* Raw time in seconds -> human readable format */
		rawtime = i.base->mtime;
		timestamp = *localtime(&rawtime);
		strftime(modified_time, sizeof(modified_time),
			 "%a %Y-%m-%d (yyyy-mm-dd) %H:%M:%S %Z", &timestamp);
		printf("Modified time: %s\n", modified_time);
		printf("Inode number: %u\n", i.base->inode_number);

		/* Display type-depending information */
		printf("Inode type: ");
		switch (i.base->inode_type) {
		case SQUASHFS_DIR_TYPE:
			printf("Basic Directory\n");
			i.dir = inode_table + inode_sizes;

			/*
			 * The index of the block in the Directory Table where
			 * the directory entry information starts
			 */
			printf("Start block: 0x%08x\n", i.dir->start_block);

			/* The number of hard links to this directory */
			printf("Hard links: %u\n", i.dir->nlink);

			/*
			 * Total (uncompressed) size in bytes of the entries in
			 * the Directory Table, including headers
			 */
			printf("File size: %u\n", i.dir->file_size);

			/*
			 * The (uncompressed) offset whitin the block in the
			 * Directory Table, where the directory entry
			 * information starts
			 */
			printf("Block offset: 0x%04x\n", i.dir->offset);

			/* The inode number of the parent of this directory */
			printf("Parent inode number: %u\n",
			       i.dir->parent_inode);
			inode_sizes += sizeof(struct squashfs_dir_inode);
			break;
		case SQUASHFS_REG_TYPE:
			printf("Basic File\n");
			i.reg = inode_table + inode_sizes;

			/*
			 * The offset from the start of the archive where the
			 * data blocks are stored
			 */
			printf("Start block: 0x%08x\n", i.reg->start_block);

			/*
			 * The offset index of a fragment entry in the fragment
			 * table which describes the data block the fragment of
			 * this file is stored in. Is equal to 0xFFFFFFFF if
			 * this file does not end with a fragment.
			 */
			printf("Fragment block index: 0x%08x\n",
			       i.reg->fragment);

			/*
			 * The (uncompressed) offset within the fragment data
			 * block where the fragment for this file is.
			 */
			printf("Fragment block offset: 0x%08x\n",
			       i.reg->offset);
			printf("(Uncompressed) File size: %u\n",
			       i.reg->file_size);

			if (IS_FRAGMENTED(i.reg->fragment))
				block_list_size = (i.reg->file_size /
						    sblk->block_size);
			else
				block_list_size =  ceil((float)i.reg->file_size
							/ sblk->block_size);

			printd("Block list size %d\n", block_list_size);
			inode_sizes += sizeof(struct squashfs_reg_inode) +
				block_list_size * sizeof(uint32_t);

			break;
		case SQUASHFS_LDIR_TYPE:
			printf("Extended Directory\n");
			i.ldir = inode_table + inode_sizes;
			printf("Start block: 0x%08x\n", i.ldir->start_block);
			printf("Hard links: %u\n", i.ldir->nlink);
			printf("File size: %u\n", i.ldir->file_size);
			printf("Block offset: 0x%04x\n", i.ldir->offset);
			printf("Parent inode number: %u\n",
			       i.ldir->parent_inode);

			/*
			 * One less than the number of directory index entries
			 * following the inode structure
			 */
			printf("Index count: %u\n", i.ldir->i_count);

			/*
			 * An index into the xattr lookup table. Is equal to
			 * 0xFFFFFFFF if the inode has no extended attributes
			 */
			printf("Xattr table index: 0x%08x\n", i.ldir->xattr);

			/*
			 * Iterate through all directory indexes in the list
			 * and accumulate all its 'name' member sizes
			 */
			if (i.ldir->i_count == 0) {
				inode_sizes +=
					sizeof(struct squashfs_ldir_inode);
				break;
			}
			for (l = 0; l < i.ldir->i_count + 1; l++)
				index_list_size += i.ldir->index[l].size + 1;
			inode_sizes += sizeof(struct squashfs_ldir_inode)
				+ (i.ldir->i_count + 1) * DIR_INDEX_BASE_LENGTH
				+ index_list_size;

			index_list_size = 0;
			break;
		case SQUASHFS_LREG_TYPE:
			printf("Extended File\n");
			i.lreg = inode_table + inode_sizes;
			printf("Start block: 0x%lx\n", i.lreg->start_block);
			printf("Fragment block index: 0x%08x\n",
			       i.lreg->fragment);
			printf("Fragment block offset: 0x%08x\n",
			       i.lreg->offset);
			printf("(Uncompressed) File size: %lu\n",
			       i.lreg->file_size);

			/*
			 * The number of bytes saved by omitting blocks of zero
			 * bytes. Used in the kernel for sparse file accounting
			 */
			printf("Sparse (?): %lu\n", i.lreg->sparse);
			printf("Hard links: %u\n", i.lreg->nlink);
			printf("Xattr table index: 0x%x\n", i.lreg->xattr);
			if (IS_FRAGMENTED(i.lreg->fragment))
				block_list_size = (i.lreg->file_size /
						    sblk->block_size);
			else
				block_list_size = ceil(i.lreg->file_size /
						    (float)sblk->block_size);

			inode_sizes += sizeof(struct squashfs_lreg_inode)
				+ block_list_size * sizeof(uint32_t);

			break;
		case SQUASHFS_SYMLINK_TYPE:
		case SQUASHFS_LSYMLINK_TYPE:
			printf("Basic Symlink\n");
			i.symlink = inode_table + inode_sizes;
			printf("Hard links: %u\n", i.symlink->nlink);

			/*
			 * The size (in bytes) of the path this symlink
			 * points to
			 */
			printf("Symlink size: %u\n", i.symlink->symlink_size);

			/*
			 * The target path this symlink points to. This path is
			 * symlink_size bytes long. There is no trailing null
			 * byte
			 */
			printf("Target path: ");
			for (l = 0; l < i.symlink->symlink_size; l++)
				printf("%c", i.symlink->symlink[l]);
			printf("\n");

			/*
			 * An index into the xattr lookup table. Set to
			 * Equals to 0xFFFFFFFF if the inode has no extended
			 * attributes
			 */
			if (i.base->inode_type == SQUASHFS_LSYMLINK_TYPE)
				printf("Xattr index: 0x%08x\n", i.ldev->xattr);
			inode_sizes += sizeof(struct squashfs_symlink_inode)
				+ i.symlink->symlink_size;
			break;
		case SQUASHFS_BLKDEV_TYPE:
		case SQUASHFS_CHRDEV_TYPE:
			printf("Basic Block | Char. device\n");
			i.dev = inode_table + inode_sizes;
			printf("Hard links: %u\n", i.dev->nlink);

			/* rdev encodes the major and minor numbers */
			printf("Major/Minor device numbers: %ld/%ld\n",
			       (i.dev->rdev >> 8) & MAJOR_NUMBER_BITMASK,
			       (i.dev->rdev & MINOR_NUMBER_BITMASK)
			       | ((i.dev->rdev >> 12) & MAJOR_NUMBER_BITMASK));
			inode_sizes += sizeof(struct squashfs_dev_inode);
			break;
		case SQUASHFS_LBLKDEV_TYPE:
		case SQUASHFS_LCHRDEV_TYPE:
			printf("Extended Block | Char. device\n");
			i.ldev = inode_table + inode_sizes;
			printf("Hard links: %u\n", i.ldev->nlink);
			printf("Major/Minor device numbers: %ld/%ld\n",
			       (i.dev->rdev >> 8) & MAJOR_NUMBER_BITMASK,
			       (i.ldev->rdev & MINOR_NUMBER_BITMASK)
			       | ((i.ldev->rdev >> 12) & MAJOR_NUMBER_BITMASK));
			printf("Xattr index: 0x%08x\n", i.ldev->xattr);
			inode_sizes += sizeof(struct squashfs_ldev_inode);
			break;
		case SQUASHFS_FIFO_TYPE:
		case SQUASHFS_SOCKET_TYPE:
			i.ipc = inode_table + inode_sizes;
			printf("Basic Fifo | Socket\n");
			printf("Hard links: %u\n", i.ipc->nlink);
			inode_sizes += sizeof(struct squashfs_ipc_inode);
			break;
		case SQUASHFS_LFIFO_TYPE:
		case SQUASHFS_LSOCKET_TYPE:
			i.lipc = inode_table + inode_sizes;
			printf("Extended Fifo | Socket\n");
			printf("Hard links: %u\n", i.lipc->nlink);
			printf("Xattr index: 0x%08x\n", i.lipc->xattr);
			inode_sizes += sizeof(struct squashfs_lipc_inode);
			break;
		default:
			printf("Unknown inode type\n");
			return -EINVAL;
		}

		printf("inode sizes: %d\n", inode_sizes);
		printf("\n\n");
	}

	free(dest_table);

	return 0;
}

int sqfs_read_metablock(void *file_mapping, int offset, bool *compressed,
			size_t *data_size)
{
	uint16_t *header;

	if (!compressed || !data_size)
		return -EINVAL;

	header = file_mapping + offset;
	printd("Metadata block header: 0x%04x\n", *header);
	*compressed = IS_COMPRESSED(*header);
	*data_size = DATA_SIZE(*header);
	printd("Data size: %ld bytes\n", *data_size);

	if (*compressed)
		printd("Compressed metadata block\n");
	else
		printd("Uncompressed metadata block\n");

	return 0;
}
