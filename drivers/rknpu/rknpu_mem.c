// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 */

#include <linux/version.h>
#include <linux/rk-dma-heap.h>
#include <linux/dma-resv.h>
#include <linux/iosys-map.h>

#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE
#include <linux/dma-map-ops.h>
#endif

#include "rknpu_drv.h"
#include "rknpu_ioctl.h"
#include "rknpu_mem.h"

#ifdef CONFIG_ROCKCHIP_RKNPU_DMA_HEAP

int rknpu_mem_create_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			   unsigned int cmd, unsigned long data)
{
	struct rknpu_mem_create args;
	int ret = -EINVAL;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;
	struct scatterlist *sgl;
	dma_addr_t phys;
	struct dma_buf *dmabuf;
	struct rknpu_mem_object *rknpu_obj = NULL;
	struct rknpu_session *session = NULL;
	int i, fd;
	unsigned int length;
	unsigned int in_size = _IOC_SIZE(cmd);
	unsigned int k_size = sizeof(struct rknpu_mem_create);
	char *k_data = (char *)&args;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_create *)data,
				    in_size))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	if (k_size > in_size)
		memset(k_data + in_size, 0, k_size - in_size);

	/* Non-contiguous (system-heap) buffers are only usable when the
	 * NPU iommu is on: it maps the scattered pages into one IOVA.
	 * librknnrt requests this layout as soon as it detects the iommu. */
	if ((args.flags & RKNPU_MEM_NON_CONTIGUOUS) && !rknpu_dev->iommu_en) {
		LOG_ERROR("%s: malloc iommu memory unsupported in current!\n",
			  __func__);
		ret = -EINVAL;
		return ret;
	}

	rknpu_obj = kzalloc(sizeof(*rknpu_obj), GFP_KERNEL);
	if (!rknpu_obj)
		return -ENOMEM;

	if (args.handle > 0) {
		fd = args.handle;

		dmabuf = dma_buf_get(fd);
		if (IS_ERR(dmabuf)) {
			ret = PTR_ERR(dmabuf);
			goto err_free_obj;
		}

		rknpu_obj->dmabuf = dmabuf;
		rknpu_obj->owner = 0;
	} else {
		/* Start test kernel alloc/free dma buf */
		dmabuf = rk_dma_heap_buffer_alloc(rknpu_dev->heap, args.size,
						  O_CLOEXEC | O_RDWR, 0x0,
						  dev_name(rknpu_dev->dev));
		if (IS_ERR(dmabuf)) {
			LOG_ERROR("dmabuf alloc failed, args.size = %llu\n",
				  args.size);
			ret = PTR_ERR(dmabuf);
			goto err_free_obj;
		}

		rknpu_obj->dmabuf = dmabuf;
		rknpu_obj->owner = 1;

		fd = dma_buf_fd(dmabuf, O_CLOEXEC | O_RDWR);
		if (fd < 0) {
			LOG_ERROR("dmabuf fd get failed\n");
			ret = -EFAULT;
			goto err_free_dma_buf;
		}
	}

	attachment = dma_buf_attach(dmabuf, rknpu_dev->dev);
	if (IS_ERR(attachment)) {
		LOG_ERROR("dma_buf_attach failed\n");
		ret = PTR_ERR(attachment);
		goto err_free_dma_buf;
	}

	table = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(table)) {
		LOG_ERROR("dma_buf_attach failed\n");
		dma_buf_detach(dmabuf, attachment);
		ret = PTR_ERR(table);
		goto err_free_dma_buf;
	}

	/* The NPU gets ONE device-visible base address for the buffer, so
	 * the buffer must be contiguous in device space: with the rknpu
	 * iommu enabled, dma_map_sgtable() coalesces it into a single
	 * segment whose dma address is the IOVA base; without an iommu it
	 * must be physically contiguous.
	 *
	 * The upstream loop kept the LAST segment's address/length: it
	 * handed the NPU the wrong base and under-mapped the kernel vmap.
	 * Buffers the exporter splits into several segments (e.g. the
	 * system dma-heap used by librknnrt) then had their tail pages
	 * unmapped, causing "Unable to handle kernel paging request" in
	 * rknpu_job_subcore_commit reading last_task->int_mask
	 * (LB2004 ramoops: pc=...+0x174, fault at vmap_base+0x307c). */
	for_each_sgtable_sg(table, sgl, i) {
		if (i == 0) {
			phys = sg_dma_address(sgl);
			length = sg_dma_len(sgl);
		}
		LOG_DEBUG("%s, %d, phys: %pad, length: %u\n", __func__,
			  __LINE__, &phys, length);
	}

	if (!rknpu_dev->iommu_en && table->nents > 1) {
		LOG_ERROR(
			"%s: scattered dmabuf (%u segments) requires the rknpu iommu, enable iommu@fde4b000\n",
			__func__, table->nents);
		ret = -EINVAL;
		goto err_detach_dma_buf;
	}

	if (args.flags & RKNPU_MEM_KERNEL_MAPPING) {
		/* dma_buf_vmap() maps every page of the buffer regardless of
		 * how the exporter laid it out.  The old hand-rolled vmap
		 * only mapped the last segment's pages, so the kernel read
		 * task descriptors past the end of the mapping. */
		dma_resv_lock(dmabuf->resv, NULL);
		ret = dma_buf_vmap(dmabuf, &rknpu_obj->vmap_map);
		dma_resv_unlock(dmabuf->resv);
		if (ret) {
			LOG_ERROR("dma_buf_vmap failed: %d\n", ret);
			goto err_detach_dma_buf;
		}
		rknpu_obj->kv_addr = rknpu_obj->vmap_map.vaddr;
		/* Hold an extra reference for the lifetime of the kernel
		 * mapping.  On process exit the fd table may close the
		 * dma_buf fd BEFORE the /dev/rknpu fd; without this extra
		 * ref the dmabuf would be released (dma_buf_release BUGs on
		 * a nonzero vmapping_counter) before rknpu_release runs the
		 * matching dma_buf_vunmap. */
		get_dma_buf(dmabuf);
	}

	rknpu_obj->size = PAGE_ALIGN(args.size);
	rknpu_obj->dma_addr = phys;
	rknpu_obj->sgt = table;

	args.size = rknpu_obj->size;
	args.obj_addr = (__u64)(uintptr_t)rknpu_obj;
	args.dma_addr = rknpu_obj->dma_addr;
	args.handle = fd;

	LOG_DEBUG(
		"args.handle: %d, args.size: %lld, rknpu_obj: %#llx, rknpu_obj->dma_addr: %#llx\n",
		args.handle, args.size, (__u64)(uintptr_t)rknpu_obj,
		(__u64)rknpu_obj->dma_addr);

	if (unlikely(copy_to_user((struct rknpu_mem_create *)data, &args,
				  in_size))) {
		LOG_ERROR("%s: copy_to_user failed\n", __func__);
		ret = -EFAULT;
		goto err_unmap_kv_addr;
	}

	dma_buf_unmap_attachment(attachment, table, DMA_BIDIRECTIONAL);
	dma_buf_detach(dmabuf, attachment);

	spin_lock(&rknpu_dev->lock);

	session = file->private_data;
	if (!session) {
		spin_unlock(&rknpu_dev->lock);
		ret = -EFAULT;
		goto err_unmap_kv_addr;
	}
	list_add_tail(&rknpu_obj->head, &session->list);

	spin_unlock(&rknpu_dev->lock);

	return 0;

