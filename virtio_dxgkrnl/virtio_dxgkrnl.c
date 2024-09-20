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

#include "dxgkrnl.h"
#include "virtio_dxgkrnl.h"

enum virtio_dxgkrnl_vq {
	VIRTIO_DXGKRNL_VQ_TX,
	VIRTIO_DXGKRNL_VQ_RX,
	VIRTIO_DXGKRNL_VQ_EVENT,
	VIRTIO_DXGKRNL_VQ_MAX
};

struct virtio_dxgkrnl {
	struct virtio_device *vdev;
	struct virtqueue *tx_vq;
	struct virtqueue *rx_vq;
};

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_DXGKRNL, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static void dxgkrnl_tx_callback(struct virtqueue *vq) {};
static void dxgkrnl_rx_callback(struct virtqueue *vq) {};

int dxgglobal_init_global_channel(void)
{
	return 1;
};

void dxgglobal_destroy_global_channel(void) {};

int dxgvmbuschannel_init(struct dxgvmbuschannel *ch, struct hv_device *hdev)
{
	return 0;
};

void dxgvmbuschannel_destroy(struct dxgvmbuschannel *ch) {};

int dxgvmb_send_async_msg(struct dxgvmbuschannel *channel, void *command,
			  u32 cmd_size)
{
	return 1;
};

int dxgvmb_send_sync_msg(struct dxgvmbuschannel *channel, void *command,
			 u32 cmd_size, void *result, u32 result_size)
{
	return 1;
};

static int init_vqs(struct virtio_dxgkrnl *vp)
{
	vq_callback_t *callbacks[VIRTIO_DXGKRNL_VQ_MAX];
	struct virtqueue *vqs[VIRTIO_DXGKRNL_VQ_MAX];
	const char *names[VIRTIO_DXGKRNL_VQ_MAX];
	int err;

	callbacks[VIRTIO_DXGKRNL_VQ_TX] = dxgkrnl_tx_callback;
	names[VIRTIO_DXGKRNL_VQ_TX] = "dxgkrnl_tx";

	callbacks[VIRTIO_DXGKRNL_VQ_RX] = dxgkrnl_rx_callback;
	names[VIRTIO_DXGKRNL_VQ_RX] = "dxgkrnl_rx";

	err = vp->vdev->config->find_vqs(vp->vdev, VIRTIO_DXGKRNL_VQ_MAX, vqs,
					 callbacks, names, NULL, NULL);
	if (err)
		return err;

	vp->tx_vq = vqs[VIRTIO_DXGKRNL_VQ_TX];
	vp->rx_vq = vqs[VIRTIO_DXGKRNL_VQ_RX];

	return 0;
}

static int virtdxgkrnl_probe(struct virtio_device *vdev)
{
	struct virtio_dxgkrnl *vp;
	int err;

	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s: config access disabled\n", __func__);
		return -EINVAL;
	}

	vp = kzalloc(sizeof(*vp), GFP_KERNEL);
	if (!vp)
		return -ENOMEM;

	vp->vdev = vdev;
	vdev->priv = vp;

	err = init_vqs(vp);
	if (err)
		goto out_free_vp;

	virtio_device_ready(vdev);

	return 0;

out_free_vp:
	kfree(vp);
	return err;
}

static void remove_common(struct virtio_dxgkrnl *vp)
{
	/* Now we reset the device so we can clean up the queues. */
	vp->vdev->config->reset(vp->vdev);

	vp->vdev->config->del_vqs(vp->vdev);
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

static unsigned int features[] = {};

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
