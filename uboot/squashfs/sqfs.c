// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 *
 * squashfs.c: SquashFS filesystem implementation
 */

#include <errno.h>
#include <fs.h>
#include <memalign.h>
#include <stdlib.h>
#include <squashfs.h>

#include "sqfs_decompressor.h"
#include "sqfs_filesystem.h"
#include "sqfs_utils.h"

static disk_partition_t cur_part_info;
static struct blk_desc *cur_dev;

static int disk_read(__u32 block, __u32 nr_blocks, void *buf)
{
	ulong ret;

	if (!cur_dev)
		return -1;

	ret = blk_dread(cur_dev, cur_part_info.start + block, nr_blocks, buf);
	printf("disk read: %ld blocks\n", ret);

	if (ret != nr_blocks)
		return -1;

	return ret;
}

/*
 * Retrieves fragment block entry and returns true if the fragment block is
 * compressed
 */
static bool sqfs_frag_lookup(uint32_t inode_fragment_index,
			     struct squashfs_fragment_block_entry *e)
{
	printf("frag lookup\n");
	struct squashfs_fragment_block_entry *entries;
	uint64_t *fragment_index_table, start_block;
	uint32_t start, n_blks, table_size;
	struct squashfs_super_block *sblk;
	uint32_t src_len;
	unsigned long int dest_len;
	int block, offset, ret, comp_type;
	uint16_t *header;
	unsigned char *metadata_buffer, *metadata;

	/* Read SquashFS super block */
	ALLOC_CACHE_ALIGN_BUFFER(char, buffer, cur_dev->blksz);
	if (disk_read(0, 1, buffer) != 1) {
		cur_dev = NULL;
		return -1;
	}

	sblk = (struct squashfs_super_block *)buffer;
	comp_type = sblk->compression;

	if (inode_fragment_index >= sblk->fragments)
		return -EINVAL;

	printf("First metadata offset %lld\n", sblk->inode_table_start);
	printf("inode fragment %d\n", inode_fragment_index);
	printf("number of fragment blocks: %d\n", sblk->fragments);

	printf("fragment T start : %lld\n", sblk->fragment_table_start);
	table_size = sblk->export_table_start - sblk->fragment_table_start;
	start = sblk->fragment_table_start / cur_dev->blksz;
	n_blks = DIV_ROUND_UP(table_size + sblk->fragment_table_start -
			      (start * cur_dev->blksz), cur_dev->blksz);
	printf("table size: %d\n", table_size);
	printf("start: %d\n", start);
	printf("n_blks (data): %d\n", n_blks);

	/* Allocate a proper sized buffer to store the fragment index table */
	ALLOC_CACHE_ALIGN_BUFFER(unsigned char, table, n_blks * cur_dev->blksz);

	if (disk_read(start, n_blks, table) < 0) {
		printf("Disk read error while searching for fragment.\n");
		return -1;
	}

	block = SQFS_FRAGMENT_INDEX(inode_fragment_index);
	offset = SQFS_FRAGMENT_INDEX_OFFSET(inode_fragment_index);

	printf("block/offset: %d/%d\n", block, offset);

	/* Start of the fragment index table in memory */
	fragment_index_table = (void *)table + sblk->fragment_table_start -
		(start * cur_dev->blksz);

	/*
	 * Get the start offset of the metadata block that contains the right
	 * fragment_block_entry
	 */
	start_block = fragment_index_table[block];

	table_size = sblk->fragment_table_start - start_block;
	start = start_block / cur_dev->blksz;
	n_blks = DIV_ROUND_UP(table_size + start_block -
			      (start * cur_dev->blksz), cur_dev->blksz);
	printf("table size: %d\n", table_size);
	printf("start: %d\n", start);
	printf("n_blks (data): %d\n", n_blks);
	metadata_buffer = malloc_cache_aligned(n_blks * cur_dev->blksz);

	if (disk_read(start, n_blks, metadata_buffer) < 0) {
		printf("Disk read error while searching for fragment.\n");
		return -1;
	}

	/* Every metadata block starts with a 16-bit header */
	header = (void *)metadata_buffer + start_block -
		(start * cur_dev->blksz);
	printf("header: 0x%04x\n", *header);
	metadata = (void *)header + SQFS_HEADER_SIZE;

	src_len = SQFS_METADATA_SIZE(*header);
	printf("metadata source len: %d\n", src_len);
	dest_len = SQFS_METADATA_BLOCK_SIZE;

	if (SQFS_COMPRESSED_METADATA(*header)) {
		printf("fragment metadata compressed\n");
		entries = malloc(SQFS_METADATA_BLOCK_SIZE);
		if (!entries) {
			return errno;
		}

		ret = sqfs_decompress(comp_type, entries, &dest_len, metadata,
				      src_len);
		if (ret) {
			free(entries);
			return ret;
		}
		printf("uncompressed size: %ld\n", dest_len);
	} else {
		printf("fragment metadata uncompressed\n");
		entries = (struct squashfs_fragment_block_entry *)metadata;
	}

	*e = entries[offset];

