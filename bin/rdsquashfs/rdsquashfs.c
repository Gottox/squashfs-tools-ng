/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * rdsquashfs.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#include "rdsquashfs.h"

static sqfs_tree_node_t *list_merge(sqfs_tree_node_t *lhs,
				    sqfs_tree_node_t *rhs)
{
	sqfs_tree_node_t *it, *head = NULL, **next_ptr = &head;

	while (lhs != NULL && rhs != NULL) {
		if (strcmp((const char *)lhs->name,
			   (const char *)rhs->name) <= 0) {
			it = lhs;
			lhs = lhs->next;
		} else {
			it = rhs;
			rhs = rhs->next;
		}

		*next_ptr = it;
		next_ptr = &it->next;
	}

	it = (lhs != NULL ? lhs : rhs);
	*next_ptr = it;
	return head;
}

static sqfs_tree_node_t *list_sort(sqfs_tree_node_t *head)
{
	sqfs_tree_node_t *it, *half, *prev;

	it = half = prev = head;

	while (it != NULL) {
		prev = half;
		half = half->next;
		it = it->next;

		if (it != NULL)
			it = it->next;
	}

	if (half == NULL)
		return head;

	prev->next = NULL;

	return list_merge(list_sort(head), list_sort(half));
}

static int tree_sort(sqfs_tree_node_t *root)
{
	sqfs_tree_node_t *it;

	if (root->children == NULL)
		return 0;

	root->children = list_sort(root->children);

	/*
	  XXX: not only an inconvenience but a security issue: e.g. we unpack a
	  SquashFS image that has a symlink pointing somewhere, and then a
	  sub-directory or file with the same name, the unpacker can be tricked
	  to follow the symlink and write anything, anywhere on the filesystem.
	 */
	for (it = root->children; it->next != NULL; it = it->next) {
		if (strcmp((const char *)it->name,
			   (const char *)it->next->name) == 0) {
			char *path = sqfs_tree_node_get_path(it);

			fprintf(stderr, "Entry '%s' found more than once!\n",
				path);

			sqfs_free(path);
			return -1;
		}
	}

	for (it = root->children; it != NULL; it = it->next) {
		if (tree_sort(it))
			return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	sqfs_xattr_reader_t *xattr = NULL;
	sqfs_compressor_config_t cfg;
	int status = EXIT_FAILURE;
	sqfs_data_reader_t *data;
	sqfs_dir_reader_t *dirrd;
	sqfs_compressor_t *cmp;
	sqfs_id_table_t *idtbl;
	sqfs_tree_node_t *n;
	sqfs_super_t super;
	sqfs_file_t *file;
	options_t opt;
	int ret;

	process_command_line(&opt, argc, argv);

	file = sqfs_open_file(opt.image_name, SQFS_FILE_OPEN_READ_ONLY);
	if (file == NULL) {
		perror(opt.image_name);
		goto out_cmd;
	}

	ret = sqfs_super_read(&super, file);
	if (ret) {
		sqfs_perror(opt.image_name, "reading super block", ret);
		goto out_file;
	}

	sqfs_compressor_config_init(&cfg, super.compression_id,
				    super.block_size,
				    SQFS_COMP_FLAG_UNCOMPRESS);

	ret = sqfs_compressor_create(&cfg, &cmp);

#ifdef WITH_LZO
	if (super.compression_id == SQFS_COMP_LZO && ret != 0)
		ret = lzo_compressor_create(&cfg, &cmp);
#endif

	if (ret != 0) {
		sqfs_perror(opt.image_name, "creating compressor", ret);
		goto out_file;
	}

	if (!(super.flags & SQFS_FLAG_NO_XATTRS)) {
		xattr = sqfs_xattr_reader_create(0);
		if (xattr == NULL) {
			sqfs_perror(opt.image_name, "creating xattr reader",
				    SQFS_ERROR_ALLOC);
			goto out_cmp;
		}

		ret = sqfs_xattr_reader_load(xattr, &super, file, cmp);
		if (ret) {
			sqfs_perror(opt.image_name, "loading xattr table",
				    ret);
			goto out_xr;
		}
	}

	idtbl = sqfs_id_table_create(0);
	if (idtbl == NULL) {
		sqfs_perror(opt.image_name, "creating ID table",
			    SQFS_ERROR_ALLOC);
		goto out_xr;
	}

	ret = sqfs_id_table_read(idtbl, file, &super, cmp);
	if (ret) {
		sqfs_perror(opt.image_name, "loading ID table", ret);
		goto out_id;
	}

	dirrd = sqfs_dir_reader_create(&super, cmp, file, 0);
	if (dirrd == NULL) {
		sqfs_perror(opt.image_name, "creating dir reader",
			    SQFS_ERROR_ALLOC);
		goto out_id;
	}

	data = sqfs_data_reader_create(file, super.block_size, cmp, 0);
	if (data == NULL) {
		sqfs_perror(opt.image_name, "creating data reader",
			    SQFS_ERROR_ALLOC);
		goto out_dr;
	}

	ret = sqfs_data_reader_load_fragment_table(data, &super);
	if (ret) {
		sqfs_perror(opt.image_name, "loading fragment table", ret);
		goto out_data;
	}

	ret = sqfs_dir_reader_get_full_hierarchy(dirrd, idtbl, opt.cmdpath,
						 opt.rdtree_flags, &n);
	if (ret) {
		sqfs_perror(opt.image_name, "reading filesystem tree", ret);
		goto out_data;
	}

	switch (opt.op) {
	case OP_LS:
		list_files(n);
		break;
	case OP_STAT:
		if (stat_file(n))
			goto out;
		break;
	case OP_CAT: {
		ostream_t *fp;

		if (!S_ISREG(n->inode->base.mode)) {
			fprintf(stderr, "/%s: not a regular file\n",
				opt.cmdpath);
			goto out;
		}

		fp = ostream_open_stdout();
		if (fp == NULL)
			goto out;

		if (sqfs_data_reader_dump(opt.cmdpath, data, n->inode,
					  fp, super.block_size)) {
			sqfs_destroy(fp);
			goto out;
		}

		sqfs_destroy(fp);
		break;
	}
	case OP_UNPACK:
		if (tree_sort(n))
			goto out;

		if (opt.unpack_root != NULL) {
			if (mkdir_p(opt.unpack_root))
				goto out;

			if (chdir(opt.unpack_root)) {
				perror(opt.unpack_root);
				goto out;
			}
		}

		if (restore_fstree(n, opt.flags))
			goto out;

		if (fill_unpacked_files(super.block_size, n, data, opt.flags))
			goto out;

		if (update_tree_attribs(xattr, n, opt.flags))
			goto out;
		break;
	case OP_DESCRIBE:
		if (describe_tree(n, opt.unpack_root))
			goto out;
		break;
	case OP_RDATTR:
		if (dump_xattrs(xattr, n->inode))
			goto out;
		break;
	default:
		break;
	}

	status = EXIT_SUCCESS;
out:
	sqfs_dir_tree_destroy(n);
out_data:
	sqfs_destroy(data);
out_dr:
	sqfs_destroy(dirrd);
out_id:
	sqfs_destroy(idtbl);
out_xr:
	if (xattr != NULL)
		sqfs_destroy(xattr);
out_cmp:
	sqfs_destroy(cmp);
out_file:
	sqfs_destroy(file);
out_cmd:
	free(opt.cmdpath);
	return status;
}
