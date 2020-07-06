// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 *
 * squashfs.c: SquashFS filesystem implementation
 */

#include <asm/unaligned.h>
#include <errno.h>
#include <fs.h>
#include <linux/types.h>
#include <linux/byteorder/little_endian.h>
#include <linux/byteorder/generic.h>
#include <memalign.h>
#include <stdlib.h>
#include <string.h>
#include <squashfs.h>

#include "sqfs_decompressor.h"
#include "sqfs_filesystem.h"
#include "sqfs_utils.h"

static disk_partition_t cur_part_info;
static struct blk_desc *cur_dev;

static int sqfs_disk_read(__u32 block, __u32 nr_blocks, void *buf)
{
	ulong ret;

	if (!cur_dev)
		return -1;

	ret = blk_dread(cur_dev, cur_part_info.start + block, nr_blocks, buf);
	if (ret != nr_blocks)
		return -1;

	return ret;
}

static int sqfs_read_sblk(struct squashfs_super_block **sblk)
{
	*sblk = malloc_cache_aligned(cur_dev->blksz);
	if (!*sblk)
		return -ENOMEM;

	if (sqfs_disk_read(0, 1, *sblk) != 1) {
		free(*sblk);
		cur_dev = NULL;
		return -EINVAL;
	}

	return 0;
}

/*
 * Calculates how many blocks are needed for the buffer used in sqfs_disk_read.
 * The memory section (e.g. inode table) start offset and its end (i.e. the next
 * table start) must be specified. It also calculates the offset from which to
 * start reading the buffer.
 */
static int sqfs_calc_n_blks(__le64 start, __le64 end, u64 *offset)
{
	u64 start_, table_size;

	table_size = le64_to_cpu(end) - le64_to_cpu(start);
	start_ = le64_to_cpu(start) / cur_dev->blksz;
	*offset = le64_to_cpu(start) - (start_ * cur_dev->blksz);

	return DIV_ROUND_UP(table_size + *offset, cur_dev->blksz);
}

/*
 * Retrieves fragment block entry and returns true if the fragment block is
 * compressed
 */
static int sqfs_frag_lookup(u32 inode_fragment_index,
			    struct squashfs_fragment_block_entry *e)
{
	u64 start, n_blks, src_len, table_offset, start_block;
	unsigned char *metadata_buffer, *metadata, *table;
	struct squashfs_fragment_block_entry *entries;
	struct squashfs_super_block *sblk;
	unsigned long dest_len;
	int block, offset, ret;
	u16 header, comp_type;

	ret = sqfs_read_sblk(&sblk);
	if (ret)
		return -EINVAL;

	comp_type = le16_to_cpu(sblk->compression);

	if (inode_fragment_index >= le32_to_cpu(sblk->fragments))
		return -EINVAL;

	start = le64_to_cpu(sblk->fragment_table_start) / cur_dev->blksz;
	n_blks = sqfs_calc_n_blks(sblk->fragment_table_start,
				  sblk->export_table_start, &table_offset);

	/* Allocate a proper sized buffer to store the fragment index table */
	table = malloc_cache_aligned(n_blks * cur_dev->blksz);
	if (!table) {
		free(sblk);
		return -ENOMEM;
	}

	if (sqfs_disk_read(start, n_blks, table) < 0) {
		free(sblk);
		free(table);
		return -EINVAL;
	}

	block = SQFS_FRAGMENT_INDEX(inode_fragment_index);
	offset = SQFS_FRAGMENT_INDEX_OFFSET(inode_fragment_index);

	/*
	 * Get the start offset of the metadata block that contains the right
	 * fragment block entry
	 */
	start_block = get_unaligned((u64 *)&table[table_offset] + block);

	start = start_block / cur_dev->blksz;
	n_blks = sqfs_calc_n_blks(cpu_to_le64(start_block),
				  sblk->fragment_table_start, &table_offset);

	metadata_buffer = malloc_cache_aligned(n_blks * cur_dev->blksz);
	if (!metadata_buffer) {
		free(sblk);
		free(table);
		return -ENOMEM;
	}

	if (sqfs_disk_read(start, n_blks, metadata_buffer) < 0) {
		ret = -EINVAL;
		goto free_mem;
	}

	/* Every metadata block starts with a 16-bit header */
	header = get_unaligned((u16 *)&metadata_buffer[table_offset]);
	metadata = &metadata_buffer[table_offset + SQFS_HEADER_SIZE];

	entries = malloc(SQFS_METADATA_BLOCK_SIZE);
	if (!entries) {
		ret = -ENOMEM;
		goto free_mem;
	}

