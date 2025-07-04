/*
 * Copyright © 2007 David Airlie
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *     David Airlie
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fb.h>
#include <linux/pm_runtime.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/amdgpu_drm.h>
#include "amdgpu.h"
#include "cikd.h"
#include "amdgpu_gem.h"

#include <drm/drm_fb_helper.h>

#include <linux/vga_switcheroo.h>

#include "amdgpu_display.h"

/* object hierarchy -
   this contains a helper + a amdgpu fb
   the helper contains a pointer to amdgpu framebuffer baseclass.
*/

#if 0
static int
amdgpufb_open(struct fb_info *info, int user)
{
	struct amdgpu_fbdev *rfbdev = info->par;
	struct amdgpu_device *adev = rfbdev->adev;
	int ret = pm_runtime_get_sync(adev->ddev->dev);
	if (ret < 0 && ret != -EACCES) {
		pm_runtime_mark_last_busy(adev->ddev->dev);
		pm_runtime_put_autosuspend(adev->ddev->dev);
		return ret;
	}
	return 0;
}

static int
amdgpufb_release(struct fb_info *info, int user)
{
	struct amdgpu_fbdev *rfbdev = info->par;
	struct amdgpu_device *adev = rfbdev->adev;

	pm_runtime_mark_last_busy(adev->ddev->dev);
	pm_runtime_put_autosuspend(adev->ddev->dev);
	return 0;
}
#endif

static struct fb_ops amdgpufb_ops = {
#if 0
	.owner = THIS_MODULE,
#endif
	DRM_FB_HELPER_DEFAULT_OPS,
#if 0
	.fb_open = amdgpufb_open,
	.fb_release = amdgpufb_release,
	.fb_fillrect = drm_fb_helper_cfb_fillrect,
	.fb_copyarea = drm_fb_helper_cfb_copyarea,
	.fb_imageblit = drm_fb_helper_cfb_imageblit,
#endif
};


int amdgpu_align_pitch(struct amdgpu_device *adev, int width, int cpp, bool tiled)
{
	int aligned = width;
	int pitch_mask = 0;

	switch (cpp) {
	case 1:
		pitch_mask = 255;
		break;
	case 2:
		pitch_mask = 127;
		break;
	case 3:
	case 4:
		pitch_mask = 63;
		break;
	}

	aligned += pitch_mask;
	aligned &= ~pitch_mask;
	return aligned * cpp;
}

static void amdgpufb_destroy_pinned_object(struct drm_gem_object *gobj)
{
	struct amdgpu_bo *abo = gem_to_amdgpu_bo(gobj);
	int ret;

	ret = amdgpu_bo_reserve(abo, true);
	if (likely(ret == 0)) {
		amdgpu_bo_kunmap(abo);
		amdgpu_bo_unpin(abo);
		amdgpu_bo_unreserve(abo);
	}
	drm_gem_object_put_unlocked(gobj);
}

static int amdgpufb_create_pinned_object(struct amdgpu_fbdev *rfbdev,
					 struct drm_mode_fb_cmd2 *mode_cmd,
					 struct drm_gem_object **gobj_p)
{
	struct amdgpu_device *adev = rfbdev->adev;
	struct drm_gem_object *gobj = NULL;
	struct amdgpu_bo *abo = NULL;
	bool fb_tiled = false; /* useful for testing */
	u32 tiling_flags = 0, domain;
	int ret;
	int aligned_size, size;
	int height = mode_cmd->height;
	u32 cpp;

	cpp = drm_format_plane_cpp(mode_cmd->pixel_format, 0);

	/* need to align pitch with crtc limits */
	mode_cmd->pitches[0] = amdgpu_align_pitch(adev, mode_cmd->width, cpp,
						  fb_tiled);
	domain = amdgpu_display_supported_domains(adev);

	height = ALIGN(mode_cmd->height, 8);
	size = mode_cmd->pitches[0] * height;
	aligned_size = ALIGN(size, PAGE_SIZE);
	ret = amdgpu_gem_object_create(adev, aligned_size, 0, domain,
				       AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED |
				       AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS |
				       AMDGPU_GEM_CREATE_VRAM_CLEARED,
				       ttm_bo_type_kernel, NULL, &gobj);
	if (ret) {
		pr_err("failed to allocate framebuffer (%d)\n", aligned_size);
		return -ENOMEM;
	}
	abo = gem_to_amdgpu_bo(gobj);

	if (fb_tiled)
		tiling_flags = AMDGPU_TILING_SET(ARRAY_MODE, GRPH_ARRAY_2D_TILED_THIN1);

