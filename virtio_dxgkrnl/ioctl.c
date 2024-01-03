// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2019, Microsoft Corporation.
 *
 * Author:
 *   Iouri Tarassov <iourit@linux.microsoft.com>
 *
 * Dxgkrnl Graphics Driver
 * Ioctl implementation
 *
 */

#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/anon_inodes.h>
#include <linux/mman.h>

#include "dxgkrnl.h"
#include "dxgvmbus.h"

#undef pr_fmt
#define pr_fmt(fmt)	"dxgk:err: " fmt
#undef dev_fmt
#define dev_fmt(fmt)	"dxgk: " fmt

struct ioctl_desc {
	int (*ioctl_callback)(struct dxgprocess *p, void __user *arg);
	u32 ioctl;
	u32 arg_size;
};
static struct ioctl_desc ioctls[LX_IO_MAX + 1];

static char *errorstr(int ret)
{
	return ret < 0 ? "err" : "";
}

static int dxgk_open_adapter_from_luid(struct dxgprocess *process,
						   void *__user inargs)
{
	struct d3dkmt_openadapterfromluid args;
	int ret;
	struct dxgadapter *entry;
	struct dxgadapter *adapter = NULL;
	struct d3dkmt_openadapterfromluid *__user result = inargs;

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s Faled to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	dxgglobal_acquire_adapter_list_lock(DXGLOCK_SHARED);
	dxgglobal_acquire_process_adapter_lock();

	list_for_each_entry(entry, &dxgglobal->adapter_list_head,
			    adapter_list_entry) {
		if (dxgadapter_acquire_lock_shared(entry) == 0) {
			dev_dbg(dxgglobaldev, "Compare luids: %d:%d  %d:%d",
				    entry->luid.b, entry->luid.a,
				    args.adapter_luid.b, args.adapter_luid.a);
			if (*(u64 *) &entry->luid ==
			    *(u64 *) &args.adapter_luid) {
				ret =
				    dxgprocess_open_adapter(process, entry,
						    &args.adapter_handle);

				if (ret >= 0) {
					ret = copy_to_user(
						&result->adapter_handle,
						&args.adapter_handle,
						sizeof(struct d3dkmthandle));
					if (ret)
						ret = -EINVAL;
				}
				adapter = entry;
			}
			dxgadapter_release_lock_shared(entry);
			if (adapter)
				break;
		}
	}

	dxgglobal_release_process_adapter_lock();
	dxgglobal_release_adapter_list_lock(DXGLOCK_SHARED);

	if (args.adapter_handle.v == 0)
		ret = -EINVAL;

cleanup:

	if (ret < 0)
		dxgprocess_close_adapter(process, args.adapter_handle);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgkp_enum_adapters(struct dxgprocess *process,
		    union d3dkmt_enumadapters_filter filter,
		    u32 adapter_count_max,
		    struct d3dkmt_adapterinfo *__user info_out,
		    u32 * __user adapter_count_out)
{
	int ret = 0;
	struct dxgadapter *entry;
	struct d3dkmt_adapterinfo *info = NULL;
	struct dxgadapter **adapters = NULL;
	int adapter_count = 0;
	int i;

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);
	if (info_out == NULL || adapter_count_max == 0) {
		dev_dbg(dxgglobaldev, "buffer is NULL");
		ret = copy_to_user(adapter_count_out,
				   &dxgglobal->num_adapters, sizeof(u32));
		if (ret) {
			pr_err("%s copy_to_user faled",	__func__);
			ret = -EINVAL;
		}
		goto cleanup;
	}

	if (adapter_count_max > 0xFFFF) {
		pr_err("too many adapters");
		ret = -EINVAL;
		goto cleanup;
	}

	info = vzalloc(sizeof(struct d3dkmt_adapterinfo) * adapter_count_max);
	if (info == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}

	adapters = vzalloc(sizeof(struct dxgadapter *) * adapter_count_max);
	if (adapters == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}

	dxgglobal_acquire_adapter_list_lock(DXGLOCK_SHARED);
	dxgglobal_acquire_process_adapter_lock();

	list_for_each_entry(entry, &dxgglobal->adapter_list_head,
			    adapter_list_entry) {
		if (dxgadapter_acquire_lock_shared(entry) == 0) {
			struct d3dkmt_adapterinfo *inf = &info[adapter_count];

			ret = dxgprocess_open_adapter(process, entry,
						      &inf->adapter_handle);
			if (ret >= 0) {
				inf->adapter_luid = entry->luid;
				adapters[adapter_count] = entry;
				dev_dbg(dxgglobaldev, "adapter: %x %x:%x",
					    inf->adapter_handle.v,
					    inf->adapter_luid.b,
					    inf->adapter_luid.a);
				adapter_count++;
			}
			dxgadapter_release_lock_shared(entry);
		}
		if (ret < 0)
			break;
	}

	dxgglobal_release_process_adapter_lock();
	dxgglobal_release_adapter_list_lock(DXGLOCK_SHARED);

	if (adapter_count > adapter_count_max) {
		ret = STATUS_BUFFER_TOO_SMALL;
		dev_dbg(dxgglobaldev, "Too many adapters");
		ret = copy_to_user(adapter_count_out,
				   &dxgglobal->num_adapters, sizeof(u32));
		if (ret) {
			pr_err("%s copy_to_user failed", __func__);
			ret = -EINVAL;
		}
		goto cleanup;
	}

	ret = copy_to_user(adapter_count_out, &adapter_count,
			   sizeof(adapter_count));
	if (ret) {
		pr_err("%s failed to copy adapter_count", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	ret = copy_to_user(info_out, info, sizeof(info[0]) * adapter_count);
	if (ret) {
		pr_err("%s failed to copy adapter info", __func__);
		ret = -EINVAL;
	}

cleanup:

	if (ret >= 0) {
		dev_dbg(dxgglobaldev, "found %d adapters", adapter_count);
		goto success;
	}
	if (info) {
		for (i = 0; i < adapter_count; i++)
			dxgprocess_close_adapter(process,
						 info[i].adapter_handle);
	}
success:
	if (info)
		vfree(info);
	if (adapters)
		vfree(adapters);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_enum_adapters(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_enumadapters2 args;
	int ret;
	struct dxgadapter *entry;
	struct d3dkmt_adapterinfo *info = NULL;
	struct dxgadapter **adapters = NULL;
	int adapter_count = 0;
	int i;

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.adapters == NULL) {
		dev_dbg(dxgglobaldev, "buffer is NULL");
		args.num_adapters = dxgglobal->num_adapters;
		ret = copy_to_user(inargs, &args, sizeof(args));
		if (ret) {
			pr_err("%s failed to copy args to user", __func__);
			ret = -EINVAL;
		}
		goto cleanup;
	}
	if (args.num_adapters < dxgglobal->num_adapters) {
		args.num_adapters = dxgglobal->num_adapters;
		dev_dbg(dxgglobaldev, "buffer is too small");
		ret = -EOVERFLOW;
		goto cleanup;
	}

	if (args.num_adapters > D3DKMT_ADAPTERS_MAX) {
		dev_dbg(dxgglobaldev, "too many adapters");
		ret = -EINVAL;
		goto cleanup;
	}

	info = vzalloc(sizeof(struct d3dkmt_adapterinfo) * args.num_adapters);
	if (info == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}

	adapters = vzalloc(sizeof(struct dxgadapter *) * args.num_adapters);
	if (adapters == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}

	dxgglobal_acquire_adapter_list_lock(DXGLOCK_SHARED);
	dxgglobal_acquire_process_adapter_lock();

	list_for_each_entry(entry, &dxgglobal->adapter_list_head,
			    adapter_list_entry) {
		if (dxgadapter_acquire_lock_shared(entry) == 0) {
			struct d3dkmt_adapterinfo *inf = &info[adapter_count];

			ret = dxgprocess_open_adapter(process, entry,
						      &inf->adapter_handle);
			if (ret >= 0) {
				inf->adapter_luid = entry->luid;
				adapters[adapter_count] = entry;
				dev_dbg(dxgglobaldev, "adapter: %x %llx",
					    inf->adapter_handle.v,
					    *(u64 *) &inf->adapter_luid);
				adapter_count++;
			}
			dxgadapter_release_lock_shared(entry);
		}
		if (ret < 0)
			break;
	}

	dxgglobal_release_process_adapter_lock();
	dxgglobal_release_adapter_list_lock(DXGLOCK_SHARED);

	args.num_adapters = adapter_count;

	ret = copy_to_user(inargs, &args, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy args to user", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	ret = copy_to_user(args.adapters, info,
			   sizeof(info[0]) * args.num_adapters);
	if (ret) {
		pr_err("%s failed to copy adapter info to user", __func__);
		ret = -EINVAL;
	}

cleanup:

	if (ret < 0) {
		if (info) {
			for (i = 0; i < args.num_adapters; i++) {
				dxgprocess_close_adapter(process,
							info[i].adapter_handle);
			}
		}
	} else {
		dev_dbg(dxgglobaldev, "found %d adapters", args.num_adapters);
	}

	if (info)
		vfree(info);
	if (adapters)
		vfree(adapters);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_enum_adapters3(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_enumadapters3 args;
	int ret;

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	ret = dxgkp_enum_adapters(process, args.filter,
				  args.adapter_count,
				  args.adapters,
				  &((struct d3dkmt_enumadapters3 *)inargs)->
				  adapter_count);

cleanup:

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_close_adapter(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmthandle args;
	int ret;

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	ret = dxgprocess_close_adapter(process, args);
	if (ret < 0)
		pr_err("%s failed", __func__);

cleanup:

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_query_adapter_info(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_queryadapterinfo args;
	int ret;
	struct dxgadapter *adapter = NULL;

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.private_data_size > DXG_MAX_VM_BUS_PACKET_SIZE ||
	    args.private_data_size == 0) {
		pr_err("invalid private data size");
		ret = -EINVAL;
		goto cleanup;
	}

	dev_dbg(dxgglobaldev, "Type: %d Size: %x",
		args.type, args.private_data_size);

	adapter = dxgprocess_adapter_by_handle(process, args.adapter);
	if (adapter == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0)
		goto cleanup;

	ret = dxgvmb_send_query_adapter_info(process, adapter, &args);

	dxgadapter_release_lock_shared(adapter);

cleanup:

	if (adapter)
		kref_put(&adapter->adapter_kref, dxgadapter_release);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_create_device(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_createdevice args;
	int ret;
	struct dxgadapter *adapter = NULL;
	struct dxgdevice *device = NULL;
	struct d3dkmthandle host_device_handle = {};
	bool adapter_locked = false;

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	/* The call acquires reference on the adapter */
	adapter = dxgprocess_adapter_by_handle(process, args.adapter);
	if (adapter == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	device = dxgdevice_create(adapter, process);
	if (device == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}

	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0)
		goto cleanup;

	adapter_locked = true;

	host_device_handle = dxgvmb_send_create_device(adapter, process, &args);
	if (host_device_handle.v) {
		ret = copy_to_user(&((struct d3dkmt_createdevice *)inargs)->
				   device, &host_device_handle,
				   sizeof(struct d3dkmthandle));
		if (ret) {
			pr_err("%s failed to copy device handle", __func__);
			ret = -EINVAL;
			goto cleanup;
		}

		hmgrtable_lock(&process->handle_table, DXGLOCK_EXCL);
		ret = hmgrtable_assign_handle(&process->handle_table, device,
					      HMGRENTRY_TYPE_DXGDEVICE,
					      host_device_handle);
		if (ret >= 0) {
			device->handle = host_device_handle;
			device->handle_valid = 1;
			device->object_state = DXGOBJECTSTATE_ACTIVE;
		}
		hmgrtable_unlock(&process->handle_table, DXGLOCK_EXCL);
	}

cleanup:

	if (ret < 0) {
		if (host_device_handle.v)
			dxgvmb_send_destroy_device(adapter, process,
						   host_device_handle);
		if (device)
			dxgdevice_destroy(device);
	}

	if (adapter_locked)
		dxgadapter_release_lock_shared(adapter);

	if (adapter)
		kref_put(&adapter->adapter_kref, dxgadapter_release);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_destroy_device(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_destroydevice args;
	int ret;
	struct dxgadapter *adapter = NULL;
	struct dxgdevice *device = NULL;

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	hmgrtable_lock(&process->handle_table, DXGLOCK_EXCL);
	device = hmgrtable_get_object_by_type(&process->handle_table,
					      HMGRENTRY_TYPE_DXGDEVICE,
					      args.device);
	if (device) {
		hmgrtable_free_handle(&process->handle_table,
				      HMGRENTRY_TYPE_DXGDEVICE, args.device);
		device->handle_valid = 0;
	}
	hmgrtable_unlock(&process->handle_table, DXGLOCK_EXCL);

	if (device == NULL) {
		pr_err("invalid device handle: %x", args.device.v);
		ret = -EINVAL;
		goto cleanup;
	}

	adapter = device->adapter;

	dxgdevice_destroy(device);

	if (dxgadapter_acquire_lock_shared(adapter) == 0) {
		dxgvmb_send_destroy_device(adapter, process, args.device);
		dxgadapter_release_lock_shared(adapter);
	}

cleanup:

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_create_context_virtual(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_createcontextvirtual args;
	int ret;
	struct dxgadapter *adapter = NULL;
	struct dxgdevice *device = NULL;
	struct dxgcontext *context = NULL;
	struct d3dkmthandle host_context_handle = {};
	bool device_lock_acquired = false;

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	/*
	 * The call acquires reference on the device. It is safe to access the
	 * adapter, because the device holds reference on it.
	 */
	device = dxgprocess_device_by_handle(process, args.device);
	if (device == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	ret = dxgdevice_acquire_lock_shared(device);
	if (ret < 0)
		goto cleanup;

	device_lock_acquired = true;

	adapter = device->adapter;
	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0) {
		adapter = NULL;
		goto cleanup;
	}

	context = dxgcontext_create(device);
	if (context == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}

	host_context_handle = dxgvmb_send_create_context(adapter,
							 process, &args);
	if (host_context_handle.v) {
		hmgrtable_lock(&process->handle_table, DXGLOCK_EXCL);
		ret = hmgrtable_assign_handle(&process->handle_table, context,
					      HMGRENTRY_TYPE_DXGCONTEXT,
					      host_context_handle);
		if (ret >= 0)
			context->handle = host_context_handle;
		hmgrtable_unlock(&process->handle_table, DXGLOCK_EXCL);
		if (ret < 0)
			goto cleanup;
		ret = copy_to_user(&((struct d3dkmt_createcontextvirtual *)
				   inargs)->context, &host_context_handle,
				   sizeof(struct d3dkmthandle));
		if (ret) {
			pr_err("%s failed to copy context handle", __func__);
			ret = -EINVAL;
		}
	} else {
		pr_err("invalid host handle");
		ret = -EINVAL;
	}

cleanup:

	if (ret < 0) {
		if (host_context_handle.v) {
			dxgvmb_send_destroy_context(adapter, process,
						    host_context_handle);
		}
		if (context)
			dxgcontext_destroy_safe(process, context);
	}

	if (adapter)
		dxgadapter_release_lock_shared(adapter);

	if (device) {
		if (device_lock_acquired)
			dxgdevice_release_lock_shared(device);
		kref_put(&device->device_kref, dxgdevice_release);
	}

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_destroy_context(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_destroycontext args;
	int ret;
	struct dxgadapter *adapter = NULL;
	struct dxgcontext *context = NULL;
	struct dxgdevice *device = NULL;
	struct d3dkmthandle device_handle = {};

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	hmgrtable_lock(&process->handle_table, DXGLOCK_EXCL);
	context = hmgrtable_get_object_by_type(&process->handle_table,
					       HMGRENTRY_TYPE_DXGCONTEXT,
					       args.context);
	if (context) {
		hmgrtable_free_handle(&process->handle_table,
				      HMGRENTRY_TYPE_DXGCONTEXT, args.context);
		context->handle.v = 0;
		device_handle = context->device_handle;
		context->object_state = DXGOBJECTSTATE_DESTROYED;
	}
	hmgrtable_unlock(&process->handle_table, DXGLOCK_EXCL);

	if (context == NULL) {
		pr_err("invalid context handle: %x", args.context.v);
		ret = -EINVAL;
		goto cleanup;
	}

	/*
	 * The call acquires reference on the device. It is safe to access the
	 * adapter, because the device holds reference on it.
	 */
	device = dxgprocess_device_by_handle(process, device_handle);
	if (device == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	adapter = device->adapter;
	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0) {
		adapter = NULL;
		goto cleanup;
	}

	ret = dxgvmb_send_destroy_context(adapter, process, args.context);

	dxgcontext_destroy_safe(process, context);

cleanup:

	if (adapter)
		dxgadapter_release_lock_shared(adapter);

	if (device)
		kref_put(&device->device_kref, dxgdevice_release);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int dxgk_create_hwcontext(struct dxgprocess *process,
					     void *__user inargs)
{
	/* This is obsolete entry point */
	return -ENOTTY;
}

static int dxgk_destroy_hwcontext(struct dxgprocess *process,
					      void *__user inargs)
{
	/* This is obsolete entry point */
	return -ENOTTY;
}

static int
get_standard_alloc_priv_data(struct dxgdevice *device,
			     struct d3dkmt_createstandardallocation *alloc_info,
			     u32 *standard_alloc_priv_data_size,
			     void **standard_alloc_priv_data,
			     u32 *standard_res_priv_data_size,
			     void **standard_res_priv_data)
{
	int ret;
	struct d3dkmdt_gdisurfacedata gdi_data = { };
	u32 priv_data_size = 0;
	u32 res_priv_data_size = 0;
	void *priv_data = NULL;
	void *res_priv_data = NULL;

	gdi_data.type = _D3DKMDT_GDISURFACE_TEXTURE_CROSSADAPTER;
	gdi_data.width = alloc_info->existing_heap_data.size;
	gdi_data.height = 1;
	gdi_data.format = _D3DDDIFMT_UNKNOWN;

	*standard_alloc_priv_data_size = 0;
	ret = dxgvmb_send_get_stdalloc_data(device,
					_D3DKMDT_STANDARDALLOCATION_GDISURFACE,
					&gdi_data, 0,
					&priv_data_size, NULL,
					&res_priv_data_size,
					NULL);
	if (ret < 0)
		goto cleanup;
	dev_dbg(dxgglobaldev, "Priv data size: %d", priv_data_size);
	if (priv_data_size == 0) {
		ret = -EINVAL;
		goto cleanup;
	}
	priv_data = vzalloc(priv_data_size);
	if (priv_data == NULL) {
		ret = -ENOMEM;
		pr_err("failed to allocate memory for priv data: %d",
			   priv_data_size);
		goto cleanup;
	}
	if (res_priv_data_size) {
		res_priv_data = vzalloc(res_priv_data_size);
		if (res_priv_data == NULL) {
			ret = -ENOMEM;
			pr_err("failed to alloc memory for res priv data: %d",
				res_priv_data_size);
			goto cleanup;
		}
	}
	ret = dxgvmb_send_get_stdalloc_data(device,
					_D3DKMDT_STANDARDALLOCATION_GDISURFACE,
					&gdi_data, 0,
					&priv_data_size,
					priv_data,
					&res_priv_data_size,
					res_priv_data);
	if (ret < 0)
		goto cleanup;
	*standard_alloc_priv_data_size = priv_data_size;
	*standard_alloc_priv_data = priv_data;
	*standard_res_priv_data_size = res_priv_data_size;
	*standard_res_priv_data = res_priv_data;
	priv_data = NULL;
	res_priv_data = NULL;

cleanup:
	if (priv_data)
		vfree(priv_data);
	if (res_priv_data)
		vfree(res_priv_data);
	if (ret)
		dev_dbg(dxgglobaldev, "err: %s %d", __func__, ret);
	return ret;
}

static int
dxgk_create_allocation(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_createallocation args;
	int ret;
	struct dxgadapter *adapter = NULL;
	struct dxgdevice *device = NULL;
	struct d3dddi_allocationinfo2 *alloc_info = NULL;
	struct d3dkmt_createstandardallocation standard_alloc;
	u32 alloc_info_size = 0;
	struct dxgresource *resource = NULL;
	struct dxgallocation **dxgalloc = NULL;
	struct dxgsharedresource *shared_resource = NULL;
	bool resource_mutex_acquired = false;
	u32 standard_alloc_priv_data_size = 0;
	void *standard_alloc_priv_data = NULL;
	u32 res_priv_data_size = 0;
	void *res_priv_data = NULL;
	int i;

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.alloc_count > D3DKMT_CREATEALLOCATION_MAX ||
	    args.alloc_count == 0) {
		pr_err("invalid number of allocations to create");
		ret = -EINVAL;
		goto cleanup;
	}

	alloc_info_size = sizeof(struct d3dddi_allocationinfo2) *
	    args.alloc_count;
	alloc_info = vzalloc(alloc_info_size);
	if (alloc_info == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}
	ret = copy_from_user(alloc_info, args.allocation_info,
				 alloc_info_size);
	if (ret) {
		pr_err("%s failed to copy alloc info", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	for (i = 0; i < args.alloc_count; i++) {
		if (args.flags.standard_allocation) {
			if (alloc_info[i].priv_drv_data_size != 0) {
				pr_err("private data size is not zero");
				ret = -EINVAL;
				goto cleanup;
			}
		}
		if (alloc_info[i].priv_drv_data_size >=
		    DXG_MAX_VM_BUS_PACKET_SIZE) {
			pr_err("private data size is too big: %d %d %ld",
				   i, alloc_info[i].priv_drv_data_size,
				   sizeof(alloc_info[0]));
			ret = -EINVAL;
			goto cleanup;
		}
	}

	if (args.flags.existing_section || args.flags.create_protected) {
		pr_err("invalid allocation flags");
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.flags.standard_allocation) {
		if (args.standard_allocation == NULL) {
			pr_err("invalid standard allocation");
			ret = -EINVAL;
			goto cleanup;
		}
		ret = copy_from_user(&standard_alloc,
				     args.standard_allocation,
				     sizeof(standard_alloc));
		if (ret) {
			pr_err("%s failed to copy std alloc data", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
		if (standard_alloc.type ==
		    _D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP) {
			if (alloc_info[0].sysmem == NULL ||
			   (unsigned long)alloc_info[0].sysmem &
			   (PAGE_SIZE - 1)) {
				pr_err("invalid sysmem pointer");
				ret = STATUS_INVALID_PARAMETER;
				goto cleanup;
			}
			if (!args.flags.existing_sysmem) {
				pr_err("expected existing_sysmem flag");
				ret = STATUS_INVALID_PARAMETER;
				goto cleanup;
			}
		} else if (standard_alloc.type ==
		    _D3DKMT_STANDARDALLOCATIONTYPE_CROSSADAPTER) {
			if (args.flags.existing_sysmem) {
				pr_err("existing_sysmem flag is invalid");
				ret = STATUS_INVALID_PARAMETER;
				goto cleanup;

			}
			if (alloc_info[0].sysmem != NULL) {
				pr_err("sysmem should be NULL");
				ret = STATUS_INVALID_PARAMETER;
				goto cleanup;
			}
		} else {
			pr_err("invalid standard allocation type");
			ret = STATUS_INVALID_PARAMETER;
			goto cleanup;
		}

		if (args.priv_drv_data_size != 0 ||
		    args.alloc_count != 1 ||
		    standard_alloc.existing_heap_data.size == 0 ||
		    standard_alloc.existing_heap_data.size & (PAGE_SIZE - 1)) {
			pr_err("invalid standard allocation");
			ret = -EINVAL;
			goto cleanup;
		}
		args.priv_drv_data_size =
		    sizeof(struct d3dkmt_createstandardallocation);
	}

	if (args.flags.create_shared && !args.flags.create_resource) {
		pr_err("create_resource must be set for create_shared");
		ret = -EINVAL;
		goto cleanup;
	}

	/*
	 * The call acquires reference on the device. It is safe to access the
	 * adapter, because the device holds reference on it.
	 */
	device = dxgprocess_device_by_handle(process, args.device);
	if (device == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	ret = dxgdevice_acquire_lock_shared(device);
	if (ret < 0) {
		kref_put(&device->device_kref, dxgdevice_release);
		device = NULL;
		goto cleanup;
	}

	adapter = device->adapter;
	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0) {
		adapter = NULL;
		goto cleanup;
	}

	if (args.flags.standard_allocation) {
		ret = get_standard_alloc_priv_data(device,
						&standard_alloc,
						&standard_alloc_priv_data_size,
						&standard_alloc_priv_data,
						&res_priv_data_size,
						&res_priv_data);
		if (ret < 0)
			goto cleanup;
		dev_dbg(dxgglobaldev, "Alloc private data: %d",
			    standard_alloc_priv_data_size);
	}

	if (args.flags.create_resource) {
		resource = dxgresource_create(device);
		if (resource == NULL) {
			ret = -ENOMEM;
			goto cleanup;
		}
		resource->private_runtime_handle =
		    args.private_runtime_resource_handle;
		if (args.flags.create_shared) {
			if (!args.flags.nt_security_sharing) {
				dev_err(dxgglobaldev,
					"%s: nt_security_sharing must be set",
					__func__);
				ret = -EINVAL;
				goto cleanup;
			}
			shared_resource = dxgsharedresource_create(adapter);
			if (shared_resource == NULL) {
				ret = -ENOMEM;
				goto cleanup;
			}
			shared_resource->runtime_private_data_size =
			    args.priv_drv_data_size;
			shared_resource->resource_private_data_size =
			    args.priv_drv_data_size;

			shared_resource->runtime_private_data_size =
			    args.private_runtime_data_size;
			shared_resource->resource_private_data_size =
			    args.priv_drv_data_size;
			dxgsharedresource_add_resource(shared_resource,
						       resource);
			if (args.flags.standard_allocation) {
				shared_resource->resource_private_data =
					res_priv_data;
				shared_resource->resource_private_data_size =
					res_priv_data_size;
				res_priv_data = NULL;
			}
			if (args.private_runtime_data_size) {
				shared_resource->runtime_private_data =
				    vzalloc(args.private_runtime_data_size);
				if (shared_resource->runtime_private_data ==
				    NULL) {
					ret = -ENOMEM;
					goto cleanup;
				}
				ret = copy_from_user(
					shared_resource->runtime_private_data,
					args.private_runtime_data,
					args.private_runtime_data_size);
				if (ret) {
					pr_err("%s failed to copy runtime data",
						__func__);
					ret = -EINVAL;
					goto cleanup;
				}
			}
			if (args.priv_drv_data_size &&
			    !args.flags.standard_allocation) {
				shared_resource->resource_private_data =
				    vzalloc(args.priv_drv_data_size);
				if (shared_resource->resource_private_data ==
				    NULL) {
					ret = -ENOMEM;
					goto cleanup;
				}
				ret = copy_from_user(
					shared_resource->resource_private_data,
					args.priv_drv_data,
					args.priv_drv_data_size);
				if (ret) {
					pr_err("%s failed to copy res data",
						__func__);
					ret = -EINVAL;
					goto cleanup;
				}
			}
		}
	} else {
		if (args.resource.v) {
			/* Adding new allocations to the given resource */

			dxgprocess_ht_lock_shared_down(process);
			resource = hmgrtable_get_object_by_type(
				&process->handle_table,
				HMGRENTRY_TYPE_DXGRESOURCE,
				args.resource);
			kref_get(&resource->resource_kref);
			dxgprocess_ht_lock_shared_up(process);

			if (resource == NULL || resource->device != device) {
				pr_err("invalid resource handle %x",
					   args.resource.v);
				ret = -EINVAL;
				goto cleanup;
			}
			if (resource->shared_owner &&
			    resource->shared_owner->sealed) {
				pr_err("Resource is sealed");
				ret = -EINVAL;
				goto cleanup;
			}
			/* Synchronize with resource destruction */
			mutex_lock(&resource->resource_mutex);
			if (!dxgresource_is_active(resource)) {
				mutex_unlock(&resource->resource_mutex);
				ret = -EINVAL;
				goto cleanup;
			}
			resource_mutex_acquired = true;
		}
	}

	dxgalloc = vzalloc(sizeof(struct dxgallocation *) * args.alloc_count);
	if (dxgalloc == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}

	for (i = 0; i < args.alloc_count; i++) {
		struct dxgallocation *alloc;
		u32 priv_data_size;

		if (args.flags.standard_allocation)
			priv_data_size = standard_alloc_priv_data_size;
		else
			priv_data_size = alloc_info[i].priv_drv_data_size;

		if (alloc_info[i].sysmem && !args.flags.standard_allocation) {
			if ((unsigned long)
			    alloc_info[i].sysmem & (PAGE_SIZE - 1)) {
				pr_err("invalid sysmem alloc %d, %p",
					   i, alloc_info[i].sysmem);
				ret = -EINVAL;
				goto cleanup;
			}
		}
		if ((alloc_info[0].sysmem == NULL) !=
		    (alloc_info[i].sysmem == NULL)) {
			pr_err("All allocations must have sysmem pointer");
			ret = -EINVAL;
			goto cleanup;
		}

		dxgalloc[i] = dxgallocation_create(process);
		if (dxgalloc[i] == NULL) {
			ret = -ENOMEM;
			goto cleanup;
		}
		alloc = dxgalloc[i];

		if (resource) {
			ret = dxgresource_add_alloc(resource, alloc);
			if (ret < 0)
				goto cleanup;
		} else {
			dxgdevice_add_alloc(device, alloc);
		}
		if (args.flags.create_shared) {
			/* Remember alloc private data to use it during open */
			alloc->priv_drv_data = vzalloc(priv_data_size +
					offsetof(struct privdata, data) - 1);
			if (alloc->priv_drv_data == NULL) {
				ret = -ENOMEM;
				goto cleanup;
			}
			if (args.flags.standard_allocation) {
				memcpy(alloc->priv_drv_data->data,
				       standard_alloc_priv_data,
				       standard_alloc_priv_data_size);
				alloc->priv_drv_data->data_size =
				    standard_alloc_priv_data_size;
			} else {
				ret = copy_from_user(
					alloc->priv_drv_data->data,
					alloc_info[i].priv_drv_data,
					priv_data_size);
				if (ret) {
					pr_err("%s failed to copy priv data",
						__func__);
					ret = -EINVAL;
					goto cleanup;
				}
				alloc->priv_drv_data->data_size =
				    priv_data_size;
			}
		}
	}

	ret = dxgvmb_send_create_allocation(process, device, &args, inargs,
					    resource, dxgalloc, alloc_info,
					    &standard_alloc);
cleanup:

	if (resource_mutex_acquired) {
		mutex_unlock(&resource->resource_mutex);
		kref_put(&resource->resource_kref, dxgresource_release);
	}
	if (ret < 0) {
		if (dxgalloc) {
			for (i = 0; i < args.alloc_count; i++) {
				if (dxgalloc[i])
					dxgallocation_destroy(dxgalloc[i]);
			}
		}
		if (resource && args.flags.create_resource) {
			if (shared_resource) {
				dxgsharedresource_remove_resource
				    (shared_resource, resource);
			}
			dxgresource_destroy(resource);
		}
	}
	if (shared_resource)
		kref_put(&shared_resource->sresource_kref,
			 dxgsharedresource_destroy);
	if (dxgalloc)
		vfree(dxgalloc);
	if (standard_alloc_priv_data)
		vfree(standard_alloc_priv_data);
	if (res_priv_data)
		vfree(res_priv_data);
	if (alloc_info)
		vfree(alloc_info);

	if (adapter)
		dxgadapter_release_lock_shared(adapter);

	if (device) {
		dxgdevice_release_lock_shared(device);
		kref_put(&device->device_kref, dxgdevice_release);
	}

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

int validate_alloc(struct dxgallocation *alloc0,
			       struct dxgallocation *alloc,
			       struct dxgdevice *device,
			       struct d3dkmthandle alloc_handle)
{
	u32 fail_reason;

	if (alloc == NULL) {
		fail_reason = 1;
		goto cleanup;
	}
	if (alloc->resource_owner != alloc0->resource_owner) {
		fail_reason = 2;
		goto cleanup;
	}
	if (alloc->resource_owner) {
		if (alloc->owner.resource != alloc0->owner.resource) {
			fail_reason = 3;
			goto cleanup;
		}
		if (alloc->owner.resource->device != device) {
			fail_reason = 4;
			goto cleanup;
		}
		if (alloc->owner.resource->shared_owner) {
			fail_reason = 5;
			goto cleanup;
		}
	} else {
		if (alloc->owner.device != device) {
			fail_reason = 6;
			goto cleanup;
		}
	}
	return 0;
cleanup:
	pr_err("Alloc validation failed: reason: %d %x",
		   fail_reason, alloc_handle.v);
	return -EINVAL;
}

static int
dxgk_destroy_allocation(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_destroyallocation2 args;
	struct dxgdevice *device = NULL;
	struct dxgadapter *adapter = NULL;
	int ret;
	struct d3dkmthandle *alloc_handles = NULL;
	struct dxgallocation **allocs = NULL;
	struct dxgresource *resource = NULL;
	int i;

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.alloc_count > D3DKMT_CREATEALLOCATION_MAX ||
	    ((args.alloc_count == 0) == (args.resource.v == 0))) {
		pr_err("invalid number of allocations");
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.alloc_count) {
		u32 handle_size = sizeof(struct d3dkmthandle) *
				   args.alloc_count;

		alloc_handles = vzalloc(handle_size);
		if (alloc_handles == NULL) {
			ret = -ENOMEM;
			goto cleanup;
		}
		allocs = vzalloc(sizeof(struct dxgallocation *) *
				 args.alloc_count);
		if (allocs == NULL) {
			ret = -ENOMEM;
			goto cleanup;
		}
		ret = copy_from_user(alloc_handles, args.allocations,
					 handle_size);
		if (ret) {
			pr_err("%s failed to copy alloc handles", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	}

	/*
	 * The call acquires reference on the device. It is safe to access the
	 * adapter, because the device holds reference on it.
	 */
	device = dxgprocess_device_by_handle(process, args.device);
	if (device == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	/* Acquire the device lock to synchronize with the device destriction */
	ret = dxgdevice_acquire_lock_shared(device);
	if (ret < 0) {
		kref_put(&device->device_kref, dxgdevice_release);
		device = NULL;
		goto cleanup;
	}

	adapter = device->adapter;
	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0) {
		adapter = NULL;
		goto cleanup;
	}

	/*
	 * Destroy the local allocation handles first. If the host handle
	 * is destroyed first, another object could be assigned to the process
	 * table at the same place as the allocation handle and it will fail.
	 */
	if (args.alloc_count) {
		dxgprocess_ht_lock_exclusive_down(process);
		for (i = 0; i < args.alloc_count; i++) {
			allocs[i] =
			    hmgrtable_get_object_by_type(&process->handle_table,
						HMGRENTRY_TYPE_DXGALLOCATION,
						alloc_handles[i]);
			ret =
			    validate_alloc(allocs[0], allocs[i], device,
					   alloc_handles[i]);
			if (ret < 0) {
				dxgprocess_ht_lock_exclusive_up(process);
				goto cleanup;
			}
		}
		dxgprocess_ht_lock_exclusive_up(process);
		for (i = 0; i < args.alloc_count; i++)
			dxgallocation_free_handle(allocs[i]);
	} else {
		struct dxgallocation *alloc;

		dxgprocess_ht_lock_exclusive_down(process);
		resource = hmgrtable_get_object_by_type(&process->handle_table,
						HMGRENTRY_TYPE_DXGRESOURCE,
						args.resource);
		if (resource == NULL) {
			pr_err("Invalid resource handle: %x",
				   args.resource.v);
			ret = -EINVAL;
		} else if (resource->device != device) {
			pr_err("Resource belongs to wrong device: %x",
				   args.resource.v);
			ret = -EINVAL;
		} else {
			hmgrtable_free_handle(&process->handle_table,
					      HMGRENTRY_TYPE_DXGRESOURCE,
					      args.resource);
			resource->object_state = DXGOBJECTSTATE_DESTROYED;
			resource->handle.v = 0;
			resource->handle_valid = 0;
		}
		dxgprocess_ht_lock_exclusive_up(process);

		if (ret < 0)
			goto cleanup;

		dxgdevice_acquire_alloc_list_lock_shared(device);
		list_for_each_entry(alloc, &resource->alloc_list_head,
				    alloc_list_entry) {
			dxgallocation_free_handle(alloc);
		}
		dxgdevice_release_alloc_list_lock_shared(device);
	}

	if (args.alloc_count && allocs[0]->resource_owner)
		resource = allocs[0]->owner.resource;

	if (resource) {
		kref_get(&resource->resource_kref);
		mutex_lock(&resource->resource_mutex);
	}

	ret = dxgvmb_send_destroy_allocation(process, device, &args,
					     alloc_handles);

	/*
	 * Destroy the allocations after the host destroyed it.
	 * The allocation gpadl teardown will wait until the host unmaps its
	 * gpadl.
	 */
	dxgdevice_acquire_alloc_list_lock(device);
	if (args.alloc_count) {
		for (i = 0; i < args.alloc_count; i++) {
			if (allocs[i]) {
				allocs[i]->alloc_handle.v = 0;
				dxgallocation_destroy(allocs[i]);
			}
		}
	} else {
		dxgresource_destroy(resource);
	}
	dxgdevice_release_alloc_list_lock(device);

	if (resource) {
		mutex_unlock(&resource->resource_mutex);
		kref_put(&resource->resource_kref, dxgresource_release);
	}

cleanup:

	if (adapter)
		dxgadapter_release_lock_shared(adapter);

	if (device) {
		dxgdevice_release_lock_shared(device);
		kref_put(&device->device_kref, dxgdevice_release);
	}

	if (alloc_handles)
		vfree(alloc_handles);

	if (allocs)
		vfree(allocs);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_create_sync_object(struct dxgprocess *process, void *__user inargs)
{
	int ret;
	struct d3dkmt_createsynchronizationobject2 args;
	struct dxgdevice *device = NULL;
	struct dxgadapter *adapter = NULL;
	struct eventfd_ctx *event = NULL;
	struct dxgsyncobject *syncobj = NULL;
	bool device_lock_acquired = false;
	struct dxgsharedsyncobject *syncobjgbl = NULL;
	struct dxghosteventcpu *host_event = NULL;

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	device = dxgprocess_device_by_handle(process, args.device);
	if (device == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	ret = dxgdevice_acquire_lock_shared(device);
	if (ret < 0)
		goto cleanup;

	device_lock_acquired = true;

	adapter = device->adapter;
	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0) {
		adapter = NULL;
		goto cleanup;
	}

	syncobj = dxgsyncobject_create(process, device, adapter, args.info.type,
				       args.info.flags);
	if (syncobj == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.info.type == _D3DDDI_CPU_NOTIFICATION) {
		event = eventfd_ctx_fdget((int)
					  args.info.cpu_notification.event);
		if (IS_ERR(event)) {
			pr_err("failed to reference the event");
			event = NULL;
			ret = -EINVAL;
			goto cleanup;
		}
		host_event = syncobj->host_event;
		host_event->hdr.event_id = dxgglobal_new_host_event_id();
		host_event->cpu_event = event;
		host_event->remove_from_list = false;
		host_event->destroy_after_signal = false;
		host_event->hdr.event_type = dxghostevent_cpu_event;
		dxgglobal_add_host_event(&host_event->hdr);
		args.info.cpu_notification.event = host_event->hdr.event_id;
		dev_dbg(dxgglobaldev, "creating CPU notification event: %lld",
			args.info.cpu_notification.event);
	}

	ret = dxgvmb_send_create_sync_object(process, adapter, &args, syncobj);
	if (ret < 0)
		goto cleanup;

	if (args.info.flags.shared) {
		if (args.info.shared_handle.v == 0) {
			pr_err("shared handle should not be 0");
			ret = -EINVAL;
			goto cleanup;
		}
		syncobjgbl = dxgsharedsyncobj_create(device->adapter, syncobj);
		if (syncobjgbl == NULL) {
			ret = -ENOMEM;
			goto cleanup;
		}
		dxgsharedsyncobj_add_syncobj(syncobjgbl, syncobj);

		syncobjgbl->host_shared_handle = args.info.shared_handle;
	}

	ret = copy_to_user(inargs, &args, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy output args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	hmgrtable_lock(&process->handle_table, DXGLOCK_EXCL);
	ret = hmgrtable_assign_handle(&process->handle_table, syncobj,
				      HMGRENTRY_TYPE_DXGSYNCOBJECT,
				      args.sync_object);
	if (ret >= 0)
		syncobj->handle = args.sync_object;
	hmgrtable_unlock(&process->handle_table, DXGLOCK_EXCL);

cleanup:

	if (ret < 0) {
		if (syncobj) {
			dxgsyncobject_destroy(process, syncobj);
			if (args.sync_object.v)
				dxgvmb_send_destroy_sync_object(process,
							args.sync_object);
			event = NULL;
		}
		if (event)
			eventfd_ctx_put(event);
	}
	if (syncobjgbl)
		kref_put(&syncobjgbl->ssyncobj_kref, dxgsharedsyncobj_release);
	if (adapter)
		dxgadapter_release_lock_shared(adapter);
	if (device_lock_acquired)
		dxgdevice_release_lock_shared(device);
	if (device)
		kref_put(&device->device_kref, dxgdevice_release);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_destroy_sync_object(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_destroysynchronizationobject args;
	struct dxgsyncobject *syncobj = NULL;
	int ret;

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	dev_dbg(dxgglobaldev, "handle 0x%x", args.sync_object.v);
	hmgrtable_lock(&process->handle_table, DXGLOCK_EXCL);
	syncobj = hmgrtable_get_object_by_type(&process->handle_table,
					       HMGRENTRY_TYPE_DXGSYNCOBJECT,
					       args.sync_object);
	if (syncobj) {
		dev_dbg(dxgglobaldev, "syncobj 0x%p", syncobj);
		syncobj->handle.v = 0;
		hmgrtable_free_handle(&process->handle_table,
				      HMGRENTRY_TYPE_DXGSYNCOBJECT,
				      args.sync_object);
	}
	hmgrtable_unlock(&process->handle_table, DXGLOCK_EXCL);

	if (syncobj == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	dxgsyncobject_destroy(process, syncobj);

	ret = dxgvmb_send_destroy_sync_object(process, args.sync_object);

cleanup:

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_signal_sync_object(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_signalsynchronizationobject2 args;
	struct d3dkmt_signalsynchronizationobject2 *__user in_args = inargs;
	struct dxgdevice *device = NULL;
	struct dxgadapter *adapter = NULL;
	int ret;
	u32 fence_count = 1;
	struct eventfd_ctx *event = NULL;
	struct dxghosteventcpu *host_event = NULL;
	bool host_event_added = false;
	u64 host_event_id = 0;

	dev_dbg(dxgglobaldev, "ioctl: %s", __func__);

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.context_count >= D3DDDI_MAX_BROADCAST_CONTEXT ||
	    args.object_count > D3DDDI_MAX_OBJECT_SIGNALED) {
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.flags.enqueue_cpu_event) {
		host_event = vzalloc(sizeof(*host_event));
		if (host_event == NULL) {
			ret = -ENOMEM;
			goto cleanup;
		}
		host_event->process = process;
		event = eventfd_ctx_fdget((int)args.cpu_event_handle);
		if (IS_ERR(event)) {
			pr_err("failed to reference the event");
			event = NULL;
			ret = -EINVAL;
			goto cleanup;
		}
		fence_count = 0;
		host_event->cpu_event = event;
		host_event_id = dxgglobal_new_host_event_id();
		host_event->hdr.event_type = dxghostevent_cpu_event;
		host_event->hdr.event_id = host_event_id;
		host_event->remove_from_list = true;
		host_event->destroy_after_signal = true;
		dxgglobal_add_host_event(&host_event->hdr);
		host_event_added = true;
	}

	device = dxgprocess_device_by_object_handle(process,
						    HMGRENTRY_TYPE_DXGCONTEXT,
						    args.context);
	if (device == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	adapter = device->adapter;
	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0) {
		adapter = NULL;
		goto cleanup;
	}

	ret = dxgvmb_send_signal_sync_object(process, adapter,
					     args.flags, args.fence.fence_value,
					     args.context, args.object_count,
					     in_args->object_array,
					     args.context_count,
					     in_args->contexts, fence_count,
					     NULL, (void *)host_event_id,
					     zerohandle);

	/*
	 * When the send operation succeeds, the host event will be destroyed
	 * after signal from the host
	 */

cleanup:

	if (ret < 0) {
		if (host_event_added) {
			/* The event might be signaled and destroyed by host */
			host_event = (struct dxghosteventcpu *)
				dxgglobal_get_host_event(host_event_id);
			if (host_event) {
				eventfd_ctx_put(event);
				event = NULL;
				vfree(host_event);
				host_event = NULL;
			}
		}
		if (event)
			eventfd_ctx_put(event);
		if (host_event)
			vfree(host_event);
	}
	if (adapter)
		dxgadapter_release_lock_shared(adapter);
	if (device)
		kref_put(&device->device_kref, dxgdevice_release);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_signal_sync_object_cpu(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_signalsynchronizationobjectfromcpu args;
	struct dxgdevice *device = NULL;
	struct dxgadapter *adapter = NULL;
	int ret;

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}
	if (args.object_count == 0 ||
	    args.object_count > D3DDDI_MAX_OBJECT_SIGNALED) {
		dev_dbg(dxgglobaldev, "Too many objects: %d",
			args.object_count);
		ret = -EINVAL;
		goto cleanup;
	}

	device = dxgprocess_device_by_handle(process, args.device);
	if (device == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	adapter = device->adapter;
	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0) {
		adapter = NULL;
		goto cleanup;
	}

	ret = dxgvmb_send_signal_sync_object(process, adapter,
					     args.flags, 0, zerohandle,
					     args.object_count, args.objects, 0,
					     NULL, args.object_count,
					     args.fence_values, NULL,
					     args.device);

cleanup:

	if (adapter)
		dxgadapter_release_lock_shared(adapter);
	if (device)
		kref_put(&device->device_kref, dxgdevice_release);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_signal_sync_object_gpu(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_signalsynchronizationobjectfromgpu args;
	struct d3dkmt_signalsynchronizationobjectfromgpu *__user user_args =
	    inargs;
	struct dxgdevice *device = NULL;
	struct dxgadapter *adapter = NULL;
	struct d3dddicb_signalflags flags = { };
	int ret;

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.object_count == 0 ||
	    args.object_count > DXG_MAX_VM_BUS_PACKET_SIZE) {
		ret = -EINVAL;
		goto cleanup;
	}

	device = dxgprocess_device_by_object_handle(process,
						    HMGRENTRY_TYPE_DXGCONTEXT,
						    args.context);
	if (device == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	adapter = device->adapter;
	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0) {
		adapter = NULL;
		goto cleanup;
	}

	ret = dxgvmb_send_signal_sync_object(process, adapter,
					     flags, 0, zerohandle,
					     args.object_count,
					     args.objects, 1,
					     &user_args->context,
					     args.object_count,
					     args.monitored_fence_values, NULL,
					     zerohandle);

cleanup:

	if (adapter)
		dxgadapter_release_lock_shared(adapter);
	if (device)
		kref_put(&device->device_kref, dxgdevice_release);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_signal_sync_object_gpu2(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_signalsynchronizationobjectfromgpu2 args;
	struct dxgdevice *device = NULL;
	struct dxgadapter *adapter = NULL;
	struct d3dkmthandle context_handle;
	struct eventfd_ctx *event = NULL;
	u64 *fences = NULL;
	u32 fence_count = 0;
	int ret;
	struct dxghosteventcpu *host_event = NULL;
	bool host_event_added = false;
	u64 host_event_id = 0;

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.flags.enqueue_cpu_event) {
		if (args.object_count != 0 || args.cpu_event_handle == 0) {
			pr_err("Bad input for EnqueueCpuEvent: %d %lld",
				   args.object_count, args.cpu_event_handle);
			ret = -EINVAL;
			goto cleanup;
		}
	} else if (args.object_count == 0 ||
		   args.object_count > DXG_MAX_VM_BUS_PACKET_SIZE ||
		   args.context_count == 0 ||
		   args.context_count > DXG_MAX_VM_BUS_PACKET_SIZE) {
		pr_err("Invalid input: %d %d",
			   args.object_count, args.context_count);
		ret = -EINVAL;
		goto cleanup;
	}

	ret = copy_from_user(&context_handle, args.contexts,
			     sizeof(struct d3dkmthandle));
	if (ret) {
		pr_err("%s failed to copy context handle", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.flags.enqueue_cpu_event) {
		host_event = vzalloc(sizeof(*host_event));
		if (host_event == NULL) {
			ret = -ENOMEM;
			goto cleanup;
		}
		host_event->process = process;
		event = eventfd_ctx_fdget((int)args.cpu_event_handle);
		if (IS_ERR(event)) {
			pr_err("failed to reference the event");
			event = NULL;
			ret = -EINVAL;
			goto cleanup;
		}
		fence_count = 0;
		host_event->cpu_event = event;
		host_event_id = dxgglobal_new_host_event_id();
		host_event->hdr.event_id = host_event_id;
		host_event->hdr.event_type = dxghostevent_cpu_event;
		host_event->remove_from_list = true;
		host_event->destroy_after_signal = true;
		dxgglobal_add_host_event(&host_event->hdr);
		host_event_added = true;
	} else {
		fences = args.monitored_fence_values;
		fence_count = args.object_count;
	}

	device = dxgprocess_device_by_object_handle(process,
						    HMGRENTRY_TYPE_DXGCONTEXT,
						    context_handle);
	if (device == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	adapter = device->adapter;
	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0) {
		adapter = NULL;
		goto cleanup;
	}

	ret = dxgvmb_send_signal_sync_object(process, adapter,
					     args.flags, 0, zerohandle,
					     args.object_count, args.objects,
					     args.context_count, args.contexts,
					     fence_count, fences,
					     (void *)host_event_id, zerohandle);

cleanup:

	if (ret < 0) {
		if (host_event_added) {
			/* The event might be signaled and destroyed by host */
			host_event = (struct dxghosteventcpu *)
				dxgglobal_get_host_event(host_event_id);
			if (host_event) {
				eventfd_ctx_put(event);
				event = NULL;
				vfree(host_event);
				host_event = NULL;
			}
		}
		if (event)
			eventfd_ctx_put(event);
		if (host_event)
			vfree(host_event);
	}
	if (adapter)
		dxgadapter_release_lock_shared(adapter);
	if (device)
		kref_put(&device->device_kref, dxgdevice_release);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_wait_sync_object(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_waitforsynchronizationobject2 args;
	struct dxgdevice *device = NULL;
	struct dxgadapter *adapter = NULL;
	int ret;

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.object_count > D3DDDI_MAX_OBJECT_WAITED_ON ||
	    args.object_count == 0) {
		ret = -EINVAL;
		goto cleanup;
	}

	device = dxgprocess_device_by_object_handle(process,
						    HMGRENTRY_TYPE_DXGCONTEXT,
						    args.context);
	if (device == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	adapter = device->adapter;
	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0) {
		adapter = NULL;
		goto cleanup;
	}

	dev_dbg(dxgglobaldev, "Fence value: %lld", args.fence.fence_value);
	ret = dxgvmb_send_wait_sync_object_gpu(process, adapter,
					       args.context, args.object_count,
					       args.object_array,
					       &args.fence.fence_value, true);

cleanup:

	if (adapter)
		dxgadapter_release_lock_shared(adapter);
	if (device)
		kref_put(&device->device_kref, dxgdevice_release);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_wait_sync_object_cpu(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_waitforsynchronizationobjectfromcpu args;
	struct dxgdevice *device = NULL;
	struct dxgadapter *adapter = NULL;
	struct eventfd_ctx *event = NULL;
	struct dxghosteventcpu host_event = { };
	struct dxghosteventcpu *async_host_event = NULL;
	struct completion local_event = { };
	u64 event_id = 0;
	int ret;
	bool host_event_added = false;

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.object_count > DXG_MAX_VM_BUS_PACKET_SIZE ||
	    args.object_count == 0) {
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.async_event) {
		async_host_event = vzalloc(sizeof(*async_host_event));
		if (async_host_event == NULL) {
			ret = -EINVAL;
			goto cleanup;
		}
		async_host_event->process = process;
		event = eventfd_ctx_fdget((int)args.async_event);
		if (IS_ERR(event)) {
			pr_err("failed to reference the event");
			event = NULL;
			ret = -EINVAL;
			goto cleanup;
		}
		async_host_event->cpu_event = event;
		async_host_event->hdr.event_id = dxgglobal_new_host_event_id();
		async_host_event->destroy_after_signal = true;
		async_host_event->hdr.event_type = dxghostevent_cpu_event;
		dxgglobal_add_host_event(&async_host_event->hdr);
		event_id = async_host_event->hdr.event_id;
		host_event_added = true;
	} else {
		init_completion(&local_event);
		host_event.completion_event = &local_event;
		host_event.hdr.event_id = dxgglobal_new_host_event_id();
		host_event.hdr.event_type = dxghostevent_cpu_event;
		dxgglobal_add_host_event(&host_event.hdr);
		event_id = host_event.hdr.event_id;
	}

	device = dxgprocess_device_by_handle(process, args.device);
	if (device == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	adapter = device->adapter;
	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0) {
		adapter = NULL;
		goto cleanup;
	}

	ret = dxgvmb_send_wait_sync_object_cpu(process, adapter,
					       &args, event_id);
	if (ret < 0)
		goto cleanup;

	if (args.async_event == 0) {
		dxgadapter_release_lock_shared(adapter);
		adapter = NULL;
		ret = wait_for_completion_killable(&local_event);
		if (ret)
			pr_err("%s: wait_for_completion_killable failed: %d",
			       __func__, ret);
	}

cleanup:

	if (adapter)
		dxgadapter_release_lock_shared(adapter);
	if (device)
		kref_put(&device->device_kref, dxgdevice_release);
	if (host_event.hdr.event_id)
		dxgglobal_remove_host_event(&host_event.hdr);
	if (ret < 0) {
		if (host_event_added) {
			async_host_event = (struct dxghosteventcpu *)
				dxgglobal_get_host_event(event_id);
			if (async_host_event) {
				if (async_host_event->hdr.event_type ==
				    dxghostevent_cpu_event) {
					eventfd_ctx_put(event);
					event = NULL;
					vfree(async_host_event);
					async_host_event = NULL;
				} else {
					pr_err("Invalid event type");
					DXGKRNL_ASSERT(FALSE);
				}
			}
		}
		if (event)
			eventfd_ctx_put(event);
		if (async_host_event)
			vfree(async_host_event);
	}

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_wait_sync_object_gpu(struct dxgprocess *process, void *__user inargs)
{
	struct d3dkmt_waitforsynchronizationobjectfromgpu args;
	struct dxgcontext *context = NULL;
	struct d3dkmthandle device_handle = {};
	struct dxgdevice *device = NULL;
	struct dxgadapter *adapter = NULL;
	struct dxgsyncobject *syncobj = NULL;
	struct d3dkmthandle *objects = NULL;
	u32 object_size;
	u64 *fences = NULL;
	int ret;
	enum hmgrentry_type syncobj_type = HMGRENTRY_TYPE_FREE;
	bool monitored_fence = false;

	ret = copy_from_user(&args, inargs, sizeof(args));
	if (ret) {
		pr_err("%s failed to copy input args", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	if (args.object_count > DXG_MAX_VM_BUS_PACKET_SIZE ||
	    args.object_count == 0) {
		pr_err("Invalid object count: %d", args.object_count);
		ret = -EINVAL;
		goto cleanup;
	}

	object_size = sizeof(struct d3dkmthandle) * args.object_count;
	objects = vzalloc(object_size);
	if (objects == NULL) {
		ret = -ENOMEM;
		goto cleanup;
	}
	ret = copy_from_user(objects, args.objects, object_size);
	if (ret) {
		pr_err("%s failed to copy objects", __func__);
		ret = -EINVAL;
		goto cleanup;
	}

	hmgrtable_lock(&process->handle_table, DXGLOCK_SHARED);
	context = hmgrtable_get_object_by_type(&process->handle_table,
					       HMGRENTRY_TYPE_DXGCONTEXT,
					       args.context);
	if (context) {
		device_handle = context->device_handle;
		syncobj_type =
		    hmgrtable_get_object_type(&process->handle_table,
					      objects[0]);
	}
	if (device_handle.v == 0) {
		pr_err("Invalid context handle: %x", args.context.v);
		ret = -EINVAL;
	} else {
		if (syncobj_type == HMGRENTRY_TYPE_MONITOREDFENCE) {
			monitored_fence = true;
		} else if (syncobj_type == HMGRENTRY_TYPE_DXGSYNCOBJECT) {
			syncobj =
			    hmgrtable_get_object_by_type(&process->handle_table,
						HMGRENTRY_TYPE_DXGSYNCOBJECT,
						objects[0]);
			if (syncobj == NULL) {
				pr_err("Invalid syncobj: %x", objects[0].v);
				ret = -EINVAL;
			} else {
				monitored_fence = syncobj->monitored_fence;
			}
		} else {
			pr_err("Invalid syncobj type: %x", objects[0].v);
			ret = -EINVAL;
		}
	}
	hmgrtable_unlock(&process->handle_table, DXGLOCK_SHARED);

	if (ret < 0)
		goto cleanup;

	if (monitored_fence) {
		object_size = sizeof(u64) * args.object_count;
		fences = vzalloc(object_size);
		if (fences == NULL) {
			ret = -ENOMEM;
			goto cleanup;
		}
		ret = copy_from_user(fences, args.monitored_fence_values,
				     object_size);
		if (ret) {
			pr_err("%s failed to copy fences", __func__);
			ret = -EINVAL;
			goto cleanup;
		}
	} else {
		fences = &args.fence_value;
	}

	device = dxgprocess_device_by_handle(process, device_handle);
	if (device == NULL) {
		ret = -EINVAL;
		goto cleanup;
	}

	adapter = device->adapter;
	ret = dxgadapter_acquire_lock_shared(adapter);
	if (ret < 0) {
		adapter = NULL;
		goto cleanup;
	}

	ret = dxgvmb_send_wait_sync_object_gpu(process, adapter,
					       args.context, args.object_count,
					       objects, fences,
					       !monitored_fence);

cleanup:

	if (adapter)
		dxgadapter_release_lock_shared(adapter);
	if (device)
		kref_put(&device->device_kref, dxgdevice_release);
	if (objects)
		vfree(objects);
	if (fences && fences != &args.fence_value)
		vfree(fences);

	dev_dbg(dxgglobaldev, "ioctl:%s %s %d", errorstr(ret), __func__, ret);
	return ret;
}

static int
dxgk_render(struct dxgprocess *process, void *__user inargs)
{
	pr_err("%s is not implemented", __func__);
	return -ENOTTY;
}

static int
dxgk_create_context(struct dxgprocess *process, void *__user inargs)
{
	pr_err("%s is not implemented", __func__);
	return -ENOTTY;
}

static int
dxgk_get_shared_resource_adapter_luid(struct dxgprocess *process,
				      void *__user inargs)
{
	pr_err("shared_resource_adapter_luid is not implemented");
	return -ENOTTY;
}

/*
 * IOCTL processing
 * The driver IOCTLs return
 * - 0 in case of success
 * - positive values, which are Windows NTSTATUS (for example, STATUS_PENDING).
 *   Positive values are success codes.
 * - Linux negative error codes
 */
static int dxgk_ioctl(struct file *f, unsigned int p1, unsigned long p2)
{
	int code = _IOC_NR(p1);
	int status;
	struct dxgprocess *process;

	if (code < 1 || code > LX_IO_MAX) {
		pr_err("bad ioctl %x %x %x %x",
			   code, _IOC_TYPE(p1), _IOC_SIZE(p1), _IOC_DIR(p1));
		return -ENOTTY;
	}
	if (ioctls[code].ioctl_callback == NULL) {
		pr_err("ioctl callback is NULL %x", code);
		return -ENOTTY;
	}
	if (ioctls[code].ioctl != p1) {
		pr_err("ioctl mismatch. Code: %x User: %x Kernel: %x",
			   code, p1, ioctls[code].ioctl);
		return -ENOTTY;
	}
	process = (struct dxgprocess *)f->private_data;
	if (process->tgid != current->tgid) {
		pr_err("Call from a wrong process: %d %d",
			   process->tgid, current->tgid);
		return -ENOTTY;
	}
	status = ioctls[code].ioctl_callback(process, (void *__user)p2);
	return status;
}

long dxgk_compat_ioctl(struct file *f, unsigned int p1, unsigned long p2)
{
	dev_dbg(dxgglobaldev, "  compat ioctl %x", p1);
	return dxgk_ioctl(f, p1, p2);
}

long dxgk_unlocked_ioctl(struct file *f, unsigned int p1, unsigned long p2)
{
	dev_dbg(dxgglobaldev, "   unlocked ioctl %x Code:%d", p1, _IOC_NR(p1));
	return dxgk_ioctl(f, p1, p2);
}

#define SET_IOCTL(callback, v)				\
	ioctls[_IOC_NR(v)].ioctl_callback = callback;	\
	ioctls[_IOC_NR(v)].ioctl = v

void init_ioctls(void)
{
	SET_IOCTL(/*0x1 */ dxgk_open_adapter_from_luid,
		  LX_DXOPENADAPTERFROMLUID);
	SET_IOCTL(/*0x2 */ dxgk_create_device,
		  LX_DXCREATEDEVICE);
	SET_IOCTL(/*0x3 */ dxgk_create_context,
		  LX_DXCREATECONTEXT);
	SET_IOCTL(/*0x4 */ dxgk_create_context_virtual,
		  LX_DXCREATECONTEXTVIRTUAL);
	SET_IOCTL(/*0x5 */ dxgk_destroy_context,
		  LX_DXDESTROYCONTEXT);
	SET_IOCTL(/*0x6 */ dxgk_create_allocation,
		  LX_DXCREATEALLOCATION);
	SET_IOCTL(/*0x9 */ dxgk_query_adapter_info,
		  LX_DXQUERYADAPTERINFO);
	SET_IOCTL(/*0x10 */ dxgk_create_sync_object,
		  LX_DXCREATESYNCHRONIZATIONOBJECT);
	SET_IOCTL(/*0x11 */ dxgk_signal_sync_object,
		  LX_DXSIGNALSYNCHRONIZATIONOBJECT);
	SET_IOCTL(/*0x12 */ dxgk_wait_sync_object,
		  LX_DXWAITFORSYNCHRONIZATIONOBJECT);
	SET_IOCTL(/*0x13 */ dxgk_destroy_allocation,
		  LX_DXDESTROYALLOCATION2);
	SET_IOCTL(/*0x14 */ dxgk_enum_adapters,
		  LX_DXENUMADAPTERS2);
	SET_IOCTL(/*0x15 */ dxgk_close_adapter,
		  LX_DXCLOSEADAPTER);
	SET_IOCTL(/*0x17 */ dxgk_create_hwcontext,
		  LX_DXCREATEHWCONTEXT);
	SET_IOCTL(/*0x19 */ dxgk_destroy_device,
		  LX_DXDESTROYDEVICE);
	SET_IOCTL(/*0x1a */ dxgk_destroy_hwcontext,
		  LX_DXDESTROYHWCONTEXT);
	SET_IOCTL(/*0x1d */ dxgk_destroy_sync_object,
		  LX_DXDESTROYSYNCHRONIZATIONOBJECT);
	SET_IOCTL(/*0x23 */ dxgk_get_shared_resource_adapter_luid,
		  LX_DXGETSHAREDRESOURCEADAPTERLUID);
	SET_IOCTL(/*0x2d */ dxgk_render,
		  LX_DXRENDER);
	SET_IOCTL(/*0x31 */ dxgk_signal_sync_object_cpu,
		  LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMCPU);
	SET_IOCTL(/*0x32 */ dxgk_signal_sync_object_gpu,
		  LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMGPU);
	SET_IOCTL(/*0x33 */ dxgk_signal_sync_object_gpu2,
		  LX_DXSIGNALSYNCHRONIZATIONOBJECTFROMGPU2);
	SET_IOCTL(/*0x3a */ dxgk_wait_sync_object_cpu,
		  LX_DXWAITFORSYNCHRONIZATIONOBJECTFROMCPU);
	SET_IOCTL(/*0x3b */ dxgk_wait_sync_object_gpu,
		  LX_DXWAITFORSYNCHRONIZATIONOBJECTFROMGPU);
	SET_IOCTL(/*0x3e */ dxgk_enum_adapters3,
		  LX_DXENUMADAPTERS3);
}
