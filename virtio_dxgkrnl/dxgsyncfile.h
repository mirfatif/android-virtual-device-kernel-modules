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
	/* dma_fence_cb struct is initialized in dma_fence_add_callback(),
	 * additional data can be passed along by embedding dma_fence_cb in another struct.
	 * Embed dma_fence_cb in dxg_sync_cb_t.
	 */
	struct dma_fence_cb  cb;
	struct d3dkmthandle  device_handle;
	int process_tgid;
	__u32                object_count;
	struct d3dkmthandle  *objects;
	__u64	             *fence_values;
};

struct dxgsyncpoint {
	struct dxghostevent	hdr;
	struct dma_fence	base;
	u64			fence_value;
	u64			context;
	/* The irqsafe spinlock to use for locking base. */
	spinlock_t		lock;
	u64			u64;
};

#endif	 /* _DXGSYNCFILE_H */
