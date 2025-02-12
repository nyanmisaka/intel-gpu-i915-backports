/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2014-2016 Intel Corporation
 */

#include <linux/anon_inodes.h>
#include <linux/mman.h>
#include <linux/pfn_t.h>
#include <linux/sizes.h>

#include <drm/drm_cache.h>

#include "gt/intel_gt.h"
#include "gt/intel_gt_requests.h"

#include "i915_drv.h"
#include "i915_gem_gtt.h"
#include "i915_gem_ioctls.h"
#include "i915_gem_lmem.h"
#include "i915_gem_object.h"
#include "i915_gem_mman.h"
#include "i915_mm.h"
#include "i915_trace.h"
#include "i915_user_extensions.h"
#include "i915_gem_ttm.h"
#include "i915_vma.h"

static int
__assign_mmap_offset(struct drm_i915_gem_object *obj,
		     enum i915_mmap_type mmap_type,
		     u64 *offset, struct drm_file *file)
{
	struct i915_mmap_offset *mmo;

	if (i915_gem_object_never_mmap(obj))
		return -ENODEV;

	if (obj->ops->mmap_offset) {
		*offset = obj->ops->mmap_offset(obj);
		return 0;
	}

	if (mmap_type != I915_MMAP_TYPE_GTT &&
	    !i915_gem_object_has_struct_page(obj) &&
	    !i915_gem_object_type_has(obj, I915_GEM_OBJECT_HAS_IOMEM))
		return -ENODEV;

	if (i915_gem_object_is_lmem(obj) &&
	    i915_is_level4_wa_active(obj->mm.region.mem->gt) &&
	    !i915_gem_object_should_migrate_smem(obj, NULL) &&
	    obj->mm.region.mem->instance > 0)
		drm_dbg(obj->base.dev,
			"Trying to mmap lmem1 when L4wa is enabled\n");

	mmo = i915_gem_mmap_offset_attach(obj, mmap_type, file);
	if (IS_ERR(mmo))
		return PTR_ERR(mmo);

	*offset = drm_vma_node_offset_addr(&mmo->vma_node);
	return 0;
}

/**
 * i915_gem_mmap_ioctl - Maps the contents of an object, returning the address
 *			 it is mapped to.
 * @dev: drm device
 * @data: ioctl data blob
 * @file: drm file
 *
 * While the mapping holds a reference on the contents of the object, it doesn't
 * imply a ref on the object itself.
 *
 * IMPORTANT:
 *
 * DRM driver writers who look a this function as an example for how to do GEM
 * mmap support, please don't implement mmap support like here. The modern way
 * to implement DRM mmap support is with an mmap offset ioctl (like
 * i915_gem_mmap_gtt) and then using the mmap syscall on the DRM fd directly.
 * That way debug tooling like valgrind will understand what's going on, hiding
 * the mmap call in a driver private ioctl will break that. The i915 driver only
 * does cpu mmaps this way because we didn't know better.
 */
int
i915_gem_mmap_ioctl(struct drm_device *dev, void *data,
		    struct drm_file *file)
{
	struct drm_i915_gem_mmap *args = data;
	struct drm_i915_gem_object *obj;
	enum i915_mmap_type mmap_type;
	unsigned long addr;
	u64 offset;

	if (args->flags & ~(I915_MMAP_WC))
		return -EINVAL;

	if (args->flags & I915_MMAP_WC && !pat_enabled())
		return -ENODEV;

	obj = i915_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	if (range_overflows(args->offset, args->size, (u64)obj->base.size)) {
		addr = -EINVAL;
		goto err;
	}

	mmap_type = I915_MMAP_TYPE_WB;
	if (args->flags & I915_MMAP_WC)
		mmap_type = I915_MMAP_TYPE_WC;

	addr = __assign_mmap_offset(obj, mmap_type, &offset, file);
	if (addr)
		goto err;

	addr = vm_mmap(file->filp, 0, args->size,
		       PROT_READ | PROT_WRITE, MAP_SHARED,
		       offset + args->offset);
	if (!IS_ERR_VALUE(addr)) {
		args->addr_ptr = (u64)addr;
		addr = 0;
	}

err:
	i915_gem_object_put(obj);
	return addr;
}

static unsigned int tile_row_pages(const struct drm_i915_gem_object *obj)
{
	return i915_gem_object_get_tile_row_size(obj) >> PAGE_SHIFT;
}

