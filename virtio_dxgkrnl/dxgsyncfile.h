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
#include <linux/mutex.h>

int dxgk_create_sync_file(struct dxgprocess *process, void *__user inargs);

/*
 * The object is used to pass additional data required for a callback
 * for a sync file.
 */
struct dxg_sync_cb_t {
	/* callback for dma_fence_add_callback() */
	struct dma_fence_cb  cb;
	struct d3dkmthandle  device;
	struct dxgprocess  *process;
	struct dxgadapter  *adapter;
	__u32			object_count;
#ifdef __KERNEL__
	struct d3dkmthandle  *objects;
	__u64	             *fence_values;
#else
	__u64                objects;
	__u64                fence_values;
#endif
    /* node in the list of callbacks.*/
	struct list_head    node;
};

struct dxgsyncpoint {
	struct dxghostevent	hdr;
	struct dma_fence	base;
    /* List of dxg_sync_cb_t */
	struct list_head sync_cb_list;
	/* lock around sync_cb_list */
	struct mutex sync_cb_mutex;
	u64			fence_value;
	u64			context;
	/* The irqsafe spinlock to use for locking base. */
	spinlock_t		lock;
	u64			u64;
};

#endif	 /* _DXGSYNCFILE_H */
