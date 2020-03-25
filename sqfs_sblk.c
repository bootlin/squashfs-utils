// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 *
 * sqfs_sblk.c: retrieve information about the SquashFS image super block
 */

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#include "sqfs_decompressor.h"
#include "sqfs_utils.h"

/*
 * References:
 * https://www.kernel.org/doc/Documentation/filesystems/squashfs.txt
 * https://dr-emann.github.io/squashfs/
 */
int sqfs_dump_sblk(void *file_mapping)
{
	struct squashfs_super_block *sblk;
	union sqfs_compression_opts opts;
	struct super_block_flags sblkf;
	char fs_creation_date[80];
	struct tm timestamp;
	time_t rawtime;
	int ret;

	sblk = file_mapping;
	rawtime = sblk->mkfs_time;
	timestamp = *localtime(&rawtime);
	strftime(fs_creation_date, sizeof(fs_creation_date),
		 "%a %Y-%m-%d (yyyy-mm-dd) %H:%M:%S %Z", &timestamp);
	printf("--- SUPER BLOCK INFORMATION ---\n");
	printf("Magic number: %c%c%c%c\n", sblk->s_magic >> 24,
	       sblk->s_magic >> 16, sblk->s_magic >> 8, sblk->s_magic);
	printf("Number of inodes: %u\n", sblk->inodes);
	printf("Filesystem creation date: %s\n", fs_creation_date);

	/* Block size must be a power of 2 between 4096 and 1048576 (1 MiB) */
	printf("Block size: %u kB\n", sblk->block_size / 1000);

	/*
	 * Fragment: piece of data which was broken up so it could fit in a
	 * non-continuous interval of memory blocks.
	 */
	printf("Number of fragments: %u\n", sblk->fragments);

	/*
	 * The block size is computed using this 32bit value as the number of
	 * bits to shift left the value 2
	 */
	printf("Block log: %u\n", sblk->block_log);
	printf("Compression type: ");
	switch (sblk->compression) {
	case ZLIB:
		printf("ZLIB\n");
		break;
	case LZMA:
		printf("LZMA\n");
		break;
	case LZO:
		printf("LZO\n");
		break;
	case XZ:
		printf("XZ\n");
		break;
	case LZ4:
		printf("LZ4\n");
		break;
	case ZSTD:
		printf("ZSTD\n");
		break;
	default:
		printf("Unknown compression type: %u\n", sblk->compression);
		return -EINVAL;
	}
	/*
	 * 'flags' data is used to retrieve information needed during the
	 * filesystem decompression stage
	 */
	printf("Super Block Flags: 0x%x\n", sblk->flags);
	printf("Major/Minor numbers: %u/%u\n", sblk->s_major, sblk->s_minor);
	/*
	 * Root inode: offset used to retrieve root inode information from
	 * inode table.
	 */
	printf("Root inode: 0x%lx\n", sblk->root_inode);
	/*
	 * bytes_used: used when reading filesystem table to prevent reading
	 * beyond filesystem end.
	 */
	printf("Bytes used: %lu\n", sblk->bytes_used);
	/*
	 * The following *_table_start identifiers represent a byte offset at
	 * which its respective table starts.
	 *
	 * The id or UID/GID table contains the user and group ids of a file.
	 */
	printf("Id table start: 0x%lx\n", sblk->id_table_start);
	/*
	 * xattr stands for extended file attributes. Those attributes consist
	 * in arbitrary pairs of keys and values. Extended attributes are not
	 * interpreted by the filesystem, differing from regular attributes.
	 */
	printf("(xattr) Id table start: %lx\n", sblk->xattr_id_table_start);
	/* Inode table: interval of metadata blocks containing all inodes. */
	printf("Inode table start: 0x%lx\n", sblk->inode_table_start);
	/*
	 * Directory table: contains the respective lists of the entries stored
	 * inside every directory inode.
	 */
	printf("Directory table start: 0x%lx\n", sblk->directory_table_start);
	/* Fragment table: describes the location and size of fragment blocks.*/
	printf("Fragment table start: 0x%lx\n", sblk->fragment_table_start);
	/*
	 * Lookup table: this table is used to make SquashFS exportable (e.g.
	 * NFS). The export code uses it to map inode numbers passed in
	 * filehandles to an inode location on disk.
	 */
	printf("Lookup table start: 0x%lx\n", sblk->lookup_table_start);

	/* Detailing super block flags */
	ret = sqfs_fill_sblk_flags(&sblkf, sblk->flags);
	if (ret)
		return -EFAULT;
	ret = sqfs_dump_sblk_flags(&sblkf);
	if (ret)
		return -EFAULT;

	/* Detailing (if available) compression options */
	if (sblkf.compressor_options) {
		ret = sqfs_fill_compression_opts(&opts, sblk->compression,
						 file_mapping);
		if (ret)
			return -EFAULT;
		ret = sqfs_dump_compression_opts(sblk->compression, &opts);
		if (ret)
			return -EFAULT;
	}

	return 0;
}

/*
 * Parse 'flags' member from squashfs_super_block structure and fill the
 * super_block_flags parameter with the parsing results.
 */
int sqfs_fill_sblk_flags(struct super_block_flags *sblkf, unsigned short flags)
{
	if (!sblkf) {
		printf("Error while filling super block flags\n");
		return -EINVAL;
	}
	sblkf->uncompressed_inodes = CHECK_FLAG(flags, 0);
	sblkf->uncompressed_data = CHECK_FLAG(flags, 1);
	sblkf->uncompressed_frags = CHECK_FLAG(flags, 3);
	sblkf->no_frags = CHECK_FLAG(flags, 4);
	sblkf->always_frags = CHECK_FLAG(flags, 5);
	sblkf->duplicates = CHECK_FLAG(flags, 6);
	sblkf->exportable = CHECK_FLAG(flags, 7);
	sblkf->uncompressed_xattrs = CHECK_FLAG(flags, 8);
	sblkf->no_xattrs = CHECK_FLAG(flags, 9);
	sblkf->compressor_options = CHECK_FLAG(flags, 10);

	return 0;
}

int sqfs_dump_sblk_flags(struct super_block_flags *sblkf)
{
	if (!sblkf) {
		printf("Error while dumping super block flags\n");
		return -EFAULT;
	}
	printf(" --- SUPER BLOCK FLAGS ---\n");
	if (sblkf->uncompressed_inodes)
		printf("Uncompressed inodes\n");
	if (sblkf->uncompressed_data)
		printf("Uncompressed data\n");
	if (sblkf->uncompressed_frags)
		printf("Uncompressed frags\n");
	if (sblkf->no_frags)
		printf("No fragments\n");
	if (sblkf->always_frags)
		printf("Always fragments\n");
	if (sblkf->duplicates)
		printf("Duplicates\n");
	if (sblkf->exportable)
		printf("Exportable\n");
	if (sblkf->uncompressed_xattrs)
		printf("Uncompressed xattrs\n");
	if (sblkf->no_xattrs)
		printf("No xattrs\n");
	if (sblkf->compressor_options)
		printf("Available compressor options\n");

	return 0;
}