/**
 * i915_gem_mmap_gtt_version - report the current feature set for GTT mmaps
 *
 * A history of the GTT mmap interface:
 *
 * 0 - Everything had to fit into the GTT. Both parties of a memcpy had to
 *     aligned and suitable for fencing, and still fit into the available
 *     mappable space left by the pinned display objects. A classic problem
 *     we called the page-fault-of-doom where we would ping-pong between
 *     two objects that could not fit inside the GTT and so the memcpy
 *     would page one object in at the expense of the other between every
 *     single byte.
 *
 * 1 - Objects can be any size, and have any compatible fencing (X Y, or none
 *     as set via i915_gem_set_tiling() [DRM_I915_GEM_SET_TILING]). If the
 *     object is too large for the available space (or simply too large
 *     for the mappable aperture!), a view is created instead and faulted
 *     into userspace. (This view is aligned and sized appropriately for
 *     fenced access.)
 *
 * 2 - Recognise WC as a separate cache domain so that we can flush the
 *     delayed writes via GTT before performing direct access via WC.
 *
 * 3 - Remove implicit set-domain(GTT) and synchronisation on initial
 *     pagefault; swapin remains transparent.
 *
 * 4 - Support multiple fault handlers per object depending on object's
 *     backing storage (a.k.a. MMAP_OFFSET).
 *
 * Restrictions:
 *
 *  * snoopable objects cannot be accessed via the GTT. It can cause machine
 *    hangs on some architectures, corruption on others. An attempt to service
 *    a GTT page fault from a snoopable object will generate a SIGBUS.
 *
 *  * the object must be able to fit into RAM (physical memory, though no
 *    limited to the mappable aperture).
 *
 *
 * Caveats:
 *
 *  * a new GTT page fault will synchronize rendering from the GPU and flush
 *    all data to system memory. Subsequent access will not be synchronized.
 *
 *  * all mappings are revoked on runtime device suspend.
 *
 *  * there are only 8, 16 or 32 fence registers to share between all users
 *    (older machines require fence register for display and blitter access
 *    as well). Contention of the fence registers will cause the previous users
 *    to be unmapped and any new access will generate new page faults.
 *
 *  * running out of memory while servicing a fault may generate a SIGBUS,
 *    rather than the expected SIGSEGV.
 */
int i915_gem_mmap_gtt_version(void)
{
	return 4;
}

static inline struct i915_ggtt_view
compute_partial_view(const struct drm_i915_gem_object *obj,
		     pgoff_t page_offset,
		     unsigned int chunk)
{
	struct i915_ggtt_view view;

	if (i915_gem_object_is_tiled(obj))
		chunk = roundup(chunk, tile_row_pages(obj) ?: 1);

	memset(&view, 0, sizeof(view));
	view.type = I915_GGTT_VIEW_PARTIAL;
	view.partial.offset = rounddown(page_offset, chunk);
	view.partial.size =
		min_t(unsigned int, chunk,
		      (obj->base.size >> PAGE_SHIFT) - view.partial.offset);

	/* If the partial covers the entire object, just create a normal VMA. */
	if (chunk >= obj->base.size >> PAGE_SHIFT)
		view.type = I915_GGTT_VIEW_NORMAL;

	return view;
}

vm_fault_t i915_error_to_vmf_fault(int err)
{
	switch (err) {
	default:
		WARN_ONCE(err, "unhandled error in %s: %i\n", __func__, err);
		fallthrough;
	case -EIO: /* shmemfs failure from swap device */
	case -EFAULT: /* purged object */
	case -ENODEV: /* bad object, how did you get here! */
	case -ENXIO: /* unable to access backing store (on device) */
	case -E2BIG: /* object does not fit in backing store */
		return VM_FAULT_SIGBUS;

	case -ENOMEM: /* our allocation failure */
		return VM_FAULT_OOM;

	case 0:
	case -EAGAIN:
	case -ENOSPC: /* transient failure to evict? */
	case -ERESTARTSYS:
	case -EINTR:
	case -EBUSY:
		/*
		 * EBUSY is ok: this just means that another thread
		 * already did the job.
		 */
		return VM_FAULT_NOPAGE;
	}
}

static struct drm_i915_gem_object *
create_swapto(struct drm_i915_gem_object *obj, bool write)
{
	struct drm_i915_gem_object *swp;
	u64 size;

	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_MMAP_SWAP_CREATE))
		return obj;

	if (!i915_gem_object_is_lmem(obj))
		return obj;

	if (i915_gem_object_has_pages(obj) ||
	    obj->mm.madv == __I915_MADV_PURGED)
		return obj;

	i915_gem_flush_free_objects(to_i915(obj->base.dev));

	/* Prefer to write directly to lmem unless we will evict */
	size = obj->base.size;
	if (write && 2 * size < atomic64_read(&obj->mm.region.mem->avail))
		return obj;

	if (HAS_FLAT_CCS(to_i915(obj->base.dev)) &&
	    !intel_gt_is_wedged(obj->mm.region.mem->gt))
		size += size >> 8;

	swp = i915_gem_object_create_shmem(to_i915(obj->base.dev), size);
	if (IS_ERR(swp))
		return obj;

	swp->flags |= I915_BO_CPU_CLEAR;
	i915_gem_object_share_resv(obj, swp);

	GEM_BUG_ON(obj->swapto);
	obj->swapto = swp;

	return swp;
}

static struct drm_i915_gem_object *
use_swapto(struct drm_i915_gem_object *obj, bool write)
{
	struct drm_i915_gem_object *swp = obj->swapto;

	if (!IS_ENABLED(CPTCFG_DRM_I915_CHICKEN_MMAP_SWAP))
		return obj;

	if (!swp || swp->mm.madv != I915_MADV_WILLNEED)
		return create_swapto(obj, write);

	GEM_BUG_ON(swp->base.resv != obj->base.resv);
	return swp;
}

