// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 *
 * sqfs_decompressor.c: decompress SquashFS image
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "sqfs_decompressor.h"
#include "sqfs_utils.h"

/*
 * Reference:
 * https://dr-emann.github.io/squashfs/#superblock
 */

/* For now, sqfs_decompress supports only zlib compression. */
int sqfs_decompress(void *dest, size_t *dest_len, const void *source,
		    size_t source_len)
{
	int ret;

	ret = uncompress(dest, dest_len, source, source_len);
	switch (ret) {
	case Z_BUF_ERROR:
		printd("Error: 'dest' buffer is not large enough.\n");
		break;
	case Z_DATA_ERROR:
		printd("Error: corrupted compressed data.\n");
		break;
	case Z_MEM_ERROR:
		printd("Error: insufficient memory.\n");
		break;
	case Z_OK:
		printd("Decompression OK.\n");
		break;
	}

	return ret;
}

int sqfs_fill_compression_opts(union sqfs_compression_opts *opts,
			       int compression, void *file_mapping)
{
	/* Metadata block reading */
	void *metadata;

	metadata = file_mapping + SUPER_BLOCK_SIZE + HEADER_SIZE;

	switch (compression) {
	case ZLIB:
		opts->gzip = (struct gzip_opts *)metadata;
		break;
	case LZMA:
		break;
	case LZO:
		opts->lzo = (struct lzo_opts *)metadata;
		break;
	case XZ:
		opts->xz = (struct xz_opts *)metadata;
		break;
	case LZ4:
		opts->lz4 = (struct lz4_opts *)metadata;
		break;
	case ZSTD:
		opts->zstd = (struct zstd_opts *)metadata;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int sqfs_dump_compression_opts(int compression,
			       union sqfs_compression_opts *opts)
{
	if (!opts) {
		printf("Error while dumping compression options\n");
		return -EINVAL;
	}
	printf(" --- COMPRESSION OPTIONS ---\n");
	printf("Compressor: ");
	switch (compression) {
	case ZLIB:
		printf("ZLIB\n");
		printf("Compression level: %u\n",
		       opts->gzip->compression_level);
		printf("Window size: %u\n", opts->gzip->window_size);
		printf("Strategies: 0x%x\n", opts->gzip->strategies);
		break;
	case LZMA:
		printf("LZMA\n");
		printf("No compression options\n");
		break;
	case LZO:
		printf("LZO\n");
		printf("Algorithm: %u\n", opts->lzo->algorithm);
		printf("Level: %u\n", opts->lzo->level);
		break;
	case XZ:
		printf("XZ\n");
		printf("Dictionary size: %u kB\n",
		       opts->xz->dictionary_size / 1000);
		printf("Executable filters: 0x%x\n",
		       opts->xz->executable_filters);
		break;
	case LZ4:
		printf("LZ4\n");
		printf("Version: %u\n", opts->lz4->version);
		printf("Flags: 0x%x\n", opts->lz4->flags);
		break;
	case ZSTD:
		printf("ZSTD\n");
		printf("Compression level: %u\n",
		       opts->zstd->compression_level);
		break;
	default:
		printf("Unknown compression type: %u\n", compression);
		return -EINVAL;
	}

	return 0;
}

