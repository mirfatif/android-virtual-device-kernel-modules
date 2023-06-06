/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Wayland Virtio Driver
 *  Copyright (C) 2022 Google, Inc.
 */

/*
 * Virtio Wayland (virtio_wl or virtwl) is a virtual device that allows a guest
 * virtual machine to use a wayland server on the host transparently (to the
 * host).  This is done by proxying the wayland protocol socket stream verbatim
 * between the host and guest over 2 (recv and send) virtio queues. The guest
 * can request new wayland server connections to give each guest wayland client
 * a different server context. Each host connection's file descriptor is exposed
 * to the guest as a virtual file descriptor (VFD). Additionally, the guest can
 * request shared memory file descriptors which are also exposed as VFDs. These
 * shared memory VFDs are directly writable by the guest via device memory
 * injected by the host. Each VFD is sendable along a connection context VFD and
 * will appear as ancillary data to the wayland server, just like a message from
 * an ordinary wayland client. When the wayland server sends a shared memory
 * file descriptor to the client (such as when sending a keymap), a VFD is
 * allocated by the device automatically and its memory is injected into as
 * device memory.
 *
 * This driver is intended to be paired with the `virtwl_guest_proxy` program
 * which is run in the guest system and acts like a wayland server. It accepts
 * wayland client connections and converts their socket messages to ioctl
 * messages exposed by this driver via the `/dev/wl` device file. While it would
 * be possible to expose a unix stream socket from this driver, the user space
 * helper is much cleaner to write.
 */

#include <linux/anon_inodes.h>
#include <linux/compat.h>
#include <linux/completion.h>
#include <linux/dma-buf.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/scatterlist.h>
#include <linux/syscalls.h>
#include <linux/virtio.h>
#include <linux/virtio_dma_buf.h>
#include <linux/virtio_wl.h>
#include <linux/vmalloc.h>

#include <uapi/linux/dma-buf.h>

#define SEND_VIRTGPU_RESOURCES
#include <linux/sync_file.h>

#define VFD_ILLEGAL_SIGN_BIT 0x80000000
#define VFD_HOST_VFD_ID_BIT 0x40000000

MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);

struct virtwl_vfd_qentry {
	struct list_head list;
	struct virtio_wl_ctrl_hdr *hdr;
	unsigned int len; /* total byte length of ctrl_vfd_* + vfds + data */
	unsigned int vfd_offset; /* int offset into vfds */
	unsigned int data_offset; /* byte offset into data */
};

struct virtwl_vfd {
	struct kobject kobj;
	struct mutex lock;

	struct virtwl_info *vi;
	/*
	 * @id, @flags, @pfn and @size never change after vfd is initialized,
	 * so we don't have to take the @lock when read-access those members.
	 */
	uint32_t id;
	uint32_t flags;
	uint64_t pfn;
	uint32_t size;
	bool hungup;

	union {
		struct list_head in_queue; /* list of virtwl_vfd_qentry */
		struct list_head fence_release_entry;
	};
	wait_queue_head_t in_waitq;

	struct dma_fence *fence;
};

struct virtwl_info {
	char name[16];
	struct miscdevice miscdev;
	struct virtio_device *vdev;

	struct mutex vq_locks[VIRTWL_QUEUE_COUNT];
	struct virtqueue *vqs[VIRTWL_QUEUE_COUNT];
	struct work_struct in_vq_work;
	struct work_struct out_vq_work;

	wait_queue_head_t out_waitq;

	struct mutex vfds_lock;
	struct idr vfds;

	bool use_send_vfd_v2;

	spinlock_t fence_lock;
	struct list_head fence_release_list;
	struct work_struct fence_release_work;
};

struct virtwl_fence {
	struct dma_fence base;
	struct virtwl_vfd *vfd;
};

static struct virtwl_vfd *virtwl_vfd_alloc(struct virtwl_info *vi);
static void virtwl_vfd_free(struct virtwl_vfd *vfd);
static int do_vfd_close(struct virtwl_vfd *vfd);

static const struct file_operations virtwl_vfd_fops;

static int virtwl_resp_err(unsigned int type)
{
	switch (type) {
	case VIRTIO_WL_RESP_OK:
	case VIRTIO_WL_RESP_VFD_NEW:
	case VIRTIO_WL_RESP_VFD_NEW_DMABUF:
		return 0;
	case VIRTIO_WL_RESP_ERR:
		return -ENODEV; /* Device is no longer reliable */
	case VIRTIO_WL_RESP_OUT_OF_MEMORY:
		return -ENOMEM;
	case VIRTIO_WL_RESP_INVALID_ID:
		return -ENOENT;
	case VIRTIO_WL_RESP_INVALID_TYPE:
		return -EINVAL;
	case VIRTIO_WL_RESP_INVALID_FLAGS:
		return -EPERM;
	case VIRTIO_WL_RESP_INVALID_CMD:
		return -ENOTTY;
	default:
		return -EPROTO;
	}
}

static int vq_return_inbuf_locked(struct virtqueue *vq, void *buffer)
{
	int ret;
	struct scatterlist sg[1];

	sg_init_one(sg, buffer, PAGE_SIZE);

	ret = virtqueue_add_inbuf(vq, sg, 1, buffer, GFP_KERNEL);
	if (ret) {
		dev_warn(&vq->vdev->dev, "failed to give inbuf to host: %d\n", ret);
		return ret;
	}

	return 0;
}

static int vq_queue_out(struct virtwl_info *vi, struct scatterlist *out_sg,
			struct scatterlist *in_sg,
			struct completion *finish_completion,
			bool nonblock)
{
	struct virtqueue *vq = vi->vqs[VIRTWL_VQ_OUT];
	struct mutex *vq_lock = &vi->vq_locks[VIRTWL_VQ_OUT];
	struct scatterlist *sgs[] = { out_sg, in_sg };
	int ret = 0;

	mutex_lock(vq_lock);
	while ((ret = virtqueue_add_sgs(vq, sgs, 1, 1, finish_completion,
					GFP_KERNEL)) == -ENOSPC) {
		mutex_unlock(vq_lock);
		if (nonblock)
			return -EAGAIN;
		if (!wait_event_timeout(vi->out_waitq, vq->num_free > 0, HZ))
			return -EBUSY;
		mutex_lock(vq_lock);
	}
	if (!ret)
		virtqueue_kick(vq);
	mutex_unlock(vq_lock);

	return ret;
}

static int vq_fill_locked(struct virtqueue *vq)
{
	void *buffer;
	int ret = 0;

	while (vq->num_free > 0) {
		buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
		if (!buffer) {
			ret = -ENOMEM;
			goto clear_queue;
		}

		ret = vq_return_inbuf_locked(vq, buffer);
		if (ret)
			goto clear_queue;
	}

	return 0;

clear_queue:
	while ((buffer = virtqueue_detach_unused_buf(vq)))
		kfree(buffer);
	return ret;
}

#define to_virtwl_fence(dma_fence) \
	container_of(dma_fence, struct virtwl_fence, base)

static void virtwl_fence_release_handler(struct work_struct *work)
{
	struct virtwl_info *vi = container_of(work, struct virtwl_info,
					      fence_release_work);
	struct virtwl_vfd *vfd, *next;
	LIST_HEAD(to_release);
	unsigned long flags;

	spin_lock_irqsave(&vi->fence_lock, flags);
	list_splice_init(&vi->fence_release_list, &to_release);
	spin_unlock_irqrestore(&vi->fence_lock, flags);

	list_for_each_entry_safe(vfd, next, &to_release, fence_release_entry) {
		uint32_t vfd_id = vfd->id;
		int ret;

		list_del_init(&vfd->fence_release_entry);
		ret = do_vfd_close(vfd);
		if (ret)
			dev_warn(&vi->vdev->dev, "failed to release vfd id %u: %d\n",
				 vfd_id, ret);
	}
}