static vm_fault_t vm_fault_cpu(struct vm_fault *vmf)
{
	struct vm_area_struct *area = vmf->vma;
	struct i915_mmap_offset *mmo = area->vm_private_data;
	struct drm_i915_gem_object *obj = mmo->obj, *pg;
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	pgoff_t page_offset = (vmf->address - area->vm_start) >> PAGE_SHIFT;
	long obj_offset = area->vm_pgoff - drm_vma_node_start(&mmo->vma_node);
	bool write = area->vm_flags & VM_WRITE;
	unsigned long vm_start, vm_size;
	struct i915_gem_ww_ctx ww;
	resource_size_t iomap;
	vm_fault_t ret;
	int err;

	/* Sanity check that we allow writing into this object */
	if (unlikely(i915_gem_object_is_readonly(obj) && write))
		return VM_FAULT_SIGBUS;

	trace_i915_gem_object_fault(obj, vmf->address, obj_offset + page_offset, false, write);

	atomic_inc(&i915->active_fault_handlers);

	/* Do not service faults if invalidate_lmem_mmaps is set */
	if (READ_ONCE(i915->invalidate_lmem_mmaps)) {
		ret = VM_FAULT_SIGBUS;
		goto out;
	}

	/* for segmented BO, lookup and fill PTEs for just one segment */
	vm_start = area->vm_start;
	vm_size = area->vm_end;
	if (i915_gem_object_has_segments(obj)) {
		obj = i915_gem_object_lookup_segment(obj,
						     (obj_offset + page_offset) << PAGE_SHIFT,
						     NULL);
		if (!obj) {
			ret = VM_FAULT_SIGBUS;
			goto out;
		}

		vm_size = obj->base.size;
		if (obj_offset > obj->segment_offset >> PAGE_SHIFT) {
			obj_offset -= obj->segment_offset >> PAGE_SHIFT;
			vm_size -= obj_offset << PAGE_SHIFT;
		} else {
			vm_start = area->vm_start + obj->segment_offset - (obj_offset << PAGE_SHIFT);
			obj_offset = 0;
		}
		vm_size = min(area->vm_end, vm_start + vm_size);
	}
	vm_size -= vm_start;

	do for_i915_gem_ww(&ww, err, true) {
		bool required = false;

		err = i915_gem_object_lock(obj, &ww);
		if (err)
			continue;

		pg = use_swapto(obj, write);

		/* Implicitly migrate BO to SMEM if criteria met */
		if (i915_gem_object_should_migrate_smem(pg, &required)) {
			if (i915_gem_object_has_pinned_pages(pg))
				/*
				 * If pinned pages, migrate will fail with
				 * -EBUSY. A retry of fault/migration will
				 * not succeed and will retry indefinitely,
				 * so fail the fault (SIGBUS) if required to
				 * migrate.
				 */
				err = -EFAULT;
			else
				err = i915_gem_object_migrate_to_smem(pg, &ww, false);
			if (err && required)
				/*
				 * Atomic hint requires migration, but we
				 * cannot. Depending on error, fail or retry.
				 */
				continue;
			else if (err == -EDEADLK)
				continue;
			else
				/* Migration not required, just best effort. */
				err = 0;
		}

		err = i915_gem_object_pin_pages_sync(pg);
		if (err)
			continue;

		iomap = -1;
		if (!i915_gem_object_has_struct_page(pg)) {
			iomap = pg->mm.region.mem->iomap.base;
			iomap -= pg->mm.region.mem->region.start;
		}

		/* PTEs are revoked in obj->ops->put_pages() */
		err = remap_io_sg(area, vm_start, vm_size,
				  pg->mm.pages->sgl, obj_offset,
				  iomap);

		i915_gem_object_unpin_pages(pg);
	} while (err == -ENXIO || err == -ENOMEM);

	ret = i915_error_to_vmf_fault(err);
out:
	if (atomic_dec_and_test(&i915->active_fault_handlers))
		wake_up_var(&i915->active_fault_handlers);

	return ret;
}

