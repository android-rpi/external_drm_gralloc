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

#ifndef _GRALLOC_DRM_H_
#define _GRALLOC_DRM_H_

#include <hardware/gralloc.h>

struct gralloc_drm_t;
struct gralloc_drm_bo_t;

struct gralloc_drm_t *gralloc_drm_create(void);
void gralloc_drm_destroy(struct gralloc_drm_t *drm);

int gralloc_drm_get_fd(struct gralloc_drm_t *drm);
int gralloc_drm_get_magic(struct gralloc_drm_t *drm, int32_t *magic);
int gralloc_drm_auth_magic(struct gralloc_drm_t *drm, int32_t magic);
int gralloc_drm_set_master(struct gralloc_drm_t *drm);
void gralloc_drm_drop_master(struct gralloc_drm_t *drm);

int gralloc_drm_init_kms(struct gralloc_drm_t *drm);
void gralloc_drm_fini_kms(struct gralloc_drm_t *drm);
int gralloc_drm_is_kms_initialized(struct gralloc_drm_t *drm);

void gralloc_drm_get_kms_info(struct gralloc_drm_t *drm, struct framebuffer_device_t *fb);
int gralloc_drm_is_kms_pipelined(struct gralloc_drm_t *drm);

static inline int gralloc_drm_get_bpp(int format)
{
	int bpp;

	switch (format) {
	case HAL_PIXEL_FORMAT_RGBA_8888:
	case HAL_PIXEL_FORMAT_RGBX_8888:
	case HAL_PIXEL_FORMAT_BGRA_8888:
		bpp = 4;
		break;
	case HAL_PIXEL_FORMAT_RGB_888:
		bpp = 3;
		break;
	case HAL_PIXEL_FORMAT_RGB_565:
	case HAL_PIXEL_FORMAT_RGBA_5551:
	case HAL_PIXEL_FORMAT_RGBA_4444:
		bpp = 2;
		break;
	default:
		bpp = 0;
		break;
	}

	return bpp;
}

struct gralloc_drm_bo_t *gralloc_drm_bo_create(struct gralloc_drm_t *drm, int width, int height, int format, int usage);
void gralloc_drm_bo_destroy(struct gralloc_drm_bo_t *bo);

struct gralloc_drm_bo_t *gralloc_drm_bo_register(struct gralloc_drm_t *drm, buffer_handle_t handle, int create);
void gralloc_drm_bo_unregister(struct gralloc_drm_bo_t *bo);

static inline struct gralloc_drm_bo_t *gralloc_drm_bo_validate(struct gralloc_drm_t *drm, buffer_handle_t handle)
{
	return gralloc_drm_bo_register(drm, handle, 0);
}

int gralloc_drm_bo_lock(struct gralloc_drm_bo_t *bo, int x, int y, int w, int h, int enable_write, void **addr);
void gralloc_drm_bo_unlock(struct gralloc_drm_bo_t *bo);
buffer_handle_t gralloc_drm_bo_get_handle(struct gralloc_drm_bo_t *bo, int *stride);

int gralloc_drm_bo_need_fb(const struct gralloc_drm_bo_t *bo);
int gralloc_drm_bo_add_fb(struct gralloc_drm_bo_t *bo);
void gralloc_drm_bo_rm_fb(struct gralloc_drm_bo_t *bo);
int gralloc_drm_bo_post(struct gralloc_drm_bo_t *bo);

#endif /* _GRALLOC_DRM_H_ */