static const char *virtwl_fence_driver_name(struct dma_fence *fence)
{
	return "virtio_wl";
}

static void virtwl_fence_release(struct dma_fence *f)
{
	struct virtwl_fence *fence = to_virtwl_fence(f);
	struct virtwl_info *vi = fence->vfd->vi;
	unsigned long flags;

	spin_lock_irqsave(&vi->fence_lock, flags);
	list_add_tail(&fence->vfd->fence_release_entry,
		      &vi->fence_release_list);
	spin_unlock_irqrestore(&vi->fence_lock, flags);

	// Release may be called from an IRQ context. Since closing the fence's
	// vfd involves waiting for a reply completion, it needs to be done on
	// a worker thread.
	schedule_work(&vi->fence_release_work);

	dma_fence_free(&fence->base);
}

static const struct dma_fence_ops virtwl_fence_ops = {
	.get_driver_name = virtwl_fence_driver_name,
	.get_timeline_name = virtwl_fence_driver_name,
	.release = virtwl_fence_release,
};

static bool vq_handle_new(struct virtwl_info *vi,
			  struct virtio_wl_ctrl_vfd_new *new, unsigned int len)
{
	struct virtwl_vfd *vfd;
	u32 id = new->vfd_id;
	int ret;

	if (id == 0)
		return true; /* return the inbuf to vq */

	if (!(id & VFD_HOST_VFD_ID_BIT) || (id & VFD_ILLEGAL_SIGN_BIT)) {
		dev_warn(&vi->vdev->dev, "received a vfd with invalid id: %u\n", id);
		return true; /* return the inbuf to vq */
	}

	vfd = virtwl_vfd_alloc(vi);
	if (!vfd)
		return true; /* return the inbuf to vq */

	vfd->id = id;
	vfd->size = new->size;
	vfd->pfn = new->pfn;
	vfd->flags = new->flags;

	mutex_lock(&vi->vfds_lock);
	ret = idr_alloc(&vi->vfds, vfd, id, id + 1, GFP_KERNEL);
	mutex_unlock(&vi->vfds_lock);

	if (ret <= 0) {
		virtwl_vfd_free(vfd);
		dev_warn(&vi->vdev->dev, "failed to place received vfd: %d\n", ret);
		return true; /* return the inbuf to vq */
	}

	return true; /* return the inbuf to vq */
}

static bool vq_handle_recv(struct virtwl_info *vi,
			   struct virtio_wl_ctrl_vfd_recv *recv,
			   unsigned int len)
{
	struct virtwl_vfd *vfd;
	struct virtwl_vfd_qentry *qentry;

	mutex_lock(&vi->vfds_lock);
	vfd = idr_find(&vi->vfds, recv->vfd_id);
	if (vfd)
		mutex_lock(&vfd->lock);
	mutex_unlock(&vi->vfds_lock);

	if (!vfd) {
		dev_warn(&vi->vdev->dev, "recv for unknown vfd_id %u\n", recv->vfd_id);
		return true; /* return the inbuf to vq */
	}

	if (vfd->flags & VIRTIO_WL_VFD_FENCE) {
		dev_warn(&vi->vdev->dev, "recv for fence vfd_id %u\n", recv->vfd_id);
		return true; /* return the inbuf to vq */
	}

	qentry = kzalloc(sizeof(*qentry), GFP_KERNEL);
	if (!qentry) {
		mutex_unlock(&vfd->lock);
		dev_warn(&vi->vdev->dev, "failed to allocate qentry for vfd\n");
		return true; /* return the inbuf to vq */
	}

	qentry->hdr = &recv->hdr;
	qentry->len = len;

	list_add_tail(&qentry->list, &vfd->in_queue);
	wake_up_interruptible_all(&vfd->in_waitq);
	mutex_unlock(&vfd->lock);

	return false; /* no return the inbuf to vq */
}

static bool vq_handle_hup(struct virtwl_info *vi,
			   struct virtio_wl_ctrl_vfd *vfd_hup,
			   unsigned int len)
{
	struct virtwl_vfd *vfd;

	mutex_lock(&vi->vfds_lock);
	vfd = idr_find(&vi->vfds, vfd_hup->vfd_id);
	if (vfd)
		mutex_lock(&vfd->lock);
	mutex_unlock(&vi->vfds_lock);

	if (!vfd) {
		dev_warn(&vi->vdev->dev, "hup for unknown vfd_id %u\n", vfd_hup->vfd_id);
		return true; /* return the inbuf to vq */
	}

	if (vfd->hungup)
		dev_warn(&vi->vdev->dev, "hup for hungup vfd_id %u\n", vfd_hup->vfd_id);

	vfd->hungup = true;

	if (vfd->flags & VIRTIO_WL_VFD_FENCE) {
		spin_lock(&vi->fence_lock);
		if (vfd->fence) {
			dma_fence_signal_locked(vfd->fence);
			dma_fence_put(vfd->fence);
		}
		spin_unlock(&vi->fence_lock);
	} else {
		wake_up_interruptible_all(&vfd->in_waitq);
	}

	mutex_unlock(&vfd->lock);

	return true;
}

static bool vq_dispatch_hdr(struct virtwl_info *vi, unsigned int len,
			    struct virtio_wl_ctrl_hdr *hdr)
{
	struct virtqueue *vq = vi->vqs[VIRTWL_VQ_IN];
	struct mutex *vq_lock = &vi->vq_locks[VIRTWL_VQ_IN];
	bool return_vq = true;
	int ret;

	switch (hdr->type) {
	case VIRTIO_WL_CMD_VFD_NEW:
		return_vq = vq_handle_new(vi,
					  (struct virtio_wl_ctrl_vfd_new *)hdr,
					  len);
		break;
	case VIRTIO_WL_CMD_VFD_RECV:
		return_vq = vq_handle_recv(vi,
			(struct virtio_wl_ctrl_vfd_recv *)hdr, len);
		break;
	case VIRTIO_WL_CMD_VFD_HUP:
		return_vq = vq_handle_hup(vi, (struct virtio_wl_ctrl_vfd *)hdr,
					  len);
		break;
	default:
		dev_warn(&vi->vdev->dev, "unhandled ctrl command: %u\n", hdr->type);
		break;
	}

	if (!return_vq)
		return false; /* no kick the vq */

	mutex_lock(vq_lock);
	ret = vq_return_inbuf_locked(vq, hdr);
	mutex_unlock(vq_lock);
	if (ret) {
		dev_warn(&vi->vdev->dev, "failed to return inbuf to host: %d\n", ret);
		kfree(hdr);
	}

	return true; /* kick the vq */
}

static void vq_in_work_handler(struct work_struct *work)
{
	struct virtwl_info *vi = container_of(work, struct virtwl_info,
					      in_vq_work);
	struct virtqueue *vq = vi->vqs[VIRTWL_VQ_IN];
	struct mutex *vq_lock = &vi->vq_locks[VIRTWL_VQ_IN];
	void *buffer;
	unsigned int len;
	bool kick_vq = false;

	mutex_lock(vq_lock);
	while ((buffer = virtqueue_get_buf(vq, &len)) != NULL) {
		struct virtio_wl_ctrl_hdr *hdr = buffer;

		mutex_unlock(vq_lock);
		kick_vq |= vq_dispatch_hdr(vi, len, hdr);
		mutex_lock(vq_lock);
	}
	mutex_unlock(vq_lock);

	if (kick_vq)
		virtqueue_kick(vq);
}

