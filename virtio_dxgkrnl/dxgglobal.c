// SPDX-License-Identifier: GPL-2.0

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

#include <linux/eventfd.h>
#include <linux/sync_file.h>

#include "dxgkrnl.h"
#include "dxgsyncfile.h"
#include "dxgvmbus.h"

struct dxgglobal *dxgglobal;
struct device *dxgglobaldev;

#undef pr_fmt
#define pr_fmt(fmt)	"dxgk:err: " fmt
#undef dev_fmt
#define dev_fmt(fmt)	"dxgk: " fmt

//
// Interface from dxgglobal
//

struct vmbus_channel *dxgglobal_get_vmbus(void)
{
	return dxgglobal->channel.channel;
}

struct dxgvmbuschannel *dxgglobal_get_dxgvmbuschannel(void)
{
	return &dxgglobal->channel;
}

int dxgglobal_acquire_channel_lock(void)
{
	down_read(&dxgglobal->channel_lock);
	if ((dxgglobal->channel.channel == NULL) && (dxgglobal->vdxgkrnl == NULL)) {
		pr_err("Failed to acquire global channel lock");
		return -ENODEV;
	} else {
		return 0;
	}
}

void dxgglobal_release_channel_lock(void)
{
	up_read(&dxgglobal->channel_lock);
}

void dxgglobal_acquire_adapter_list_lock(enum dxglockstate state)
{
	if (state == DXGLOCK_EXCL)
		down_write(&dxgglobal->adapter_list_lock);
	else
		down_read(&dxgglobal->adapter_list_lock);
}

void dxgglobal_release_adapter_list_lock(enum dxglockstate state)
{
	if (state == DXGLOCK_EXCL)
		up_write(&dxgglobal->adapter_list_lock);
	else
		up_read(&dxgglobal->adapter_list_lock);
}

struct dxgadapter *find_pci_adapter(struct pci_dev *dev)
{
	struct dxgadapter *entry;
	struct dxgadapter *adapter = NULL;

	dxgglobal_acquire_adapter_list_lock(DXGLOCK_EXCL);

	list_for_each_entry(entry, &dxgglobal->adapter_list_head,
			    adapter_list_entry) {
		if (dev == entry->pci_dev) {
			adapter = entry;
			break;
		}
	}

	dxgglobal_release_adapter_list_lock(DXGLOCK_EXCL);
	return adapter;
}

void dxgglobal_add_host_event(struct dxghostevent *event)
{
	spin_lock_irq(&dxgglobal->host_event_list_mutex);
	list_add_tail(&event->host_event_list_entry,
		      &dxgglobal->host_event_list_head);
	spin_unlock_irq(&dxgglobal->host_event_list_mutex);
}

void dxgglobal_remove_host_event(struct dxghostevent *event)
{
	spin_lock_irq(&dxgglobal->host_event_list_mutex);
	if (event->host_event_list_entry.next != NULL) {
		list_del(&event->host_event_list_entry);
		event->host_event_list_entry.next = NULL;
	}
	spin_unlock_irq(&dxgglobal->host_event_list_mutex);
}

void signal_host_cpu_event(struct dxghostevent *eventhdr)
{
	struct dxghosteventcpu *event = (struct dxghosteventcpu *)eventhdr;

	if (event->remove_from_list ||
		event->destroy_after_signal) {
		list_del(&eventhdr->host_event_list_entry);
		eventhdr->host_event_list_entry.next = NULL;
	}
	if (event->cpu_event) {
		dev_dbg(dxgglobaldev, "signal cpu event\n");
		eventfd_signal(event->cpu_event, 1);
		if (event->destroy_after_signal)
			eventfd_ctx_put(event->cpu_event);
	} else {
		dev_dbg(dxgglobaldev, "signal completion\n");
		complete(event->completion_event);
	}
	if (event->destroy_after_signal) {
		dev_dbg(dxgglobaldev, "destroying event %p\n",
			event);
		vfree(event);
	}
}

void signal_dma_fence(struct dxghostevent *eventhdr)
{
	struct dxgsyncpoint *event = (struct dxgsyncpoint *)eventhdr;

	event->fence_value++;
	list_del(&eventhdr->host_event_list_entry);
	dma_fence_signal(&event->base);
}

