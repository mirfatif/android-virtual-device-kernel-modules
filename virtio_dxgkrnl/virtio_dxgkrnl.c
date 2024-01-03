// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Virtio dxgkrnl implementation.
 *
 *  Copyright (C) 2021 Google, Inc.
 */

#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_types.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>
#include <linux/atomic.h>

#include "dxgkrnl.h"
#include "dxgvmbus.h"
#include "dxgglobal.h"
#include "virtio_dxgkrnl.h"

enum virtio_dxgkrnl_vq {
	VIRTIO_DXGKRNL_VQ_SETUP,
	VIRTIO_DXGKRNL_VQ_COMMAND,
	VIRTIO_DXGKRNL_VQ_EVENT,
	VIRTIO_DXGKRNL_VQ_MAX
};

struct virtio_dxgkrnl_command {
	struct list_head command_list_entry;
	enum dxgkvmb_commandtype command_type;
	struct d3dkmthandle process;
	bool async;
	struct completion *completion;
	void *command;
	u32 cmd_size;
	void *result;
	u32 result_size;
	refcount_t ref_count;
	int seqno;
};

struct virtio_dxgkrnl_event_buffer {
	union {
		struct dxgkvmb_command_signalguestevent signalguestevent;
		struct dxgkvmb_command_setguestdata setguestdata;
	};
};

#define VIRTIO_DXGKRNL_NUM_EVENT_BUFFERS 64

struct virtio_dxgkrnl {
	struct virtio_device *vdev;
	struct virtqueue *setup_vq;
	struct virtqueue *command_vq;
	spinlock_t command_qlock;
	struct virtqueue *event_vq;

	struct virtio_shm_region iospace_region;

	/* list of commands that are being processed on the host */
	struct list_head command_list_head;
	spinlock_t command_list_mutex;

	/* work queues */
	struct work_struct event_work;
	struct work_struct command_result_work;

	/* event buffers, there's a fixed number */
	struct virtio_dxgkrnl_event_buffer
		event_buffers[VIRTIO_DXGKRNL_NUM_EVENT_BUFFERS];
};

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_DXGKRNL, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

void set_cmd_type(struct dxgvmbuschannel *channel,
		  struct virtio_dxgkrnl_command *ctx)
{
	struct dxgkvmb_command_vgpu_to_host *cmd2;
	struct dxgkvmb_command_vm_to_host *cmd1;
	struct dxgvmb_ext_header *hdr;
	char *adapter_or_global;
	char *sync_mode;

	hdr = ctx->command;

	if (ctx->async)
		sync_mode = "async";
	else
		sync_mode = "sync";

	if (channel->adapter == NULL) {
		cmd1 = (struct dxgkvmb_command_vm_to_host *)&hdr[1];
		ctx->command_type =
			(enum dxgkvmb_commandtype)cmd1->command_type;
		ctx->process = cmd1->process;
		adapter_or_global = "global";
	} else {
		cmd2 = (struct dxgkvmb_command_vgpu_to_host *)&hdr[1];
		ctx->command_type = cmd2->command_type;
		ctx->process = cmd2->process;
		adapter_or_global = "adapter";
	}

	dev_dbg(dxgglobaldev, "send_%s_msg %s: %d %p %d", sync_mode,
		adapter_or_global, cmd1->command_type, ctx->command,
		ctx->cmd_size);
}

