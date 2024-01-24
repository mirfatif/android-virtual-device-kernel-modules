// SPDX-License-Identifier: GPL-2.0

#include "defconfig_test.h"

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>

#include <linux/device.h>
#include <linux/pci_regs.h>
#include <linux/pci_ids.h>
#include <linux/pci.h>
#include <linux/page16.h>

#include <goldfish/goldfish_address_space.h>

MODULE_DESCRIPTION("A Goldfish driver that allocates address space ranges in "
		   "the guest to populate them later in the host. This allows "
		   "sharing host's memory with the guest.");
MODULE_AUTHOR("Roman Kiryanov <rkir@google.com>");
MODULE_LICENSE("GPL v2");

#define AS_DEBUG 1

#if AS_DEBUG
	#define AS_DPRINT(fmt, ...) \
		printk(KERN_ERR "%s:%d rkir555: " fmt "\n", \
		       __func__, __LINE__, ##__VA_ARGS__);
#else
	#define AS_DPRINT(fmt, ...)
#endif

enum as_register_id {
	AS_REGISTER_COMMAND = 0,
	AS_REGISTER_STATUS = 4,
	AS_REGISTER_GUEST_PAGE_SIZE = 8,
	AS_REGISTER_BLOCK_SIZE_LOW = 12,
	AS_REGISTER_BLOCK_SIZE_HIGH = 16,
	AS_REGISTER_BLOCK_OFFSET_LOW = 20,
	AS_REGISTER_BLOCK_OFFSET_HIGH = 24,
	AS_REGISTER_PING = 28,
	AS_REGISTER_PING_INFO_ADDR_LOW = 32,
	AS_REGISTER_PING_INFO_ADDR_HIGH = 36,
	AS_REGISTER_HANDLE = 40,
	AS_REGISTER_PHYS_START_LOW = 44,
	AS_REGISTER_PHYS_START_HIGH = 48,
	AS_REGISTER_PING_WITH_DATA = 52,
};

enum as_command_id {
	AS_COMMAND_ALLOCATE_BLOCK = 1,
	AS_COMMAND_DEALLOCATE_BLOCK = 2,
	AS_COMMAND_GEN_HANDLE = 3,
	AS_COMMAND_DESTROY_HANDLE = 4,
	AS_COMMAND_TELL_PING_INFO_ADDR = 5,
};

#define AS_PCI_VENDOR_ID	0x607D
#define AS_PCI_DEVICE_ID	0xF153
#define AS_ALLOCATED_BLOCKS_INITIAL_CAPACITY 32
#define AS_INVALID_HANDLE (~(0))

enum as_pci_bar_id {
	AS_PCI_CONTROL_BAR_ID = 0,
	AS_PCI_AREA_BAR_ID = 1,
};

struct as_device_state {
	struct miscdevice	miscdevice;
	struct pci_dev		*dev;
	struct as_driver_state	*driver_state;

	void __iomem		*io_registers;

	void			*address_area;	/* to claim the address space */

	/* physical address to allocate from */
	unsigned long		address_area_phys_address;

	struct mutex		registers_lock;	/* protects registers */
};

struct as_block {
	u64 offset;
	u64 size;
};

struct as_allocated_blocks {
	struct as_block *blocks;  /* a dynamic array of allocated blocks */
	int blocks_size;
	int blocks_capacity;
	struct mutex blocks_lock; /* protects operations with blocks */
};

struct as_ping_info_internal {
	__u64 offset;
	__u64 size;
	__u64 metadata;
	__u32 version;
	__u32 wait_fd;
	__u32 wait_flags;
	__u32 direction;
	__u64 data_size;
	__u8 data[0];
};

struct as_file_state {
	struct as_device_state *device_state;
	struct as_allocated_blocks allocated_blocks;
	struct as_allocated_blocks shared_allocated_blocks;
	struct as_ping_info_internal *ping_info;
	struct mutex ping_info_lock;	/* protects ping_info */
	u32 handle; /* handle generated by the host */
};

static void __iomem *as_register_address(void __iomem *base,
					 int offset)
{
	WARN_ON(!base);

	return ((char __iomem *)base) + offset;
}

static void as_write_register(void __iomem *registers,
			      int offset,
			      u32 value)
{
	writel(value, as_register_address(registers, offset));
}

static u32 as_read_register(void __iomem *registers, int offset)
{
	return readl(as_register_address(registers, offset));
}

static int as_run_command(struct as_device_state *state, enum as_command_id cmd)
{
	WARN_ON(!state);

	as_write_register(state->io_registers, AS_REGISTER_COMMAND, cmd);
	return -as_read_register(state->io_registers, AS_REGISTER_STATUS);
}

static void as_ping_impl(struct as_device_state *state, u32 handle)
{
	as_write_register(state->io_registers, AS_REGISTER_PING, handle);
}

static void as_ping_with_data_impl(struct as_device_state *state, u32 handle)
{
	as_write_register(state->io_registers, AS_REGISTER_PING_WITH_DATA, handle);
}

static long
as_ioctl_allocate_block_locked_impl(struct as_device_state *state,
				    u64 *size, u64 *offset)
{
	long res;

	as_write_register(state->io_registers,
			  AS_REGISTER_BLOCK_SIZE_LOW,
			  lower_32_bits(*size));
	as_write_register(state->io_registers,
			  AS_REGISTER_BLOCK_SIZE_HIGH,
			  upper_32_bits(*size));

	res = as_run_command(state, AS_COMMAND_ALLOCATE_BLOCK);
	if (!res) {
		u64 low = as_read_register(state->io_registers,
					   AS_REGISTER_BLOCK_OFFSET_LOW);
		u64 high = as_read_register(state->io_registers,
					    AS_REGISTER_BLOCK_OFFSET_HIGH);
		*offset = low | (high << 32);

		low = as_read_register(state->io_registers,
				       AS_REGISTER_BLOCK_SIZE_LOW);
		high = as_read_register(state->io_registers,
					AS_REGISTER_BLOCK_SIZE_HIGH);
		*size = low | (high << 32);
	}

	return res;
}

static long
as_ioctl_unallocate_block_locked_impl(struct as_device_state *state, u64 offset)
{
	as_write_register(state->io_registers,
			  AS_REGISTER_BLOCK_OFFSET_LOW,
			  lower_32_bits(offset));
	as_write_register(state->io_registers,
			  AS_REGISTER_BLOCK_OFFSET_HIGH,
			  upper_32_bits(offset));

	return as_run_command(state, AS_COMMAND_DEALLOCATE_BLOCK);
}

static int as_blocks_grow_capacity(int old_capacity)
{
	WARN_ON(old_capacity < 0);

	return old_capacity + old_capacity;
}

static int
as_blocks_insert(struct as_allocated_blocks *allocated_blocks,
		 u64 offset,
		 u64 size)
{
	int blocks_size;

	if (mutex_lock_interruptible(&allocated_blocks->blocks_lock))
		return -ERESTARTSYS;

	blocks_size = allocated_blocks->blocks_size;

	WARN_ON(allocated_blocks->blocks_capacity < 1);
	WARN_ON(allocated_blocks->blocks_capacity <
		allocated_blocks->blocks_size);
	WARN_ON(!allocated_blocks->blocks);

	if (allocated_blocks->blocks_capacity == blocks_size) {
		int new_capacity =
			as_blocks_grow_capacity(
				allocated_blocks->blocks_capacity);
		struct as_block *new_blocks =
			kcalloc(new_capacity,
				sizeof(allocated_blocks->blocks[0]),
				GFP_KERNEL);

		if (!new_blocks) {
			mutex_unlock(&allocated_blocks->blocks_lock);
			return -ENOMEM;
		}

		memcpy(new_blocks, allocated_blocks->blocks,
		       blocks_size * sizeof(allocated_blocks->blocks[0]));

		kfree(allocated_blocks->blocks);
		allocated_blocks->blocks = new_blocks;
		allocated_blocks->blocks_capacity = new_capacity;
	}

	WARN_ON(blocks_size >= allocated_blocks->blocks_capacity);

	allocated_blocks->blocks[blocks_size] =
		(struct as_block){ .offset = offset, .size = size };
	allocated_blocks->blocks_size = blocks_size + 1;

	mutex_unlock(&allocated_blocks->blocks_lock);
	return 0;
}

static int
as_blocks_remove(struct as_allocated_blocks *allocated_blocks, u64 offset)
{
	long res = -ENXIO;
	struct as_block *blocks;
	int blocks_size;
	int i;

	if (mutex_lock_interruptible(&allocated_blocks->blocks_lock))
		return -ERESTARTSYS;

	blocks = allocated_blocks->blocks;
	WARN_ON(!blocks);

	blocks_size = allocated_blocks->blocks_size;
	WARN_ON(blocks_size < 0);

	for (i = 0; i < blocks_size; ++i) {
		if (offset == blocks[i].offset) {
			int last = blocks_size - 1;

			if (last > i)
				blocks[i] = blocks[last];

			--allocated_blocks->blocks_size;
			res = 0;
			break;
		}
	}

	if (res)
		pr_err("%s: Block not found atoffset: 0x%llx\n",
			__func__, offset);

	mutex_unlock(&allocated_blocks->blocks_lock);
	return res;
}

static int
as_blocks_check_if_mine(struct as_allocated_blocks *allocated_blocks,
			const u64 offset,
			const u64 size)
{
	const u64 end = offset + size;
	int res = -EPERM;
	struct as_block *block;
	int blocks_size;

	if (mutex_lock_interruptible(&allocated_blocks->blocks_lock))
		return -ERESTARTSYS;

	block = allocated_blocks->blocks;
	WARN_ON(!block);

	blocks_size = allocated_blocks->blocks_size;
	WARN_ON(blocks_size < 0);

	AS_DPRINT("checking offset=0x%llx size=0x%llx",
		  (unsigned long long)offset,
		  (unsigned long long)size);

	for (; blocks_size > 0; --blocks_size, ++block) {
		u64 block_offset = block->offset;
		u64 block_end = block_offset + block->size;

		AS_DPRINT("block_offset=0x%llx block_end=0x%llx",
			  (unsigned long long)block_offset,
			  (unsigned long long)block_end);

		if (offset >= block_offset && end <= block_end) {
			res = 0;
			break;
		}
	}
	AS_DPRINT("res=%d", res);

	mutex_unlock(&allocated_blocks->blocks_lock);
	if (res) {
		AS_DPRINT("ERROR: res=%d", res);
	}
	return res;
}

static int as_open(struct inode *inode, struct file *filp)
{
	struct as_file_state *file_state;
	struct as_device_state *device_state;
	struct as_ping_info_internal *ping_info;
	u64 ping_info_phys;
	u64 ping_info_phys_returned;
	int err;

	AS_DPRINT("Get free page");
	ping_info =
		(struct as_ping_info_internal *)
		__get_free_page(GFP_KERNEL);
	ping_info_phys = virt_to_phys(ping_info);
	AS_DPRINT("Got free page: %p 0x%llx", ping_info,
		  (unsigned long long)ping_info_phys);

	if (!ping_info) {
		printk(KERN_ERR "Could not alloc goldfish_address_space command buffer!\n");
		err = -ENOMEM;
		goto err_ping_info_alloc_failed;
	}

	file_state = kzalloc(sizeof(*file_state), GFP_KERNEL);
	if (!file_state) {
		err = -ENOMEM;
		goto err_file_state_alloc_failed;
	}

	file_state->device_state =
		container_of(filp->private_data,
			     struct as_device_state,
			     miscdevice);
	device_state = file_state->device_state;

	file_state->allocated_blocks.blocks =
		kcalloc(AS_ALLOCATED_BLOCKS_INITIAL_CAPACITY,
				sizeof(file_state->allocated_blocks.blocks[0]),
				GFP_KERNEL);

	if (!file_state->allocated_blocks.blocks) {
		err = -ENOMEM;
		goto err_file_state_blocks_alloc_failed;
	}

	file_state->shared_allocated_blocks.blocks =
		kcalloc(
			AS_ALLOCATED_BLOCKS_INITIAL_CAPACITY,
			sizeof(file_state->shared_allocated_blocks.blocks[0]),
			GFP_KERNEL);

	if (!file_state->shared_allocated_blocks.blocks) {
		err = -ENOMEM;
		goto err_file_state_blocks_alloc_failed;
	}

	file_state->allocated_blocks.blocks_size = 0;
	file_state->allocated_blocks.blocks_capacity =
		AS_ALLOCATED_BLOCKS_INITIAL_CAPACITY;
	mutex_init(&file_state->allocated_blocks.blocks_lock);

	file_state->shared_allocated_blocks.blocks_size = 0;
	file_state->shared_allocated_blocks.blocks_capacity =
		AS_ALLOCATED_BLOCKS_INITIAL_CAPACITY;
	mutex_init(&file_state->shared_allocated_blocks.blocks_lock);

	mutex_init(&file_state->ping_info_lock);
	file_state->ping_info = ping_info;

	AS_DPRINT("Acq regs lock");
	mutex_lock(&device_state->registers_lock);
	AS_DPRINT("Got regs lock, gen handle");
	as_run_command(device_state, AS_COMMAND_GEN_HANDLE);
	file_state->handle = as_read_register(
		device_state->io_registers,
		AS_REGISTER_HANDLE);
	AS_DPRINT("Got regs lock, read handle: %u", file_state->handle);
	mutex_unlock(&device_state->registers_lock);

	if (file_state->handle == AS_INVALID_HANDLE) {
		err = -EINVAL;
		goto err_gen_handle_failed;
	}

	AS_DPRINT("Acq regs lock 2");
	mutex_lock(&device_state->registers_lock);
	AS_DPRINT("Acqd regs lock 2, write handle and ping info addr");
	as_write_register(
		device_state->io_registers,
		AS_REGISTER_HANDLE,
		file_state->handle);
	as_write_register(
		device_state->io_registers,
		AS_REGISTER_PING_INFO_ADDR_LOW,
		lower_32_bits(ping_info_phys));
	as_write_register(
		device_state->io_registers,
		AS_REGISTER_PING_INFO_ADDR_HIGH,
		upper_32_bits(ping_info_phys));
	AS_DPRINT("Do tell ping info addr");
	as_run_command(device_state, AS_COMMAND_TELL_PING_INFO_ADDR);
	ping_info_phys_returned =
		((u64)as_read_register(device_state->io_registers,
				       AS_REGISTER_PING_INFO_ADDR_LOW)) |
		((u64)as_read_register(device_state->io_registers,
				       AS_REGISTER_PING_INFO_ADDR_HIGH) << 32);
	AS_DPRINT("Read back");

	if (ping_info_phys != ping_info_phys_returned) {
		printk(KERN_ERR "%s: Invalid result for ping info phys addr: expected 0x%llx, got 0x%llx\n",
		       __func__,
		       ping_info_phys, ping_info_phys_returned);
		err = -EINVAL;
		goto err_ping_info_failed;
	}

	mutex_unlock(&device_state->registers_lock);

	filp->private_data = file_state;
	return 0;

err_ping_info_failed:
err_gen_handle_failed:
	kfree(file_state->allocated_blocks.blocks);
	kfree(file_state->shared_allocated_blocks.blocks);
err_file_state_blocks_alloc_failed:
	kfree(file_state);
err_file_state_alloc_failed:
	free_page((unsigned long)ping_info);
err_ping_info_alloc_failed:
	return err;
}

static int as_release(struct inode *inode, struct file *filp)
{
	struct as_file_state *file_state = filp->private_data;
	struct as_allocated_blocks *allocated_blocks =
		&file_state->allocated_blocks;
	struct as_allocated_blocks *shared_allocated_blocks =
		&file_state->shared_allocated_blocks;
	struct as_ping_info_internal *ping_info = file_state->ping_info;
	struct as_device_state *state = file_state->device_state;
	int blocks_size, shared_blocks_size;
	int i;

	WARN_ON(!state);
	WARN_ON(!allocated_blocks);
	WARN_ON(!allocated_blocks->blocks);
	WARN_ON(allocated_blocks->blocks_size < 0);
	WARN_ON(!shared_allocated_blocks);
	WARN_ON(!shared_allocated_blocks->blocks);
	WARN_ON(shared_allocated_blocks->blocks_size < 0);
	WARN_ON(!ping_info);

	blocks_size = allocated_blocks->blocks_size;
	shared_blocks_size = shared_allocated_blocks->blocks_size;

	mutex_lock(&state->registers_lock);

	as_write_register(state->io_registers, AS_REGISTER_HANDLE,
			  file_state->handle);
	as_run_command(state, AS_COMMAND_DESTROY_HANDLE);

	for (i = 0; i < blocks_size; ++i) {
		WARN_ON(as_ioctl_unallocate_block_locked_impl(
				state, allocated_blocks->blocks[i].offset));
	}

	// Do not unalloc shared blocks as they are host-owned

	mutex_unlock(&state->registers_lock);

	kfree(allocated_blocks->blocks);
	kfree(shared_allocated_blocks->blocks);
	free_page((unsigned long)ping_info);
	kfree(file_state);
	return 0;
}

static int as_mmap_impl(struct as_device_state *state,
			size_t size,
			struct vm_area_struct *vma)
{
	unsigned long pfn = (state->address_area_phys_address >> PAGE_SHIFT) +
		vma->vm_pgoff;
	int r;

	r = remap_pfn_range(vma,
			    vma->vm_start,
			    pfn,
			    size,
			    vma->vm_page_prot);
	if (r) {
		AS_DPRINT("vma=%px addr=0x%lx pfn=0x%lx size=0x%lx pgprot=0x%lx",
			  vma, vma->vm_start, pfn, (unsigned long)size,
			  vma->vm_page_prot.pgprot);
	}
	return r;
}

static int as_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct as_file_state *file_state = filp->private_data;
	struct as_allocated_blocks *allocated_blocks =
		&file_state->allocated_blocks;
	struct as_allocated_blocks *shared_allocated_blocks =
		&file_state->shared_allocated_blocks;
	const size_t size0 = vma->vm_end - vma->vm_start;
	const size_t size = __PAGE_ALIGN(size0);
	int r;

	AS_DPRINT("size0=0x%lx size=0x%lx", (unsigned long)size0, (unsigned long)size);

	WARN_ON(!allocated_blocks);

	r = as_blocks_check_if_mine(allocated_blocks,
				    vma->vm_pgoff << PAGE_SHIFT,
				    size);
	if (!r) {
		return as_mmap_impl(file_state->device_state, size, vma);
	} else if (r == -ERESTARTSYS) {
		return r;
	}

	r = as_blocks_check_if_mine(shared_allocated_blocks,
				    vma->vm_pgoff << PAGE_SHIFT,
				    size);
	if (!r) {
		return as_mmap_impl(file_state->device_state, size, vma);
	}

	return r;
}