void set_guest_data(struct dxgkvmb_command_host_to_vm *packet,
		    u32 packet_length)
{
	struct dxgkvmb_command_setguestdata *command = (void *)packet;

	dev_dbg(dxgglobaldev, "%s: %d %d %p %p", __func__,
		command->data_type,
		command->data32,
		command->guest_pointer,
		&dxgglobal->device_state_counter);
	if (command->data_type == SETGUESTDATA_DATATYPE_DWORD &&
	    command->guest_pointer == &dxgglobal->device_state_counter &&
	    command->data32 != 0) {
		atomic_inc(&dxgglobal->device_state_counter);
	}
}

void signal_guest_event(struct dxgkvmb_command_host_to_vm *packet,
			u32 packet_length)
{
	struct dxgkvmb_command_signalguestevent *command = (void *)packet;

	if (packet_length < sizeof(struct dxgkvmb_command_signalguestevent)) {
		pr_err("invalid packet size");
		return;
	}
	if (command->event == 0) {
		pr_err("invalid event pointer");
		return;
	}
	dxgglobal_signal_host_event(command->event);
}

void dxgglobal_signal_host_event(u64 event_id)
{
	struct dxghostevent *event;
	unsigned long flags;

	dev_dbg(dxgglobaldev, "%s %lld\n", __func__, event_id);

	spin_lock_irqsave(&dxgglobal->host_event_list_mutex, flags);
	list_for_each_entry(event, &dxgglobal->host_event_list_head,
			    host_event_list_entry) {
		if (event->event_id == event_id) {
			dev_dbg(dxgglobaldev, "found event to signal %lld\n",
				    event_id);
			if (event->event_type == dxghostevent_cpu_event)
				signal_host_cpu_event(event);
			else if (event->event_type == dxghostevent_dma_fence)
				signal_dma_fence(event);
			else
				pr_err("Unknown host event type");
			break;
		}
	}
	spin_unlock_irqrestore(&dxgglobal->host_event_list_mutex, flags);
	dev_dbg(dxgglobaldev, "dxgglobal_signal_host_event_end %lld\n",
		event_id);
}

struct dxghostevent *dxgglobal_get_host_event(u64 event_id)
{
	struct dxghostevent *entry;
	struct dxghostevent *event = NULL;

	spin_lock_irq(&dxgglobal->host_event_list_mutex);
	list_for_each_entry(entry, &dxgglobal->host_event_list_head,
			    host_event_list_entry) {
		if (entry->event_id == event_id) {
			list_del(&entry->host_event_list_entry);
			entry->host_event_list_entry.next = NULL;
			event = entry;
			break;
		}
	}
	spin_unlock_irq(&dxgglobal->host_event_list_mutex);
	return event;
}

u64 dxgglobal_new_host_event_id(void)
{
	return atomic64_inc_return(&dxgglobal->host_event_id);
}

void dxgglobal_acquire_process_adapter_lock(void)
{
	mutex_lock(&dxgglobal->process_adapter_mutex);
}

void dxgglobal_release_process_adapter_lock(void)
{
	mutex_unlock(&dxgglobal->process_adapter_mutex);
}

int dxgglobal_create_adapter(struct pci_dev *dev, struct winluid guid,
			     struct winluid host_vgpu_luid)
{
	struct dxgadapter *adapter;
	int ret = 0;

	adapter = vzalloc(sizeof(struct dxgadapter));
	if (adapter == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}

	adapter->adapter_state = DXGADAPTER_STATE_WAITING_VMBUS;
	adapter->host_vgpu_luid = host_vgpu_luid;
	kref_init(&adapter->adapter_kref);
	init_rwsem(&adapter->core_lock);

	INIT_LIST_HEAD(&adapter->adapter_process_list_head);
	INIT_LIST_HEAD(&adapter->shared_resource_list_head);
	INIT_LIST_HEAD(&adapter->adapter_shared_syncobj_list_head);
	INIT_LIST_HEAD(&adapter->syncobj_list_head);
	init_rwsem(&adapter->shared_resource_list_lock);
	adapter->pci_dev = dev;
	adapter->luid = guid;

	dxgglobal_acquire_adapter_list_lock(DXGLOCK_EXCL);

	list_add_tail(&adapter->adapter_list_entry,
		      &dxgglobal->adapter_list_head);
	dxgglobal->num_adapters++;
	dxgglobal_release_adapter_list_lock(DXGLOCK_EXCL);