static struct virtio_dxgkrnl_command *
virtio_dxgkrnl_command_create(struct virtio_dxgkrnl *vp,
			      struct dxgvmbuschannel *channel, u32 cmd_size,
			      u32 result_size, bool async)
{
	struct virtio_dxgkrnl_command *cmd;
	static atomic_t cmd_count = ATOMIC_INIT(0);

	cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	if (!cmd) {
		dev_err(&vp->vdev->dev, "%s: failed allocate command context\n",
			__func__);
		return NULL;
	}
	cmd->seqno = atomic_inc_return(&cmd_count);
	cmd->async = async;
	cmd->cmd_size = cmd_size;
	cmd->result_size = result_size;

	cmd->command = kzalloc(cmd_size, GFP_KERNEL);
	if (!cmd->command) {
		dev_err(&vp->vdev->dev, "%s: failed allocate command buffer\n",
			__func__);
		kfree(cmd);
		return NULL;
	}

	if (result_size != 0) {
		cmd->result = kzalloc(result_size, GFP_KERNEL);
		if (!cmd->result) {
			dev_err(&vp->vdev->dev,
				"%s: failed allocate result buffer\n",
				__func__);
			kfree(cmd);
			return NULL;
		}
	} else {
		cmd->result = NULL;
	}

	INIT_LIST_HEAD(&cmd->command_list_entry);

	/* This reference is dropped in dxgkrnl_command_result_work(). */
	refcount_set(&cmd->ref_count, 1);

	set_cmd_type(channel, cmd);

	return cmd;
}

void virtio_dxgkrnl_cmd_ref(struct virtio_dxgkrnl_command *cmd)
{
	refcount_inc(&cmd->ref_count);
}

void virtio_dxgkrnl_cmd_unref(struct virtio_dxgkrnl_command *cmd)
{
	if (refcount_dec_and_test(&cmd->ref_count)) {
		kfree(cmd->command);
		kfree(cmd->result);
		kfree(cmd);
	}
}

static void dxgkrnl_event_callback(struct virtqueue *vq)
{
	struct virtio_dxgkrnl *vp;

	vp = vq->vdev->priv;

	queue_work(system_freezable_wq, &vp->event_work);
};

static void dxgkrnl_event_work(struct work_struct *work)
{
	struct virtio_dxgkrnl_event_buffer *eb;
	struct dxgkvmb_command_host_to_vm *hdr;
	struct virtio_dxgkrnl *vp;
	struct scatterlist sg;
	unsigned int len;
	bool should_kick;

	vp = container_of(work, struct virtio_dxgkrnl, event_work);

	should_kick = false;

	while ((eb = virtqueue_get_buf(vp->event_vq, &len)) != NULL) {
		hdr = (struct dxgkvmb_command_host_to_vm *)eb;
		switch (hdr->command_type) {
		case DXGK_VMBCOMMAND_SETGUESTDATA:
			set_guest_data(
				hdr,
				sizeof(struct dxgkvmb_command_setguestdata));
			break;
		case DXGK_VMBCOMMAND_SIGNALGUESTEVENT:
		case DXGK_VMBCOMMAND_SIGNALGUESTEVENTPASSIVE:
			signal_guest_event(
				hdr,
				sizeof(struct dxgkvmb_command_signalguestevent));
			break;
		case DXGK_VMBCOMMAND_SENDWNFNOTIFICATION:
			/* This message is not used by the driver currently. */
			break;
		default:
			pr_err("unexpected host message %d", hdr->command_type);
		}

		/* We clear out the event buffer and re-add for use by the host */
		memset(eb, 0, sizeof(struct virtio_dxgkrnl_event_buffer));
		sg_init_one(&sg, eb,
			    sizeof(struct virtio_dxgkrnl_event_buffer));
		virtqueue_add_inbuf(vp->event_vq, &sg, 1, eb, GFP_KERNEL);
		should_kick = true;
	};

	if (should_kick)
		virtqueue_kick(vp->event_vq);
}

static void dxgkrnl_command_callback(struct virtqueue *vq)
{
	struct virtio_dxgkrnl *vp;

	vp = vq->vdev->priv;

	queue_work(system_freezable_wq, &vp->command_result_work);
};