	ret = amdgpu_bo_reserve(abo, false);
	if (unlikely(ret != 0))
		goto out_unref;

	if (tiling_flags) {
		ret = amdgpu_bo_set_tiling_flags(abo,
						 tiling_flags);
		if (ret)
			dev_err(adev->dev, "FB failed to set tiling flags\n");
	}


	ret = amdgpu_bo_pin(abo, domain);
	if (ret) {
		amdgpu_bo_unreserve(abo);
		goto out_unref;
	}

	ret = amdgpu_ttm_alloc_gart(&abo->tbo);
	if (ret) {
		amdgpu_bo_unreserve(abo);
		dev_err(adev->dev, "%p bind failed\n", abo);
		goto out_unref;
	}

	ret = amdgpu_bo_kmap(abo, NULL);
	amdgpu_bo_unreserve(abo);
	if (ret) {
		goto out_unref;
	}

	*gobj_p = gobj;
	return 0;
out_unref:
	amdgpufb_destroy_pinned_object(gobj);
	*gobj_p = NULL;
	return ret;
}

static int amdgpufb_create(struct drm_fb_helper *helper,
			   struct drm_fb_helper_surface_size *sizes)
{
	struct amdgpu_fbdev *rfbdev = (struct amdgpu_fbdev *)helper;
	struct amdgpu_device *adev = rfbdev->adev;
	struct fb_info *info;
	struct drm_framebuffer *fb = NULL;
	struct drm_mode_fb_cmd2 mode_cmd;
	struct drm_gem_object *gobj = NULL;
	struct amdgpu_bo *abo = NULL;
	int ret;
	device_t vga_dev = device_get_parent(adev->dev->bsddev);

	mode_cmd.width = sizes->surface_width;
	mode_cmd.height = sizes->surface_height;

	if (sizes->surface_bpp == 24)
		sizes->surface_bpp = 32;

	mode_cmd.pixel_format = drm_mode_legacy_fb_format(sizes->surface_bpp,
							  sizes->surface_depth);

	ret = amdgpufb_create_pinned_object(rfbdev, &mode_cmd, &gobj);
	if (ret) {
		DRM_ERROR("failed to create fbcon object %d\n", ret);
		return ret;
	}

	abo = gem_to_amdgpu_bo(gobj);

	/* okay we have an object now allocate the framebuffer */
	info = drm_fb_helper_alloc_fbi(helper);
	if (IS_ERR(info)) {
		ret = PTR_ERR(info);
		goto out;
	}

	info->par = rfbdev;
#if 0
 	info->skip_vt_switch = true;
#endif

	ret = amdgpu_display_framebuffer_init(adev->ddev, &rfbdev->rfb,
					      &mode_cmd, gobj);
	if (ret) {
		DRM_ERROR("failed to initialize framebuffer %d\n", ret);
		goto out;
	}

	fb = &rfbdev->rfb.base;

	/* setup helper */
	rfbdev->helper.fb = fb;

#ifdef __DragonFly__
	info->width = sizes->fb_width;
	info->height = sizes->fb_height;
	info->stride = fb->pitches[0];
	info->depth = sizes->surface_bpp;
	info->is_vga_boot_display = vga_pci_is_boot_display(vga_dev);
	info->fbops = amdgpufb_ops;

	unsigned long tmp = amdgpu_bo_gpu_offset(abo) - adev->gmc.vram_start;
	info->vaddr = (vm_offset_t)amdgpu_bo_kptr(abo);
	info->paddr = adev->gmc.aper_base + tmp;
#else

	strcpy(info->fix.id, "amdgpudrmfb");

	drm_fb_helper_fill_fix(info, fb->pitches[0], fb->format->depth);

	info->fbops = &amdgpufb_ops;

	tmp = amdgpu_bo_gpu_offset(abo) - adev->gmc.vram_start;
	info->fix.smem_start = adev->gmc.aper_base + tmp;
	info->fix.smem_len = amdgpu_bo_size(abo);
	info->screen_base = amdgpu_bo_kptr(abo);
	info->screen_size = amdgpu_bo_size(abo);

	drm_fb_helper_fill_var(info, &rfbdev->helper, sizes->fb_width, sizes->fb_height);

	/* setup aperture base/size for vesafb takeover */
	info->apertures->ranges[0].base = adev->ddev->mode_config.fb_base;
	info->apertures->ranges[0].size = adev->gmc.aper_size;

	/* Use default scratch pixmap (info->pixmap.flags = FB_PIXMAP_SYSTEM) */

