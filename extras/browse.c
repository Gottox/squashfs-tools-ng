#include "sqfs/compressor.h"
#include "sqfs/dir_reader.h"
#include "sqfs/id_table.h"
#include "sqfs/inode.h"
#include "sqfs/super.h"
#include "sqfs/error.h"
#include "sqfs/dir.h"
#include "sqfs/io.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>

#include <readline/readline.h>
#include <readline/history.h>

static sqfs_dir_reader_t *dr;
static sqfs_super_t super;
static sqfs_inode_generic_t *working_dir;
static sqfs_id_table_t *idtbl;

static void change_directory(const char *dirname)
{
	sqfs_inode_generic_t *inode;

	if (dirname == NULL || *dirname == '/') {
		free(working_dir);
		working_dir = NULL;

		sqfs_dir_reader_get_root_inode(dr, &working_dir);
	}

	if (dirname != NULL) {
		sqfs_dir_reader_find_by_path(dr, working_dir,
					     dirname, &inode);

		free(working_dir);
		working_dir = inode;
	}
}

static void list_directory(const char *dirname)
{
	sqfs_inode_generic_t *root, *inode;
	size_t i, max_len, len, col_count;
	sqfs_dir_entry_t *ent;
	int ret;

	if (dirname == NULL) {
		ret = sqfs_dir_reader_open_dir(dr, working_dir);
		if (ret)
			goto fail_open;
	} else if (*dirname == '/') {
		sqfs_dir_reader_get_root_inode(dr, &root);

		ret = sqfs_dir_reader_find_by_path(dr, root, dirname, &inode);
		free(root);
		if (ret)
			goto fail_resolve;

		ret = sqfs_dir_reader_open_dir(dr, inode);
		free(inode);
		if (ret)
			goto fail_open;
	} else {
		ret = sqfs_dir_reader_find_by_path(dr, working_dir,
						   dirname, &inode);
		if (ret)
			goto fail_resolve;

		ret = sqfs_dir_reader_open_dir(dr, inode);
		free(inode);
		if (ret)
			goto fail_open;
	}

	for (max_len = 0; ; max_len = len > max_len ? len : max_len) {
		ret = sqfs_dir_reader_read(dr, &ent);
		if (ret > 0)
			break;

		if (ret < 0) {
			fputs("Error while reading directory list\n", stderr);
			break;
		}

		len = ent->size + 1;
		free(ent);
	}

	sqfs_dir_reader_rewind(dr);

	col_count = 79 / (max_len + 1);
	col_count = col_count < 1 ? 1 : col_count;
	i = 0;

	for (;;) {
		ret = sqfs_dir_reader_read(dr, &ent);
		if (ret > 0)
			break;

		if (ret < 0) {
			fputs("Error while reading directory list\n", stderr);
			break;
		}

		switch (ent->type) {
		case SQFS_INODE_DIR:
			fputs("\033[01;34m", stdout);
			break;
		case SQFS_INODE_FILE:
			break;
		case SQFS_INODE_SLINK:
			fputs("\033[01;36m", stdout);
			break;
		case SQFS_INODE_BDEV:
			fputs("\033[22;33m", stdout);
			break;
		case SQFS_INODE_CDEV:
			fputs("\033[01;33m", stdout);
			break;
		case SQFS_INODE_FIFO:
		case SQFS_INODE_SOCKET:
			fputs("\033[01;35m", stdout);
			break;
		}

		len = ent->size + 1;

		printf("%.*s", ent->size + 1, ent->name);
		fputs("\033[0m", stdout);
		free(ent);

		++i;
		if (i == col_count) {
			i = 0;
			fputc('\n', stdout);
		} else {
			while (len++ < max_len)
				fputc(' ', stdout);
			fputc(' ', stdout);
		}
	}

	if (i != 0)
		fputc('\n', stdout);

	return;
fail_open:
	printf("Error opening '%s', error code %d\n", dirname, ret);
	return;
fail_resolve:
	printf("Error resolving '%s', error code %d\n", dirname, ret);
	return;
}

