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

/*
 * The object is used to pass additional data required for a callback
 * for a sync file.
 */
struct dxg_sync_cb_t {
	/* callback for dma_fence_add_callback() */
	struct dma_fence_cb cb;
	struct d3dkmthandle device;
#ifdef __KERNEL__
	struct d3dkmthandle	*objects;
	__u64			    *fence_values;
#else
	__u64			objects;
	__u64			fence_values;
#endif
    /* list_head to be used to add dxg_sync_cb_t to a list of callbacks.*/
	struct list_head list;
};

struct dxgsyncpoint {
	struct dxghostevent	hdr;
	struct dma_fence	base;
    /* List of dxg_sync_cb_t */
	struct list_head sync_cb_list;
	u64			fence_value;
	u64			context;
	spinlock_t		lock;
	u64			u64;
};

#endif	 /* _DXGSYNCFILE_H */