static void dxgkrnl_command_result_work(struct work_struct *work)
{
	struct virtio_dxgkrnl_command *cmd;
	struct virtio_dxgkrnl *vp;
	unsigned int len;

	dev_dbg(dxgglobaldev, "%s begins", __func__);
	vp = container_of(work, struct virtio_dxgkrnl, command_result_work);

	spin_lock(&vp->command_qlock);
	while ((cmd = virtqueue_get_buf(vp->command_vq, &len)) != NULL) {
		dev_dbg(dxgglobaldev, "virtqueue_get_buf for command #%d",
			cmd->seqno);
		spin_unlock(&vp->command_qlock);
		if (!cmd->async && cmd->completion != NULL) {
			dev_dbg(dxgglobaldev, "mark completion for command #%d",
				cmd->seqno);
			complete(cmd->completion);
		}

		virtio_dxgkrnl_cmd_unref(cmd);
		spin_lock(&vp->command_qlock);
	};
	spin_unlock(&vp->command_qlock);

	dev_dbg(dxgglobaldev, "%s ends", __func__);
}

int dxgglobal_init_global_channel(void)
{
	struct virtio_dxgkrnl *vp;
	int ret;

	vp = dxgglobal->vdxgkrnl;

	if (!virtio_get_shm_region(vp->vdev, &vp->iospace_region,
				   VIRTIO_DXGKRNL_SHM_ID_IOSPACE)) {
		dev_err(&vp->vdev->dev,
			"Could not get virtio shared memory region\n");
		return -EINVAL;
	}

	if (!devm_request_mem_region(&vp->vdev->dev, vp->iospace_region.addr,
				     vp->iospace_region.len,
				     dev_name(&vp->vdev->dev))) {
		dev_err(&vp->vdev->dev, "Could not reserve iospace region\n");
		return -ENOENT;
	}

	dev_info(&vp->vdev->dev, "virtio-dxgkrnl iospace: 0x%lx +0x%lx\n",
		 (unsigned long)vp->iospace_region.addr,
		 (unsigned long)vp->iospace_region.len);

	dxgglobal->mmiospace_base = vp->iospace_region.addr;
	dxgglobal->mmiospace_size = vp->iospace_region.len;

	ret = dxgvmb_send_set_iospace_region(dxgglobal->mmiospace_base,
					     dxgglobal->mmiospace_size, 0);
	if (ret < 0) {
		dev_err(&vp->vdev->dev, "send_set_iospace_region failed");
		return ret;
	}

	dxgglobal->dxgdevice.minor = MISC_DYNAMIC_MINOR;
	dxgglobal->dxgdevice.name = "dxg";
	dxgglobal->dxgdevice.fops = &dxgk_fops;
	dxgglobal->dxgdevice.mode = 0666;
	ret = misc_register(&dxgglobal->dxgdevice);
	if (ret) {
		dev_err(&vp->vdev->dev, "misc_register failed: %d", ret);
		return ret;
	}
	dxgglobaldev = dxgglobal->dxgdevice.this_device;
	dxgglobal->dxg_dev_initialized = true;

	return ret;
}

void dxgvmbuschannel_destroy(struct dxgvmbuschannel *ch)
{
	kmem_cache_destroy(ch->packet_cache);
	ch->packet_cache = NULL;
}

void dxgglobal_destroy_global_channel(void)
{
	down_write(&dxgglobal->channel_lock);

	dxgglobal->global_channel_initialized = false;

	dxgvmbuschannel_destroy(&dxgglobal->channel);

	up_write(&dxgglobal->channel_lock);
};

int dxgvmbuschannel_init(struct dxgvmbuschannel *ch, struct hv_device *hdev)
{
	int ret = 0;

	ch->hdev = hdev;
	spin_lock_init(&ch->packet_list_mutex);
	INIT_LIST_HEAD(&ch->packet_list_head);
	atomic64_set(&ch->packet_request_id, 0);

	ch->packet_cache = kmem_cache_create(
		"DXGK packet cache", sizeof(struct dxgvmbuspacket), 0, 0, NULL);
	if (ch->packet_cache == NULL) {
		pr_err("packet_cache alloc failed");
		return -ENOMEM;
	}

	return ret;
};