static void mode_to_str(sqfs_u16 mode, char *p)
{
	*(p++) = (mode & SQFS_INODE_OWNER_R) ? 'r' : '-';
	*(p++) = (mode & SQFS_INODE_OWNER_W) ? 'w' : '-';

	switch (mode & (SQFS_INODE_OWNER_X | SQFS_INODE_SET_UID)) {
	case SQFS_INODE_OWNER_X | SQFS_INODE_SET_UID: *(p++) = 's'; break;
	case SQFS_INODE_OWNER_X:                      *(p++) = 'x'; break;
	case SQFS_INODE_SET_UID:                      *(p++) = 'S'; break;
	default:                                      *(p++) = '-'; break;
	}

	*(p++) = (mode & SQFS_INODE_GROUP_R) ? 'r' : '-';
	*(p++) = (mode & SQFS_INODE_GROUP_W) ? 'w' : '-';

	switch (mode & (SQFS_INODE_GROUP_X | SQFS_INODE_SET_GID)) {
	case SQFS_INODE_GROUP_X | SQFS_INODE_SET_GID: *(p++) = 's'; break;
	case SQFS_INODE_GROUP_X:                      *(p++) = 'x'; break;
	case SQFS_INODE_SET_GID:                      *(p++) = 'S'; break;
	default:                                      *(p++) = '-'; break;
	}

	*(p++) = (mode & SQFS_INODE_OTHERS_R) ? 'r' : '-';
	*(p++) = (mode & SQFS_INODE_OTHERS_W) ? 'w' : '-';

	switch (mode & (SQFS_INODE_OTHERS_X | SQFS_INODE_STICKY)) {
	case SQFS_INODE_OTHERS_X | SQFS_INODE_STICKY: *(p++) = 't'; break;
	case SQFS_INODE_OTHERS_X:                     *(p++) = 'x'; break;
	case SQFS_INODE_STICKY:                       *(p++) = 'T'; break;
	default:                                      *(p++) = '-'; break;
	}

	*p = '\0';
}

