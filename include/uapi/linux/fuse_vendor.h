/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FUSE_VENDOR_H
#define _LINUX_FUSE_VENDOR_H

#include <linux/types.h>

enum fuse_vendor_opcodes {
	/*
	 * Opcodes 1-52 and 2016 are reserved as of FUSE 7.39
	 */
	FUSE_NONATOMIC_TMPFILE    = 4294967295,           /* UINT_MAX */
};

struct fuse_tmpfile_in {
	uint32_t mode;
	uint32_t umask;
};

#endif /* _LINUX_FUSE_VENDOR_H */