int dxgvmb_send_async_msg(struct dxgvmbuschannel *channel, void *command,
			  u32 cmd_size)
{
	struct virtio_dxgkrnl_command *ctx;
	struct virtio_dxgkrnl *vp;
	struct scatterlist sg;
	int err;
	int cur_command_seqno;

	vp = dxgglobal->vdxgkrnl;

	ctx = virtio_dxgkrnl_command_create(vp, channel, cmd_size, 0, true);
	if (!ctx) {
		err = -ENOMEM;
		dev_err(&vp->vdev->dev, "%s: failed allocate command\n",
			__func__);
		return err;
	}
	cur_command_seqno = ctx->seqno;

	memcpy(ctx->command, command, cmd_size);

	sg_init_one(&sg, ctx->command, cmd_size);
	spin_lock(&vp->command_qlock);
	dev_dbg(dxgglobaldev, "virtqueue_add_outbuf for command #%d",
		cur_command_seqno);
	err = virtqueue_add_outbuf(vp->command_vq, &sg, 1, ctx, GFP_KERNEL);
	spin_unlock(&vp->command_qlock);

	if (err) {
		dev_err(&vp->vdev->dev, "%s: failed to add output: %d\n",
			__func__, err);
		return err;
	}
	spin_lock(&vp->command_qlock);
	dev_dbg(dxgglobaldev, "virtqueue_kick for command #%d",
		cur_command_seqno);
	if (unlikely(!virtqueue_kick(vp->command_vq))) {
		dev_err(&vp->vdev->dev,
			"%s: virtqueue_kick failed with command virtqueue\n",
			__func__);
		err = -1;
	}
	spin_unlock(&vp->command_qlock);

	return err;
}

int dxgvmb_send_sync_msg(struct dxgvmbuschannel *channel, void *command,
			 u32 cmd_size, void *result, u32 result_size)
{
	struct scatterlist *sgs[2], command_sg, result_sg;
	struct completion completion = {};
	struct virtio_dxgkrnl_command *ctx;
	struct virtio_dxgkrnl *vp;
	int err;
	int cur_command_seqno;
	bool command_queue_locked = false;

	vp = dxgglobal->vdxgkrnl;

	ctx = virtio_dxgkrnl_command_create(vp, channel, cmd_size, result_size,
					    false);
	if (!ctx) {
		err = -ENOMEM;
		dev_err(&vp->vdev->dev, "%s: failed allocate command\n",
			__func__);
		return err;
	}
	memcpy(ctx->command, command, cmd_size);

	/* Take a ref to this command because `completion` is on the stack here and
	 * we need to remove the pointer to completion in case we're interrupted
	 * during wait_for_completion_interruptible.
	 */
	virtio_dxgkrnl_cmd_ref(ctx);
	cur_command_seqno = ctx->seqno;
	init_completion(&completion);
	ctx->completion = &completion;

	sg_init_one(&command_sg, ctx->command, cmd_size);
	sgs[0] = &command_sg;

	sg_init_one(&result_sg, ctx->result, result_size);
	sgs[1] = &result_sg;

	spin_lock(&vp->command_qlock);
	command_queue_locked = true;
	dev_dbg(dxgglobaldev, "virtqueue_add_sgs for command #%d",
		cur_command_seqno);
	err = virtqueue_add_sgs(vp->command_vq, sgs, 1, 1, ctx, GFP_ATOMIC);
	if (err) {
		dev_err(&vp->vdev->dev, "%s: failed to add output: %d\n",
			__func__, err);
		goto cleanup;
	}

	dev_dbg(dxgglobaldev, "virtqueue_kick for command #%d",
		cur_command_seqno);
	if (unlikely(!virtqueue_kick(vp->command_vq))) {
		dev_err(&vp->vdev->dev,
			"%s: virtqueue_kick failed with command virtqueue\n",
			__func__);
		err = -1;
		goto cleanup;
	}
	spin_unlock(&vp->command_qlock);
	command_queue_locked = false;

	/* Spin for a response, the kick causes an ioport write, trapping
	 * into the hypervisor, so the request should be handled immediately.
	 */
	dev_dbg(dxgglobaldev, "wait_for_completion_interruptible #%d start",
		cur_command_seqno);
	wait_for_completion_interruptible(&completion);
	// In case we've been interrupted, set completion to NULL here on ctx.
	ctx->completion = NULL;
	dev_dbg(dxgglobaldev, "wait_for_completion_interruptible #%d end",
		cur_command_seqno);

	memcpy(result, ctx->result, result_size);

	/* Calling code expects this to be >0 if result_size >0. This doesn't
	 * reflect if any data was actually written to the result buffer, however.
	 * The uses of this >0 return value should be reexamined, perhaps it's
	 * unnecessary.
	 */
	err = result_size;

cleanup:
	virtio_dxgkrnl_cmd_unref(ctx);
	if (command_queue_locked)
		spin_unlock(&vp->command_qlock);

	return err;
}