static vm_fault_t vm_fault_gtt(struct vm_fault *vmf)
{
#define MIN_CHUNK_PAGES (SZ_1M >> PAGE_SHIFT)
	const unsigned int guard = PIN_OFFSET_GUARD | SZ_4K;
	struct vm_area_struct *area = vmf->vma;
	struct i915_mmap_offset *mmo = area->vm_private_data;
	struct drm_i915_gem_object *obj = mmo->obj;
	struct drm_device *dev = obj->base.dev;
	struct drm_i915_private *i915 = to_i915(dev);
	struct intel_runtime_pm *rpm = &i915->runtime_pm;
	struct i915_ggtt *ggtt = to_gt(i915)->ggtt;
	bool write = area->vm_flags & VM_WRITE;
	struct i915_gem_ww_ctx ww;
	intel_wakeref_t wakeref;
	struct i915_vma *vma;
	pgoff_t page_offset;
	int srcu;
	int ret;

	/* We don't use vmf->pgoff since that has the fake offset */
	page_offset = (vmf->address - area->vm_start) >> PAGE_SHIFT;

	trace_i915_gem_object_fault(obj, vmf->address, page_offset, true, write);

	wakeref = intel_runtime_pm_get(rpm);

	i915_gem_ww_ctx_init(&ww, true);
retry:
	ret = i915_gem_object_lock(obj, &ww);
	if (ret)
		goto err_rpm;

	/* Sanity check that we allow writing into this object */
	if (i915_gem_object_is_readonly(obj) && write) {
		ret = -EFAULT;
		goto err_rpm;
	}

	ret = i915_gem_object_pin_pages_sync(obj);
	if (ret)
		goto err_rpm;

	ret = intel_gt_reset_lock_interruptible(ggtt->vm.gt, &srcu);
	if (ret)
		goto err_pages;

	/* Now pin it into the GTT as needed */
	vma = i915_gem_object_ggtt_pin_ww(obj, &ww, ggtt, NULL, 0, 0,
					  guard |
					  PIN_MAPPABLE |
					  PIN_NONBLOCK /* NOWARN */ |
					  PIN_NOEVICT);
	if (IS_ERR(vma) && vma != ERR_PTR(-EDEADLK)) {
		/* Use a partial view if it is bigger than available space */
		struct i915_ggtt_view view =
			compute_partial_view(obj, page_offset, MIN_CHUNK_PAGES);
		unsigned int flags;

		flags = PIN_MAPPABLE | PIN_NOSEARCH;
		if (view.type == I915_GGTT_VIEW_NORMAL)
			flags |= PIN_NONBLOCK; /* avoid warnings for pinned */

		/*
		 * Userspace is now writing through an untracked VMA, abandon
		 * all hope that the hardware is able to track future writes.
		 */

		vma = i915_gem_object_ggtt_pin_ww(obj, &ww, ggtt, &view,
						  0, 0, guard | flags);
		if (IS_ERR(vma) && vma != ERR_PTR(-EDEADLK)) {
			flags = PIN_MAPPABLE;
			view.type = I915_GGTT_VIEW_PARTIAL;
			vma = i915_gem_object_ggtt_pin_ww(obj, &ww, ggtt, &view,
							  0, 0, guard | flags);
		}

		/* The entire mappable GGTT is pinned? Unexpected! */
		GEM_BUG_ON(vma == ERR_PTR(-ENOSPC));
	}
	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto err_reset;
	}

	/* Access to snoopable pages through the GTT is incoherent. */
	if (!(i915_gem_object_has_cache_level(obj, I915_CACHE_NONE) ||
	      HAS_LLC(i915))) {
		ret = -EFAULT;
		goto err_unpin;
	}

	ret = i915_vma_pin_fence_wait(vma);
	if (ret)
		goto err_unpin;

	/* Finally, remap it using the new GTT offset */
	ret = remap_io_mapping(area,
			       area->vm_start + (vma->ggtt_view.partial.offset << PAGE_SHIFT),
			       (ggtt->gmadr.start + i915_ggtt_offset(vma)) >> PAGE_SHIFT,
			       min_t(u64, vma->size, area->vm_end - area->vm_start),
			       &ggtt->iomap);
	if (ret)
		goto err_fence;

	assert_rpm_wakelock_held(rpm);

	/* Mark as being mmapped into userspace for later revocation */
	mutex_lock(&to_gt(i915)->ggtt->vm.mutex);
	if (!i915_vma_set_userfault(vma) && !obj->userfault_count++)
		list_add(&obj->userfault_link, &to_gt(i915)->ggtt->userfault_list);
	mutex_unlock(&to_gt(i915)->ggtt->vm.mutex);

	/* Track the mmo associated with the fenced vma */
	vma->mmo = mmo;

	if (CPTCFG_DRM_I915_USERFAULT_AUTOSUSPEND)
		intel_wakeref_auto(&to_gt(i915)->ggtt->userfault_wakeref,
				   msecs_to_jiffies_timeout(CPTCFG_DRM_I915_USERFAULT_AUTOSUSPEND));

	if (write) {
		GEM_BUG_ON(!i915_gem_object_has_pinned_pages(obj));
		i915_vma_set_ggtt_write(vma);
	}

err_fence:
	i915_vma_unpin_fence(vma);
err_unpin:
	__i915_vma_unpin(vma);
err_reset:
	intel_gt_reset_unlock(ggtt->vm.gt, srcu);
err_pages:
	i915_gem_object_unpin_pages(obj);
err_rpm:
	if (ret == -EDEADLK) {
		ret = i915_gem_ww_ctx_backoff(&ww);
		if (!ret)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);
	intel_runtime_pm_put(rpm, wakeref);
	return i915_error_to_vmf_fault(ret);
}

static int
vm_access(struct vm_area_struct *area, unsigned long addr,
	  void *buf, int len, int write)
{
	struct i915_mmap_offset *mmo = area->vm_private_data;
	struct drm_i915_gem_object *obj = mmo->obj;
	struct i915_gem_ww_ctx ww;
	unsigned long offset;
	void *vaddr;
	int err = 0;

	if (i915_gem_object_is_readonly(obj) && write)
		return -EACCES;

	addr -= area->vm_start;
	if (range_overflows_t(u64, addr, len, obj->base.size))
		return -EINVAL;

	if (i915_gem_object_has_segments(obj)) {
		obj = i915_gem_object_lookup_segment(obj, addr, &offset);
		if (!obj)
			return -EINVAL;

		if (len > obj->base.size - offset) {
			/*  XXX more work to support multiple segments */
			return -ENXIO;
		}
	} else {
		offset = addr;
	}

	i915_gem_ww_ctx_init(&ww, true);
retry:
	err = i915_gem_object_lock(obj, &ww);
	if (err)
		goto out;

	/* As this is primarily for debugging, let's focus on simplicity */
	vaddr = i915_gem_object_pin_map(obj, I915_MAP_FORCE_WC);
	if (IS_ERR(vaddr)) {
		err = PTR_ERR(vaddr);
		goto out;
	}

	if (write) {
		memcpy(vaddr + offset, buf, len);
		__i915_gem_object_flush_map(obj, offset, len);
	} else {
		memcpy(buf, vaddr + offset, len);
	}

	i915_gem_object_unpin_map(obj);
out:
	if (err == -EDEADLK) {
		err = i915_gem_ww_ctx_backoff(&ww);
		if (!err)
			goto retry;
	}
	i915_gem_ww_ctx_fini(&ww);

	if (err)
		return err;

	return len;
}