	if (SQFS_COMPRESSED_METADATA(header)) {
		src_len = SQFS_METADATA_SIZE(header);
		dest_len = SQFS_METADATA_BLOCK_SIZE;
		ret = sqfs_decompress(comp_type, entries, &dest_len, metadata,
				      src_len);
		if (ret) {
			free(entries);
			ret = -EINVAL;
			goto free_mem;
		}
	} else {
		memcpy(entries, metadata, dest_len);
	}

	*e = entries[offset];
	ret = SQFS_COMPRESSED_BLOCK(e->size);

	free(entries);
free_mem:
	free(sblk);
	free(table);
	free(metadata_buffer);

	return ret;
}

/*
 * The entry name is a flexible array member, and we don't know its size before
 * actually reading the entry. So we need a first copy to retrieve this size so
 * we can finally copy the whole struct.
 */
static int sqfs_read_entry(struct squashfs_directory_entry **dest, void *src)
{
	struct squashfs_directory_entry tmp;

	memcpy(&tmp, src, sizeof(tmp));
	*dest = malloc(sizeof(tmp) + tmp.name_size + 2);
	if (!*dest)
		return -ENOMEM;

	memcpy(*dest, src, sizeof(tmp) + tmp.name_size + 1);
	(*dest)->name[tmp.name_size + 1] = '\0';

	return 0;
}

/*
 * m_list contains each metadata block's position, and m_count is the number of
 * elements of m_list. Those metadata blocks come from the compressed directory
 * table.
 */
