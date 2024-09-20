/* SPDX-License-Identifier: (GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause */
/* This header is BSD licensed so anyone can use the definitions to implement
 * compatible drivers/servers.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of IBM nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL IBM OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _LINUX_VIRTIO_DXGKRNL_H
#define _LINUX_VIRTIO_DXGKRNL_H

#include <linux/types.h>
#include <linux/virtio_types.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>

#define VIRTIO_ID_DXGKRNL 59 /* virtio dxgkrnl (experimental id) */

/* Status values for a virtio_dxgkrnl request. */
#define VIRTIO_DXGKRNL_S_OK 0
#define VIRTIO_DXGKRNL_S_IOERR 1
#define VIRTIO_DXGKRNL_S_UNSUPP 2

/* The feature bitmap for virtio dxgkrnl */
/* Async commands */
#define VIRTIO_DXGKRNL_F_ASYNC_COMMANDS 0

struct virtio_dxgkrnl_config {
	/* Number of dxgkrnl adapters */
	__u64 num_adapters;
};

struct virtio_dxgkrnl_enum_adapters_req {
	/* Number of adapters to enumerate.*/
	__u64 num_adapters;
	/* Offset into adapters to start enumeration. */
	__u64 adapter_offset;
};

struct virtio_dxgkrnl_enum_adapters_resp {
	/* Status of this request, one of VIRTIO_DXGKRNL_S_*. */
	__u8 status;
	__u8 padding[7];
	/* Array of luids for device to return. */
	__s64 vgpu_luids[0];
};

/* For the id field in virtio_pci_shm_cap */
#define VIRTIO_DXGKRNL_SHM_ID_IOSPACE 0

#endif /* _LINUX_VIRTIO_DXGKRNL_H */