void __i915_gem_object_release_mmap_gtt(struct drm_i915_gem_object *obj)
{
	struct i915_vma *vma;

	GEM_BUG_ON(!obj->userfault_count);

	for_each_ggtt_vma(vma, obj)
		i915_vma_revoke_mmap(vma);

	GEM_BUG_ON(obj->userfault_count);
}

/*
 * It is vital that we remove the page mapping if we have mapped a tiled
 * object through the GTT and then lose the fence register due to
 * resource pressure. Similarly if the object has been moved out of the
 * aperture, than pages mapped into userspace must be revoked. Removing the
 * mapping will then trigger a page fault on the next user access, allowing
 * fixup by vm_fault_gtt().
 */
void i915_gem_object_release_mmap_gtt(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	intel_wakeref_t wakeref;

	/*
	 * Serialisation between user GTT access and our code depends upon
	 * revoking the CPU's PTE whilst the mutex is held. The next user
	 * pagefault then has to wait until we release the mutex.
	 *
	 * Note that RPM complicates somewhat by adding an additional
	 * requirement that operations to the GGTT be made holding the RPM
	 * wakeref.
	 */
	wakeref = intel_runtime_pm_get(&i915->runtime_pm);
	mutex_lock(&to_gt(i915)->ggtt->vm.mutex);

	if (!obj->userfault_count)
		goto out;

	__i915_gem_object_release_mmap_gtt(obj);

	/*
	 * Ensure that the CPU's PTE are revoked and there are not outstanding
	 * memory transactions from userspace before we return. The TLB
	 * flushing implied above by changing the PTE above *should* be
	 * sufficient, an extra barrier here just provides us with a bit
	 * of paranoid documentation about our requirement to serialise
	 * memory writes before touching registers / GSM.
	 */
	wmb();

out:
	mutex_unlock(&to_gt(i915)->ggtt->vm.mutex);
	intel_runtime_pm_put(&i915->runtime_pm, wakeref);
}

static inline void
drm_vma_node_unmap_range(struct drm_vma_offset_node *node,
			 struct address_space *file_mapping,
			 unsigned long offset,
			 unsigned long length)
{
	unmap_mapping_range(file_mapping,
			    drm_vma_node_offset_addr(node) + offset,
			    length, 1);
}

/*
 * For segmented BOs, this function will be called as needed directly
 * for each BO segment to unmap only that segment which is known by
 * caller to have backing store.  However, during object free of the
 * parent BO, the parent BO is ultimately responsible to clear all of
 * the mmaps as obj->parent for the segment BOs will be NULL.
 */
void i915_gem_object_release_mmap_offset(struct drm_i915_gem_object *obj)
{
	struct i915_mmap_offset *mmo, *mn;
	unsigned long unmap_size = obj->base.size;
	unsigned long vma_offset = 0;

	if (i915_gem_object_is_segment(obj)) {
		/*
		 * Segmented BOs use single mmo in parent. If parent
		 * is NULL, then just return (see comment above).
		 */
		if (!obj->parent)
			return;
		vma_offset = obj->segment_offset;
		obj = obj->parent;
	}

	spin_lock(&obj->mmo.lock);
	rbtree_postorder_for_each_entry_safe(mmo, mn,
					     &obj->mmo.offsets, offset) {
		/*
		 * vma_node_unmap for GTT mmaps handled already in
		 * __i915_gem_object_release_mmap_gtt
		 */
		if (mmo->mmap_type == I915_MMAP_TYPE_GTT)
			continue;

		spin_unlock(&obj->mmo.lock);
		drm_vma_node_unmap_range(&mmo->vma_node,
					 obj->base.dev->anon_inode->i_mapping,
					 vma_offset, unmap_size);
		spin_lock(&obj->mmo.lock);
	}
	spin_unlock(&obj->mmo.lock);
}

/**
 * i915_gem_object_release_mmap - remove physical page mappings
 * @obj: obj in question
 *
 * Preserve the reservation of the mmapping with the DRM core code, but
 * relinquish ownership of the pages back to the system.
 */
void i915_gem_object_release_mmap(struct drm_i915_gem_object *obj)
{
	i915_gem_object_release_mmap_gtt(obj);
	i915_gem_object_release_mmap_offset(obj);
}

static struct i915_mmap_offset *
lookup_mmo(struct drm_i915_gem_object *obj,
	   enum i915_mmap_type mmap_type)
{
	struct rb_node *rb;