static void vq_out_work_handler(struct work_struct *work)
{
	struct virtwl_info *vi = container_of(work, struct virtwl_info,
					      out_vq_work);
	struct virtqueue *vq = vi->vqs[VIRTWL_VQ_OUT];
	struct mutex *vq_lock = &vi->vq_locks[VIRTWL_VQ_OUT];
	unsigned int len;
	struct completion *finish_completion;
	bool wake_waitq = false;

	mutex_lock(vq_lock);
	while ((finish_completion = virtqueue_get_buf(vq, &len)) != NULL) {
		wake_waitq = true;
		complete(finish_completion);
	}
	mutex_unlock(vq_lock);

	if (wake_waitq)
		wake_up_interruptible_all(&vi->out_waitq);
}

static void vq_in_cb(struct virtqueue *vq)
{
	struct virtwl_info *vi = vq->vdev->priv;

	schedule_work(&vi->in_vq_work);
}

static void vq_out_cb(struct virtqueue *vq)
{
	struct virtwl_info *vi = vq->vdev->priv;

	schedule_work(&vi->out_vq_work);
}

static struct virtwl_vfd *virtwl_vfd_alloc(struct virtwl_info *vi)
{
	struct virtwl_vfd *vfd = kzalloc(sizeof(struct virtwl_vfd), GFP_KERNEL);

	if (!vfd)
		return ERR_PTR(-ENOMEM);

	vfd->vi = vi;

	mutex_init(&vfd->lock);
	INIT_LIST_HEAD(&vfd->in_queue);
	init_waitqueue_head(&vfd->in_waitq);

	return vfd;
}

static int virtwl_vfd_file_flags(struct virtwl_vfd *vfd)
{
	int flags = 0;
	int rw_mask = VIRTIO_WL_VFD_WRITE | VIRTIO_WL_VFD_READ;

	if ((vfd->flags & rw_mask) == rw_mask)
		flags |= O_RDWR;
	else if (vfd->flags & VIRTIO_WL_VFD_WRITE)
		flags |= O_WRONLY;
	else if (vfd->flags & VIRTIO_WL_VFD_READ)
		flags |= O_RDONLY;
	if (vfd->pfn)
		flags |= O_RDWR;
	return flags;
}

/* Locks the vfd and unlinks its id from vi */
static void virtwl_vfd_lock_unlink(struct virtwl_vfd *vfd)
{
	struct virtwl_info *vi = vfd->vi;

	/* this order is important to avoid deadlock */
	mutex_lock(&vi->vfds_lock);
	mutex_lock(&vfd->lock);
	idr_remove(&vi->vfds, vfd->id);
	mutex_unlock(&vfd->lock);
	mutex_unlock(&vi->vfds_lock);
}

/*
 * Only used to free a vfd that is not referenced any place else and contains
 * no queed virtio buffers. This must not be called while vfd is included in a
 * vi->vfd.
 */
static void virtwl_vfd_free(struct virtwl_vfd *vfd)
{
	kfree(vfd);
}

/*
 * Thread safe and also removes vfd from vi as well as any queued virtio buffers
 */
static void virtwl_vfd_remove(struct virtwl_vfd *vfd)
{
	struct virtwl_info *vi = vfd->vi;
	struct virtqueue *vq = vi->vqs[VIRTWL_VQ_IN];
	struct mutex *vq_lock = &vi->vq_locks[VIRTWL_VQ_IN];
	struct virtwl_vfd_qentry *qentry, *next;

	virtwl_vfd_lock_unlink(vfd);

	mutex_lock(vq_lock);
	list_for_each_entry_safe(qentry, next, &vfd->in_queue, list) {
		vq_return_inbuf_locked(vq, qentry->hdr);
		list_del(&qentry->list);
		kfree(qentry);
	}
	mutex_unlock(vq_lock);
	virtqueue_kick(vq);

	virtwl_vfd_free(vfd);
}

static void vfd_qentry_free_if_empty(struct virtwl_vfd *vfd,
				     struct virtwl_vfd_qentry *qentry)
{
	struct virtwl_info *vi = vfd->vi;
	struct virtqueue *vq = vi->vqs[VIRTWL_VQ_IN];
	struct mutex *vq_lock = &vi->vq_locks[VIRTWL_VQ_IN];

	if (qentry->hdr->type == VIRTIO_WL_CMD_VFD_RECV) {
		struct virtio_wl_ctrl_vfd_recv *recv =
			(struct virtio_wl_ctrl_vfd_recv *)qentry->hdr;
		ssize_t data_len =
			(ssize_t)qentry->len - (ssize_t)sizeof(*recv) -
			(ssize_t)recv->vfd_count * (ssize_t)sizeof(__le32);

		if (qentry->vfd_offset < recv->vfd_count)
			return;

		if ((s64)qentry->data_offset < data_len)
			return;
	}

	mutex_lock(vq_lock);
	vq_return_inbuf_locked(vq, qentry->hdr);
	mutex_unlock(vq_lock);
	list_del(&qentry->list);
	kfree(qentry);
	virtqueue_kick(vq);
}

static ssize_t vfd_out_locked(struct virtwl_vfd *vfd, char __user *buffer,
			      size_t len)
{
	struct virtwl_vfd_qentry *qentry, *next;
	size_t read_count = 0;

	list_for_each_entry_safe(qentry, next, &vfd->in_queue, list) {
		struct virtio_wl_ctrl_vfd_recv *recv =
			(struct virtio_wl_ctrl_vfd_recv *)qentry->hdr;
		size_t recv_offset = sizeof(*recv) + recv->vfd_count *
				     sizeof(__le32) + qentry->data_offset;
		u8 *buf = (u8 *)recv + recv_offset;
		size_t to_read = (size_t)qentry->len - recv_offset;

		/* Detect underflow caused by invalid recv->vfd_count value. */
		if (to_read > (size_t)qentry->len)
			return -EIO;

		if (qentry->hdr->type != VIRTIO_WL_CMD_VFD_RECV)
			continue;

		if (len - read_count < to_read)
			to_read = len - read_count;

		if (copy_to_user(buffer + read_count, buf, to_read))
			return -EFAULT;

		read_count += to_read;

		qentry->data_offset += to_read;
		vfd_qentry_free_if_empty(vfd, qentry);

		if (read_count >= len)
			break;
	}

	return read_count;
}

/* must hold both vfd->lock and vi->vfds_lock */
static size_t vfd_out_vfds_locked(struct virtwl_vfd *vfd,
				  struct virtwl_vfd **vfds, size_t count)
{
	struct virtwl_info *vi = vfd->vi;
	struct virtwl_vfd_qentry *qentry, *next;
	size_t i;
	size_t read_count = 0;