static long as_ioctl_allocate_block_impl(
	struct as_device_state *state,
	struct goldfish_address_space_allocate_block *request)
{
	long res;

	if (mutex_lock_interruptible(&state->registers_lock))
		return -ERESTARTSYS;

	res = as_ioctl_allocate_block_locked_impl(state,
						  &request->size,
						  &request->offset);
	if (!res) {
		request->phys_addr =
			state->address_area_phys_address + request->offset;
	}

	mutex_unlock(&state->registers_lock);
	return res;
}

static void
as_ioctl_unallocate_block_impl(struct as_device_state *state, u64 offset)
{
	mutex_lock(&state->registers_lock);
	WARN_ON(as_ioctl_unallocate_block_locked_impl(state, offset));
	mutex_unlock(&state->registers_lock);
}

static long
as_ioctl_allocate_block(struct as_allocated_blocks *allocated_blocks,
			struct as_device_state *state,
			void __user *ptr)
{
	long res;
	struct goldfish_address_space_allocate_block request;

	if (copy_from_user(&request, ptr, sizeof(request)))
		return -EFAULT;

	res = as_ioctl_allocate_block_impl(state, &request);
	if (!res) {
		res = as_blocks_insert(allocated_blocks,
				       request.offset,
				       request.size);

		if (res) {
			as_ioctl_unallocate_block_impl(state, request.offset);
		} else if (copy_to_user(ptr, &request, sizeof(request))) {
			as_ioctl_unallocate_block_impl(state, request.offset);
			res = -EFAULT;
		}
	}

	return res;
}

