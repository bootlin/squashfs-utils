// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Bootlin
 *
 * Author: Joao Marcos Costa <joaomarcos.costa@bootlin.com>
 *
 * main.c:	parse the command line and redirect its arguments to
 *		its respective implemented function
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sqfs_filesystem.h"
#include "sqfs_utils.h"

#define SQFS_USAGE \
	"usage: sqfs [-h]\n" \
	"       sqfs [-s] [-i] [-d] <fs-image>\n" \
	"       sqfs [-e] <fs-image> /path/to/dir/\n" \
	"       sqfs [-e] <fs-image> /path/to/file\n" \
	"\n" \
	"Tool to analyze the content of a SquashFS image\n" \
	"\n" \
	"Options:\n" \
	"       -h: Prints the usage and exits\n" \
	"       -s: Dumps the contents of a SquashFS image's superblock\n" \
	"       -i: Dumps the contents of a SquashFS image's inode table\n" \
	"       -d: Dumps the contents of a SquashFS image's directory table\n"\
	"       -e: Dumps the contents of a SquashFS image's"\
	" file or directory.\n\t   For directories, end path with '/'.\n"\
	"\n" \
	"Parameters:\n" \
	"       <fs-image>: Path to the filesystem image\n" \
	"\n"

int main(int argc, char *argv[])
{
	bool dump_sb = false, dump_inodes = false, dump_dir_table = false,
	     dump_entry = false;
	char *fs_image = NULL;
	void *file_mapping;
	struct stat sb;
	int opt, ret;
	int fd;

	/* Command line parsing */
	while ((opt = getopt(argc, argv, "hside")) != -1) {
		switch (opt) {
		case 'h':
			printf(SQFS_USAGE);
			return EXIT_SUCCESS;
		case 's':
			dump_sb = true;
			break;
		case 'i':
			dump_inodes = true;
			break;
		case 'd':
			dump_dir_table = true;
			break;
		case 'e':
			dump_entry = true;
			break;
		default:
			break;
		}
	}

	/*
	 * Incorrect argument number. For -e option (dump_entry): argc must be
	 * 3 or 4.
	 */
	if ((optind != (argc - 1)) && !dump_entry) {
		printf(SQFS_USAGE);
		return EXIT_FAILURE;
	} else if (dump_entry) {
		if (!(argc == 3 || argc == 4)) {
			printf(SQFS_USAGE);
			return EXIT_FAILURE;
		}
	}

	fs_image = argv[optind];
	fd = open(fs_image, O_RDONLY);
	if (fd < 0) {
		printf("No such file or directory\n");
		return EXIT_FAILURE;
	}

	/* Memory mapping of SquashFS image */
	fstat(fd, &sb);
	file_mapping = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (file_mapping == MAP_FAILED) {
		fprintf(stderr, "Error: file could not be read\n");
		return -errno;
	}

	/* Command execution */
	if (dump_sb) {
		ret = sqfs_dump_sblk(file_mapping);
		if (ret) {
			errno = ret;
			munmap(file_mapping, sb.st_size);
			close(fd);
			return EXIT_FAILURE;
		}

		munmap(file_mapping, sb.st_size);
		close(fd);

		return EXIT_SUCCESS;
	}

	if (dump_inodes) {
		ret = sqfs_dump_inode_table(file_mapping);
		if (ret) {
			errno = ret;
			munmap(file_mapping, sb.st_size);
			close(fd);
			return EXIT_FAILURE;
		}

		munmap(file_mapping, sb.st_size);
		close(fd);

		return EXIT_SUCCESS;
	}

	if (dump_dir_table) {
		ret = sqfs_dump_directory_table(file_mapping);
		if (ret) {
			errno = ret;
			munmap(file_mapping, sb.st_size);
			close(fd);
			return EXIT_FAILURE;
		}

		munmap(file_mapping, sb.st_size);
		close(fd);

		return EXIT_SUCCESS;
	}

	if (dump_entry) {
		/* If no path is given, presume it is intended to be root */
		if (argc == 3)
			ret = sqfs_dump_entry(file_mapping, "/");
		else
			ret = sqfs_dump_entry(file_mapping, argv[3]);
		if (ret) {
			errno = ret;
			munmap(file_mapping, sb.st_size);
			close(fd);
			return EXIT_FAILURE;
		}

		munmap(file_mapping, sb.st_size);
		close(fd);

		return EXIT_SUCCESS;
	}

	/* Wrong command */
	printf(SQFS_USAGE);
	munmap(file_mapping, sb.st_size);
	close(fd);

	return EXIT_FAILURE;
}
