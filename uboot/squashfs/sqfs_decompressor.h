/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 */

#ifndef SQFS_DECOMPRESSOR_H
#define SQFS_DECOMPRESSOR_H

#include <stdint.h>

#define SQFS_COMP_ZLIB 1
#define SQFS_COMP_LZMA 2
#define SQFS_COMP_LZO 3
#define SQFS_COMP_XZ 4
#define SQFS_COMP_LZ4 5
#define SQFS_COMP_ZSTD 6

/* LZMA does not support any compression options */

struct squashfs_gzip_opts {
	uint32_t compression_level;
	uint16_t window_size;
	uint16_t strategies;
};

struct squashfs_xz_opts {
	uint32_t dictionary_size;
	uint32_t executable_filters;
};

struct squashfs_lz4_opts {
	uint32_t version;
	uint32_t flags;
};

struct squashfs_zstd_opts {
	uint32_t compression_level;
};

struct squashfs_lzo_opts {
	uint32_t algorithm;
	uint32_t level;
};

union squashfs_compression_opts {
	struct squashfs_gzip_opts *gzip;
	struct squashfs_xz_opts *xz;
	struct squashfs_lz4_opts *lz4;
	struct squashfs_zstd_opts *zstd;
	struct squashfs_lzo_opts *lzo;
};

int sqfs_decompress(int comp_type, void *dest, uint32_t *dest_len, void *source,
		    uint32_t *lenp);

#endif /* SQFS_DECOMPRESSOR_H */