	list_for_each_entry_safe(qentry, next, &vfd->in_queue, list) {
		struct virtio_wl_ctrl_vfd_recv *recv =
			(struct virtio_wl_ctrl_vfd_recv *)qentry->hdr;
		size_t vfd_offset = sizeof(*recv) + qentry->vfd_offset *
				    sizeof(__le32);
		__le32 *vfds_le = (__le32 *)((void *)recv + vfd_offset);
		ssize_t vfds_to_read = recv->vfd_count - qentry->vfd_offset;

		if (read_count >= count)
			break;
		if (vfds_to_read <= 0)
			continue;
		if (qentry->hdr->type != VIRTIO_WL_CMD_VFD_RECV)
			continue;

		if ((vfds_to_read + read_count) > count)
			vfds_to_read = count - read_count;

		for (i = 0; i < vfds_to_read; i++) {
			uint32_t vfd_id = le32_to_cpu(vfds_le[i]);
			vfds[read_count] = idr_find(&vi->vfds, vfd_id);
			if (vfds[read_count]) {
				read_count++;
			} else {
				dev_warn(&vi->vdev->dev, "received a vfd with unrecognized id: %u\n",
					 vfd_id);
			}
			qentry->vfd_offset++;
		}

		vfd_qentry_free_if_empty(vfd, qentry);
	}

	return read_count;
}

/* this can only be called if the caller has unique ownership of the vfd */
static int do_vfd_close(struct virtwl_vfd *vfd)
{
	struct virtio_wl_ctrl_vfd *ctrl_close;
	struct virtwl_info *vi = vfd->vi;
	struct completion finish_completion;
	struct scatterlist out_sg;
	struct scatterlist in_sg;
	int ret = 0;

	ctrl_close = kzalloc(sizeof(*ctrl_close), GFP_KERNEL);
	if (!ctrl_close)
		return -ENOMEM;

	ctrl_close->hdr.type = VIRTIO_WL_CMD_VFD_CLOSE;
	ctrl_close->vfd_id = vfd->id;

	sg_init_one(&out_sg, &ctrl_close->hdr,
		    sizeof(struct virtio_wl_ctrl_vfd));
	sg_init_one(&in_sg, &ctrl_close->hdr,
		    sizeof(struct virtio_wl_ctrl_hdr));

	init_completion(&finish_completion);
	ret = vq_queue_out(vi, &out_sg, &in_sg, &finish_completion,
			   false /* block */);
	if (ret) {
		dev_warn(&vi->vdev->dev, "failed to queue close vfd id %u: %d\n",
			 vfd->id,
			ret);
		goto free_ctrl_close;
	}

	wait_for_completion(&finish_completion);
	virtwl_vfd_remove(vfd);

free_ctrl_close:
	kfree(ctrl_close);
	return ret;
}

static ssize_t virtwl_vfd_recv(struct file *filp, char __user *buffer,
			       size_t len, struct virtwl_vfd **vfds,
			       size_t *vfd_count)
{
	struct virtwl_vfd *vfd = filp->private_data;
	struct virtwl_info *vi = vfd->vi;
	ssize_t read_count = 0;
	size_t vfd_read_count = 0;
	bool force_to_wait = false;

	mutex_lock(&vi->vfds_lock);
	mutex_lock(&vfd->lock);

	while (read_count == 0 && vfd_read_count == 0) {
		while (force_to_wait || list_empty(&vfd->in_queue)) {
			force_to_wait = false;
			if (vfd->hungup)
				goto out_unlock;

			mutex_unlock(&vfd->lock);
			mutex_unlock(&vi->vfds_lock);
			if (filp->f_flags & O_NONBLOCK)
				return -EAGAIN;

			if (wait_event_interruptible(vfd->in_waitq,
				!list_empty(&vfd->in_queue) || vfd->hungup))
				return -ERESTARTSYS;

			mutex_lock(&vi->vfds_lock);
			mutex_lock(&vfd->lock);
		}

		read_count = vfd_out_locked(vfd, buffer, len);
		if (read_count < 0)
			goto out_unlock;
		if (vfds && vfd_count && *vfd_count)
			vfd_read_count = vfd_out_vfds_locked(vfd, vfds,
							     *vfd_count);
		else if (read_count == 0 && !list_empty(&vfd->in_queue))
			/*
			 * Indicates a corner case where the in_queue has ONLY
			 * incoming VFDs but the caller has given us no space to
			 * store them. We force a wait for more activity on the
			 * in_queue to prevent busy waiting.
			 */
			force_to_wait = true;
	}

out_unlock:
	mutex_unlock(&vfd->lock);
	mutex_unlock(&vi->vfds_lock);
	if (vfd_count)
		*vfd_count = vfd_read_count;
	return read_count;
}

static int encode_vfd_ids(struct virtwl_vfd **vfds, size_t vfd_count,
			  __le32 *vfd_ids)
{
	size_t i;

	for (i = 0; i < vfd_count; i++) {
		if (vfds[i])
			vfd_ids[i] = cpu_to_le32(vfds[i]->id);
		else
			return -EBADFD;
	}
	return 0;
}

#ifdef SEND_VIRTGPU_RESOURCES
static int get_dma_buf_id(struct dma_buf *dma_buf, u32 *id)
{
	uuid_t uuid;
	int ret = 0;

	ret = virtio_dma_buf_get_uuid(dma_buf, &uuid);
	*id = be32_to_cpu(*(__be32 *)(uuid.b + 12));

	return ret;
}

static int encode_external_fence(struct dma_fence *fence,
				 struct virtio_wl_ctrl_vfd_send_vfd_v2 *vfd_id)
{
	const char *name = fence->ops->get_driver_name(fence);

	// We only support virtgpu based fences. Since all virtgpu fences are
	// in the same context, merging sync_files will always reduce to a
	// single virtgpu fence.
	if (strcmp(name, "virtio_gpu") != 0)
		return -EBADFD;

	if (dma_fence_is_signaled(fence)) {
		vfd_id->kind =
			VIRTIO_WL_CTRL_VFD_SEND_KIND_VIRTGPU_SIGNALED_FENCE;
	} else {
		vfd_id->kind = VIRTIO_WL_CTRL_VFD_SEND_KIND_VIRTGPU_FENCE;
		vfd_id->seqno = cpu_to_le32(fence->seqno);
	}
	return 0;
}

static bool is_local_fence(struct dma_fence *fence)
{
	return fence && fence->ops == &virtwl_fence_ops;
}

static int encode_vfd_ids_foreign(struct virtwl_vfd **vfds,
				  struct dma_buf **virtgpu_dma_bufs,
				  struct dma_fence **virtgpu_dma_fence,
				  size_t vfd_count,
				  struct virtio_wl_ctrl_vfd_send_vfd *ids,
				  struct virtio_wl_ctrl_vfd_send_vfd_v2 *ids_v2)
{
	size_t i;
	int ret;

	for (i = 0; i < vfd_count; i++) {
		uint32_t kind = UINT_MAX;
		uint32_t id = 0;

		if (vfds[i]) {
			kind = VIRTIO_WL_CTRL_VFD_SEND_KIND_LOCAL;
			id = vfds[i]->id;
		} else if (virtgpu_dma_bufs[i]) {
			ret = get_dma_buf_id(virtgpu_dma_bufs[i],
					     &id);
			if (ret)
				return ret;
			kind = VIRTIO_WL_CTRL_VFD_SEND_KIND_VIRTGPU;
		} else if (virtgpu_dma_fence[i]) {
			ret = encode_external_fence(virtgpu_dma_fence[i],
						    ids_v2 + i);
			if (ret)
				return ret;
		} else {
			return -EBADFD;
		}
		if (kind != UINT_MAX) {
			if (ids) {
				ids[i].kind = kind;
				ids[i].id = cpu_to_le32(id);
			} else {
				ids_v2[i].kind = kind;
				ids_v2[i].id = cpu_to_le32(id);
			}
		}
	}
	return 0;
}
#endif