struct winluid luid_from_int64(int64_t value)
{
	struct large_integer i;
	struct winluid luid;

	i.quadpart = value;

	luid.a = i.u.lowpart;
	luid.b = i.u.highpart;
	return luid;
}

static int initialize_adapters(struct virtio_dxgkrnl *vp)
{
	struct virtio_dxgkrnl_enum_adapters_resp *resp;
	struct virtio_dxgkrnl_enum_adapters_req *req;
	struct scatterlist *sgs[2], req_sg, resp_sg;
	struct dxgvgpuchannel *vgpuch;
	size_t req_size, resp_size;
	struct winluid luid;
	__s64 luid_value;
	u64 num_adapters;
	unsigned int tmp;
	int err = 0;
	int i;

	dev_err(&vp->vdev->dev, "%s: initializing adapters\n", __func__);

	num_adapters = virtio_cread64(
		vp->vdev, offsetof(struct virtio_dxgkrnl_config, num_adapters));

	req_size = sizeof(struct virtio_dxgkrnl_enum_adapters_req);
	req = kzalloc(req_size, GFP_ATOMIC);
	req->num_adapters = num_adapters;
	req->adapter_offset = 0;

	sg_init_one(&req_sg, req, req_size);
	sgs[0] = &req_sg;

	resp_size = sizeof(struct virtio_dxgkrnl_enum_adapters_resp) +
		    sizeof(__s64) * num_adapters;
	resp = kzalloc(resp_size, GFP_ATOMIC);

	sg_init_one(&resp_sg, resp, resp_size);
	sgs[1] = &resp_sg;

	err = virtqueue_add_sgs(vp->setup_vq, sgs, 1, 1, vp, GFP_ATOMIC);
	if (err) {
		dev_err(&vp->vdev->dev, "%s: failed to add output: %d\n",
			__func__, err);
		goto cleanup;
	}

	if (unlikely(!virtqueue_kick(vp->setup_vq))) {
		dev_err(&vp->vdev->dev,
			"%s: virtqueue_kick failed with setup virtqueue\n",
			__func__);
		err = -1;
		goto cleanup;
	}

	dev_err(&vp->vdev->dev, "%s: virtqueue_kick succeeded\n", __func__);

	while (!virtqueue_get_buf(vp->setup_vq, &tmp) &&
	       !virtqueue_is_broken(vp->setup_vq))
		cpu_relax();

	if (resp->status) {
		dev_err(&vp->vdev->dev,
			"%s: enum_adapters failed with response status %llu\n",
			__func__, resp->status);
		err = resp->status;
		goto cleanup;
	}

	for (i = 0; i < num_adapters; i++) {
		dev_err(&vp->vdev->dev, "%s: processing adapter %i\n", __func__,
			i);
		luid_value = resp->vgpu_luids[i];
		luid = luid_from_int64(luid_value);
		dev_err(&vp->vdev->dev, "%s: got luid from value %lli\n",
			__func__, luid_value);

		vgpuch = vzalloc(sizeof(struct dxgvgpuchannel));
		if (vgpuch == NULL) {
			err = -ENOMEM;
			goto cleanup;
		}
		vgpuch->adapter_luid = luid_from_int64(i);
		vgpuch->hdev = NULL;
		list_add_tail(&vgpuch->vgpu_ch_list_entry,
			      &dxgglobal->vgpu_ch_list_head);

		err = dxgglobal_create_adapter(NULL, vgpuch->adapter_luid,
					       luid);
		dev_err(&vp->vdev->dev, "%s: created adapter\n", __func__);
		if (err) {
			dev_err(&vp->vdev->dev,
				"%s: failed to create adapter for luid %x-%x: %u\n",
				__func__, luid.a, luid.b, err);
			goto cleanup;
		}
	}

	dev_err(&vp->vdev->dev, "%s: starting adapters\n", __func__);

	dxgglobal_start_adapters();

cleanup:
	kfree(req);
	kfree(resp);
	return err;
}

