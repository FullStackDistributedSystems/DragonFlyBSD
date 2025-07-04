/*
 * Copyright © 2014-2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <drm/drmP.h>
#include <drm/i915_drm.h>
#include "i915_drv.h"

#define QUIET (__GFP_NORETRY | __GFP_NOWARN)
#define MAYFAIL (__GFP_RETRY_MAYFAIL | __GFP_NOWARN)

/* convert swiotlb segment size into sensible units (pages)! */
#define IO_TLB_SEGPAGES (IO_TLB_SEGSIZE << IO_TLB_SHIFT >> PAGE_SHIFT)

static void internal_free_pages(struct sg_table *st)
{
	struct scatterlist *sg;

	for (sg = st->sgl; sg; sg = __sg_next(sg)) {
		if (sg_page(sg))
			__free_pages(sg_page(sg), get_order(sg->length));
	}

	sg_free_table(st);
	kfree(st);
}

static int i915_gem_object_get_pages_internal(struct drm_i915_gem_object *obj)
{
	struct drm_i915_private *i915 = to_i915(obj->base.dev);
	struct sg_table *st;
	struct scatterlist *sg;
	unsigned int sg_page_sizes;
	unsigned int npages;
	int max_order;
	gfp_t gfp;

	max_order = MAX_ORDER;
#ifdef CONFIG_SWIOTLB
	if (swiotlb_nr_tbl()) {
		unsigned int max_segment;

		max_segment = swiotlb_max_segment();
		if (max_segment) {
			max_segment = max_t(unsigned int, max_segment,
					    PAGE_SIZE) >> PAGE_SHIFT;
			max_order = min(max_order, ilog2(max_segment));
		}
	}
#endif

	gfp = GFP_KERNEL | __GFP_HIGHMEM | __GFP_RECLAIMABLE;
	if (IS_I965GM(i915) || IS_I965G(i915)) {
		/* 965gm cannot relocate objects above 4GiB. */
		gfp &= ~__GFP_HIGHMEM;
		gfp |= __GFP_DMA32;
	}

create_st:
	st = kmalloc(sizeof(*st), M_DRM, GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	npages = obj->base.size / PAGE_SIZE;
	if (sg_alloc_table(st, npages, GFP_KERNEL)) {
		kfree(st);
		return -ENOMEM;
	}

	sg = st->sgl;
	st->nents = 0;
	sg_page_sizes = 0;

	do {
		int order = min(fls(npages) - 1, max_order);
		struct page *page;

		do {
			page = alloc_pages(gfp | (order ? QUIET : MAYFAIL),
					   order);
			if (page)
				break;
			if (!order--)
				goto err;

			/* Limit subsequent allocations as well */
			max_order = order;
		} while (1);

		sg_set_page(sg, page, PAGE_SIZE << order, 0);
		sg_page_sizes |= PAGE_SIZE << order;
		st->nents++;

		npages -= 1 << order;
		if (!npages) {
			sg_mark_end(sg);
			break;
		}

		sg = __sg_next(sg);
	} while (1);

	if (i915_gem_gtt_prepare_pages(obj, st)) {
		/* Failed to dma-map try again with single page sg segments */
		if (get_order(st->sgl->length)) {
			internal_free_pages(st);
			max_order = 0;
			goto create_st;
		}
		goto err;
	}

	/* Mark the pages as dontneed whilst they are still pinned. As soon
	 * as they are unpinned they are allowed to be reaped by the shrinker,
	 * and the caller is expected to repopulate - the contents of this
	 * object are only valid whilst active and pinned.
	 */
	obj->mm.madv = I915_MADV_DONTNEED;

	__i915_gem_object_set_pages(obj, st, sg_page_sizes);

	return 0;

err:
	sg_set_page(sg, NULL, 0, 0);
	sg_mark_end(sg);
	internal_free_pages(st);

	return -ENOMEM;
}

static void i915_gem_object_put_pages_internal(struct drm_i915_gem_object *obj,
					       struct sg_table *pages)
{
	i915_gem_gtt_finish_pages(obj, pages);
	internal_free_pages(pages);

	obj->mm.dirty = false;
	obj->mm.madv = I915_MADV_WILLNEED;
}

static const struct drm_i915_gem_object_ops i915_gem_object_internal_ops = {
	.flags = I915_GEM_OBJECT_HAS_STRUCT_PAGE |
		 I915_GEM_OBJECT_IS_SHRINKABLE,
	.get_pages = i915_gem_object_get_pages_internal,
	.put_pages = i915_gem_object_put_pages_internal,
};

/**
 * i915_gem_object_create_internal: create an object with volatile pages
 * @i915: the i915 device
 * @size: the size in bytes of backing storage to allocate for the object
 *
 * Creates a new object that wraps some internal memory for private use.
 * This object is not backed by swappable storage, and as such its contents
 * are volatile and only valid whilst pinned. If the object is reaped by the
 * shrinker, its pages and data will be discarded. Equally, it is not a full
 * GEM object and so not valid for access from userspace. This makes it useful
 * for hardware interfaces like ringbuffers (which are pinned from the time
 * the request is written to the time the hardware stops accessing it), but
 * not for contexts (which need to be preserved when not active for later
 * reuse). Note that it is not cleared upon allocation.
 */
struct drm_i915_gem_object *
i915_gem_object_create_internal(struct drm_i915_private *i915,
				phys_addr_t size)
{
	struct drm_i915_gem_object *obj;
	unsigned int cache_level;

	GEM_BUG_ON(!size);
	GEM_BUG_ON(!IS_ALIGNED(size, PAGE_SIZE));

	if (overflows_type(size, obj->base.size))
		return ERR_PTR(-E2BIG);

	obj = i915_gem_object_alloc(i915);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	drm_gem_private_object_init(&i915->drm, &obj->base, size);
	i915_gem_object_init(obj, &i915_gem_object_internal_ops);

	obj->read_domains = I915_GEM_DOMAIN_CPU;
	obj->write_domain = I915_GEM_DOMAIN_CPU;

	cache_level = HAS_LLC(i915) ? I915_CACHE_LLC : I915_CACHE_NONE;
	i915_gem_object_set_cache_coherency(obj, cache_level);

	return obj;
}