static long
as_ioctl_unallocate_block(struct as_allocated_blocks *allocated_blocks,
			  struct as_device_state *state,
			  void __user *ptr)
{
	long res;
	u64 offset;

	if (copy_from_user(&offset, ptr, sizeof(offset)))
		return -EFAULT;

	res = as_blocks_remove(allocated_blocks, offset);
	if (!res)
		as_ioctl_unallocate_block_impl(state, offset);

	return res;
}

static long
as_ioctl_claim_block(struct as_allocated_blocks *allocated_blocks,
			struct as_device_state *state,
			void __user *ptr)
{
	long res;
	struct goldfish_address_space_claim_shared request;

	if (copy_from_user(&request, ptr, sizeof(request)))
		return -EFAULT;

	res = as_blocks_insert(allocated_blocks,
				   request.offset,
				   request.size);

	if (res)
		return res;
	else if (copy_to_user(ptr, &request, sizeof(request)))
		return -EFAULT;

	return 0;
}

static long
as_ioctl_unclaim_block(struct as_allocated_blocks *allocated_blocks,
			  struct as_device_state *state,
			  void __user *ptr)
{
	long res;
	u64 offset;

	if (copy_from_user(&offset, ptr, sizeof(offset)))
		return -EFAULT;

	res = as_blocks_remove(allocated_blocks, offset);
	if (res)
		pr_err("%s: as_blocks_remove failed (%ld)\n", __func__, res);

	return res;
}