err_unmap_kv_addr:
	if (rknpu_obj->kv_addr) {
		dma_resv_lock(rknpu_obj->dmabuf->resv, NULL);
		dma_buf_vunmap(rknpu_obj->dmabuf, &rknpu_obj->vmap_map);
		dma_resv_unlock(rknpu_obj->dmabuf->resv);
		rknpu_obj->kv_addr = NULL;
		iosys_map_clear(&rknpu_obj->vmap_map);
		/* drop the extra reference taken for the kernel map */
		dma_buf_put(rknpu_obj->dmabuf);
	}

err_detach_dma_buf:
	dma_buf_unmap_attachment(attachment, table, DMA_BIDIRECTIONAL);
	dma_buf_detach(dmabuf, attachment);

err_free_dma_buf:
	if (rknpu_obj->owner)
		rk_dma_heap_buffer_free(dmabuf);
	else
		dma_buf_put(dmabuf);

err_free_obj:
	kfree(rknpu_obj);

	return ret;
}

int rknpu_mem_destroy_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			    unsigned long data)
{
	struct rknpu_mem_object *rknpu_obj, *entry, *q;
	struct rknpu_session *session = NULL;
	struct rknpu_mem_destroy args;
	int ret = -EFAULT;
	bool found = false;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_destroy *)data,
				    sizeof(struct rknpu_mem_destroy)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	if (!kern_addr_valid(args.obj_addr)) {
		LOG_ERROR("%s: invalid obj_addr: %#llx\n", __func__,
			  (__u64)(uintptr_t)args.obj_addr);
		ret = -EINVAL;
		return ret;
	}

	rknpu_obj = (struct rknpu_mem_object *)(uintptr_t)args.obj_addr;
	LOG_DEBUG(
		"free args.handle: %d, rknpu_obj: %#llx, rknpu_obj->dma_addr: %#llx\n",
		args.handle, (__u64)(uintptr_t)rknpu_obj,
		(__u64)rknpu_obj->dma_addr);

	spin_lock(&rknpu_dev->lock);
	session = file->private_data;
	if (!session) {
		spin_unlock(&rknpu_dev->lock);
		ret = -EFAULT;
		return ret;
	}
	list_for_each_entry_safe(entry, q, &session->list, head) {
		if (entry == rknpu_obj) {
			list_del(&entry->head);
			found = true;
			break;
		}
	}
	spin_unlock(&rknpu_dev->lock);

	/* If the object is not on this session's list it was already
	 * freed by rknpu_release (fd close order: /dev/rknpu fd may be
	 * closed before the dma_buf fd).  Skipping the vunmap here would
	 * leak the dmabuf vmapping_counter and the extra get_dma_buf
	 * reference taken at create time, which eventually trips
	 * BUG_ON/WARN_ON in dma_buf_release. */
	if (!found) {
		LOG_ERROR("%s: object %#llx not in session list\n", __func__,
			  (__u64)(uintptr_t)rknpu_obj);
		return -EINVAL;
	}

	if (rknpu_obj->kv_addr && rknpu_obj->dmabuf) {
		dma_resv_lock(rknpu_obj->dmabuf->resv, NULL);
		dma_buf_vunmap(rknpu_obj->dmabuf, &rknpu_obj->vmap_map);
		dma_resv_unlock(rknpu_obj->dmabuf->resv);
		rknpu_obj->kv_addr = NULL;
		iosys_map_clear(&rknpu_obj->vmap_map);
		/* drop the extra reference taken for the kernel map */
		dma_buf_put(rknpu_obj->dmabuf);
	}

	if (rknpu_obj->dmabuf && !rknpu_obj->owner)
		dma_buf_put(rknpu_obj->dmabuf);

	kfree(rknpu_obj);

	return 0;
}