static int vmalloc_to_sgt(void *data, uint32_t size, struct sg_table *sgt)
{
	int ret, s, i;
	struct scatterlist *sg;
	struct page *pg;

	ret = sg_alloc_table(sgt, DIV_ROUND_UP(size, PAGE_SIZE), GFP_KERNEL);
	if (ret)
		return -ENOMEM;

	for_each_sgtable_sg(sgt, sg, i) {
		pg = vmalloc_to_page(data);
		if (!pg) {
			sg_free_table(sgt);
			return -EFAULT;
		}

		s = min_t(int, PAGE_SIZE, size);
		sg_set_page(sg, pg, s, 0);

		size -= s;
		data += s;
	}

	return 0;
}

static int virtwl_vfd_send(struct file *filp, const char __user *buffer,
					       u32 len, int *vfd_fds)
{
	struct virtwl_vfd *vfd = filp->private_data;
	struct virtwl_info *vi = vfd->vi;
	struct fd vfd_files[VIRTWL_SEND_MAX_ALLOCS] = { { 0 } };
	struct virtwl_vfd *vfds[VIRTWL_SEND_MAX_ALLOCS] = { 0 };
#ifdef SEND_VIRTGPU_RESOURCES
	struct dma_buf *virtgpu_dma_bufs[VIRTWL_SEND_MAX_ALLOCS] = { 0 };
	struct dma_fence *virtgpu_dma_fence[VIRTWL_SEND_MAX_ALLOCS] = { 0 };
	bool foreign_id = false;
#endif
	size_t vfd_count = 0;
	size_t vfd_ids_size;
	size_t ctrl_send_size;
	struct virtio_wl_ctrl_vfd_send *ctrl_send;
	u8 *vfd_ids;
	u8 *out_buffer;
	struct completion finish_completion;
	struct scatterlist out_sg;
	struct scatterlist in_sg;
	struct sg_table sgt;
	bool vmalloced;
	int ret;
	int i;

	if (vfd_fds) {
		for (i = 0; i < VIRTWL_SEND_MAX_ALLOCS; i++) {
			struct fd vfd_file;
			int fd = vfd_fds[i];
			struct dma_fence *fence;
			struct dma_buf *dma_buf = ERR_PTR(-EINVAL);
			bool handled = false;

			if (fd < 0)
				break;

			vfd_file = fdget(vfd_fds[i]);
			if (!vfd_file.file) {
				ret = -EBADFD;
				goto put_files;
			}

			if (vfd_file.file->f_op == &virtwl_vfd_fops) {
				vfd_files[i] = vfd_file;
				vfds[i] = vfd_file.file->private_data;
				handled = true;
			}

			if (!handled) {
				fence = sync_file_get_fence(vfd_fds[i]);
				if (fence && is_local_fence(fence)) {
					vfd_files[i] = vfd_file;
					vfds[i] = to_virtwl_fence(fence)->vfd;
					handled = true;
				}
			}

			if (handled) {
				if (vfds[i] && vfds[i]->id) {
					vfd_count++;
					continue;
				}

				ret = -EINVAL;
				goto put_files;
			}

#ifdef SEND_VIRTGPU_RESOURCES
			if (!fence)
				dma_buf = dma_buf_get(vfd_fds[i]);

			handled = true;
			if (!IS_ERR(dma_buf))
				virtgpu_dma_bufs[i] = dma_buf;
			else if (fence && vi->use_send_vfd_v2)
				virtgpu_dma_fence[i] = fence;
			else
				handled = false;

			foreign_id = true;
			vfd_count++;
#endif
			fdput(vfd_file);
			if (!handled) {
				if (fence)
					dma_fence_put(fence);

				ret = IS_ERR(dma_buf) ?
					PTR_ERR(dma_buf) :
					-EINVAL;
				goto put_files;
			}
		}
	}

	/* Empty writes always succeed. */
	if (len == 0 && vfd_count == 0)
		return 0;

	vfd_ids_size = vfd_count * sizeof(__le32);
#ifdef SEND_VIRTGPU_RESOURCES
	if (foreign_id) {
		vfd_ids_size = vfd_count * (vi->use_send_vfd_v2
			? sizeof(struct virtio_wl_ctrl_vfd_send_vfd_v2)
			: sizeof(struct virtio_wl_ctrl_vfd_send_vfd));
	}
#endif
	ctrl_send_size = sizeof(*ctrl_send) + vfd_ids_size + len;
	vmalloced = false;
	if (ctrl_send_size < PAGE_SIZE)
		ctrl_send = kmalloc(ctrl_send_size, GFP_KERNEL);
	else {
		vmalloced = true;
		ctrl_send = vmalloc(ctrl_send_size);
	}
	if (!ctrl_send) {
		ret = -ENOMEM;
		goto put_files;
	}

	vfd_ids = (u8 *)ctrl_send + sizeof(*ctrl_send);
	out_buffer = (u8 *)ctrl_send + ctrl_send_size - len;

	ctrl_send->hdr.type = VIRTIO_WL_CMD_VFD_SEND;
	ctrl_send->hdr.flags = 0;
#ifdef SEND_VIRTGPU_RESOURCES
	if (foreign_id) {
		struct virtio_wl_ctrl_vfd_send_vfd *v1 = NULL;
		struct virtio_wl_ctrl_vfd_send_vfd_v2 *v2 = NULL;

		if (vi->use_send_vfd_v2)
			v2 = (struct virtio_wl_ctrl_vfd_send_vfd_v2 *) vfd_ids;
		else
			v1 = (struct virtio_wl_ctrl_vfd_send_vfd *) vfd_ids;

		ctrl_send->hdr.type = VIRTIO_WL_CMD_VFD_SEND_FOREIGN_ID;
		ret = encode_vfd_ids_foreign(vfds,
			virtgpu_dma_bufs, virtgpu_dma_fence, vfd_count,
			v1, v2);
	} else {
		ret = encode_vfd_ids(vfds, vfd_count, (__le32 *)vfd_ids);
	}
#else
	ret = encode_vfd_ids(vfds, vfd_count, (__le32 *)vfd_ids);
#endif
	if (ret)
		goto free_ctrl_send;
	ctrl_send->vfd_id = vfd->id;
	ctrl_send->vfd_count = vfd_count;

	if (copy_from_user(out_buffer, buffer, len)) {
		ret = -EFAULT;
		goto free_ctrl_send;
	}

	init_completion(&finish_completion);
	if (!vmalloced) {
		sg_init_one(&out_sg, ctrl_send, ctrl_send_size);
		sg_init_one(&in_sg, ctrl_send,
		    sizeof(struct virtio_wl_ctrl_hdr));
		ret = vq_queue_out(vi, &out_sg, &in_sg, &finish_completion,
		    filp->f_flags & O_NONBLOCK);
	} else {
		ret = vmalloc_to_sgt(ctrl_send, ctrl_send_size, &sgt);
		if (ret < 0)
			goto free_ctrl_send;

		sg_init_table(&in_sg, 1);
		sg_set_page(&in_sg, sg_page(sgt.sgl),
			    sizeof(struct virtio_wl_ctrl_hdr), 0);

		ret = vq_queue_out(vi, sgt.sgl, &in_sg, &finish_completion,
		    filp->f_flags & O_NONBLOCK);
	}
	if (ret)
		goto free_sgt;

	wait_for_completion(&finish_completion);

	ret = virtwl_resp_err(ctrl_send->hdr.type);

free_sgt:
	if (vmalloced)
		sg_free_table(&sgt);
free_ctrl_send:
	kvfree(ctrl_send);
put_files:
	for (i = 0; i < VIRTWL_SEND_MAX_ALLOCS; i++) {
		if (vfd_files[i].file)
			fdput(vfd_files[i]);
#ifdef SEND_VIRTGPU_RESOURCES
		if (virtgpu_dma_bufs[i])
			dma_buf_put(virtgpu_dma_bufs[i]);
		if (virtgpu_dma_fence[i])
			dma_fence_put(virtgpu_dma_fence[i]);
#endif
	}
	return ret;
}