	printf("fragment block size: %ld\n", SQFS_BLOCK_SIZE(e->size));
	printf("fragment block size: %d\n", e->size);
	printf("fragment block off: %lld\n", e->start);
	if (SQFS_COMPRESSED_BLOCK(e->size))
		printf("compressed fragment block\n");

	if (SQFS_COMPRESSED_METADATA(*header))
		free(entries);

	free(metadata_buffer);

	return SQFS_COMPRESSED_BLOCK(e->size);
}

/*
 * m_list contains each metadata block's position, and m_count is the number of
 * elements of m_list. Those metadata blocks come from the compressed directory
 * table.
 */
static int sqfs_search_dir(struct squashfs_dir_stream *dirs, char **token_list,
			   int token_count, uint32_t *m_list, int m_count)
{
	int j, k, name_length, total_length = 0, new_inode_number;
	struct squashfs_directory_header *parent_header;
	struct squashfs_directory_entry *dir_entry;
	struct squashfs_super_block *sblk;
	union squashfs_inode i;
	bool equals = false;

//	printf("TOKEN LIST:\n");
//	for (j = 0; j < token_count; j++)
//		printf("token: %s\n", token_list[j]);

	ALLOC_CACHE_ALIGN_BUFFER(char, buffer, cur_dev->blksz);

	/* Read block device data into buffer */
	if (disk_read(0, 1, buffer) != 1) {
		cur_dev = NULL;
		return -1;
	}

	sblk = (struct squashfs_super_block *)buffer;
	i.base = sqfs_find_inode(dirs->inode_table, sblk->inodes, sblk->inodes,
				 sblk->block_size);
//	printf("inode table: %p\n", inode_table);
//	printf("inode base: %p\n", i.base);
//	printf("inode base type: %d\n", i.base->inode_type);
//	printf("inode base number: %d\n", i.base->inode_number);
//	printf("inode offset: %d\n", sqfs_dir_offset(&i, m_list,
//							 m_count));
	parent_header = (void *)dirs->dir_table +
		sqfs_dir_offset(&i, m_list, m_count);

	/* No path given -> root directory */
	if (!strcmp(token_list[0], "/")) {
		dirs->table = parent_header;
		dirs->i.base = i.base;
		return 0;
	}

	for (j = 0; j < token_count; j++) {
		if (sqfs_is_dir(&i)) {
			parent_header = (void *)dirs->dir_table +
				sqfs_dir_offset(&i, m_list, m_count);
		} else {
			printf("Directory not found: %s\n", token_list[j]);
			return -EINVAL;
		}

		for (k = 0; k <= parent_header->count; k++) {
			dir_entry = (void *)parent_header + SQFS_DIR_HEADER_SIZE
				+ total_length;

			/* Skip comparison between different length strings */
			name_length = dir_entry->name_size + 1;
			if (strlen(token_list[j]) != name_length) {
				total_length += SQFS_ENTRY_BASE_LENGTH +
					name_length;
				continue;
			}

			equals = !strncmp(dir_entry->name, token_list[j],
					  name_length);
			if (equals) {
				/* Redefine inode as the found token */
				new_inode_number = dir_entry->inode_offset
					+ parent_header->inode_number;

				i.base = sqfs_find_inode(dirs->inode_table,
							 new_inode_number,
							 sblk->inodes,
							 sblk->block_size);


				if (!sqfs_is_dir(&i)) {
					printf("%s is not a directory.\n",
					       token_list[j]);
				}

				parent_header = (void *)dirs->dir_table +
					sqfs_dir_offset(&i, m_list, m_count);

				if (sqfs_is_empty_dir(&i))
					return SQFS_EMPTY_DIR;

				break;
			}

			total_length += SQFS_ENTRY_BASE_LENGTH + name_length;
		}

		if (!equals) {
			printf("Directory not found: %s\n", token_list[j]);
			return -EINVAL;
		}

		total_length = 0;
	}

	dirs->table = (void *)dirs->dir_table +
		sqfs_dir_offset(&i, m_list, m_count);
	dirs->i.base = i.base;

	return 0;
}

static int sqfs_count_tokens(const char *path)
{
	int token_count = 1, l;

	for (l = 1; l < strlen(path); l++) {
		if (path[l] == '/')
			token_count++;
	}

	return token_count;
}

/*
 * Inode and directory tables are stored as a series of metadata blocks, and
 * given the compressed size of this table, we can calculate how much metadata
 * blocks are needed to store the result of the decompression, since a
 * decompressed metadata block should have a size of 8KiB.
 */
static int count_metablks(void *table, uint32_t offset, int table_size)
{
	int count = 0, cur_size = 0, ret;
	uint32_t data_size;
	bool comp;

	do {
		ret = sqfs_read_metablock(table, offset + cur_size, &comp,
					  &data_size);
//		printf("data size %d\n", data_size);
		if (ret)
			return -EINVAL;
		cur_size += data_size + SQFS_HEADER_SIZE;
//		printf("current_size %d\n", cur_size);
		count++;
	} while (cur_size < table_size);

	return count;
}

/*
 * Storing the metadata blocks header's positions will be useful while looking
 * for an entry in the directory table, using the reference (index and offset)
 * given by its inode.
 */