	if (info->screen_base == NULL) {
		ret = -ENOSPC;
		goto out;
	}
#endif

	DRM_INFO("fb mappable at 0x%lX\n",  info->paddr);
	DRM_INFO("vram apper at 0x%lX\n",  (unsigned long)adev->gmc.aper_base);
	DRM_INFO("size %lu\n", (unsigned long)amdgpu_bo_size(abo));
	DRM_INFO("fb depth is %d\n", fb->format->depth);
	DRM_INFO("   pitch is %d\n", fb->pitches[0]);

	vga_switcheroo_client_fb_set(adev->ddev->pdev, info);
	return 0;

out:
	if (abo) {

	}
	if (fb && ret) {
		drm_gem_object_put_unlocked(gobj);
		drm_framebuffer_unregister_private(fb);
		drm_framebuffer_cleanup(fb);
		kfree(fb);
	}
	return ret;
}

static int amdgpu_fbdev_destroy(struct drm_device *dev, struct amdgpu_fbdev *rfbdev)
{
	struct amdgpu_framebuffer *rfb = &rfbdev->rfb;

	drm_fb_helper_unregister_fbi(&rfbdev->helper);

	if (rfb->base.obj[0]) {
		amdgpufb_destroy_pinned_object(rfb->base.obj[0]);
		rfb->base.obj[0] = NULL;
		drm_framebuffer_unregister_private(&rfb->base);
		drm_framebuffer_cleanup(&rfb->base);
	}
	drm_fb_helper_fini(&rfbdev->helper);

	return 0;
}

static const struct drm_fb_helper_funcs amdgpu_fb_helper_funcs = {
	.fb_probe = amdgpufb_create,
};

int amdgpu_fbdev_init(struct amdgpu_device *adev)
{
	struct amdgpu_fbdev *rfbdev;
	int bpp_sel = 32;
	int ret;

	/* don't init fbdev on hw without DCE */
	if (!adev->mode_info.mode_config_initialized)
		return 0;

	/* don't init fbdev if there are no connectors */
	if (list_empty(&adev->ddev->mode_config.connector_list))
		return 0;

	/* select 8 bpp console on low vram cards */
	if (adev->gmc.real_vram_size <= (32*1024*1024))
		bpp_sel = 8;

	rfbdev = kzalloc(sizeof(struct amdgpu_fbdev), GFP_KERNEL);
	if (!rfbdev)
		return -ENOMEM;

	rfbdev->adev = adev;
	adev->mode_info.rfbdev = rfbdev;

	drm_fb_helper_prepare(adev->ddev, &rfbdev->helper,
			&amdgpu_fb_helper_funcs);

	ret = drm_fb_helper_init(adev->ddev, &rfbdev->helper,
				 AMDGPUFB_CONN_LIMIT);
	if (ret) {
		kfree(rfbdev);
		return ret;
	}

	drm_fb_helper_single_add_all_connectors(&rfbdev->helper);

	/* disable all the possible outputs/crtcs before entering KMS mode */
	if (!amdgpu_device_has_dc_support(adev))
		drm_helper_disable_unused_functions(adev->ddev);

	drm_fb_helper_initial_config(&rfbdev->helper, bpp_sel);
	return 0;
}

void amdgpu_fbdev_fini(struct amdgpu_device *adev)
{
	if (!adev->mode_info.rfbdev)
		return;

	amdgpu_fbdev_destroy(adev->ddev, adev->mode_info.rfbdev);
	kfree(adev->mode_info.rfbdev);
	adev->mode_info.rfbdev = NULL;
}

void amdgpu_fbdev_set_suspend(struct amdgpu_device *adev, int state)
{
#if 0
	if (adev->mode_info.rfbdev)
		drm_fb_helper_set_suspend_unlocked(&adev->mode_info.rfbdev->helper,
						   state);
#endif
}

int amdgpu_fbdev_total_size(struct amdgpu_device *adev)
{
	struct amdgpu_bo *robj;
	int size = 0;

	if (!adev->mode_info.rfbdev)
		return 0;

	robj = gem_to_amdgpu_bo(adev->mode_info.rfbdev->rfb.base.obj[0]);
	size += amdgpu_bo_size(robj);
	return size;
}

bool amdgpu_fbdev_robj_is_fb(struct amdgpu_device *adev, struct amdgpu_bo *robj)
{
	if (!adev->mode_info.rfbdev)
		return false;
	if (robj == gem_to_amdgpu_bo(adev->mode_info.rfbdev->rfb.base.obj[0]))
		return true;
	return false;
}
