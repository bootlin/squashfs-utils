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
#include <u-boot/zlib.h>

#include "sqfs_decompressor.h"
#include "sqfs_filesystem.h"
#include "sqfs_utils.h"

static void zlib_decompression_status(int ret)
{
	switch (ret) {
	case Z_BUF_ERROR:
		printf("Error: 'dest' buffer is not large enough.\n");
		break;
	case Z_DATA_ERROR:
		printf("Error: corrupted compressed data.\n");
		break;
	case Z_MEM_ERROR:
		printf("Error: insufficient memory.\n");
		break;
	}
}

int sqfs_decompress(int comp_type, void *dest, unsigned long int *dest_len,
		    void *source, uint32_t lenp)
{
	int ret = 0;
//	printf("SQFS_DECOMPRESS\n");
//	printf("dest: %p\n", dest);
//	printf("dest_len: %ld\n", *dest_len);
//	printf("source: %p\n", source);
//	printf("source_len: %d\n\n", lenp);

	switch (comp_type) {
	case SQFS_COMP_ZLIB:
		ret = uncompress(dest, dest_len, source, lenp);
		if (ret) {
			zlib_decompression_status(ret);
			return -EINVAL;
		}

		break;
	default:
		printf("Error: unknown compression type.\n");
		return -EINVAL;
	}

	return ret;
}