static long
as_ioctl_ping_impl(struct as_ping_info_internal *ping_info,
		   struct as_device_state *state,
		   u32 handle,
		   void __user *ptr)
{
	struct goldfish_address_space_ping user_copy;

	if (copy_from_user(&user_copy, ptr, sizeof(user_copy)))
		return -EFAULT;

	/* Underlying struct is actually bigger */
	memcpy(ping_info, &user_copy, sizeof(user_copy));

	ping_info->offset += state->address_area_phys_address;
	ping_info->data_size = 0;

	mutex_lock(&state->registers_lock);
	as_ping_impl(state, handle);
	mutex_unlock(&state->registers_lock);

	memcpy(&user_copy, ping_info, sizeof(user_copy));
	if (copy_to_user(ptr, &user_copy, sizeof(user_copy)))
		return -EFAULT;

	return 0;
}

static long
as_ioctl_ping_with_data_impl(struct as_ping_info_internal *ping_info,
		   struct as_device_state *state,
		   u32 handle,
		   void __user *ptr)
{
	struct goldfish_address_space_ping_with_data user_copy;

	if (copy_from_user(&user_copy, ptr, sizeof(user_copy)))
		return -EFAULT;

	if (user_copy.data_size > __PAGE_SIZE - offsetof(struct goldfish_address_space_ping_with_data, data_ptr))
		return -EFAULT;

	ping_info->offset = user_copy.offset;
	ping_info->size = user_copy.size;
	ping_info->metadata = user_copy.metadata;
	ping_info->version = user_copy.version;
	ping_info->wait_fd = user_copy.wait_fd;
	ping_info->wait_flags = user_copy.wait_flags;
	ping_info->direction = user_copy.direction;
	ping_info->data_size = user_copy.data_size;

	if (copy_from_user(&ping_info->data, (void*)(user_copy.data_ptr), user_copy.data_size))
		return -EFAULT;

	ping_info->offset += state->address_area_phys_address;

	mutex_lock(&state->registers_lock);
	as_ping_with_data_impl(state, handle);
	mutex_unlock(&state->registers_lock);

	/* No response in data field */

	memcpy(&user_copy, ping_info, sizeof(user_copy));
	if (copy_to_user(ptr, &user_copy, sizeof(user_copy)))
		return -EFAULT;

	return 0;
}