static int virtwl_vfd_dmabuf_sync(struct file *filp, u32 flags)
{
	struct virtio_wl_ctrl_vfd_dmabuf_sync *ctrl_dmabuf_sync;
	struct virtwl_vfd *vfd = filp->private_data;
	struct virtwl_info *vi = vfd->vi;
	struct completion finish_completion;
	struct scatterlist out_sg;
	struct scatterlist in_sg;
	int ret = 0;

	ctrl_dmabuf_sync = kzalloc(sizeof(*ctrl_dmabuf_sync), GFP_KERNEL);
	if (!ctrl_dmabuf_sync)
		return -ENOMEM;

	ctrl_dmabuf_sync->hdr.type = VIRTIO_WL_CMD_VFD_DMABUF_SYNC;
	ctrl_dmabuf_sync->vfd_id = vfd->id;
	ctrl_dmabuf_sync->flags = flags;

	sg_init_one(&out_sg, &ctrl_dmabuf_sync->hdr,
		    sizeof(struct virtio_wl_ctrl_vfd_dmabuf_sync));
	sg_init_one(&in_sg, &ctrl_dmabuf_sync->hdr,
		    sizeof(struct virtio_wl_ctrl_hdr));

	init_completion(&finish_completion);
	ret = vq_queue_out(vi, &out_sg, &in_sg, &finish_completion,
			   false /* block */);
	if (ret) {
		dev_warn(&vi->vdev->dev, "failed to queue dmabuf sync vfd id %u: %d\n",
			 vfd->id, ret);
		goto free_ctrl_dmabuf_sync;
	}

	wait_for_completion(&finish_completion);

free_ctrl_dmabuf_sync:
	kfree(ctrl_dmabuf_sync);
	return ret;
}

static ssize_t virtwl_vfd_read(struct file *filp, char __user *buffer,
			       size_t size, loff_t *pos)
{
	return virtwl_vfd_recv(filp, buffer, size, NULL, NULL);
}

static ssize_t virtwl_vfd_write(struct file *filp, const char __user *buffer,
				size_t size, loff_t *pos)
{
	int ret = 0;

	if (size > U32_MAX)
		size = U32_MAX;

	ret = virtwl_vfd_send(filp, buffer, size, NULL);
	if (ret)
		return ret;

	return size;
}

static int virtwl_vfd_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct virtwl_vfd *vfd = filp->private_data;
	unsigned long vm_size = vma->vm_end - vma->vm_start;
	int ret = 0;

	if (!vfd->pfn)
		return -EACCES;

	if (vm_size + (vma->vm_pgoff << PAGE_SHIFT) > PAGE_ALIGN(vfd->size))
		return -EINVAL;

	ret = io_remap_pfn_range(vma, vma->vm_start, vfd->pfn, vm_size,
				 vma->vm_page_prot);
	if (ret)
		return ret;

	vma->vm_flags |= VM_PFNMAP | VM_IO | VM_DONTEXPAND | VM_DONTDUMP;

	return ret;
}

static unsigned int virtwl_vfd_poll(struct file *filp,
				    struct poll_table_struct *wait)
{
	struct virtwl_vfd *vfd = filp->private_data;
	struct virtwl_info *vi = vfd->vi;
	unsigned int mask = 0;

	mutex_lock(&vi->vq_locks[VIRTWL_VQ_OUT]);
	poll_wait(filp, &vi->out_waitq, wait);
	if (vi->vqs[VIRTWL_VQ_OUT]->num_free)
		mask |= POLLOUT | POLLWRNORM;
	mutex_unlock(&vi->vq_locks[VIRTWL_VQ_OUT]);

	mutex_lock(&vfd->lock);
	poll_wait(filp, &vfd->in_waitq, wait);
	if (!list_empty(&vfd->in_queue))
		mask |= POLLIN | POLLRDNORM;
	if (vfd->hungup)
		mask |= POLLHUP;
	mutex_unlock(&vfd->lock);

	return mask;
}

static int virtwl_vfd_release(struct inode *inodep, struct file *filp)
{
	struct virtwl_vfd *vfd = filp->private_data;
	uint32_t vfd_id = vfd->id;
	int ret;

	/*
	 * If release is called, filp must be out of references and we have the
	 * last reference.
	 */
	ret = do_vfd_close(vfd);
	if (ret)
		dev_warn(&vfd->vi->vdev->dev, "failed to release vfd id %u: %d\n",
			 vfd_id, ret);
	return 0;
}

static int virtwl_open(struct inode *inodep, struct file *filp)
{
	struct virtwl_info *vi = container_of(filp->private_data,
					      struct virtwl_info, miscdev);

	filp->private_data = vi;

	return 0;
}