	spin_lock(&obj->mmo.lock);
	rb = obj->mmo.offsets.rb_node;
	while (rb) {
		struct i915_mmap_offset *mmo =
			rb_entry(rb, typeof(*mmo), offset);

		if (mmo->mmap_type == mmap_type) {
			spin_unlock(&obj->mmo.lock);
			return mmo;
		}

		if (mmo->mmap_type < mmap_type)
			rb = rb->rb_right;
		else
			rb = rb->rb_left;
	}
	spin_unlock(&obj->mmo.lock);

	return NULL;
}

static struct i915_mmap_offset *
insert_mmo(struct drm_i915_gem_object *obj, struct i915_mmap_offset *mmo)
{
	struct rb_node *rb, **p;

	spin_lock(&obj->mmo.lock);
	rb = NULL;
	p = &obj->mmo.offsets.rb_node;
	while (*p) {
		struct i915_mmap_offset *pos;

		rb = *p;
		pos = rb_entry(rb, typeof(*pos), offset);

		if (pos->mmap_type == mmo->mmap_type) {
			spin_unlock(&obj->mmo.lock);
			drm_vma_offset_remove(obj->base.dev->vma_offset_manager,
					      &mmo->vma_node);
			kfree(mmo);
			return pos;
		}

		if (pos->mmap_type < mmo->mmap_type)
			p = &rb->rb_right;
		else
			p = &rb->rb_left;
	}
	rb_link_node(&mmo->offset, rb, p);
	rb_insert_color(&mmo->offset, &obj->mmo.offsets);
	spin_unlock(&obj->mmo.lock);

	return mmo;
}

static int
vma_node_allow_once(struct drm_vma_offset_node *node, struct drm_file *tag)
{
        struct rb_node **iter;
        struct rb_node *parent = NULL;
        struct drm_vma_offset_file *new, *entry;
        int ret = 0;

        new = kmalloc(sizeof(*entry), GFP_KERNEL);
        write_lock(&node->vm_lock);

        iter = &node->vm_files.rb_node;
        while (likely(*iter)) {
                parent = *iter;
                entry = rb_entry(*iter, struct drm_vma_offset_file, vm_rb);

                if (tag == entry->vm_tag)
                        goto unlock;
                else if (tag > entry->vm_tag)
                        iter = &parent->rb_right;
                else
                        iter = &parent->rb_left;
        }

        if (!new) {
                ret = -ENOMEM;
                goto unlock;
        }

        new->vm_tag = tag;
        new->vm_count = 1;
        rb_link_node(&new->vm_rb, parent, iter);
        rb_insert_color(&new->vm_rb, &node->vm_files);
        new = NULL;

unlock:
        write_unlock(&node->vm_lock);
        kfree(new);
        return ret;
}

struct i915_mmap_offset *
i915_gem_mmap_offset_attach(struct drm_i915_gem_object *obj,
			    enum i915_mmap_type mmap_type,
			    struct drm_file *file)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct i915_mmap_offset *mmo;
	struct intel_gt *gt;
	int err;
	int i;

	GEM_BUG_ON(obj->ops->mmap_offset || obj->ops->mmap_ops);

	mmo = lookup_mmo(obj, mmap_type);
	if (mmo)
		goto out;

	mmo = kmalloc(sizeof(*mmo), GFP_KERNEL);
	if (!mmo)
		return ERR_PTR(-ENOMEM);

	mmo->obj = obj;
	mmo->mmap_type = mmap_type;
	drm_vma_node_reset(&mmo->vma_node);

	err = drm_vma_offset_add(obj->base.dev->vma_offset_manager,
				 &mmo->vma_node, obj->base.size / PAGE_SIZE);
	if (likely(!err))
		goto insert;

	/* Attempt to reap some mmap space from dead objects */
	for_each_gt(gt, i915, i)
		intel_gt_retire_requests(gt);
	i915_gem_drain_freed_objects(i915);

	err = drm_vma_offset_add(obj->base.dev->vma_offset_manager,
				 &mmo->vma_node, obj->base.size / PAGE_SIZE);
	if (err)
		goto err;

insert:
	mmo = insert_mmo(obj, mmo);
	GEM_BUG_ON(lookup_mmo(obj, mmap_type) != mmo);
out:
	if (file) {
		err = vma_node_allow_once(&mmo->vma_node, file);
		if (err)
			return ERR_PTR(err);
	}

	return mmo;

err:
	kfree(mmo);
	return ERR_PTR(err);
}

static int
__assign_mmap_offset_handle(struct drm_file *file,
			    u32 handle,
			    enum i915_mmap_type mmap_type,
			    u64 *offset)
{
	struct drm_i915_gem_object *obj;
	int err;

	obj = i915_gem_object_lookup(file, handle);
	if (!obj)
		return -ENOENT;

	err = __assign_mmap_offset(obj, mmap_type, offset, file);
	i915_gem_object_put(obj);
	return err;
}

int
i915_gem_dumb_mmap_offset(struct drm_file *file,
			  struct drm_device *dev,
			  u32 handle,
			  u64 *offset)
{
	struct drm_i915_private *i915 = to_i915(dev);
	enum i915_mmap_type mmap_type;

	if (pat_enabled())
		mmap_type = I915_MMAP_TYPE_WC;
	else if (!i915_ggtt_has_aperture(to_gt(i915)->ggtt))
		return -ENODEV;
	else
		mmap_type = I915_MMAP_TYPE_GTT;

	return __assign_mmap_offset_handle(file, handle, mmap_type, offset);
}