static void fill_event_queue(struct virtio_dxgkrnl *vp)
{
	struct scatterlist sg;
	int i;

	for (i = 0; i < VIRTIO_DXGKRNL_NUM_EVENT_BUFFERS; i++) {
		sg_init_one(&sg, &vp->event_buffers[i],
			    sizeof(struct virtio_dxgkrnl_event_buffer));
		virtqueue_add_inbuf(vp->event_vq, &sg, 1, &vp->event_buffers[i],
				    GFP_KERNEL);
	}
	virtqueue_kick(vp->event_vq);
}

static int init_vqs(struct virtio_dxgkrnl *vp)
{
	vq_callback_t *callbacks[VIRTIO_DXGKRNL_VQ_MAX];
	struct virtqueue *vqs[VIRTIO_DXGKRNL_VQ_MAX];
	const char *names[VIRTIO_DXGKRNL_VQ_MAX];
	int err;

	callbacks[VIRTIO_DXGKRNL_VQ_SETUP] = NULL;
	names[VIRTIO_DXGKRNL_VQ_SETUP] = "dxgkrnl_setup";

	callbacks[VIRTIO_DXGKRNL_VQ_COMMAND] = dxgkrnl_command_callback;
	names[VIRTIO_DXGKRNL_VQ_COMMAND] = "dxgkrnl_command";
	INIT_WORK(&vp->command_result_work, dxgkrnl_command_result_work);

	callbacks[VIRTIO_DXGKRNL_VQ_EVENT] = dxgkrnl_event_callback;
	names[VIRTIO_DXGKRNL_VQ_EVENT] = "dxgkrnl_event";
	INIT_WORK(&vp->event_work, dxgkrnl_event_work);

	err = vp->vdev->config->find_vqs(vp->vdev, VIRTIO_DXGKRNL_VQ_MAX, vqs,
					 callbacks, names, NULL, NULL);
	if (err)
		return err;

	vp->setup_vq = vqs[VIRTIO_DXGKRNL_VQ_SETUP];
	vp->command_vq = vqs[VIRTIO_DXGKRNL_VQ_COMMAND];
	vp->event_vq = vqs[VIRTIO_DXGKRNL_VQ_EVENT];

	return 0;
}

static int dxgglobal_create(void)
{
	int ret = 0;

	dxgglobal = vzalloc(sizeof(struct dxgglobal));
	if (!dxgglobal)
		return -ENOMEM;

	INIT_LIST_HEAD(&dxgglobal->plisthead);
	mutex_init(&dxgglobal->plistmutex);
	mutex_init(&dxgglobal->device_mutex);
	mutex_init(&dxgglobal->process_adapter_mutex);

	INIT_LIST_HEAD(&dxgglobal->thread_info_list_head);
	mutex_init(&dxgglobal->thread_info_mutex);

	INIT_LIST_HEAD(&dxgglobal->vgpu_ch_list_head);
	INIT_LIST_HEAD(&dxgglobal->adapter_list_head);
	init_rwsem(&dxgglobal->adapter_list_lock);

	init_rwsem(&dxgglobal->channel_lock);

	INIT_LIST_HEAD(&dxgglobal->host_event_list_head);
	spin_lock_init(&dxgglobal->host_event_list_mutex);
	atomic64_set(&dxgglobal->host_event_id, 1);

	hmgrtable_init(&dxgglobal->handle_table, NULL);

	// Setting this on prevents using GPADL for existing sysmem allocations.
	dxgglobal->map_guest_pages_enabled = true;

	dev_dbg(dxgglobaldev, "dxgglobal_init end\n");
	return ret;
}

