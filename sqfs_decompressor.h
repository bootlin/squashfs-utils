/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 *
 * sqfs_decompressor.h:	function prototypes and struct definitions,
 *			included at sqfs_decompressor.c
 */

#ifndef SQFS_DECOMPRESSOR_H
#define SQFS_DECOMPRESSOR_H

#include <stdint.h>

/* LZMA does not support any compression options */

struct gzip_opts {
	uint32_t compression_level;
	uint16_t window_size;
	uint16_t strategies;
};

struct xz_opts {
	uint32_t dictionary_size;
	uint32_t executable_filters;
};

struct lz4_opts {
	uint32_t version;
	uint32_t flags;
};

struct zstd_opts {
	uint32_t compression_level;
};

struct lzo_opts {
	uint32_t algorithm;
	uint32_t level;
};

union sqfs_compression_opts {
	struct gzip_opts *gzip;
	struct xz_opts *xz;
	struct lz4_opts *lz4;
	struct zstd_opts *zstd;
	struct lzo_opts *lzo;
};

enum squashfs_compression_type;

int sqfs_fill_compression_opts(union sqfs_compression_opts *opts,
			       int compression, void *file_mapping);
int sqfs_dump_compression_opts(int compression,
			       union sqfs_compression_opts *opts);

int sqfs_decompress(void *dest, size_t *dest_len, const void *source,
		    size_t source_len);

#endif /* SQFS_DECOMPRESSOR_H */