static int get_metadatablk_pos(uint32_t *pos_list, void *table, uint32_t offset,
			       int metablks_count)
{
	uint32_t data_size, cur_size = 0;
	int j, ret = 0;
	bool comp;

	if (!metablks_count)
		return -EINVAL;

	for (j = 0; j < metablks_count; j++) {

		ret = sqfs_read_metablock(table, offset + cur_size, &comp,
					  &data_size);
		if (ret)
			return -EINVAL;

		cur_size += data_size + SQFS_HEADER_SIZE;
		pos_list[j] = cur_size;
	}

	return ret;
}

int sqfs_opendir(const char *filename, struct fs_dir_stream **dirsp)
{
	printf("OPENDIR CALLED\n");
	uint32_t src_len, table_size, dest_offset = 0;
	unsigned long int dest_len;
	uint32_t start, n_blks, table_offset, *pos_list;
	int i, j, token_count, ret = 0, metablks_count, comp_type;
	char **token_list, *aux, *path, *inode_table, *dir_table;
	struct squashfs_super_block *sblk;
	struct squashfs_dir_stream *dirs;
	unsigned char *src_table, *itb, *dtb;
	bool compressed;

	ALLOC_CACHE_ALIGN_BUFFER(char, buffer, cur_dev->blksz);

	/* Read block device data into buffer */
	if (disk_read(0, 1, buffer) != 1) {
		cur_dev = NULL;
		return -1;
	}

	sblk = (struct squashfs_super_block *)buffer;
	comp_type = sblk->compression;

	/* INODE TABLE */

	table_size = sblk->directory_table_start - sblk->inode_table_start;
	printf("table size %d\n", table_size);
	start = sblk->inode_table_start / cur_dev->blksz;
	printf("start: %d\n", start);
	table_offset = sblk->inode_table_start - (start * cur_dev->blksz);
	printf("table offset %d\n", table_offset);
	n_blks = DIV_ROUND_UP(table_size + table_offset, cur_dev->blksz);
	printf("n blks %d\n", n_blks);

	/* Allocate a proper sized buffer (itb) to store the inode table */
	itb = malloc_cache_aligned(n_blks * cur_dev->blksz);
	printf("ITB SIZE %ld\n", n_blks * cur_dev->blksz);
	printf("itb address %p\n", itb);

	if (disk_read(start, n_blks, itb) < 0) {
		cur_dev = NULL;
		return -1;
	}

	/* Parse inode table (metadata block) header */
	ret = sqfs_read_metablock(itb, table_offset, &compressed, &src_len);
	if (ret)
		return -EINVAL;

	/* Calculate size to store the whole decompressed table */
	metablks_count = count_metablks(itb, table_offset, table_size);
	if (metablks_count < 1)
		return -EINVAL;

	printf("i table blocks: %d\n", metablks_count);
	inode_table = malloc_cache_aligned(metablks_count * SQFS_METADATA_BLOCK_SIZE);
	printf("inode_table %p\n", inode_table);
	if (!inode_table)
		return errno;

	printf("%d debug\n", __LINE__);
//	for (j = 0; j < metablks_count; j++)
//		printf("IT pos_list %d\n", pos_list[j]);

	src_table = itb + table_offset + SQFS_HEADER_SIZE;

	/* Extract compressed Inode table */
	for (j = 0; j < metablks_count; j++) {
	printf("%d debug\n", __LINE__);
		sqfs_read_metablock(itb, table_offset, &compressed, &src_len);
		printf("inode src_len %d\n", src_len);
		if (compressed) {
	printf("%d debug\n", __LINE__);
			dest_len = SQFS_METADATA_BLOCK_SIZE;
			ret = sqfs_decompress(comp_type, inode_table +
					      dest_offset, &dest_len,
					      src_table, src_len);
			if (ret) {
		printf("%d debug\n", __LINE__);
				free(inode_table);
				return ret;
			}

			printf("uncomp size %ld\n", dest_len);
//			memcpy(inode_table + dest_offset, table, dest_len);

		} else {
			memcpy(inode_table + (j * SQFS_METADATA_BLOCK_SIZE),
			       src_table, src_len);
		}

	printf("%d debug\n", __LINE__);
		/*
		 * Offsets to the decompression destination, to the metadata
		 * buffer 'itb' and to the decompression source, respectively.
		 */
		dest_offset += dest_len;
		table_offset += src_len + SQFS_HEADER_SIZE;
		printf("table offset %d\n", table_offset);
		src_table += src_len + SQFS_HEADER_SIZE;
	}

	printf("%d bytes copied %d\n", __LINE__, dest_offset);

//	for (j = 0; j < metablks_count * SQFS_METADATA_BLOCK_SIZE; j++)
//		printf("%c", inode_table[j]);


	/* DIRECTORY TABLE */

	table_size = sblk->fragment_table_start - sblk->directory_table_start;
	start = sblk->directory_table_start / cur_dev->blksz;
	table_offset = sblk->directory_table_start - (start * cur_dev->blksz);
	n_blks = DIV_ROUND_UP(table_size + table_offset, cur_dev->blksz);

	/* Allocate a proper sized buffer (dtb) to store the directory table */
	dtb = malloc_cache_aligned(n_blks * cur_dev->blksz);

	if (disk_read(start, n_blks, dtb) < 0) {
		cur_dev = NULL;
		free(inode_table);
		return -1;
	}

	/* Parse directory table (metadata block) header */
	ret = sqfs_read_metablock(dtb, table_offset, &compressed, &src_len);
	if (ret) {
		free(inode_table);
		return -EINVAL;
	}

	/* Calculate total size to store the whole decompressed table */
	metablks_count = count_metablks(dtb, table_offset, table_size);
	if (metablks_count < 1) {
		free(inode_table);
		return -EINVAL;
	}

	printf("%d debug\n", __LINE__);
	dir_table = malloc(metablks_count * SQFS_METADATA_BLOCK_SIZE);
	if (!dir_table) {
		free(inode_table);
		free(dir_table);
		return errno;
	}

	pos_list = malloc(metablks_count * sizeof(uint32_t));
	if (!pos_list) {
		free(inode_table);
		free(dir_table);
		return errno;
	}

	printf("%d debug\n", __LINE__);
	ret = get_metadatablk_pos(pos_list, dtb, table_offset, metablks_count);
	if (ret)
		goto free_mem;

	printf("%d debug\n", __LINE__);

	for (j = 0; j < metablks_count; j++)
		printf("DT pos_list %d\n", pos_list[j]);

	src_table = dtb + table_offset + SQFS_HEADER_SIZE;

	/* Extract compressed Directory table */
	dest_offset = 0;
	for (j = 0; j < metablks_count; j++) {
		sqfs_read_metablock(dtb, table_offset, &compressed, &src_len);
		if (compressed) {
			dest_len = SQFS_METADATA_BLOCK_SIZE;
			ret = sqfs_decompress(comp_type, dir_table +
					      (j * SQFS_METADATA_BLOCK_SIZE),
					      &dest_len, src_table, src_len);
//			ret = sqfs_decompress(table, &dest_len, src_table, &src_len);
			printf("uncomp size %ld\n", dest_len);
//			memcpy(dir_table + dest_offset, table, dest_len);
			if (ret)
				goto free_mem;

			if (dest_len < SQFS_METADATA_BLOCK_SIZE) {
				dest_offset += dest_len;
				break;
			}
		} else {
			memcpy(dir_table + (j * SQFS_METADATA_BLOCK_SIZE),
			       src_table, src_len);
		}

		/*
		 * Offsets to the decompression destination, to the metadata
		 * buffer 'dtb' and to the decompression source, respectively.
		 */
		dest_offset += dest_len;
		table_offset += src_len + SQFS_HEADER_SIZE;
		src_table += src_len + SQFS_HEADER_SIZE;
	}
//	for (j = 0; j < dest_offset; j++)
//		printf("%c", dir_table[j]);

	printf("%d bytes copied %d\n", __LINE__, dest_offset);


	/* Copy filename into path, since 'strtok' can't use const char */
	path = malloc(strlen(filename) + 1);
	strcpy(path, filename);

//	printf("debug\n");
	/* Parsing path to file/directory */
	token_count = sqfs_count_tokens(path);
	if (token_count < 0) {
		ret = -EINVAL;
		goto free_mem;
	}

	/* Ignore trailing '/' in path */
	if (filename[strlen(filename) - 1] == '/')
		token_count--;

	/* Tokenize path string */
	token_list = malloc(token_count * sizeof(char *));
	if (!token_list) {
		ret = errno;
		goto free_mem;
	}

	/* Allocate and fill token list */
	if (path[0] == '/' && strlen(path) == 1) {
		token_list[0] = malloc(strlen(path) + 1);
		strcpy(token_list[0], path);
	} else {
		for (j = 0; j < token_count; j++) {
			aux = strtok(!j ? path : NULL, "/");
			token_list[j] = malloc(strlen(aux) + 1);
			if (!token_list[j]) {
				for (i = 0; i < j; i++)
					free(token_list[i]);
				ret = errno;
				goto free_mem;
			}

			strcpy(token_list[j], aux);
		}
	}

	printf("%d debug\n", __LINE__);
	dirs = (struct squashfs_dir_stream *)dirsp;
	dirs->inode_table = inode_table;
	dirs->dir_table = dir_table;
	printf("%d debug\n", __LINE__);
	ret = sqfs_search_dir(dirs, token_list, token_count, pos_list,
			      metablks_count);


	printf("%d debug\n", __LINE__);
//	sqfs_dump_directory_table(sblk, pos_list, metablks_count);

	for (j = 0; j < token_count; j++)
		free(token_list[j]);
	free(token_list);

	printf("dir_table %p\n", dirs->dir_table);
	printf("inode_table %p\n", dirs->inode_table);
free_mem:
	free(itb);
	free(dtb);
	free(pos_list);

	return ret;
}