/*
 * begin cpu access => for_cpu = true
 * end cpu access => for_cpu = false
 */
static void __maybe_unused rknpu_dma_buf_sync(
	struct rknpu_device *rknpu_dev, struct rknpu_mem_object *rknpu_obj,
	u32 offset, u32 length, enum dma_data_direction dir, bool for_cpu)
{
	struct device *dev = rknpu_dev->dev;
	struct sg_table *sgt = rknpu_obj->sgt;
	struct scatterlist *sg = sgt->sgl;
	dma_addr_t sg_dma_addr = sg_dma_address(sg);
	unsigned int len = 0;
	int i;

	for_each_sgtable_sg(sgt, sg, i) {
		unsigned int sg_offset, sg_left, size = 0;

		len += sg->length;
		if (len <= offset) {
			sg_dma_addr += sg->length;
			continue;
		}

		sg_left = len - offset;
		sg_offset = sg->length - sg_left;

		size = (length < sg_left) ? length : sg_left;

		if (for_cpu)
			dma_sync_single_range_for_cpu(dev, sg_dma_addr,
						      sg_offset, size, dir);
		else
			dma_sync_single_range_for_device(dev, sg_dma_addr,
							 sg_offset, size, dir);

		offset += size;
		length -= size;
		sg_dma_addr += sg->length;

		if (length == 0)
			break;
	}
}

int rknpu_mem_sync_ioctl(struct rknpu_device *rknpu_dev, unsigned long data)
{
	struct rknpu_mem_object *rknpu_obj = NULL;
	struct rknpu_mem_sync args;
#ifdef CONFIG_DMABUF_PARTIAL
	struct dma_buf *dmabuf;
#endif
	int ret = -EFAULT;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_sync *)data,
				    sizeof(struct rknpu_mem_sync)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	if (!kern_addr_valid(args.obj_addr)) {
		LOG_ERROR("%s: invalid obj_addr: %#llx\n", __func__,
			  (__u64)(uintptr_t)args.obj_addr);
		ret = -EINVAL;
		return ret;
	}

	rknpu_obj = (struct rknpu_mem_object *)(uintptr_t)args.obj_addr;

#ifndef CONFIG_DMABUF_PARTIAL
	if (args.flags & RKNPU_MEM_SYNC_TO_DEVICE) {
		rknpu_dma_buf_sync(rknpu_dev, rknpu_obj, args.offset, args.size,
				   DMA_TO_DEVICE, false);
	}
	if (args.flags & RKNPU_MEM_SYNC_FROM_DEVICE) {
		rknpu_dma_buf_sync(rknpu_dev, rknpu_obj, args.offset, args.size,
				   DMA_FROM_DEVICE, true);
	}
#else
	dmabuf = rknpu_obj->dmabuf;
	if (args.flags & RKNPU_MEM_SYNC_TO_DEVICE) {
		dmabuf->ops->end_cpu_access_partial(dmabuf, DMA_TO_DEVICE,
						    args.offset, args.size);
	}
	if (args.flags & RKNPU_MEM_SYNC_FROM_DEVICE) {
		dmabuf->ops->begin_cpu_access_partial(dmabuf, DMA_FROM_DEVICE,
						      args.offset, args.size);
	}
#endif

	return 0;
}

#endif