static void stat_cmd(const char *filename)
{
	sqfs_inode_generic_t *inode, *root;
	sqfs_dir_index_t *idx;
	sqfs_u32 uid, gid;
	const char *type;
	char buffer[64];
	time_t timeval;
	struct tm *tm;
	size_t i;
	int ret;

	if (filename == NULL) {
		printf("Missing argument: file name\n");
		return;
	}

	if (*filename == '/') {
		sqfs_dir_reader_get_root_inode(dr, &root);
		ret = sqfs_dir_reader_find_by_path(dr, root, filename, &inode);
		free(root);
		if (ret)
			goto fail_resolve;
	} else {
		ret = sqfs_dir_reader_find_by_path(dr, working_dir,
						   filename, &inode);
		if (ret)
			goto fail_resolve;
	}

	printf("Stat: %s\n", filename);

	switch (inode->base.type) {
	case SQFS_INODE_DIR:        type = "directory";                 break;
	case SQFS_INODE_FILE:       type = "file";                      break;
	case SQFS_INODE_SLINK:      type = "symbolic link";             break;
	case SQFS_INODE_BDEV:       type = "block device";              break;
	case SQFS_INODE_CDEV:       type = "character device";          break;
	case SQFS_INODE_FIFO:       type = "named pipe";                break;
	case SQFS_INODE_SOCKET:     type = "socket";                    break;
	case SQFS_INODE_EXT_DIR:    type = "extended directory";        break;
	case SQFS_INODE_EXT_FILE:   type = "extended file";             break;
	case SQFS_INODE_EXT_SLINK:  type = "extended symbolic link";    break;
	case SQFS_INODE_EXT_BDEV:   type = "extended block device";     break;
	case SQFS_INODE_EXT_CDEV:   type = "extended character device"; break;
	case SQFS_INODE_EXT_FIFO:   type = "extended named pipe";       break;
	case SQFS_INODE_EXT_SOCKET: type = "extended socket";           break;
	default:                    type = "UNKNOWN";                   break;
	}

	printf("Type: %s\n", type);
	printf("Inode number: %u\n", inode->base.inode_number);

	mode_to_str(inode->base.mode & ~SQFS_INODE_MODE_MASK, buffer);
	printf("Access: 0%o/%s\n", inode->base.mode & ~SQFS_INODE_MODE_MASK,
	       buffer);

	if (sqfs_id_table_index_to_id(idtbl, inode->base.uid_idx, &uid)) {
		strcpy(buffer, "-- error --");
	} else {
		sprintf(buffer, "%u", uid);
	}

	printf("UID: %s (index = %u)\n", buffer, inode->base.uid_idx);

	if (sqfs_id_table_index_to_id(idtbl, inode->base.gid_idx, &gid)) {
		strcpy(buffer, "-- error --");
	} else {
		sprintf(buffer, "%u", gid);
	}

	printf("GID: %s (index = %u)\n", buffer, inode->base.gid_idx);

	timeval = inode->base.mod_time;
	tm = gmtime(&timeval);
	strftime(buffer, sizeof(buffer), "%a, %d %b %Y %T %z", tm);
	printf("Last modified: %s (%u)\n", buffer, inode->base.mod_time);

	switch (inode->base.type) {
	case SQFS_INODE_BDEV:
	case SQFS_INODE_CDEV:
		printf("Hard link count: %u\n", inode->data.dev.nlink);
		printf("Device number: %u\n", inode->data.dev.devno);
		break;
	case SQFS_INODE_EXT_BDEV:
	case SQFS_INODE_EXT_CDEV:
		printf("Hard link count: %u\n", inode->data.dev_ext.nlink);
		printf("Xattr index: 0x%X\n", inode->data.dev_ext.xattr_idx);
		printf("Device number: %u\n", inode->data.dev_ext.devno);
		break;
	case SQFS_INODE_FIFO:
	case SQFS_INODE_SOCKET:
		printf("Hard link count: %u\n", inode->data.ipc.nlink);
		break;
	case SQFS_INODE_EXT_FIFO:
	case SQFS_INODE_EXT_SOCKET:
		printf("Hard link count: %u\n", inode->data.ipc_ext.nlink);
		printf("Xattr index: 0x%X\n", inode->data.ipc_ext.xattr_idx);
		break;
	case SQFS_INODE_SLINK:
		printf("Hard link count: %u\n", inode->data.slink.nlink);
		printf("Link target: %.*s\n", inode->data.slink.target_size,
		       (const char *)inode->extra);
		break;
	case SQFS_INODE_EXT_SLINK:
		printf("Hard link count: %u\n", inode->data.slink_ext.nlink);
		printf("Xattr index: 0x%X\n", inode->data.slink_ext.xattr_idx);
		printf("Link target: %.*s\n", inode->data.slink_ext.target_size,
		       (const char *)inode->extra);
		break;
	case SQFS_INODE_FILE:
		printf("Blocks start: %u\n", inode->data.file.blocks_start);
		printf("Block count: %lu\n",
		       (unsigned long)inode->num_file_blocks);
		printf("Fragment index: 0x%X\n",
		       inode->data.file.fragment_index);
		printf("Fragment offset: %u\n",
		       inode->data.file.fragment_offset);
		printf("File size: %u\n", inode->data.file.file_size);

		for (i = 0; i < inode->num_file_blocks; ++i) {
			printf("\tBlock #%lu size: %u (%s)\n", (unsigned long)i,
			       inode->extra[i] & 0x00FFFFFF,
			       inode->extra[i] & (1 << 24) ?
			       "uncompressed" : "compressed");
		}
		break;
	case SQFS_INODE_EXT_FILE:
		printf("Blocks start: %lu\n",
		       inode->data.file_ext.blocks_start);
		printf("Block count: %lu\n",
		       (unsigned long)inode->num_file_blocks);
		printf("Fragment index: 0x%X\n",
		       inode->data.file_ext.fragment_idx);
		printf("Fragment offset: %u\n",
		       inode->data.file_ext.fragment_offset);
		printf("File size: %lu\n", inode->data.file_ext.file_size);
		printf("Sparse: %lu\n", inode->data.file_ext.sparse);
		printf("Hard link count: %u\n", inode->data.file_ext.nlink);
		printf("Xattr index: 0x%X\n", inode->data.file_ext.xattr_idx);

		for (i = 0; i < inode->num_file_blocks; ++i) {
			printf("\tBlock #%lu size: %u (%s)\n", (unsigned long)i,
			       inode->extra[i] & 0x00FFFFFF,
			       inode->extra[i] & (1 << 24) ?
			       "compressed" : "uncompressed");
		}
		break;
	case SQFS_INODE_DIR:
		printf("Start block: %u\n", inode->data.dir.start_block);
		printf("Offset: %u\n", inode->data.dir.offset);
		printf("Hard link count: %u\n", inode->data.dir.nlink);
		printf("Size: %u\n", inode->data.dir.size);
		printf("Parent inode: %u\n", inode->data.dir.parent_inode);
		break;
	case SQFS_INODE_EXT_DIR:
		printf("Start block: %u\n", inode->data.dir_ext.start_block);
		printf("Offset: %u\n", inode->data.dir_ext.offset);
		printf("Hard link count: %u\n", inode->data.dir_ext.nlink);
		printf("Size: %u\n", inode->data.dir_ext.size);
		printf("Parent inode: %u\n", inode->data.dir_ext.parent_inode);
		printf("Xattr index: 0x%X\n", inode->data.dir_ext.xattr_idx);
		printf("Directory index entries: %u\n",
		       inode->data.dir_ext.inodex_count);

		if (inode->data.dir_ext.size == 0)
			break;

		for (i = 0; ; ++i) {
			ret = sqfs_inode_unpack_dir_index_entry(inode, &idx, i);
			if (ret == SQFS_ERROR_OUT_OF_BOUNDS)
				break;
			if (ret < 0) {
				printf("Error reading index, error code %d\n",
				       ret);
				break;
			}

			printf("\tIndex: %u\n", idx->index);
			printf("\tStart block: %u\n", idx->start_block);
			printf("\tSize: %u\n", idx->size + 1);
			printf("\tEntry: %.*s\n\n", idx->size + 1, idx->name);

			free(idx);
		}
		break;
	}

	free(inode);
	return;
fail_resolve:
	printf("Error resolving '%s', error code %d\n", filename, ret);
	return;
}


