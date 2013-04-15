/*
 * Copyright (C) 2010-2011 Chia-I Wu <olvaffe@gmail.com>
 * Copyright (C) 2010-2011 LunarG Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef _GRALLOC_DRM_PRIV_H_
#define _GRALLOC_DRM_PRIV_H_

#include <pthread.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "gralloc_drm_handle.h"

#ifdef __cplusplus
extern "C" {
#endif

/* how a bo is posted */
enum drm_swap_mode {
	DRM_SWAP_NOOP,
	DRM_SWAP_FLIP,
	DRM_SWAP_COPY,
	DRM_SWAP_SETCRTC,
};

enum hdmi_output_mode {
	HDMI_CLONED,
	HDMI_EXTENDED,
};

struct gralloc_drm_plane_t {
	drmModePlane *drm_plane;

	/* plane has been set to display a layer */
	uint32_t active;

	/* handle to display */
	buffer_handle_t handle;

	/* position, crop and scale */
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t dst_x;
	uint32_t dst_y;
	uint32_t dst_w;
	uint32_t dst_h;

	/* previous buffer, for refcounting */
	struct gralloc_drm_bo_t *prev;
};

struct gralloc_drm_output
{
	uint32_t crtc_id;
	uint32_t connector_id;
	drmModeModeInfo mode;
	int xdpi, ydpi;
	int fb_format;
	int bpp;
	uint32_t active;

	/* 'private fb' for this output */
	struct gralloc_drm_bo_t *bo;
};

struct gralloc_drm_t {
	/* initialized by gralloc_drm_create */
	int fd;
	struct gralloc_drm_drv_t *drv;

	/* initialized by gralloc_drm_init_kms */
	drmModeResPtr resources;
	struct gralloc_drm_output primary;
	struct gralloc_drm_output hdmi;
	enum hdmi_output_mode hdmi_mode;

	/* hdmi hotplug */
	pthread_mutex_t hdmi_mutex;
	pthread_t hdmi_hotplug_thread;

#ifdef DRM_MODE_FEATURE_DIRTYFB
	drmModeClip clip;
#endif

	/* initialized by drv->init_kms_features */
	enum drm_swap_mode swap_mode;
	int swap_interval;
	int mode_quirk_vmwgfx;
	int mode_sync_flip; /* page flip should block */
	int vblank_secondary;

	drmEventContext evctx;

	int first_post;
	struct gralloc_drm_bo_t *current_front, *next_front;
	int waiting_flip;
	unsigned int last_swap;

	/* plane support */
	drmModePlaneResPtr plane_resources;
	struct gralloc_drm_plane_t *planes;
};

struct drm_module_t {
	gralloc_module_t base;

	/* HWC plane API */
	int (*hwc_reserve_plane) (struct gralloc_drm_t *mod, buffer_handle_t handle,
		uint32_t dst_x, uint32_t dst_y, uint32_t dst_w, uint32_t dst_h,
		uint32_t src_x, uint32_t src_y, uint32_t src_w, uint32_t src_h);
	void (*hwc_disable_planes) (struct gralloc_drm_t *mod);

	pthread_mutex_t mutex;
	struct gralloc_drm_t *drm;
};

struct gralloc_drm_drv_t {
	/* destroy the driver */
	void (*destroy)(struct gralloc_drm_drv_t *drv);

	/* initialize KMS features */
	void (*init_kms_features)(struct gralloc_drm_drv_t *drv,
				  struct gralloc_drm_t *drm);

	/* allocate or import a bo */
	struct gralloc_drm_bo_t *(*alloc)(struct gralloc_drm_drv_t *drv,
			                  struct gralloc_drm_handle_t *handle);

	/* free a bo */
	void (*free)(struct gralloc_drm_drv_t *drv,
		     struct gralloc_drm_bo_t *bo);

	/* map a bo for CPU access */
	int (*map)(struct gralloc_drm_drv_t *drv,
		   struct gralloc_drm_bo_t *bo,
		   int x, int y, int w, int h, int enable_write, void **addr);

	/* unmap a bo */
	void (*unmap)(struct gralloc_drm_drv_t *drv,
		      struct gralloc_drm_bo_t *bo);

	/* blit between two bo's, used for DRM_SWAP_COPY and general blitting */
	void (*blit)(struct gralloc_drm_drv_t *drv,
		     struct gralloc_drm_bo_t *dst,
		     struct gralloc_drm_bo_t *src,
		     uint16_t dst_x1, uint16_t dst_y1,
		     uint16_t dst_x2, uint16_t dst_y2,
		     uint16_t src_x1, uint16_t src_y1,
		     uint16_t src_x2, uint16_t src_y2);

	/* query component offsets, strides and handles for a format */
	void (*resolve_format)(struct gralloc_drm_drv_t *drv,
		     struct gralloc_drm_bo_t *bo,
		     uint32_t *pitches, uint32_t *offsets, uint32_t *handles);
};

struct gralloc_drm_bo_t {
	struct gralloc_drm_t *drm;
	struct gralloc_drm_handle_t *handle;

	int imported;  /* the handle is from a remote proces when true */
	int fb_handle; /* the GEM handle of the bo */
	int fb_id;     /* the fb id */

	int lock_count;
	int locked_for;

	unsigned int refcount;
};

struct gralloc_drm_drv_t *gralloc_drm_drv_create_for_pipe(int fd, const char *name);
struct gralloc_drm_drv_t *gralloc_drm_drv_create_for_intel(int fd);
struct gralloc_drm_drv_t *gralloc_drm_drv_create_for_radeon(int fd);
struct gralloc_drm_drv_t *gralloc_drm_drv_create_for_nouveau(int fd);

#ifdef __cplusplus
}
#endif
#endif /* _GRALLOC_DRM_PRIV_H_ */
