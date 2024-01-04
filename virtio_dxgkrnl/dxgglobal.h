/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2019, Microsoft Corporation.
 *
 * Author:
 *   Iouri Tarassov <iourit@linux.microsoft.com>
 *
 * Dxgkrnl Graphics Driver
 * Interface with Linux kernel and the VM bus driver
 *
 */

extern const struct file_operations dxgk_fops;

struct vmbus_channel *dxgglobal_get_vmbus(void);
struct dxgvmbuschannel *dxgglobal_get_dxgvmbuschannel(void);
int dxgglobal_acquire_channel_lock(void);
int dxgglobal_create_adapter(struct pci_dev *dev, struct winluid guid, struct winluid host_vgpu_luid);
void dxgglobal_start_adapters(void);
void dxgglobal_stop_adapters(void);
void set_guest_data(struct dxgkvmb_command_host_to_vm *packet,
		    u32 packet_length);
void signal_guest_event(struct dxgkvmb_command_host_to_vm *packet,
			u32 packet_length);