static long as_ioctl_ping(struct as_file_state *file_state, void __user *ptr)
{
	long ret;

	mutex_lock(&file_state->ping_info_lock);
	ret = as_ioctl_ping_impl(file_state->ping_info,
				 file_state->device_state,
				 file_state->handle,
				 ptr);
	mutex_unlock(&file_state->ping_info_lock);

	return ret;
}

static long as_ioctl_ping_with_data(struct as_file_state *file_state, void __user *ptr)
{
	long ret;

	mutex_lock(&file_state->ping_info_lock);
	ret = as_ioctl_ping_with_data_impl(file_state->ping_info,
				 file_state->device_state,
				 file_state->handle,
				 ptr);
	mutex_unlock(&file_state->ping_info_lock);

	return ret;
}

static long as_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct as_file_state *file_state = filp->private_data;
	long res = -ENOTTY;

	switch (cmd) {
	case GOLDFISH_ADDRESS_SPACE_IOCTL_ALLOCATE_BLOCK:
		res = as_ioctl_allocate_block(&file_state->allocated_blocks,
						   file_state->device_state,
						   (void __user *)arg);
		break;

	case GOLDFISH_ADDRESS_SPACE_IOCTL_DEALLOCATE_BLOCK:
		res = as_ioctl_unallocate_block(&file_state->allocated_blocks,
						 file_state->device_state,
						 (void __user *)arg);
		break;

	case GOLDFISH_ADDRESS_SPACE_IOCTL_PING:
		res = as_ioctl_ping(file_state, (void __user *)arg);
		break;

	case GOLDFISH_ADDRESS_SPACE_IOCTL_PING_WITH_DATA:
		res = as_ioctl_ping_with_data(file_state, (void __user *)arg);
		break;

	case GOLDFISH_ADDRESS_SPACE_IOCTL_CLAIM_SHARED:
		res = as_ioctl_claim_block(
			&file_state->shared_allocated_blocks,
			file_state->device_state,
			(void __user *)arg);
		break;

	case GOLDFISH_ADDRESS_SPACE_IOCTL_UNCLAIM_SHARED:
		res = as_ioctl_unclaim_block(
			&file_state->shared_allocated_blocks,
			file_state->device_state,
			(void __user *)arg);
		break;

	default:
		res = -ENOTTY;
	}

	return res;
}

