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

#include "sqfs_utils.h"

/*
 * References:
 * https://www.kernel.org/doc/Documentation/filesystems/squashfs.txt
 * https://dr-emann.github.io/squashfs/
 */

/*
 * Parse 'flags' member from squashfs_super_block structure and fill the
 * super_block_flags parameter with the parsing results.
 */
int sqfs_fill_sblk_flags(struct squashfs_super_block_flags *sblkf,
			 unsigned short flags)
{
	if (!sblkf) {
		printf("Error while filling super block flags\n");
		return -EINVAL;
	}

	sblkf->uncompressed_inodes = SQFS_CHECK_FLAG(flags, 0);
	sblkf->uncompressed_data = SQFS_CHECK_FLAG(flags, 1);
	sblkf->uncompressed_frags = SQFS_CHECK_FLAG(flags, 3);
	sblkf->no_frags = SQFS_CHECK_FLAG(flags, 4);
	sblkf->always_frags = SQFS_CHECK_FLAG(flags, 5);
	sblkf->duplicates = SQFS_CHECK_FLAG(flags, 6);
	sblkf->exportable = SQFS_CHECK_FLAG(flags, 7);
	sblkf->uncompressed_xattrs = SQFS_CHECK_FLAG(flags, 8);
	sblkf->no_xattrs = SQFS_CHECK_FLAG(flags, 9);
	sblkf->compressor_options = SQFS_CHECK_FLAG(flags, 10);

	return 0;
}