/**
 * i915_gem_mmap_offset_ioctl - prepare an object for GTT mmap'ing
 * @dev: DRM device
 * @data: GTT mapping ioctl data
 * @file: GEM object info
 *
 * Simply returns the fake offset to userspace so it can mmap it.
 * The mmap call will end up in drm_gem_mmap(), which will set things
 * up so we can get faults in the handler above.
 *
 * The fault handler will take care of binding the object into the GTT
 * (since it may have been evicted to make room for something), allocating
 * a fence register, and mapping the appropriate aperture address into
 * userspace.
 */
int
i915_gem_mmap_offset_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file)
{
	struct drm_i915_private *i915 = to_i915(dev);
	struct drm_i915_gem_mmap_offset *args = data;
	enum i915_mmap_type type;
	int err;

	/*
	 * Historically we failed to check args.pad and args.offset
	 * and so we cannot use those fields for user input and we cannot
	 * add -EINVAL for them as the ABI is fixed, i.e. old userspace
	 * may be feeding in garbage in those fields.
	 *
	 * if (args->pad) return -EINVAL; is verbotten!
	 */

	err = i915_user_extensions(u64_to_user_ptr(args->extensions),
				   NULL, 0, NULL);
	if (err)
		return err;

	switch (args->flags) {
	case I915_MMAP_OFFSET_GTT:
		if (!i915_ggtt_has_aperture(to_gt(i915)->ggtt))
			return -ENODEV;
		type = I915_MMAP_TYPE_GTT;
		break;

	case I915_MMAP_OFFSET_WC:
		if (!pat_enabled())
			return -ENODEV;
		type = I915_MMAP_TYPE_WC;
		break;

	case I915_MMAP_OFFSET_WB:
		type = I915_MMAP_TYPE_WB;
		break;

	case I915_MMAP_OFFSET_UC:
		if (!pat_enabled())
			return -ENODEV;
		type = I915_MMAP_TYPE_UC;
		break;

	default:
		return -EINVAL;
	}

	return __assign_mmap_offset_handle(file, args->handle, type, &args->offset);
}

static void vm_open(struct vm_area_struct *vma)
{
	struct i915_mmap_offset *mmo = vma->vm_private_data;
	struct drm_i915_gem_object *obj = mmo->obj;
	struct drm_i915_private *i915;

	GEM_BUG_ON(!obj);
	i915 = to_i915(obj->base.dev);
	pvc_wa_disallow_rc6(i915);
	i915_gem_object_get(obj);
}

static void vm_close(struct vm_area_struct *vma)
{
	struct i915_mmap_offset *mmo = vma->vm_private_data;
	struct drm_i915_gem_object *obj = mmo->obj;
	struct drm_i915_private *i915;

	GEM_BUG_ON(!obj);
	i915 = to_i915(obj->base.dev);
	pvc_wa_allow_rc6(i915);
	i915_gem_object_put(obj);
}

static const struct vm_operations_struct vm_ops_gtt = {
	.fault = vm_fault_gtt,
	.access = vm_access,
	.open = vm_open,
	.close = vm_close,
};

static const struct vm_operations_struct vm_ops_cpu = {
	.fault = vm_fault_cpu,
	.access = vm_access,
	.open = vm_open,
	.close = vm_close,
};

static int singleton_release(struct inode *inode, struct file *file)
{
	struct drm_i915_private *i915 = file->private_data;

	cmpxchg(&i915->gem.mmap_singleton, file, NULL);
	drm_dev_put(&i915->drm);

	return 0;
}

static const struct file_operations singleton_fops = {
	.owner = THIS_MODULE,
	.release = singleton_release,
};

static struct file *mmap_singleton(struct drm_i915_private *i915)
{
	struct file *file;

	rcu_read_lock();
	file = READ_ONCE(i915->gem.mmap_singleton);
	if (file && !get_file_rcu(file))
		file = NULL;
	rcu_read_unlock();
	if (file)
		return file;

	file = anon_inode_getfile("i915.gem", &singleton_fops, i915, O_RDWR);
	if (IS_ERR(file))
		return file;

	/* Everyone shares a single global address space */
	file->f_mapping = i915->drm.anon_inode->i_mapping;

	smp_store_mb(i915->gem.mmap_singleton, file);
	drm_dev_get(&i915->drm);

	return file;
}