static const struct file_operations userspace_file_operations = {
	.owner = THIS_MODULE,
	.open = as_open,
	.release = as_release,
	.mmap = as_mmap,
	.unlocked_ioctl = as_ioctl,
	.compat_ioctl = as_ioctl,
};

static void __iomem __must_check *ioremap_pci_bar(struct pci_dev *dev,
						  int bar_id)
{
	void __iomem *io;
	unsigned long size = pci_resource_len(dev, bar_id);

	if (!size)
		return IOMEM_ERR_PTR(-ENXIO);

	io = ioremap(pci_resource_start(dev, bar_id), size);
	if (!io)
		return IOMEM_ERR_PTR(-ENOMEM);

	return io;
}

static void __must_check *memremap_pci_bar(struct pci_dev *dev,
					   int bar_id,
					   unsigned long flags)
{
	void *mem;
	unsigned long size = pci_resource_len(dev, bar_id);

	if (!size)
		return ERR_PTR(-ENXIO);

	mem = memremap(pci_resource_start(dev, bar_id), size, flags);
	if (!mem)
		return ERR_PTR(-ENOMEM);

	return mem;
}


static void fill_miscdevice(struct miscdevice *miscdev)
{
	memset(miscdev, 0, sizeof(*miscdev));

	miscdev->minor = MISC_DYNAMIC_MINOR;
	miscdev->name = GOLDFISH_ADDRESS_SPACE_DEVICE_NAME;
	miscdev->fops = &userspace_file_operations;
}