static void dxgglobal_destroy(void)
{
	if (dxgglobal) {
		dxgglobal_stop_adapters();

		dxgglobal_destroy_global_channel();
		hmgrtable_destroy(&dxgglobal->handle_table);

		vfree(dxgglobal);
		dxgglobal = NULL;
	}
}

static struct virtio_dxgkrnl *virtio_dxgkrnl_create(void)
{
	struct virtio_dxgkrnl *vp;

	vp = kzalloc(sizeof(*vp), GFP_KERNEL);
	if (vp) {
		spin_lock_init(&vp->command_qlock);
		spin_lock_init(&vp->command_list_mutex);
		INIT_LIST_HEAD(&vp->command_list_head);
	}

	return vp;
}

static int virtdxgkrnl_probe(struct virtio_device *vdev)
{
	struct virtio_dxgkrnl *vp;
	int err;

	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s: config access disabled\n", __func__);
		return -EINVAL;
	}

	vp = virtio_dxgkrnl_create();
	if (!vp)
		return -ENOMEM;

	vp->vdev = vdev;
	vdev->priv = vp;

	dxgglobaldev = &vp->vdev->dev;

	err = dxgglobal_create();
	if (err) {
		pr_err("dxgglobal_init failed");
		goto out_free_vp;
	}

	// virtio-dxgkrnl always uses extension header
	dxgglobal->vmbus_ver = DXGK_VMBUS_INTERFACE_VERSION;

	err = init_vqs(vp);
	if (err)
		goto out_free_vp;

	virtio_device_ready(vdev);

	dxgglobal->vdxgkrnl = vp;

	fill_event_queue(vp);
	init_ioctls();

	if (virtio_has_feature(vp->vdev, VIRTIO_DXGKRNL_F_ASYNC_COMMANDS))
		dxgglobal->async_msg_enabled = true;

	return initialize_adapters(vp);

out_free_vp:
	kfree(vp);
	return err;
}

static void remove_common(struct virtio_dxgkrnl *vp)
{
	/* Now we reset the device so we can clean up the queues. */
	vp->vdev->config->reset(vp->vdev);

	vp->vdev->config->del_vqs(vp->vdev);

	dxgglobal_destroy();
}

static void virtdxgkrnl_remove(struct virtio_device *vdev)
{
	struct virtio_dxgkrnl *vp = vdev->priv;

	remove_common(vp);

	kfree(vp);
}

#ifdef CONFIG_PM_SLEEP
static int virtdxgkrnl_freeze(struct virtio_device *vdev)
{
	struct virtio_dxgkrnl *vp = vdev->priv;

	remove_common(vp);
	return 0;
}

static int virtdxgkrnl_restore(struct virtio_device *vdev)
{
	int ret;

	ret = init_vqs(vdev->priv);
	if (ret)
		return ret;

	virtio_device_ready(vdev);

	return 0;
}
#endif

static int virtdxgkrnl_validate(struct virtio_device *vdev)
{
	return 0;
}

static unsigned int features[] = { VIRTIO_DXGKRNL_F_ASYNC_COMMANDS };

static struct virtio_driver virtio_dxgkrnl_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name = KBUILD_MODNAME,
	.driver.owner = THIS_MODULE,
	.id_table = id_table,
	.validate = virtdxgkrnl_validate,
	.probe = virtdxgkrnl_probe,
	.remove = virtdxgkrnl_remove,
#ifdef CONFIG_PM_SLEEP
	.freeze = virtdxgkrnl_freeze,
	.restore = virtdxgkrnl_restore,
#endif
};

module_virtio_driver(virtio_dxgkrnl_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio dxgkrnl driver");
MODULE_LICENSE("GPL");