static struct virtwl_vfd *do_new(struct virtwl_info *vi,
				 struct virtwl_ioctl_new *ioctl_new,
				 size_t ioctl_new_size, bool nonblock)
{
	struct virtio_wl_ctrl_vfd_new *ctrl_new;
	struct virtwl_vfd *vfd;
	struct completion finish_completion;
	struct scatterlist out_sg;
	struct scatterlist in_sg;
	int ret = 0;

	if (ioctl_new->type != VIRTWL_IOCTL_NEW_CTX &&
		ioctl_new->type != VIRTWL_IOCTL_NEW_CTX_NAMED &&
		ioctl_new->type != VIRTWL_IOCTL_NEW_ALLOC &&
		ioctl_new->type != VIRTWL_IOCTL_NEW_PIPE_READ &&
		ioctl_new->type != VIRTWL_IOCTL_NEW_PIPE_WRITE &&
		ioctl_new->type != VIRTWL_IOCTL_NEW_DMABUF)
		return ERR_PTR(-EINVAL);

	ctrl_new = kzalloc(sizeof(*ctrl_new), GFP_KERNEL);
	if (!ctrl_new)
		return ERR_PTR(-ENOMEM);

	vfd = virtwl_vfd_alloc(vi);
	if (!vfd) {
		ret = -ENOMEM;
		goto free_ctrl_new;
	}

	/*
	 * We keep ->vfds_lock until we fully setup new vfd. By doing so we
	 * prevent this vfd from being looked up and being used in some other
	 * context concurrently (e.g. virtwl_vfd_mmap()).
	 */
	mutex_lock(&vi->vfds_lock);
	ret = idr_alloc(&vi->vfds, vfd, 1, VIRTWL_MAX_ALLOC, GFP_KERNEL);
	if (ret <= 0)
		goto unlock_free_vfd;

	vfd->id = ret;
	ret = 0;

	ctrl_new->vfd_id = vfd->id;
	switch (ioctl_new->type) {
	case VIRTWL_IOCTL_NEW_CTX:
		ctrl_new->hdr.type = VIRTIO_WL_CMD_VFD_NEW_CTX;
		ctrl_new->flags = VIRTIO_WL_VFD_WRITE | VIRTIO_WL_VFD_READ;
		break;
	case VIRTWL_IOCTL_NEW_CTX_NAMED:
		ctrl_new->hdr.type = VIRTIO_WL_CMD_VFD_NEW_CTX_NAMED;
		ctrl_new->flags = VIRTIO_WL_VFD_WRITE | VIRTIO_WL_VFD_READ;
		memcpy(ctrl_new->name, ioctl_new->name, sizeof(ctrl_new->name));
		break;
	case VIRTWL_IOCTL_NEW_ALLOC:
		ctrl_new->hdr.type = VIRTIO_WL_CMD_VFD_NEW;
		ctrl_new->size = PAGE_ALIGN(ioctl_new->size);
		break;
	case VIRTWL_IOCTL_NEW_PIPE_READ:
		ctrl_new->hdr.type = VIRTIO_WL_CMD_VFD_NEW_PIPE;
		ctrl_new->flags = VIRTIO_WL_VFD_READ;
		break;
	case VIRTWL_IOCTL_NEW_PIPE_WRITE:
		ctrl_new->hdr.type = VIRTIO_WL_CMD_VFD_NEW_PIPE;
		ctrl_new->flags = VIRTIO_WL_VFD_WRITE;
		break;
	case VIRTWL_IOCTL_NEW_DMABUF:
		/* Make sure ioctl_new contains enough data for NEW_DMABUF. */
		if (ioctl_new_size == sizeof(*ioctl_new)) {
			ctrl_new->hdr.type = VIRTIO_WL_CMD_VFD_NEW_DMABUF;
			/* FIXME: convert from host byte order. */
			memcpy(&ctrl_new->dmabuf, &ioctl_new->dmabuf,
			       sizeof(ioctl_new->dmabuf));
			break;
		}
		/* fall-through */
	default:
		ret = -EINVAL;
		goto unlock_free_vfd;
	}

	init_completion(&finish_completion);
	sg_init_one(&out_sg, ctrl_new, sizeof(*ctrl_new));
	sg_init_one(&in_sg, ctrl_new, sizeof(*ctrl_new));

	ret = vq_queue_out(vi, &out_sg, &in_sg, &finish_completion, nonblock);
	if (ret)
		goto unlock_free_vfd;

	wait_for_completion(&finish_completion);

	ret = virtwl_resp_err(ctrl_new->hdr.type);
	if (ret)
		goto unlock_free_vfd;

	vfd->size = ctrl_new->size;
	vfd->pfn = ctrl_new->pfn;
	vfd->flags = ctrl_new->flags;

	if (ioctl_new->type == VIRTWL_IOCTL_NEW_DMABUF) {
		/* FIXME: convert to host byte order. */
		memcpy(&ioctl_new->dmabuf, &ctrl_new->dmabuf,
		       sizeof(ctrl_new->dmabuf));
	}

	mutex_unlock(&vi->vfds_lock);
	kfree(ctrl_new);

	return vfd;

unlock_free_vfd:
	mutex_unlock(&vi->vfds_lock);
	/* this is safe since the id cannot change after the vfd is created */
	if (vfd->id)
		virtwl_vfd_lock_unlink(vfd);
	virtwl_vfd_free(vfd);
free_ctrl_new:
	kfree(ctrl_new);
	return ERR_PTR(ret);
}

static long virtwl_ioctl_send(struct file *filp, void __user *ptr)
{
	struct virtwl_ioctl_txn ioctl_send;
	void __user *user_data = ptr + sizeof(struct virtwl_ioctl_txn);
	int ret;

	ret = copy_from_user(&ioctl_send, ptr, sizeof(struct virtwl_ioctl_txn));
	if (ret)
		return -EFAULT;

	return virtwl_vfd_send(filp, user_data, ioctl_send.len, ioctl_send.fds);
}

static long virtwl_ioctl_recv(struct file *filp, void __user *ptr)
{
	struct virtwl_ioctl_txn ioctl_recv;
	void __user *user_data = ptr + sizeof(struct virtwl_ioctl_txn);
	int __user *user_fds = (int __user *)ptr;
	size_t vfd_count = VIRTWL_SEND_MAX_ALLOCS;
	struct virtwl_vfd *vfds[VIRTWL_SEND_MAX_ALLOCS] = { 0 };
	int fds[VIRTWL_SEND_MAX_ALLOCS];
	struct file *files[VIRTWL_SEND_MAX_ALLOCS] = { 0 };
	size_t i;
	int ret = 0;

	for (i = 0; i < VIRTWL_SEND_MAX_ALLOCS; i++)
		fds[i] = -1;

	ret = copy_from_user(&ioctl_recv, ptr, sizeof(struct virtwl_ioctl_txn));
	if (ret)
		return -EFAULT;

	ret = virtwl_vfd_recv(filp, user_data, ioctl_recv.len, vfds,
			      &vfd_count);
	if (ret < 0)
		return ret;

	ret = copy_to_user(&((struct virtwl_ioctl_txn __user *)ptr)->len, &ret,
			   sizeof(ioctl_recv.len));
	if (ret) {
		ret = -EFAULT;
		goto free_vfds;
	}

	for (i = 0; i < vfd_count; i++) {
		struct file *file;
		int flags;

		if (vfds[i]->flags & VIRTIO_WL_VFD_FENCE) {
			struct virtwl_vfd *vfd = filp->private_data;
			struct virtwl_info *vi = vfd->vi;
			struct virtwl_fence *fence;
			struct sync_file *sync;

			fence = kzalloc(sizeof(*fence), GFP_KERNEL);
			if (!fence) {
				ret = -ENOMEM;
				goto free_vfds;
			}
			fence->vfd = vfds[i];
			dma_fence_init(&fence->base, &virtwl_fence_ops,
				       &vi->fence_lock, fence->vfd->id, 1);

			// If something fails, cleanup of the dma_fence will
			// clean up this vfd.
			vfds[i] = NULL;

			sync = sync_file_create(&fence->base);
			dma_fence_put(&fence->base);
			if (!sync) {
				// Maybe not -ENOMEM, but sync_file_create
				// doesn't expose what actually went wrong.
				ret = -ENOMEM;
				goto free_vfds;
			}

			spin_lock(&vi->fence_lock);
			if (!fence->vfd->hungup)
				fence->vfd->fence = dma_fence_get(&fence->base);
			else
				dma_fence_signal_locked(&fence->base);
			spin_unlock(&vi->fence_lock);

			file = sync->file;
			flags = O_CLOEXEC;
		} else {
			flags = virtwl_vfd_file_flags(vfds[i]) | O_CLOEXEC;
			file = anon_inode_getfile("[virtwl_vfd]", &virtwl_vfd_fops,
						  vfds[i], flags);
		}

		if (IS_ERR(file)) {
			ret = PTR_ERR(file);
			goto free_vfds;
		}

		vfds[i] = NULL;
		files[i] = file;

		fds[i] = get_unused_fd_flags(flags);
		if (fds[i] < 0)
			goto free_vfds;
	}

	ret = copy_to_user(user_fds, fds, sizeof(int) * VIRTWL_SEND_MAX_ALLOCS);
	if (ret) {
		ret = -EFAULT;
		goto free_vfds;
	}

	for (i = 0; i < vfd_count; i++)
		fd_install(fds[i], files[i]);

	return 0;

free_vfds:
	for (i = 0; i < vfd_count; i++) {
		if (vfds[i])
			do_vfd_close(vfds[i]);
		if (files[i])
			fput(files[i]);
		if (fds[i] >= 0)
			put_unused_fd(fds[i]);
	}
	return ret;
}