int sqfs_readdir(struct fs_dir_stream *fs_dirs, struct fs_dirent **dentp)
{

	printf("sqfs_readdir\n");
	struct squashfs_dir_stream *dirs;
	union squashfs_inode inode;
	int offset = 0;

	dirs = (struct squashfs_dir_stream *)fs_dirs;
	printf("dirs base addr %p\n", dirs);
//	printf("dir_table %p\n", dirs->dir_table);
	printf("inode_table %p\n", dirs->inode_table);
	printf("offset %d\n", dirs->entry->offset);
	printf("fist entry name: %s\n", dirs->entry->name);
	printf("%d debug\n", __LINE__);
	inode.base = (void *)dirs->inode_table + dirs->entry->offset;


	printf("%d debug\n", __LINE__);
	/* No entries to be read */
	if (dirs->size < sizeof(struct squashfs_directory_header))
		return 0;
//	printf("passou, entry: %p\n", dirs->entry);
	/* Set entry type and size */
	switch (dirs->entry->type) {
	case SQFS_DIR_TYPE:
	case SQFS_LDIR_TYPE:
	printf("%d debug\n", __LINE__);
		(*dentp)->type = FS_DT_DIR;
		break;
	case SQFS_REG_TYPE:
	case SQFS_LREG_TYPE:
	printf("%d debug\n", __LINE__);
		(*dentp)->size = (loff_t)inode.reg->file_size;

	printf("%d debug\n", __LINE__);
		/*
		 * Entries do not differentiate extended from regular, so it
		 * needs to be verified manually.
		 */
		if (inode.base->inode_type == SQFS_LREG_TYPE)
			(*dentp)->size = (loff_t)inode.lreg->file_size;
	printf("%d debug\n", __LINE__);

		(*dentp)->type = FS_DT_REG;
		break;
	case SQFS_BLKDEV_TYPE:
	case SQFS_CHRDEV_TYPE:
	case SQFS_LBLKDEV_TYPE:
	case SQFS_LCHRDEV_TYPE:
	case SQFS_FIFO_TYPE:
	case SQFS_SOCKET_TYPE:
	case SQFS_LFIFO_TYPE:
	case SQFS_LSOCKET_TYPE:
	printf("%d debug\n", __LINE__);
		(*dentp)->type = SQFS_MISC_ENTRY_TYPE;
		break;
	case SQFS_SYMLINK_TYPE:
	case SQFS_LSYMLINK_TYPE:
	printf("%d debug\n", __LINE__);
		(*dentp)->type = FS_DT_LNK;
		break;
	default:
		/*
		 * This macro evaluates to 0, which usually means a successful
		 * execution, but in this case it returns 0 to stop the while
		 * loop.
		 */
		return SQFS_STOP_READDIR;
	}

	printf("%d debug\n", __LINE__);

	/* Set entry name */
	strncpy((*dentp)->name, dirs->entry->name, dirs->entry->name_size + 1);
	(*dentp)->name[dirs->entry->name_size + 1] = '\0';

	offset = dirs->entry->name_size + 1 + SQFS_ENTRY_BASE_LENGTH;
	dirs->entry_count--;
	dirs->size -= offset;

	/* Keep a reference to the current entry before incrementing it */
	dirs->table = dirs->entry;


	printf("%d debug\n", __LINE__);
	/* Are there any remaining headers? */
	if (!dirs->entry_count) {
//		printf("dirs size %d\n", dirs->size);
		dirs->size -= sizeof(struct squashfs_directory_header);
		if (dirs->size > SQFS_EMPTY_FILE_SIZE) {
//			printf("mudou o header");
			dirs->dir_header = (void *)dirs->entry + offset;
			dirs->entry_count = dirs->dir_header->count + 1;
			/* Increment pointer to the entry following the header */
			dirs->entry = (void*)dirs->dir_header +
				sizeof(struct squashfs_directory_header);

			/* Calculate remaining size */

		}
	} else {
		/* Increment pointer to the next entry */
		dirs->entry = (void *)dirs->entry + offset;
	}



	return (dirs->size > 0);
}