	dev_dbg(dxgglobaldev, "new adapter added %p %x-%x\n", adapter,
		    adapter->luid.a, adapter->luid.b);
cleanup:
	dev_dbg(dxgglobaldev, "%s end: %d", __func__, ret);
	return ret;
}

void dxgglobal_start_adapters(void)
{
	struct dxgadapter *adapter;

	if ((dxgglobal->hdev == NULL) && (dxgglobal->vdxgkrnl == NULL)) {
		dev_dbg(dxgglobaldev, "Global channel is not ready");
		return;
	}
	dxgglobal_acquire_adapter_list_lock(DXGLOCK_EXCL);
	list_for_each_entry(adapter, &dxgglobal->adapter_list_head,
			    adapter_list_entry) {
		if (adapter->adapter_state == DXGADAPTER_STATE_WAITING_VMBUS)
			dxgadapter_start(adapter);
	}
	dxgglobal_release_adapter_list_lock(DXGLOCK_EXCL);
}

void dxgglobal_stop_adapters(void)
{
	struct dxgadapter *adapter;

	if (dxgglobal->hdev == NULL) {
		dev_dbg(dxgglobaldev, "Global channel is not ready");
		return;
	}
	dxgglobal_acquire_adapter_list_lock(DXGLOCK_EXCL);
	list_for_each_entry(adapter, &dxgglobal->adapter_list_head,
			    adapter_list_entry) {
		if (adapter->adapter_state == DXGADAPTER_STATE_ACTIVE)
			dxgadapter_stop(adapter);
	}
	dxgglobal_release_adapter_list_lock(DXGLOCK_EXCL);
}

/*
 * File operations
 */

static struct dxgprocess *dxgglobal_get_current_process(void)
{
	/*
	 * Find the DXG process for the current process.
	 * A new process is created if necessary.
	 */
	struct dxgprocess *process = NULL;
	struct dxgprocess *entry = NULL;

	mutex_lock(&dxgglobal->plistmutex);
	list_for_each_entry(entry, &dxgglobal->plisthead, plistentry) {
		/* All threads of a process have the same thread group ID */
		if (entry->process->tgid == current->tgid) {
			if (kref_get_unless_zero(&entry->process_kref)) {
				process = entry;
				dev_dbg(dxgglobaldev, "found dxgprocess");
			} else {
				dev_dbg(dxgglobaldev, "process is destroyed");
			}
			break;
		}
	}
	mutex_unlock(&dxgglobal->plistmutex);

	if (process == NULL)
		process = dxgprocess_create();

	return process;
}

static int dxgk_open(struct inode *n, struct file *f)
{
	int ret = 0;
	struct dxgprocess *process;

	dev_dbg(dxgglobaldev, "%s %p %d %d",
		     __func__, f, current->pid, current->tgid);


	/* Find/create a dxgprocess structure for this process */
	process = dxgglobal_get_current_process();

	if (process) {
		f->private_data = process;
	} else {
		dev_dbg(dxgglobaldev, "cannot create dxgprocess for open\n");
		ret = -EBADF;
	}

	dev_dbg(dxgglobaldev, "%s end %x", __func__, ret);
	return ret;
}

static int dxgk_release(struct inode *n, struct file *f)
{
	struct dxgprocess *process;

	process = (struct dxgprocess *)f->private_data;
	dev_dbg(dxgglobaldev, "%s %p, %p", __func__, f, process);

	if (process == NULL)
		return -EINVAL;

	kref_put(&process->process_kref, dxgprocess_release);

	f->private_data = NULL;
	return 0;
}

static ssize_t dxgk_read(struct file *f, char __user *s, size_t len,
			 loff_t *o)
{
	dev_dbg(dxgglobaldev, "file read\n");
	return 0;
}

static ssize_t dxgk_write(struct file *f, const char __user *s, size_t len,
			  loff_t *o)
{
	dev_dbg(dxgglobaldev, "file write\n");
	return len;
}

const struct file_operations dxgk_fops = {
	.owner = THIS_MODULE,
	.open = dxgk_open,
	.release = dxgk_release,
	.compat_ioctl = dxgk_compat_ioctl,
	.unlocked_ioctl = dxgk_unlocked_ioctl,
	.write = dxgk_write,
	.read = dxgk_read,
};