int i915_gem_update_vma_info(struct drm_i915_gem_object *obj,
			     struct i915_mmap_offset *mmo,
			     struct vm_area_struct *vma)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct file *anon;

	if (i915_gem_object_is_readonly(obj)) {
		if (vma->vm_flags & VM_WRITE)
			return -EINVAL;

		vma->vm_flags &= ~VM_MAYWRITE;
	}

	anon = mmap_singleton(to_i915(obj->base.dev));
	if (IS_ERR(anon))
		return PTR_ERR(anon);

	pvc_wa_disallow_rc6(i915);
	vma->vm_flags |= VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_private_data = mmo;
	vma->vm_pgoff = drm_vma_node_start(&mmo->vma_node);

	if (i915_gem_object_has_iomem(obj))
		vma->vm_flags |= VM_IO;

	/*
	 * We keep the ref on mmo->obj, not vm_file, but we require
	 * vma->vm_file->f_mapping, see vma_link(), for later revocation.
	 * Our userspace is accustomed to having per-file resource cleanup
	 * (i.e. contexts, objects and requests) on their close(fd), which
	 * requires avoiding extraneous references to their filp, hence why
	 * we prefer to use an anonymous file for their mmaps.
	 */
	vma_set_file(vma, anon);
	/* Drop the initial creation reference, the vma is now holding one. */
	fput(anon);

	if (obj->ops->mmap_ops) {
		vma->vm_page_prot = pgprot_decrypted(vm_get_page_prot(vma->vm_flags));
		vma->vm_ops = obj->ops->mmap_ops;
		//vma->vm_private_data = node->driver_private;
		return 0;
	}

	vma->vm_private_data = mmo;

	switch (mmo->mmap_type) {
	case I915_MMAP_TYPE_WC:
		vma->vm_page_prot =
			pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
		vma->vm_ops = &vm_ops_cpu;
		break;

	case I915_MMAP_TYPE_WB:
		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
		vma->vm_ops = &vm_ops_cpu;
		break;

	case I915_MMAP_TYPE_UC:
		vma->vm_page_prot =
			pgprot_noncached(vm_get_page_prot(vma->vm_flags));
		vma->vm_ops = &vm_ops_cpu;
		break;

	case I915_MMAP_TYPE_GTT:
		vma->vm_page_prot =
			pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
		vma->vm_ops = &vm_ops_gtt;
		break;
	}
	vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);

	return 0;
}

static void barrier_open(struct vm_area_struct *vma)
{
	drm_dev_get(vma->vm_private_data);
}

static void barrier_close(struct vm_area_struct *vma)
{
	drm_dev_put(vma->vm_private_data);
}

static const struct vm_operations_struct vm_ops_barrier = {
	.open = barrier_open,
	.close = barrier_close,
};

static int i915_pci_barrier_mmap(struct file *filp,
				 struct vm_area_struct *vma)
{
	struct drm_file *priv = filp->private_data;
	struct drm_device *dev = priv->minor->dev;
	unsigned long pfn;
	pgprot_t prot;

	if (GRAPHICS_VER(to_i915(dev)) < 12)
		return -ENODEV;

	if (vma->vm_end - vma->vm_start > PAGE_SIZE)
		return -EINVAL;

	if (is_cow_mapping(vma->vm_flags))
		return -EINVAL;

	if (vma->vm_flags & (VM_READ | VM_EXEC))
		return -EINVAL;

	vma->vm_flags &= ~(VM_MAYREAD | VM_MAYEXEC);
	vma->vm_flags |= VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP | VM_IO;

	prot = vm_get_page_prot(vma->vm_flags);
#define LAST_DB_PAGE_OFFSET 0x7ff001
	pfn = PHYS_PFN(pci_resource_start(to_pci_dev(dev->dev), 0) +
		       LAST_DB_PAGE_OFFSET);
	if (vmf_insert_pfn_prot(vma, vma->vm_start, pfn,
				pgprot_noncached(prot)) != VM_FAULT_NOPAGE)
		return -EFAULT;

	vma->vm_ops = &vm_ops_barrier;
	vma->vm_private_data = dev;
	drm_dev_get(vma->vm_private_data);
	return 0;
}

/*
 * This overcomes the limitation in drm_gem_mmap's assignment of a
 * drm_gem_object as the vma->vm_private_data. Since we need to
 * be able to resolve multiple mmap offsets which could be tied
 * to a single gem object.
 */
int i915_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_vma_offset_node *node;
	struct drm_file *priv = filp->private_data;
	struct drm_device *dev = priv->minor->dev;
	struct drm_i915_gem_object *obj = NULL;
	struct i915_mmap_offset *mmo = NULL;
	int err;

	if (drm_dev_is_unplugged(dev))
		return -ENODEV;

	switch (vma->vm_pgoff) {
	case PRELIM_I915_PCI_BARRIER_MMAP_OFFSET >> PAGE_SHIFT:
		return i915_pci_barrier_mmap(filp, vma);
	}

	rcu_read_lock();
	drm_vma_offset_lock_lookup(dev->vma_offset_manager);
	node = drm_vma_offset_exact_lookup_locked(dev->vma_offset_manager,
						  vma->vm_pgoff,
						  vma_pages(vma));
	if (node && drm_vma_node_is_allowed(node, priv)) {
		/*
		 * Skip 0-refcnted objects as it is in the process of being
		 * destroyed and will be invalid when the vma manager lock
		 * is released.
		 */
		mmo = container_of(node, struct i915_mmap_offset, vma_node);
		obj = i915_gem_object_get_rcu(mmo->obj);
	}
	drm_vma_offset_unlock_lookup(dev->vma_offset_manager);
	rcu_read_unlock();
	if (!obj)
		return node ? -EACCES : -EINVAL;

	err = i915_gem_update_vma_info(obj, mmo, vma);
	if (err)
		i915_gem_object_put(obj);

	return err;
}

#if IS_ENABLED(CPTCFG_DRM_I915_SELFTEST)
#include "selftests/i915_gem_mman.c"
#endif