int sqfs_probe(struct blk_desc *fs_dev_desc, disk_partition_t *fs_partition)
{
	struct squashfs_super_block *sblk;

	ALLOC_CACHE_ALIGN_BUFFER(char, buffer, fs_dev_desc->blksz);

	cur_dev = fs_dev_desc;
	cur_part_info = *fs_partition;

	/* Read block device data into buffer */
	if (disk_read(0, 1, buffer) != 1) {
		cur_dev = NULL;
		return -1;
	}

	sblk = (struct squashfs_super_block *)buffer;

	/* Make sure it has a valid SquashFS magic number*/
	if (sblk->s_magic != SQFS_MAGIC_NUMBER) {
		printf("Bad magic number for SquashFS image.\n");
		cur_dev = NULL;
		return -1;
	}

	return 0;
}

int sqfs_read(const char *filename, void *buf, loff_t offset,
	      loff_t len, loff_t *actread)
{
	char **token_list, *dir, *aux, *fragment_block, *datablock = NULL;
	char *filename_, *data_buffer, *data, *fragment;
	uint32_t start, n_blks, table_size;
	unsigned long int dest_len;
	int ret, i, j, token_count, path_size, i_number, comp_type;
	int datablk_count = 0, data_offset;
	struct squashfs_fragment_block_entry frag_entry;
	struct squashfs_directory_entry *entry = NULL;
	struct squashfs_file_info finfo = {0};
	struct fs_dir_stream *dirsp = NULL;
	struct squashfs_super_block *sblk;
	struct squashfs_dir_stream dirs;
	union squashfs_inode inode;
	struct fs_dirent dent;
	const char *file;

	*actread = 0;

	/* Read SquashFS super block */
	sblk = malloc(cur_dev->blksz);
	if (disk_read(0, 1, sblk) != 1) {
		free(sblk);
		cur_dev = NULL;
		return CMD_RET_FAILURE;
	}

	comp_type = sblk->compression;

	/*
	 * This copy ensures it does not matter if the filename starts by a '/'
	 * or not, because the root direcory (in the user's perspective) is
	 * implicit.
	 */
	if (filename[0] == '/') {
		filename_ = malloc(strlen(filename));
		if (!filename_) {
			free(sblk);
			return errno;
		}

		strcpy(filename_, filename);
	} else {
		filename_ = malloc(strlen(filename) + 1);
		if (!filename_) {
			free(sblk);
			return errno;
		}

		filename_[0] = '/';
		strcpy(filename_ + 1, filename);
	}

	token_count = sqfs_count_tokens(filename_);
	if (token_count < 0) {
		free(sblk);
		free(filename_);
		return CMD_RET_FAILURE;
	}

	path_size = strlen(filename_);
	for (i = 1; i < path_size + 1; i++) {
		if (filename_[path_size - i] == '/') {
			path_size -= i;
			break;
		}
	}

	if (!path_size)
		path_size = 1;

	/*
	 * filename = /path/to/file_
	 * dir = /path/to/ (path_size = strlen(dir))
	 * file = file_
	 */
	dir = malloc(path_size * sizeof(char *) + 1);
	if (!dir) {
		free(sblk);
		free(filename_);
		free(dir);
		return errno;
	}

	dir[path_size] = '\0';
	strncpy(dir, filename_, path_size);

	/* If file is in root dir., there is only 1 slash to skip */
	if (path_size == 1)
		file = filename_ + path_size;
	else
		file = filename_ + path_size + 1;

	/* Tokenize path string */
	if (token_count > 1)
		token_count--;

	token_list = malloc(token_count * sizeof(char *));
	if (!token_list) {
		free(sblk);
		free(filename_);
		free(dir);
		return errno;
	}

	/* Allocate and fill token list */
	if (dir[0] == '/' && strlen(dir) == 1) {
		token_list[0] = malloc(strlen(dir) + 1);
		strcpy(token_list[0], dir);
	} else {
		for (i = 0; i < token_count; i++) {
			aux = strtok(!i ? dir : NULL, "/");
			token_list[i] = malloc(strlen(aux) + 1);
			if (!token_list[i]) {
				for (j = 0; j < i; j++)
					free(token_list[j]);
				free(token_list);
				free(sblk);
				free(filename_);
				free(dir);
				return errno;
			}

			strcpy(token_list[i], aux);
		}
	}

	printf("TOKEN LIST:\n");
	for (j = 0; j < token_count; j++)
		printf("token: %s\n", token_list[j]);


	/*
	 * sqfs_opendir will uncompress inode and directory tables, and will
	 * return a pointer to the directory that contains the requested file.
	 */
	ret = sqfs_opendir(dir, &dirs.fs_dirs);
	dirsp = (struct fs_dir_stream *)&dirs;
	printf("%d debug\n", __LINE__);
	if (ret) {
		sqfs_closedir(dirsp);
		ret = CMD_RET_FAILURE;
		goto free_strings;
	}

	printf("%d debug\n", __LINE__);

	dirs.dentp = &dent;
	dirs.dir_header = (struct squashfs_directory_header *)dirs.table;
	dirs.entry = (void *)dirs.dir_header + SQFS_DIR_HEADER_SIZE;
	dirs.entry_count = dirs.dir_header->count + 1;

	printf("read: inode t %p\n", dirs.inode_table);
	printf("dir t %p\n", dirs.dir_table);
	printf("table: %p\n", dirs.table);
	printf("fist entry name: %s\n", dirs.entry->name);

	if (dirs.i.base->inode_type == SQFS_LDIR_TYPE)
		dirs.size = dirs.i.ldir->file_size;
	else if (dirs.i.base->inode_type == SQFS_DIR_TYPE)
		dirs.size = dirs.i.dir->file_size;

	printf("dirs base addr %p\n", dirsp);
	/* For now, only regular files are able to be loaded */
	while (sqfs_readdir(dirsp, &dirs.dentp)) {
//		printf("cur entry name: %s\n", dent.name);
		if (!strcmp(dent.name, file)) {
			entry = dirs.table;
			break;
		}
	}

	if (!entry) {
		printf("File not found.\n");
		*actread = 0;
		sqfs_closedir(dirsp);
		ret = CMD_RET_FAILURE;
		goto free_strings;
	}

	i_number = dirs.dir_header->inode_number + entry->inode_offset;
	inode.base = sqfs_find_inode(dirs.inode_table, i_number, sblk->inodes,
				     sblk->block_size);
	printf("\n\n");
	printf("block size: %d\n", sblk->block_size);
	switch (inode.base->inode_type) {
	case SQFS_REG_TYPE:
		printf("reg file\n");
		finfo.size = inode.reg->file_size;
		printf("file size: %ld\n", finfo.size);
		finfo.offset = &inode.reg->offset;
		printf("offset: %d\n", inode.reg->offset);
		finfo.start = inode.reg->start_block;
		printf("start: %d\n", inode.reg->start_block);
		finfo.frag = SQFS_IS_FRAGMENTED(inode.reg->fragment);
		finfo.blk_sizes = inode.reg->block_list;
		printf("\n\n");

		if (finfo.frag) {
			datablk_count = finfo.size / sblk->block_size;
			finfo.comp = sqfs_frag_lookup(inode.reg->fragment,
						      &frag_entry);
			printf("Ater sqfs_frag_lookup...\n");
			printf("fragment block size: %ld\n",
			       SQFS_BLOCK_SIZE(frag_entry.size));
			printf("fragment block off: %lld\n", frag_entry.start);
		} else {
			datablk_count = DIV_ROUND_UP(finfo.size,
						     sblk->block_size);
		}

		printf("datablk count %d\n", datablk_count);

		break;
	case SQFS_LREG_TYPE:
		printf("lreg file\n");
		finfo.size = inode.lreg->file_size;
		printf("file size: %ld\n", finfo.size);
		finfo.offset = &inode.lreg->offset;
		printf("offset: %d\n", inode.lreg->offset);
		finfo.start = inode.lreg->start_block;
		finfo.frag = SQFS_IS_FRAGMENTED(inode.lreg->fragment);
		printf("start: %lld\n", inode.lreg->start_block);
		finfo.blk_sizes = inode.lreg->block_list;
		printf("\n\n");

		/* Count number of data blocks used to store the file */
		if (finfo.frag) {
			datablk_count = finfo.size / sblk->block_size;
			finfo.comp = sqfs_frag_lookup(inode.lreg->fragment,
						      &frag_entry);
			printf("Ater sqfs_frag_lookup...\n");
			printf("fragment block size: %ld\n",
			       SQFS_BLOCK_SIZE(frag_entry.size));
			printf("fragment block off: %lld\n", frag_entry.start);
		} else {
			datablk_count = DIV_ROUND_UP(finfo.size,
						     sblk->block_size);
		}

		printf("datablk count %d\n", datablk_count);
		break;
	case SQFS_SYMLINK_TYPE:
	case SQFS_LSYMLINK_TYPE:
	case SQFS_BLKDEV_TYPE:
	case SQFS_CHRDEV_TYPE:
	case SQFS_LBLKDEV_TYPE:
	case SQFS_LCHRDEV_TYPE:
	case SQFS_FIFO_TYPE:
	case SQFS_SOCKET_TYPE:
	case SQFS_LFIFO_TYPE:
	case SQFS_LSOCKET_TYPE:
	default:
		printf("Unsupported entry type\n");
		ret = CMD_RET_FAILURE;
		goto free_strings;
	}

	/* If the user specifies a length, check its sanity */
	if (len) {
		if (len > finfo.size) {
			printf("Size to load (%lld bytes) exceeds file size"\
			       "(%ld bytes)\n", len, finfo.size);
			ret = CMD_RET_FAILURE;
			goto free_strings;
		}

		finfo.size = len;
	}


	if (datablk_count) {
		data_offset = finfo.start;
		datablock = malloc(sblk->block_size);
		if(!datablock) {
			ret = CMD_RET_FAILURE;
			goto free_strings;
		}
	}

	for (j = 0; j < datablk_count; j++) {
		start = data_offset / cur_dev->blksz;
		table_size = SQFS_BLOCK_SIZE(finfo.blk_sizes[j]);
		n_blks = DIV_ROUND_UP(table_size + data_offset -
				      (start * cur_dev->blksz), cur_dev->blksz);
		data_buffer = malloc_cache_aligned(n_blks * cur_dev->blksz);
		if (!data_buffer) {
			ret = CMD_RET_FAILURE;
			goto free_mem;
		}

		printf("table size: %d\n", table_size);
		printf("malloc size: %ld\n", n_blks * cur_dev->blksz);

		if (disk_read(start, n_blks, data_buffer) < 0) {
			/*
			 * Tip: re-compile the SquashFS image with mksquashfs's
			 * -b <block_size> option.
			 */
			printf("Error: too many data blocks or too large"\
			       "SquashFS block size.\n");
			ret = CMD_RET_FAILURE;
			free(data_buffer);
			goto free_mem;
		}

		data = data_buffer + data_offset - (start * cur_dev->blksz);

		/* Load the data */
		if (SQFS_COMPRESSED_BLOCK(finfo.blk_sizes[j])) {
			dest_len = sblk->block_size;
			ret = sqfs_decompress(comp_type, datablock, &dest_len,
					      data, table_size);
			if (ret) {
				ret = CMD_RET_FAILURE;
				free(data_buffer);
				goto free_mem;
			}

			memcpy(buf + offset + *actread, datablock, dest_len);
			*actread += dest_len;
		} else {
			memcpy(buf + offset + *actread, data, table_size);
			*actread += table_size;
		}

		data_offset += table_size;
		free(data_buffer);
	}




	/*
	 * There is no need to continue if the file is not fragmented.
	 */
	if (!finfo.frag) {
		ret = CMD_RET_SUCCESS;
		goto free_mem;
	}

	printf("read fragments...\n");
	table_size = SQFS_BLOCK_SIZE(frag_entry.size);
	printf("fragment block off: %lld\n", frag_entry.start);
	start = frag_entry.start / cur_dev->blksz;
	n_blks = DIV_ROUND_UP(table_size + frag_entry.start -
			      (start * cur_dev->blksz), cur_dev->blksz);

	printf("table size: %d\n", table_size);
	printf("start: %d\n", start);
	printf("n_blks (frag): %d\n", n_blks);
	fragment = malloc_cache_aligned(n_blks * cur_dev->blksz);


	if (disk_read(start, n_blks, fragment) < 0) {
		ret = CMD_RET_FAILURE;
		goto free_mem;
	}

	/* File compressed and fragmented */
	if (finfo.frag && finfo.comp) {
		printf("file compressed, compressed fragment block\n");
		dest_len = sblk->block_size;
		fragment_block = malloc(sblk->block_size);
		if (!fragment_block) {
			ret = CMD_RET_FAILURE;
			goto free_mem;
		}

		ret = sqfs_decompress(comp_type, fragment_block, &dest_len,
				      (void *)fragment  +
				      frag_entry.start -
				      start * cur_dev->blksz,
				      frag_entry.size);
		if (ret) {
			free(fragment_block);
			ret = CMD_RET_FAILURE;
			goto free_mem;
		}

		for (j = offset + *actread; j < finfo.size; j++) {
			memcpy(buf + j, &fragment_block[*finfo.offset + j], 1);
			(*actread)++;
		}

		free(fragment_block);

	} else if (finfo.frag && !finfo.comp) {
		fragment_block = (void *)fragment + frag_entry.start -
			(start * cur_dev->blksz);

		for (j = offset + *actread; j < finfo.size; j++) {
			memcpy(buf + j, &fragment_block[*finfo.offset + j], 1);
			(*actread)++;
		}
	}


free_mem:
	if (datablk_count)
		free(datablock);
free_strings:
	free(filename_);
	for (i = 0; i < token_count; i++)
		free(token_list[i]);
	free(token_list);
	free(dir);
	free(sblk);

	return ret;
}



