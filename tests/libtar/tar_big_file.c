/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tar_big_file.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#include "config.h"
#include "tar.h"
#include "../test.h"

int main(int argc, char **argv)
{
	tar_header_decoded_t hdr;
	istream_t *fp;
	(void)argc; (void)argv;

	fp = istream_open_file(STRVALUE(TESTPATH) "/" STRVALUE(TESTFILE));
	TEST_NOT_NULL(fp);
	TEST_ASSERT(read_header(fp, &hdr) == 0);
	TEST_EQUAL_UI(hdr.mode, S_IFREG | 0644);
	TEST_EQUAL_UI(hdr.uid, 01750);
	TEST_EQUAL_UI(hdr.gid, 01750);
	TEST_EQUAL_UI(hdr.actual_size, 8589934592);
	TEST_EQUAL_UI(hdr.mtime, 1542959190);
	TEST_STR_EQUAL(hdr.name, "big-file.bin");
	TEST_ASSERT(!hdr.unknown_record);
	clear_header(&hdr);
	sqfs_destroy(fp);
	return EXIT_SUCCESS;
}
