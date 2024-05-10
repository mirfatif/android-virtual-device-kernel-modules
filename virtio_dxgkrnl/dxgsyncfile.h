/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2019, Microsoft Corporation.
 *
 * Author:
 *   Iouri Tarassov <iourit@linux.microsoft.com>
 *
 * Dxgkrnl Graphics Driver
 * Headers for sync file objects
 *
 */

#ifndef _DXGSYNCFILE_H
#define _DXGSYNCFILE_H

#include <linux/sync_file.h>

int dxgk_create_sync_file(struct dxgprocess *process, void *__user inargs);

struct dxgsyncobject {
#ifdef __KERNEL__
	struct d3dkmthandle	object;
	__u64			    fence_value;
#else
	__u64			object;
	__u64			fence_value;
#endif
};

struct dxgsyncpoint {
	struct d3dkmthandle device;
	struct dxghostevent	hdr;
	struct dma_fence	base;
	/* List of dxgsyncobject */
    struct list_head sync_object_list;
	u64			fence_value;
	u64			context;
	spinlock_t		lock;
	u64			u64;
};

#endif	 /* _DXGSYNCFILE_H */