static long virtwl_ioctl_dmabuf_sync(struct file *filp, void __user *ptr)
{
	struct virtwl_ioctl_dmabuf_sync ioctl_dmabuf_sync;
	int ret;

	ret = copy_from_user(&ioctl_dmabuf_sync, ptr,
			     sizeof(struct virtwl_ioctl_dmabuf_sync));
	if (ret)
		return -EFAULT;

	if (ioctl_dmabuf_sync.flags & ~DMA_BUF_SYNC_VALID_FLAGS_MASK)
		return -EINVAL;

	return virtwl_vfd_dmabuf_sync(filp, ioctl_dmabuf_sync.flags);
}

static long virtwl_vfd_ioctl(struct file *filp, unsigned int cmd,
			     void __user *ptr)
{
	switch (cmd) {
	case VIRTWL_IOCTL_SEND:
		return virtwl_ioctl_send(filp, ptr);
	case VIRTWL_IOCTL_RECV:
		return virtwl_ioctl_recv(filp, ptr);
	case VIRTWL_IOCTL_DMABUF_SYNC:
		return virtwl_ioctl_dmabuf_sync(filp, ptr);
	default:
		return -ENOTTY;
	}
}

static long virtwl_ioctl_new(struct file *filp, void __user *ptr,
			     size_t in_size)
{
	struct virtwl_info *vi = filp->private_data;
	struct virtwl_vfd *vfd;
	struct virtwl_ioctl_new ioctl_new = {};
	size_t size = min(in_size, sizeof(ioctl_new));
	struct file *file;
	int ret, flags;

	ret = copy_from_user(&ioctl_new, ptr, size);
	if (ret)
		return -EFAULT;

	vfd = do_new(vi, &ioctl_new, size, filp->f_flags & O_NONBLOCK);
	if (IS_ERR(vfd))
		return PTR_ERR(vfd);

	flags = virtwl_vfd_file_flags(vfd) | O_CLOEXEC;
	file = anon_inode_getfile("[virtwl_vfd]", &virtwl_vfd_fops, vfd, flags);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto err_close_vfd;
	}

	ret = get_unused_fd_flags(flags);
	if (ret < 0)
		goto err_put_file;
	ioctl_new.fd = ret;

	ret = copy_to_user(ptr, &ioctl_new, size);
	if (ret)
		goto err_put_fd;

	fd_install(ioctl_new.fd, file);

	return 0;

err_put_fd:
	put_unused_fd(ioctl_new.fd);
err_put_file:
	fput(file);
err_close_vfd:
	do_vfd_close(vfd);
	return ret;
}

static long virtwl_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	void __user *ptr = (void __user *)arg;

	if (filp->f_op == &virtwl_vfd_fops)
		return virtwl_vfd_ioctl(filp, cmd, ptr);

	switch (_IOC_NR(cmd)) {
	case _IOC_NR(VIRTWL_IOCTL_NEW):
		return virtwl_ioctl_new(filp, ptr, _IOC_SIZE(cmd));
	default:
		return -ENOTTY;
	}
}

static const struct file_operations virtwl_fops = {
	.open = virtwl_open,
	.unlocked_ioctl = virtwl_ioctl,
	.compat_ioctl = virtwl_ioctl,
};

static const struct file_operations virtwl_vfd_fops = {
	.read = virtwl_vfd_read,
	.write = virtwl_vfd_write,
	.mmap = virtwl_vfd_mmap,
	.poll = virtwl_vfd_poll,
	.unlocked_ioctl = virtwl_ioctl,
	.compat_ioctl = virtwl_ioctl,
	.release = virtwl_vfd_release,
};

static int probe_common(struct virtio_device *vdev)
{
	int i;
	int ret;
	struct virtwl_info *vi = NULL;
	vq_callback_t *vq_callbacks[] = { vq_in_cb, vq_out_cb };
	static const char * const vq_names[] = { "in", "out" };
	static atomic_t virtwl_num = ATOMIC_INIT(-1);

	vi = kzalloc(sizeof(struct virtwl_info), GFP_KERNEL);
	if (!vi)
		return -ENOMEM;

	vdev->priv = vi;
	vi->vdev = vdev;

	snprintf(vi->name, sizeof(vi->name), "wl%d", atomic_inc_return(&virtwl_num));
	vi->miscdev.minor = MISC_DYNAMIC_MINOR;
	vi->miscdev.name = vi->name;
	vi->miscdev.parent = &vdev->dev;
	vi->miscdev.fops = &virtwl_fops;

	ret = misc_register(&vi->miscdev);
	if (ret) {
		dev_warn(&vdev->dev, "failed to add virtio wayland misc device to system: %d\n",
			 ret);
		goto free_vi;
	}

	for (i = 0; i < VIRTWL_QUEUE_COUNT; i++)
		mutex_init(&vi->vq_locks[i]);

	ret = virtio_find_vqs(vdev, VIRTWL_QUEUE_COUNT, vi->vqs, vq_callbacks,
			      vq_names, NULL);
	if (ret) {
		dev_warn(&vdev->dev, "failed to find virtio wayland queues: %d\n", ret);
		goto unregister_dev;
	}

	INIT_LIST_HEAD(&vi->fence_release_list);

	INIT_WORK(&vi->in_vq_work, vq_in_work_handler);
	INIT_WORK(&vi->out_vq_work, vq_out_work_handler);
	INIT_WORK(&vi->fence_release_work, virtwl_fence_release_handler);
	init_waitqueue_head(&vi->out_waitq);

	mutex_init(&vi->vfds_lock);
	idr_init(&vi->vfds);
	spin_lock_init(&vi->fence_lock);

	vi->use_send_vfd_v2 = virtio_has_feature(vdev, VIRTIO_WL_F_SEND_FENCES);

	/* lock is unneeded as we have unique ownership */
	ret = vq_fill_locked(vi->vqs[VIRTWL_VQ_IN]);
	if (ret) {
		dev_warn(&vdev->dev, "failed to fill in virtqueue: %d", ret);
		goto unregister_dev;
	}

	virtio_device_ready(vdev);
	virtqueue_kick(vi->vqs[VIRTWL_VQ_IN]);

	return 0;

unregister_dev:
	misc_deregister(&vi->miscdev);
free_vi:
	kfree(vi);
	return ret;
}

static void remove_common(struct virtio_device *vdev)
{
	struct virtwl_info *vi = vdev->priv;

	misc_deregister(&vi->miscdev);
	kfree(vi);
}

static int virtwl_probe(struct virtio_device *vdev)
{
	return probe_common(vdev);
}

static void virtwl_remove(struct virtio_device *vdev)
{
	remove_common(vdev);
}

static void virtwl_scan(struct virtio_device *vdev)
{
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_WL, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features_legacy[] = {
	VIRTIO_WL_F_TRANS_FLAGS
};

static unsigned int features[] = {
	VIRTIO_WL_F_TRANS_FLAGS,
	VIRTIO_WL_F_SEND_FENCES,
};

static struct virtio_driver virtio_wl_driver = {
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.feature_table_legacy = features_legacy,
	.feature_table_size_legacy = ARRAY_SIZE(features_legacy),
	.probe =	virtwl_probe,
	.remove =	virtwl_remove,
	.scan =		virtwl_scan,
};

module_virtio_driver(virtio_wl_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio wayland driver");
MODULE_LICENSE("GPL");
