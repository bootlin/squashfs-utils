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
#include <string.h>

#include "sqfs_decompressor.h"
#include "sqfs_filesystem.h"
#include "sqfs_utils.h"

/*
 * Given the uncompressed inode table, the inode to be found and the number of
 * inodes in the table, return inode position in case of success.
 */
void *sqfs_find_inode(void *inode_table, int inode_number, int inode_count,
		      uint32_t block_size)
{
	int k, l, blk_list_size = 0, offset = 0, index_list_size = 0;
	union squashfs_inode i;

	if (!inode_table) {
		printf("%s: Invalid pointer to inode table.\n", __func__);
		return NULL;
	}

	for (k = 0; k < inode_count; k++) {
		i.base = inode_table + offset;
		if (i.base->inode_number == inode_number) {
//			printf("INODE FOUND: %d\n", inode_number);
//			printf("inode type: %d\n", i.base->inode_type);
//			printf("inode number: %d\n", i.base->inode_number);
//			printf("offset into IT %d\n", offset);
//			printf("start block: %d\n", i.dir->start_block);
//			printf("offset %d\n", i.dir->offset);
			return (void *)(inode_table + offset);
		}

		switch (i.base->inode_type) {
		case SQFS_DIR_TYPE:
//			printf("dir inode\t");
			i.dir = inode_table + offset;
//			printf("start block: %d\n", i.dir->start_block);
//			printf("offset %d\n", i.dir->offset);
			offset += sizeof(struct squashfs_dir_inode);
			break;
		case SQFS_REG_TYPE:
//			printf("reg inode\t");
			i.reg = inode_table + offset;

			if (SQFS_IS_FRAGMENTED(i.reg->fragment)) {
				blk_list_size = (i.reg->file_size /
						    block_size);
			} else {
				blk_list_size = DIV_ROUND_UP(i.reg->file_size,
							     block_size);
			}

			offset += sizeof(struct squashfs_reg_inode) +
				blk_list_size * sizeof(uint32_t);
			break;
		case SQFS_LDIR_TYPE:
//			printf("ldir inode\t");
			i.ldir = inode_table + offset;
			if (i.ldir->i_count == 0) {
				offset += sizeof(struct squashfs_ldir_inode);
				break;
			}

			for (l = 0; l < i.ldir->i_count + 1; l++)
				index_list_size += i.ldir->index[l].size + 1;

			offset += sizeof(struct squashfs_ldir_inode) +
				(i.ldir->i_count + 1) *
				SQFS_DIR_INDEX_BASE_LENGTH +
				index_list_size;

			index_list_size = 0;
			break;
		case SQFS_LREG_TYPE:
//			printf("lreg inode\t");
			i.lreg = inode_table + offset;
			if (i.lreg->fragment == 0xFFFFFFFF) {
				blk_list_size = DIV_ROUND_UP(i.lreg->file_size,
							     block_size);
			} else {
				blk_list_size = (i.lreg->file_size /
						 block_size);
			}

			offset += sizeof(struct squashfs_lreg_inode)
				+ blk_list_size * sizeof(uint32_t);

			break;
		case SQFS_SYMLINK_TYPE:
		case SQFS_LSYMLINK_TYPE:
//			printf("symlink inode\t");
			i.symlink = inode_table + offset;
			offset += sizeof(struct squashfs_symlink_inode)
				+ i.symlink->symlink_size;

			break;
		case SQFS_BLKDEV_TYPE:
		case SQFS_CHRDEV_TYPE:
			printf("blk/char dev inode\t");
			i.dev = inode_table + offset;
			offset += sizeof(struct squashfs_dev_inode);
			break;
		case SQFS_LBLKDEV_TYPE:
		case SQFS_LCHRDEV_TYPE:
			printf("lblk/lchar dev inode\t");
			i.ldev = inode_table + offset;
			offset += sizeof(struct squashfs_ldev_inode);
			break;
		case SQFS_FIFO_TYPE:
		case SQFS_SOCKET_TYPE:
			printf("fifo/socket inode\t");
			i.ipc = inode_table + offset;
			offset += sizeof(struct squashfs_ipc_inode);
			break;
		case SQFS_LFIFO_TYPE:
		case SQFS_LSOCKET_TYPE:
			printf("lfifo/lsocket inode\t");
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

int sqfs_read_metablock(void *file_mapping, int offset, bool *compressed,
			uint32_t *data_size)
{
	uint16_t *header;

	header = file_mapping + offset;
	*compressed = SQFS_COMPRESSED_METADATA(*header);
	*data_size = SQFS_METADATA_SIZE(*header);

	if (*data_size > SQFS_METADATA_BLOCK_SIZE) {
		printf("Invalid metatada block size: %d bytes.\n", *data_size);
		return -EINVAL;
	}

	return 0;
}
