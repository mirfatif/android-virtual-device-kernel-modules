/* Spdx-License-Identifier: GPL-2.0 */
#include <linux/errno.h>
#include <linux/fuse_vendor.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/types.h>
#include <trace/hooks/tmpfile.h>

#include "fuse_i.h"

static void fuse_vendor_tmpfile_secctx(void *data, struct fuse_args *args,
				       u32 security_ctxlen, void *security_ctx,
				       bool *skip_ctxargset)
{
	if (args->opcode != FUSE_NONATOMIC_TMPFILE)
                return;

	BUG_ON(args->in_numargs >= ARRAY_SIZE(args->in_args));

	args->in_args[args->in_numargs].size = security_ctxlen;
	args->in_args[args->in_numargs].value = security_ctx;
	args->in_numargs++;
	*skip_ctxargset = true;
}

static void fuse_vendor_tmpfile_check_inode(void *data, struct fuse_args *args,
					    struct inode *inode, int *err)
{
        *err = (args->opcode == FUSE_NONATOMIC_TMPFILE && inode->i_nlink != 0) ?
	       -EIO : *err;

}

static void fuse_vendor_tmpfile_create(void *data, struct fuse_args *args,
				       struct dentry **d, struct dentry *entry,
				       struct inode *inode, bool *skip_splice)
{
        if (args->opcode != FUSE_NONATOMIC_TMPFILE)
		return;

	/*
	 * d_tmpfile will decrement the link count and print a warning
	 * if the link count is 0, and we checked that the server sent
	 * us an inode with an nlink count of 0 above.  Set the nlink
	 * count to 1 to suppress the warning. btrfs does the same
	 * thing.
	 */
	set_nlink(inode, 1);
	d_tmpfile(entry, inode);
	d = NULL;
	*skip_splice = true;
}

static void fuse_vendor_tmpfile_handle_op(void *data, struct inode *dir,
					  struct dentry *entry, umode_t mode,
					  int (*f)(struct fuse_mount *,
						   struct fuse_args *,
						   struct inode*,
						   struct dentry *, umode_t),
					  int *ret)
{
	struct fuse_tmpfile_in inarg;
	struct fuse_mount *fm = get_fuse_mount(dir);
	FUSE_ARGS(args);

	if (!fm->fc->dont_mask)
		mode &= ~current_umask();

	memset(&inarg, 0, sizeof(inarg));
	inarg.mode = mode;
	inarg.umask = current_umask();
	args.opcode = FUSE_NONATOMIC_TMPFILE;
	args.in_numargs = 1;
	args.in_args[0].size = sizeof(inarg);
	args.in_args[0].value = &inarg;

	*ret = f(fm, &args, dir, entry, S_IFREG);
}

static void fuse_vendor_tmpfile_send_open(void *data, uint32_t *flags)
{
	*flags &= ~O_TMPFILE;
}

int fuse_vendor_init(void) {
	int ret;

	ret = register_trace_android_vh_tmpfile_secctx(
		fuse_vendor_tmpfile_secctx, NULL) ?:
	      register_trace_android_vh_tmpfile_create_check_inode(
		fuse_vendor_tmpfile_check_inode, NULL) ?:
	      register_trace_android_rvh_tmpfile_create(
		fuse_vendor_tmpfile_create, NULL) ?:
	      register_trace_android_rvh_tmpfile_handle_op(
		fuse_vendor_tmpfile_handle_op, NULL) ?:
	      register_trace_android_vh_tmpfile_send_open(
		fuse_vendor_tmpfile_send_open, NULL);
        return ret;
}

module_init(fuse_vendor_init);
MODULE_DESCRIPTION("ARCVM FUSE vendor driver");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
MODULE_LICENSE("GPL v2");