static int sqfs_search_dir(struct squashfs_dir_stream *dirs, char **token_list,
			   int token_count, u32 *m_list, int m_count)
{
	int j, ret, new_inode_number, offset;
	struct squashfs_super_block *sblk;
	struct squashfs_ldir_inode ldir;
	struct squashfs_dir_inode dir;
	struct fs_dir_stream *dirsp;
	unsigned char *table;
	struct fs_dirent dent;

	/* Read SquashFS super block */
	ret = sqfs_read_sblk(&sblk);
	if (ret)
		return ret;

	dirsp = (struct fs_dir_stream *)dirs;
	dirs->dentp = &dent;

	/* Start by root inode */
	table = sqfs_find_inode(dirs->inode_table, le32_to_cpu(sblk->inodes),
				sblk->inodes, sblk->block_size);

	/* root is a regular directory, not an extended one */
	memcpy(&dir, table, sizeof(dir));

	/* get directory offset in directory table */
	offset = sqfs_dir_offset(table, m_list, m_count);
	dirs->table = &dirs->dir_table[offset];

	/* Setup directory header */
	dirs->dir_header = malloc(SQFS_DIR_HEADER_SIZE);
	if (!dirs->dir_header) {
		free(sblk);
		return -ENOMEM;
	}

	memcpy(dirs->dir_header, dirs->table, SQFS_DIR_HEADER_SIZE);
	dirs->table += SQFS_DIR_HEADER_SIZE;

	dirs->size = le16_to_cpu(dir.file_size);
	dirs->entry_count = dirs->dir_header->count + 1;
	dirs->size -= SQFS_DIR_HEADER_SIZE;

	/* No path given -> root directory */
	if (!strcmp(token_list[0], "/")) {
		dirs->table = &dirs->dir_table[offset];
		memcpy(&dirs->i_dir, &dir, sizeof(dir));
		free(sblk);
		return 0;
	}

	for (j = 0; j < token_count; j++) {
		if (!sqfs_is_dir(dir.inode_type)) {
			printf("Directory not found: %s\n", token_list[j]);
			free(sblk);
			return -EINVAL;
		}

		offset = sqfs_dir_offset(table, m_list, m_count);
		memcpy(dirs->dir_header, &dirs->dir_table[offset],
		       SQFS_DIR_HEADER_SIZE);

		while (sqfs_readdir(dirsp, &dirs->dentp)) {
			ret = strcmp(dent.name, token_list[j]);
			if (!ret)
				break;
			free(dirs->entry);
		}

		if (ret) {
			printf("Directory not found: %s\n", token_list[j]);
			free(dirs->entry);
			free(sblk);
			return -EINVAL;
		}

		/* Redefine inode as the found token */
		new_inode_number = dirs->entry->inode_offset +
			dirs->dir_header->inode_number;

		/* Get reference to inode in the inode table */
		table = sqfs_find_inode(dirs->inode_table, new_inode_number,
					sblk->inodes, sblk->block_size);
		memcpy(&dir, table, sizeof(dir));

		/* Check inode type sanity */
		if (!sqfs_is_dir(dir.inode_type)) {
			printf("%s is not a directory.\n", token_list[j]);
			free(dirs->entry);
			free(sblk);
			return -EINVAL;
		}

		/* Check if it is an extended dir. */
		if (dir.inode_type == SQFS_LDIR_TYPE)
			memcpy(&ldir, table, sizeof(ldir));

		/* Get dir. offset into the directory table */
		offset = sqfs_dir_offset(table, m_list, m_count);

		/* Copy directory header */
		memcpy(dirs->dir_header, &dirs->dir_table[offset],
		       SQFS_DIR_HEADER_SIZE);

		/* Check for empty directory */
		if (sqfs_is_empty_dir(table)) {
			free(dirs->entry);
			free(sblk);
			return SQFS_EMPTY_DIR;
		}
	}

	free(dirs->entry);
	offset = sqfs_dir_offset(table, m_list, m_count);
	dirs->table = &dirs->dir_table[offset];

	if (dir.inode_type == SQFS_DIR_TYPE)
		memcpy(&dirs->i_dir, &dir, sizeof(dir));
	else
		memcpy(&dirs->i_ldir, &ldir, sizeof(ldir));

	free(sblk);

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

static int sqfs_tokenize_path(const char *filename)
{
	int token_count;

	token_count = sqfs_count_tokens(filename);
	if (token_count < 0)
		return -EINVAL;

	/* Ignore trailing '/' in path */
	if (filename[strlen(filename) - 1] == '/')
		token_count--;

	if (!token_count)
		token_count = 1;

	return token_count;
}

/*
 * Inode and directory tables are stored as a series of metadata blocks, and
 * given the compressed size of this table, we can calculate how much metadata
 * blocks are needed to store the result of the decompression, since a
 * decompressed metadata block should have a size of 8KiB.
 */
static int sqfs_count_metablks(void *table, u32 offset, int table_size)
{
	int count = 0, cur_size = 0, ret;
	u32 data_size;
	bool comp;

	do {
		ret = sqfs_read_metablock(table, offset + cur_size, &comp,
					  &data_size);
		if (ret)
			return -EINVAL;
		cur_size += data_size + SQFS_HEADER_SIZE;
		count++;
	} while (cur_size < table_size);

	return count;
}

/*
 * Storing the metadata blocks header's positions will be useful while looking
 * for an entry in the directory table, using the reference (index and offset)
 * given by its inode.
 */
static int sqfs_get_metablk_pos(u32 *pos_list, void *table, u32 offset,
				int metablks_count)
{
	u32 data_size, cur_size = 0;
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
	unsigned char *src_table, *itb, *dtb, *inode_table, *dir_table;
	int i, j, token_count, ret = 0, metablks_count, comp_type;
	u64 start, n_blks, table_offset, table_size;
	u32 src_len, dest_offset = 0, *pos_list;
	struct squashfs_super_block *sblk;
	struct squashfs_dir_stream *dirs;
	char **token_list, *path, *aux;
	unsigned long dest_len;
	bool compressed;

	/* Read SquashFS super block */
	ret = sqfs_read_sblk(&sblk);
	if (ret)
		return ret;

	comp_type = sblk->compression;

	/* INODE TABLE */

	table_size = le64_to_cpu(sblk->directory_table_start -
				 sblk->inode_table_start);
	start = sblk->inode_table_start / cur_dev->blksz;
	n_blks = sqfs_calc_n_blks(sblk->inode_table_start,
				  sblk->directory_table_start, &table_offset);

	/* Allocate a proper sized buffer (itb) to store the inode table */
	itb = malloc_cache_aligned(n_blks * cur_dev->blksz);
	if (!itb) {
		free(sblk);
		cur_dev = NULL;
		return -ENOMEM;
	}

	if (sqfs_disk_read(start, n_blks, itb) < 0) {
		free(sblk);
		cur_dev = NULL;
		return -EINVAL;
	}

	/* Parse inode table (metadata block) header */
	ret = sqfs_read_metablock(itb, table_offset, &compressed, &src_len);
	if (ret) {
		free(sblk);
		return -EINVAL;
	}

	/* Calculate size to store the whole decompressed table */
	metablks_count = sqfs_count_metablks(itb, table_offset, table_size);
	if (metablks_count < 1) {
		free(sblk);
		return -EINVAL;
	}

	inode_table = malloc(metablks_count * SQFS_METADATA_BLOCK_SIZE);
	if (!inode_table) {
		free(sblk);
		return -ENOMEM;
	}

	src_table = itb + table_offset + SQFS_HEADER_SIZE;

	/* Extract compressed Inode table */
	for (j = 0; j < metablks_count; j++) {
		sqfs_read_metablock(itb, table_offset, &compressed, &src_len);
		if (compressed) {
			dest_len = SQFS_METADATA_BLOCK_SIZE;
			ret = sqfs_decompress(comp_type, inode_table +
					      dest_offset, &dest_len,
					      src_table, src_len);
			if (ret) {
				free(sblk);
				free(itb);
				return ret;
			}

		} else {
			memcpy(inode_table + (j * SQFS_METADATA_BLOCK_SIZE),
			       src_table, src_len);
		}

		/*
		 * Offsets to the decompression destination, to the metadata
		 * buffer 'itb' and to the decompression source, respectively.
		 */
		dest_offset += dest_len;
		table_offset += src_len + SQFS_HEADER_SIZE;
		src_table += src_len + SQFS_HEADER_SIZE;
	}

	free(itb);

	/* DIRECTORY TABLE */
	table_size = le64_to_cpu(sblk->fragment_table_start -
				 sblk->directory_table_start);
	start = sblk->directory_table_start / cur_dev->blksz;
	n_blks = sqfs_calc_n_blks(sblk->directory_table_start,
				  sblk->fragment_table_start, &table_offset);

	/* Allocate a proper sized buffer (dtb) to store the directory table */
	dtb = malloc_cache_aligned(n_blks * cur_dev->blksz);

	if (!dtb) {
		free(inode_table);
		cur_dev = NULL;
		free(sblk);
		return -EINVAL;
	}

	if (sqfs_disk_read(start, n_blks, dtb) < 0) {
		cur_dev = NULL;
		free(sblk);
		free(inode_table);
		return -EINVAL;
	}

	/* Parse directory table (metadata block) header */
	ret = sqfs_read_metablock(dtb, table_offset, &compressed, &src_len);
	if (ret) {
		free(inode_table);
		free(sblk);
		return -EINVAL;
	}

	/* Calculate total size to store the whole decompressed table */
	metablks_count = sqfs_count_metablks(dtb, table_offset, table_size);
	if (metablks_count < 1) {
		free(inode_table);
		free(dtb);
		free(sblk);
		return -EINVAL;
	}

	dir_table = malloc(metablks_count * SQFS_METADATA_BLOCK_SIZE);
	if (!dir_table) {
		free(inode_table);
		free(sblk);
		free(dtb);
		return -ENOMEM;
	}

	pos_list = malloc(metablks_count * sizeof(u32));
	if (!pos_list) {
		free(inode_table);
		free(dir_table);
		free(sblk);
		free(dtb);
		return -ENOMEM;
	}

	ret = sqfs_get_metablk_pos(pos_list, dtb, table_offset, metablks_count);
	if (ret) {
		free(dtb);
		goto free_mem;
	}

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
			if (ret) {
				free(dir_table);
				goto free_mem;
			}

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

	free(dtb);

	token_count = sqfs_tokenize_path(filename);
	if (token_count < 0) {
		ret = -EINVAL;
		goto free_mem;
	}

	path = strdup(filename);
	if (!path)
		return -ENOMEM;

	token_list = malloc(token_count * sizeof(char *));
	if (!token_list) {
		free(path);
		ret = -EINVAL;
		goto free_mem;
	}

	/* Allocate and fill token list */
	if (!strcmp(path, "/")) {
		token_list[0] = strdup(path);
		if (!token_list[0]) {
			free(path);
			free(token_list);
			ret = -ENOMEM;
			goto free_mem;
		}

	} else {
		for (j = 0; j < token_count; j++) {
			aux = strtok(!j ? path : NULL, "/");
			token_list[j] = strdup(aux);
			if (!token_list[j]) {
				for (i = 0; i < j; i++)
					free(token_list[i]);
				free(token_list);
				free(path);
				ret = -ENOMEM;
				goto free_mem;
			}
		}
	}

	free(path);

	dirs = (struct squashfs_dir_stream *)dirsp;
	/*
	 * ldir's (extended directory) size is greater than dir, so it works as
	 * a general solution for the malloc size, since 'i' is a union.
	 */
	dirs->inode_table = inode_table;
	dirs->dir_table = dir_table;
	ret = sqfs_search_dir(dirs, token_list, token_count, pos_list,
			      metablks_count);

	if (le16_to_cpu(dirs->i_dir.inode_type) == SQFS_DIR_TYPE)
		dirs->size = le16_to_cpu(dirs->i_dir.file_size);
	else
		dirs->size = le32_to_cpu(dirs->i_ldir.file_size);

	/* Setup directory header */
	memcpy(dirs->dir_header, dirs->table, SQFS_DIR_HEADER_SIZE);
	dirs->entry_count = dirs->dir_header->count + 1;
	dirs->size -= SQFS_DIR_HEADER_SIZE;

	/* Setup entry */
	dirs->entry = NULL;
	dirs->table += SQFS_DIR_HEADER_SIZE;

	/* Free token list */
	for (j = 0; j < token_count; j++)
		free(token_list[j]);
	free(token_list);
free_mem:
	free(sblk);
	free(pos_list);

	return ret;
}

int sqfs_readdir(struct fs_dir_stream *fs_dirs, struct fs_dirent **dentp)
{
	struct squashfs_lreg_inode lreg;
	struct squashfs_base_inode base;
	struct squashfs_reg_inode reg;
	struct squashfs_dir_stream *dirs;
	int offset = 0, ret;

	dirs = (struct squashfs_dir_stream *)fs_dirs;
	if (!dirs->size)
		return SQFS_STOP_READDIR;

	if (!dirs->entry_count) {
		if (dirs->size > SQFS_DIR_HEADER_SIZE) {
			dirs->size -= SQFS_DIR_HEADER_SIZE;
		} else {
			dirs->size = 0;
			return SQFS_STOP_READDIR;
		}

		if (dirs->size > SQFS_EMPTY_FILE_SIZE) {
			/* Read follow-up (emitted) dir. header */
			memcpy(dirs->dir_header, dirs->table,
			       SQFS_DIR_HEADER_SIZE);
			dirs->entry_count = dirs->dir_header->count + 1;
			ret = sqfs_read_entry(&dirs->entry, dirs->table +
					      SQFS_DIR_HEADER_SIZE);
			if (ret)
				return SQFS_STOP_READDIR;

			dirs->table += SQFS_DIR_HEADER_SIZE;
		}
	} else {
		ret = sqfs_read_entry(&dirs->entry, dirs->table);
		if (ret)
			return SQFS_STOP_READDIR;
	}

	memcpy(&base, &dirs->inode_table[dirs->entry->offset], sizeof(base));

	/* Set entry type and size */
	switch (dirs->entry->type) {
	case SQFS_DIR_TYPE:
	case SQFS_LDIR_TYPE:
		(*dentp)->type = FS_DT_DIR;
		break;
	case SQFS_REG_TYPE:
	case SQFS_LREG_TYPE:
		/*
		 * Entries do not differentiate extended from regular types, so
		 * it needs to be verified manually.
		 */
		if (base.inode_type == SQFS_LREG_TYPE) {
			memcpy(&lreg, &dirs->inode_table[dirs->entry->offset],
			       sizeof(lreg));
			(*dentp)->size = (loff_t)le64_to_cpu(lreg.file_size);
		} else {
			memcpy(&reg, &dirs->inode_table[dirs->entry->offset],
			       sizeof(reg));
			(*dentp)->size = (loff_t)le32_to_cpu(reg.file_size);
		}

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
		(*dentp)->type = SQFS_MISC_ENTRY_TYPE;
		break;
	case SQFS_SYMLINK_TYPE:
	case SQFS_LSYMLINK_TYPE:
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

	/* Set entry name */
	strncpy((*dentp)->name, dirs->entry->name, dirs->entry->name_size + 1);
	(*dentp)->name[dirs->entry->name_size + 1] = '\0';

	offset = dirs->entry->name_size + 1 + SQFS_ENTRY_BASE_LENGTH;
	dirs->entry_count--;

	/* Decrement size to be read */
	if (dirs->size > offset)
		dirs->size -= offset;
	else
		dirs->size = 0;

	/* Keep a reference to the current entry before incrementing it */
	dirs->table += offset;

	return SQFS_CONTINUE_READDIR;
}

int sqfs_probe(struct blk_desc *fs_dev_desc, disk_partition_t *fs_partition)
{
	struct squashfs_super_block *sblk;

	cur_dev = fs_dev_desc;
	cur_part_info = *fs_partition;
	sblk = malloc_cache_aligned(cur_dev->blksz);

	if (!sblk)
		return -ENOMEM;

	/* Read SquashFS super block */
	if (sqfs_disk_read(0, 1, sblk) != 1) {
		free(sblk);
		cur_dev = NULL;
		return -EINVAL;
	}

	/* Make sure it has a valid SquashFS magic number*/
	if (sblk->s_magic != SQFS_MAGIC_NUMBER) {
		printf("Bad magic number for SquashFS image.\n");
		cur_dev = NULL;
		return -EINVAL;
	}

	free(sblk);

	return 0;
}

static char *sqfs_basename(char *path)
{
	char *fname;

	fname = path + strlen(path) - 1;
	while (fname >= path) {
		if (*fname == '/') {
			fname++;
			break;
		}

		fname--;
	}

	return fname;
}

static char *sqfs_dirname(char *path)
{
	char *fname;

	fname = sqfs_basename(path);
	--fname;
	*fname = '\0';

	return path;
}

/*
 * Takes a path to file and splits it in two parts: the filename itself and the
 * directory's path, e.g.:
 * path: /path/to/file.txt
 * file: file.txt
 * dir: /path/to
 */
static int sqfs_split_path(char **file, char **dir, const char *path)
{
	char *dirc, *basec, *bname, *dname, *tmp_path;
	int ret = 0;

	/* check for first slash in path*/
	if (path[0] == '/') {
		tmp_path = strdup(path);
		if (!tmp_path)
			return -ENOMEM;
	} else {
		tmp_path = malloc(strlen(path) + 2);
		if (!tmp_path)
			return -ENOMEM;
		tmp_path[0] = '/';
		strcpy(tmp_path + 1, path);
	}

	/* String duplicates */
	dirc = strdup(tmp_path);
	if (!dirc) {
		free(tmp_path);
		return -ENOMEM;
	}

	basec = strdup(tmp_path);
	if (!basec) {
		free(tmp_path);
		free(dirc);
		return -ENOMEM;
	}

	dname = sqfs_dirname(dirc);
	bname = sqfs_basename(basec);

	*file = strdup(bname);

	if (!*file) {
		ret = -ENOMEM;
		goto free_mem;
	}

	if (*dname == '\0') {
		*dir = malloc(2);
		if (!*dir) {
			ret = -ENOMEM;
			goto free_mem;
		}

		(*dir)[0] = '/';
		(*dir)[1] = '\0';
	} else {
		*dir = strdup(dname);
		if (!*dir) {
			ret = -ENOMEM;
			goto free_mem;
		}
	}

free_mem:
	free(tmp_path);
	free(dirc);
	free(basec);

	return ret;
}

static int sqfs_get_regfile_info(struct squashfs_reg_inode *reg,
				 struct squashfs_file_info *finfo,
				 struct squashfs_fragment_block_entry *fentry,
				 __le32 blksz)
{
	int datablk_count = 0, ret;

	finfo->size = le32_to_cpu(reg->file_size);
	finfo->offset = le32_to_cpu(reg->offset);
	finfo->start = le32_to_cpu(reg->start_block);
	finfo->frag = SQFS_IS_FRAGMENTED(le32_to_cpu(reg->fragment));

	if (finfo->frag) {
		datablk_count = finfo->size / le32_to_cpu(blksz);
		ret = sqfs_frag_lookup(reg->fragment, fentry);
		if (ret < 0)
			return -EINVAL;
		finfo->comp = true;
	} else {
		datablk_count = DIV_ROUND_UP(finfo->size, le32_to_cpu(blksz));
	}

	finfo->blk_sizes = malloc(datablk_count * sizeof(u32));

	return datablk_count;
}

static int sqfs_get_lregfile_info(struct squashfs_lreg_inode *lreg,
				  struct squashfs_file_info *finfo,
				  struct squashfs_fragment_block_entry *fentry,
				 __le32 blksz)
{
	int datablk_count = 0, ret;

	finfo->size = le64_to_cpu(lreg->file_size);
	finfo->offset = le32_to_cpu(lreg->offset);
	finfo->start = le64_to_cpu(lreg->start_block);
	finfo->frag = SQFS_IS_FRAGMENTED(le32_to_cpu(lreg->fragment));

	if (finfo->frag) {
		datablk_count = finfo->size / le32_to_cpu(blksz);
		ret = sqfs_frag_lookup(lreg->fragment, fentry);
		if (ret < 0)
			return -EINVAL;
		finfo->comp = true;
	} else {
		datablk_count = DIV_ROUND_UP(finfo->size, le32_to_cpu(blksz));
	}

	finfo->blk_sizes = malloc(datablk_count * sizeof(u32));
	if (!finfo->blk_sizes)
		return -ENOMEM;

	return datablk_count;
}

int sqfs_read(const char *filename, void *buf, loff_t offset, loff_t len,
	      loff_t *actread)
{
	char *dir, *fragment_block, *datablock = NULL, *data_buffer, *data;
	char *fragment, *file;
	int ret, j, i_number, comp_type, datablk_count = 0;
	u64 start, n_blks, table_size, data_offset, table_offset;
	struct squashfs_fragment_block_entry frag_entry;
	struct squashfs_directory_entry *entry = NULL;
	struct squashfs_file_info finfo = {0};
	struct fs_dir_stream *dirsp = NULL;
	struct squashfs_super_block *sblk;
	struct squashfs_dir_stream dirs;
	struct squashfs_lreg_inode lreg;
	struct squashfs_base_inode base;
	struct squashfs_reg_inode reg;
	unsigned long dest_len;
	struct fs_dirent dent;
	unsigned char *ipos;

	*actread = 0;

	/* Read SquashFS super block */
	ret = sqfs_read_sblk(&sblk);
	if (ret)
		return ret;

	comp_type = le16_to_cpu(sblk->compression);

	/*
	 * sqfs_opendir will uncompress inode and directory tables, and will
	 * return a pointer to the directory that contains the requested file.
	 */
	sqfs_split_path(&file, &dir, filename);
	ret = sqfs_opendir(dir, &dirs.fs_dirs);
	if (ret) {
		sqfs_closedir(dirsp);
		free(sblk);
		return CMD_RET_FAILURE;
	}

	free(dir);

	dirsp = (struct fs_dir_stream *)&dirs;
	dirs.dentp = &dent;
	/* For now, only regular files are able to be loaded */
	while (sqfs_readdir(dirsp, &dirs.dentp)) {
		if (!strncmp(dent.name, file, strlen(dent.name))) {
			ret = sqfs_read_entry(&entry, dirs.entry);
			if (ret)
				return CMD_RET_FAILURE;
			break;
		}

		free(dirs.entry);
	}

	free(file);

	if (!entry) {
		printf("File not found.\n");
		*actread = 0;
		sqfs_closedir(dirsp);
		free(sblk);
		return CMD_RET_FAILURE;
	}

	i_number = dirs.dir_header->inode_number + entry->inode_offset;
	ipos = sqfs_find_inode(dirs.inode_table, i_number, sblk->inodes,
			       sblk->block_size);

	memcpy(&base, ipos, sizeof(base));
	printf("\n\n");
	switch (base.inode_type) {
	case SQFS_REG_TYPE:
		memcpy(&reg, ipos, sizeof(reg));
		datablk_count = sqfs_get_regfile_info(&reg, &finfo, &frag_entry,
						      sblk->block_size);
		memcpy(finfo.blk_sizes, ipos + sizeof(reg),
		       datablk_count * sizeof(u32));
		break;
	case SQFS_LREG_TYPE:
		memcpy(&lreg, ipos, sizeof(lreg));
		datablk_count = sqfs_get_lregfile_info(&lreg, &finfo,
						       &frag_entry,
						       sblk->block_size);
		memcpy(finfo.blk_sizes, ipos + sizeof(lreg),
		       datablk_count * sizeof(u32));
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
		free(sblk);
		return CMD_RET_FAILURE;
	}

	if (datablk_count < 0) {
		ret = CMD_RET_FAILURE;
		goto free_mem;
	}

	/* If the user specifies a length, check its sanity */
	if (len) {
		if (len > finfo.size) {
			free(sblk);
			return CMD_RET_FAILURE;
		}

		finfo.size = len;
	}

	if (datablk_count) {
		data_offset = finfo.start;
		datablock = malloc(sblk->block_size);
		if (!datablock) {
			free(sblk);
			return CMD_RET_FAILURE;
		}
	}

	for (j = 0; j < datablk_count; j++) {
		start = data_offset / cur_dev->blksz;
		table_size = SQFS_BLOCK_SIZE(finfo.blk_sizes[j]);
		table_offset = data_offset - (start * cur_dev->blksz);
		n_blks = DIV_ROUND_UP(table_size + table_offset,
				      cur_dev->blksz);

		data_buffer = malloc_cache_aligned(n_blks * cur_dev->blksz);

		if (!data_buffer) {
			ret = CMD_RET_FAILURE;
			goto free_mem;
		}

		if (sqfs_disk_read(start, n_blks, data_buffer) < 0) {
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

		data = data_buffer + table_offset;

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

	free(finfo.blk_sizes);

	/*
	 * There is no need to continue if the file is not fragmented.
	 */
	if (!finfo.frag) {
		ret = CMD_RET_SUCCESS;
		goto free_mem;
	}

	start = frag_entry.start / cur_dev->blksz;
	table_size = SQFS_BLOCK_SIZE(frag_entry.size);
	table_offset = frag_entry.start - (start * cur_dev->blksz);
	n_blks = DIV_ROUND_UP(table_size + table_offset, cur_dev->blksz);

	fragment = malloc_cache_aligned(n_blks * cur_dev->blksz);

	if (!fragment) {
		ret = CMD_RET_FAILURE;
		goto free_mem;
	}

	if (sqfs_disk_read(start, n_blks, fragment) < 0) {
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
				      (void *)fragment  + table_offset,
				      frag_entry.size);
		if (ret) {
			free(fragment_block);
			ret = CMD_RET_FAILURE;
			goto free_mem;
		}

		for (j = offset + *actread; j < finfo.size; j++) {
			memcpy(buf + j, &fragment_block[finfo.offset + j], 1);
			(*actread)++;
		}

		free(fragment_block);

	} else if (finfo.frag && !finfo.comp) {
		fragment_block = (void *)fragment + table_offset;

		for (j = offset + *actread; j < finfo.size; j++) {
			memcpy(buf + j, &fragment_block[finfo.offset + j], 1);
			(*actread)++;
		}
	}

free_mem:
	if (datablk_count)
		free(datablock);
	free(sblk);

	return ret;
}

int sqfs_ls(const char *filename)
{
	int ret = 0, nfiles = 0, ndirs = 0;
	struct squashfs_dir_stream dirs;
	struct fs_dir_stream *dirsp;
	struct fs_dirent dent;

	ret = sqfs_opendir(filename, &dirs.fs_dirs);
	if (ret)
		return CMD_RET_FAILURE;

	dirs.dentp = &dent;
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

		free(dirs.entry);
	}

	printf("\n%d file(s), %d dir(s)\n\n", nfiles, ndirs);

	sqfs_closedir(dirsp);

	return ret;
}

int sqfs_size(const char *filename, loff_t *size)
{
	struct squashfs_symlink_inode symlink;
	struct fs_dir_stream *dirsp = NULL;
	struct squashfs_super_block *sblk;
	struct squashfs_base_inode base;
	struct squashfs_dir_stream dirs;
	int ret, i_number;
	struct squashfs_lreg_inode lreg;
	struct squashfs_reg_inode reg;
	char *dir, *file;
	struct fs_dirent dent;
	unsigned char *ipos;

	/* Read SquashFS super block */
	ret = sqfs_read_sblk(&sblk);
	if (ret)
		return ret;

	sqfs_split_path(&file, &dir, filename);

	/*
	 * sqfs_opendir will uncompress inode and directory tables, and will
	 * return a pointer to the directory that contains the requested file.
	 */
	ret = sqfs_opendir(dir, &dirs.fs_dirs);
	free(dir);
	if (ret) {
		sqfs_closedir(dirsp);
		ret = CMD_RET_FAILURE;
		goto free_strings;
	}

	dirsp = (struct fs_dir_stream *)&dirs;
	dirs.dentp = &dent;

	/* For now, only regular files are able to be loaded */
	while (sqfs_readdir(dirsp, &dirs.dentp)) {
		ret = strcmp(dent.name, file);
		if (!ret)
			break;
		free(dirs.entry);
	}

	free(file);

	if (ret) {
		sqfs_closedir(dirsp);
		ret = CMD_RET_FAILURE;
		goto free_strings;
	}

	i_number = dirs.dir_header->inode_number + dirs.entry->inode_offset;
	ipos = sqfs_find_inode(dirs.inode_table, i_number, sblk->inodes,
			       sblk->block_size);

	memcpy(&base, ipos, sizeof(base));
	switch (base.inode_type) {
	case SQFS_REG_TYPE:
		memcpy(&reg, ipos, sizeof(reg));
		*size = reg.file_size;
		break;
	case SQFS_LREG_TYPE:
		memcpy(&lreg, ipos, sizeof(lreg));
		*size = lreg.file_size;
		break;
	case SQFS_SYMLINK_TYPE:
	case SQFS_LSYMLINK_TYPE:
		memcpy(&symlink, ipos, sizeof(symlink));
		*size = symlink.symlink_size;
		break;
	case SQFS_BLKDEV_TYPE:
	case SQFS_CHRDEV_TYPE:
	case SQFS_LBLKDEV_TYPE:
	case SQFS_LCHRDEV_TYPE:
	case SQFS_FIFO_TYPE:
	case SQFS_SOCKET_TYPE:
	case SQFS_LFIFO_TYPE:
	case SQFS_LSOCKET_TYPE:
	default:
		printf("Unable to recover entry's size.\n");
		*size = 0;
		break;
	}

free_strings:
	free(sblk);
	sqfs_closedir(dirsp);

	return 0;
}

void sqfs_close(void)
{
}

void sqfs_closedir(struct fs_dir_stream *dirs)
{
	struct squashfs_dir_stream *sqfs_dirs;

	sqfs_dirs = (struct squashfs_dir_stream *)dirs;
	free(sqfs_dirs->inode_table);
	free(sqfs_dirs->dir_table);
	free(sqfs_dirs->dir_header);
}