static int __must_check
create_as_device(struct pci_dev *dev, const struct pci_device_id *id)
{
	int res;
	struct as_device_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	res = pci_request_region(dev,
				 AS_PCI_CONTROL_BAR_ID,
				 "Address space control");
	if (res) {
		pr_err("(bn 0x%X, sn 0x%X) failed to allocate PCI resource for BAR%d",
		       dev->bus->number,
		       dev->devfn,
		       AS_PCI_CONTROL_BAR_ID);
		goto out_free_device_state;
	}

	res = pci_request_region(dev,
				 AS_PCI_AREA_BAR_ID,
				 "Address space area");
	if (res) {
		pr_err("(bn 0x%X, sn 0x%X) failed to allocate PCI resource for BAR%d",
		       dev->bus->number,
		       dev->devfn,
		       AS_PCI_AREA_BAR_ID);
		goto out_release_control_bar;
	}

	fill_miscdevice(&state->miscdevice);
	res = misc_register(&state->miscdevice);
	if (res)
		goto out_release_area_bar;

	state->io_registers = ioremap_pci_bar(dev,
					      AS_PCI_CONTROL_BAR_ID);
	if (IS_ERR(state->io_registers)) {
		res = PTR_ERR(state->io_registers);
		goto out_misc_deregister;
	}

	state->address_area = memremap_pci_bar(dev,
					       AS_PCI_AREA_BAR_ID,
					       MEMREMAP_WB);
	if (IS_ERR(state->address_area)) {
		res = PTR_ERR(state->address_area);
		goto out_iounmap;
	}

	state->address_area_phys_address =
		pci_resource_start(dev, AS_PCI_AREA_BAR_ID);

	as_write_register(state->io_registers,
			  AS_REGISTER_GUEST_PAGE_SIZE,
			  __PAGE_SIZE);
	as_write_register(state->io_registers,
			  AS_REGISTER_PHYS_START_LOW,
			  lower_32_bits(state->address_area_phys_address));
	as_write_register(state->io_registers,
			  AS_REGISTER_PHYS_START_HIGH,
			  upper_32_bits(state->address_area_phys_address));

	state->dev = dev;
	mutex_init(&state->registers_lock);

	pci_set_drvdata(dev, state);
	return 0;

out_iounmap:
	iounmap(state->io_registers);
out_misc_deregister:
	misc_deregister(&state->miscdevice);
out_release_area_bar:
	pci_release_region(dev, AS_PCI_AREA_BAR_ID);
out_release_control_bar:
	pci_release_region(dev, AS_PCI_CONTROL_BAR_ID);
out_free_device_state:
	kfree(state);

	return res;
}