int sqfs_ls(const char *filename)
{
	struct squashfs_dir_stream dirs;
	struct fs_dir_stream *dirsp;
	int ret = 0, nfiles = 0, ndirs = 0;

	struct fs_dirent dent;
	char *path;

	path = malloc(strlen(filename) + 1);
	strcpy(path, filename);

	ret = sqfs_opendir(path, &dirs.fs_dirs);
	printf("%d debug\n", __LINE__);
	if (ret) {
		free(path);
		return CMD_RET_FAILURE;
	}

	if (dirs.i.base->inode_type == SQFS_LDIR_TYPE)
		dirs.size = dirs.i.ldir->file_size;
	else if (dirs.i.base->inode_type == SQFS_DIR_TYPE)
		dirs.size = dirs.i.dir->file_size;
	printf("%d debug\n", __LINE__);

	dirs.dentp = &dent;
	dirs.entry = (void *)dirs.table + SQFS_DIR_HEADER_SIZE;
	dirs.dir_header = dirs.table;
	dirs.entry_count = dirs.dir_header->count + 1;
	printf("dirs.table %p\n", dirs.table);
	printf("dir_table %p\n", dirs.dir_table);
	printf("inode_table %p\n", dirs.inode_table);

	printf("%d debug\n", __LINE__);
	dirsp = (struct fs_dir_stream *)&dirs;
	while (sqfs_readdir(dirsp, &dirs.dentp)) {
		switch (dent.type) {
		case FS_DT_DIR:
			printf("            %s/\n", dent.name);
			ndirs++;
			break;
		case FS_DT_REG:
			printf("%8lld   %s\n", dent.size, dent.name);
			nfiles++;
			break;
		case FS_DT_LNK:
			printf("<SYMLINK>   %s\n", dent.name);
			nfiles++;
			break;
		case SQFS_MISC_ENTRY_TYPE:
			printf("            %s\n", dent.name);
			nfiles++;
			break;
		default:
			break;
		}
	}

	printf("\n%d file(s), %d dir(s)\n\n", nfiles, ndirs);

	sqfs_closedir(dirsp);
	free(path);

	return ret;
}

int sqfs_size(const char *filename, loff_t *size)
{
       return 0;
}

void sqfs_close(void)
{
	printf("sqf_close called\n");
}

void sqfs_closedir(struct fs_dir_stream *dirs)
{
	struct squashfs_dir_stream *sqfs_dirs;

	sqfs_dirs = (struct squashfs_dir_stream *)dirs;

	free(sqfs_dirs->inode_table);
	free(sqfs_dirs->dir_table);
}