static const struct {
	const char *cmd;
	void (*handler)(const char *arg);
} commands[] = {
	{ "ls", list_directory },
	{ "cd", change_directory },
	{ "stat", stat_cmd },
};

int main(int argc, char **argv)
{
	char *cmd, *arg, *buffer = NULL;
	sqfs_compressor_config_t cfg;
	sqfs_compressor_t *cmp;
	sqfs_file_t *file;
	int status = EXIT_FAILURE;
	size_t i;

	/* open the SquashFS file we want to read */
	if (argc != 2) {
		fputs("Usage: sqfsbrowse <squashfs-file>\n", stderr);
		return EXIT_FAILURE;
	}

	file = sqfs_open_file(argv[1], SQFS_FILE_OPEN_READ_ONLY);
	if (file == NULL) {
		perror(argv[1]);
		return EXIT_FAILURE;
	}

	/* read the super block, create a compressor and
	   process the compressor options */
	if (sqfs_super_read(&super, file)) {
		fprintf(stderr, "%s: error reading super block.\n", argv[1]);
		goto out_fd;
	}

	if (!sqfs_compressor_exists(super.compression_id)) {
		fprintf(stderr, "%s: unknown compressor used.\n", argv[1]);
		goto out_fd;
	}

	sqfs_compressor_config_init(&cfg, super.compression_id,
				    super.block_size,
				    SQFS_COMP_FLAG_UNCOMPRESS);

	cmp = sqfs_compressor_create(&cfg);
	if (cmp == NULL) {
		fprintf(stderr, "%s: error creating compressor.\n", argv[1]);
		goto out_fd;
	}

	if (super.flags & SQFS_FLAG_COMPRESSOR_OPTIONS) {
		if (cmp->read_options(cmp, file)) {
			fprintf(stderr,
				"%s: error reading compressor options.\n",
				argv[1]);
			goto out_cmp;
		}
	}

	/* Create and read the UID/GID mapping table */
	idtbl = sqfs_id_table_create();
	if (idtbl == NULL) {
		fputs("Error creating ID table.\n", stderr);
		goto out_cmp;
	}

	if (sqfs_id_table_read(idtbl, file, &super, cmp)) {
		fprintf(stderr, "%s: error loading ID table.\n", argv[1]);
		goto out_id;
	}

	/* create a directory reader and scan the entire directory hiearchy */
	dr = sqfs_dir_reader_create(&super, cmp, file);
	if (dr == NULL) {
		fprintf(stderr, "%s: error creating directory reader.\n",
			argv[1]);
		goto out_id;
	}

	if (sqfs_dir_reader_get_root_inode(dr, &working_dir)) {
		fprintf(stderr, "%s: error reading root inode.\n", argv[1]);
		goto out;
	}

	/* main readline loop */
	for (;;) {
		free(buffer);
		buffer = readline("$ ");

		if (buffer == NULL)
			goto out;

		for (cmd = buffer; isspace(*cmd); ++cmd)
			;

		if (*cmd == '\0')
			continue;

		add_history(cmd);

		for (arg = cmd; *arg != '\0' && !isspace(*arg); ++arg)
			;

		if (isspace(*arg)) {
			*(arg++) = '\0';
			while (isspace(*arg))
				++arg;
			if (*arg == '\0')
				arg = NULL;
		} else {
			arg = NULL;
		}

		for (i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
			if (strcmp(commands[i].cmd, cmd) == 0) {
				commands[i].handler(arg);
				break;
			}
		}
	}

	/* cleanup */
	status = EXIT_SUCCESS;
	free(buffer);
out:
	if (working_dir != NULL)
		free(working_dir);
	sqfs_dir_reader_destroy(dr);
out_id:
	sqfs_id_table_destroy(idtbl);
out_cmp:
	cmp->destroy(cmp);
out_fd:
	file->destroy(file);
	return status;
}