static void as_pci_destroy_device(struct as_device_state *state)
{
	memunmap(state->address_area);
	iounmap(state->io_registers);
	misc_deregister(&state->miscdevice);
	pci_release_region(state->dev, AS_PCI_AREA_BAR_ID);
	pci_release_region(state->dev, AS_PCI_CONTROL_BAR_ID);
	kfree(state);
}

static int __must_check
as_pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	int res;
	u8 hardware_revision;

	res = pci_enable_device(dev);
	if (res)
		return res;

	res = pci_read_config_byte(dev, PCI_REVISION_ID, &hardware_revision);
	if (res)
		goto out_disable_pci;

	switch (hardware_revision) {
	case 1:
		res = create_as_device(dev, id);
		break;

	default:
		res = -ENODEV;
		goto out_disable_pci;
	}

	return 0;

out_disable_pci:
	pci_disable_device(dev);

	return res;
}

static void as_pci_remove(struct pci_dev *dev)
{
	struct as_device_state *state = pci_get_drvdata(dev);

	as_pci_destroy_device(state);
	pci_disable_device(dev);
}

static const struct pci_device_id as_pci_tbl[] = {
	{ PCI_DEVICE(AS_PCI_VENDOR_ID, AS_PCI_DEVICE_ID), },
	{ }
};
MODULE_DEVICE_TABLE(pci, as_pci_tbl);

static struct pci_driver goldfish_address_space_driver = {
	.name		= GOLDFISH_ADDRESS_SPACE_DEVICE_NAME,
	.id_table	= as_pci_tbl,
	.probe		= as_pci_probe,
	.remove		= as_pci_remove,
};

module_pci_driver(goldfish_address_space_driver);
