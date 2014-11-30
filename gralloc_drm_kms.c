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

#define LOG_TAG "GRALLOC-KMS"

#include <cutils/properties.h>
#include <cutils/log.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <poll.h>
#include <math.h>
#include "gralloc_drm.h"
#include "gralloc_drm_priv.h"
#include <hardware_legacy/uevent.h>

#include <drm_fourcc.h>

/*
 * Return true if a bo needs fb.
 */
int gralloc_drm_bo_need_fb(const struct gralloc_drm_bo_t *bo)
{
	return ((bo->handle->usage & GRALLOC_USAGE_HW_FB) &&
		bo->drm->swap_mode != DRM_SWAP_COPY);
}

static unsigned int drm_format_from_hal(int hal_format)
{
	switch(hal_format) {
		case HAL_PIXEL_FORMAT_RGB_888:
		case HAL_PIXEL_FORMAT_BGRA_8888:
			return DRM_FORMAT_XRGB8888;
		case HAL_PIXEL_FORMAT_RGBX_8888:
			return DRM_FORMAT_XBGR8888;
		case HAL_PIXEL_FORMAT_RGBA_8888:
			return DRM_FORMAT_RGBA8888;
		case HAL_PIXEL_FORMAT_RGB_565:
			return DRM_FORMAT_RGB565;
		case HAL_PIXEL_FORMAT_YV12:
			return DRM_FORMAT_YUV420;
		case HAL_PIXEL_FORMAT_DRM_NV12:
			return DRM_FORMAT_NV12;
		default:
			return 0;
	}
}

/*
 * Modify pitches, offsets and handles according to
 * the format and return corresponding drm format value
 */
static int resolve_drm_format(struct gralloc_drm_bo_t *bo,
	uint32_t *pitches, uint32_t *offsets, uint32_t *handles)
{
	struct gralloc_drm_t *drm = bo->drm;

	pitches[0] = bo->handle->stride;
	handles[0] = bo->fb_handle;

	/* driver takes care of HW specific padding, alignment etc. */
	if (drm->drv->resolve_format)
		drm->drv->resolve_format(drm->drv, bo,
			pitches, offsets, handles);

	return drm_format_from_hal(bo->handle->format);
}

/*
 * Returns planes that are supported for a particular format
 */
unsigned int planes_for_format(struct gralloc_drm_t *drm,
	int hal_format)
{
	unsigned int i, j, mask = 0;
	unsigned int drm_format = drm_format_from_hal(hal_format);
	struct gralloc_drm_plane_t *plane = drm->planes;

	/* no planes available */
	if (!plane)
		return 0;

	/* iterate through planes, mark those that match format */
	for (i=0; i<drm->plane_resources->count_planes; i++, plane++)
		for (j=0; j<plane->drm_plane->count_formats; j++)
			if (plane->drm_plane->formats[j] == drm_format)
				mask |= (1U << plane->drm_plane->plane_id);

	return mask;
}

/*
 * Add a fb object for a bo.
 */
int gralloc_drm_bo_add_fb(struct gralloc_drm_bo_t *bo)
{
	uint32_t pitches[4] = { 0, 0, 0, 0 };
	uint32_t offsets[4] = { 0, 0, 0, 0 };
	uint32_t handles[4] = { 0, 0, 0, 0 };

	if (bo->fb_id)
		return 0;

	int drm_format = resolve_drm_format(bo, pitches, offsets, handles);

	if (drm_format == 0) {
		ALOGE("error resolving drm format");
		return -EINVAL;
	}

	return drmModeAddFB2(bo->drm->fd,
		bo->handle->width, bo->handle->height,
		drm_format, handles, pitches, offsets,
		(uint32_t *) &bo->fb_id, 0);
}

/*
 * Remove a fb object for a bo.
 */
void gralloc_drm_bo_rm_fb(struct gralloc_drm_bo_t *bo)
{
	if (bo->fb_id) {
		drmModeRmFB(bo->drm->fd, bo->fb_id);
		bo->fb_id = 0;
	}
}

/*
 * Program CRTC.
 */
static int drm_kms_set_crtc(struct gralloc_drm_t *drm,
	struct gralloc_drm_output *output, int fb_id)
{
	int ret;

	ret = drmModeSetCrtc(drm->fd, output->crtc_id, fb_id,
			0, 0, &output->connector_id, 1, &output->mode);
	if (ret) {
		ALOGE("failed to set crtc (%s) (crtc_id %d, fb_id %d, conn %d, mode %dx%d)",
			strerror(errno), output->crtc_id, fb_id, output->connector_id,
			output->mode.hdisplay, output->mode.vdisplay);
		return ret;
	}

	if (drm->mode_quirk_vmwgfx)
		ret = drmModeDirtyFB(drm->fd, fb_id, &drm->clip, 1);

	return ret;
}

/*
 * Callback for a page flip event.
 */
static void page_flip_handler(int fd, unsigned int sequence,
		unsigned int tv_sec, unsigned int tv_usec,
		void *user_data)
{
	struct gralloc_drm_t *drm = (struct gralloc_drm_t *) user_data;

	/* ack the last scheduled flip */
	drm->current_front = drm->next_front;
	drm->next_front = NULL;
}

/*
 * Set a plane.
 */
static int gralloc_drm_bo_setplane(struct gralloc_drm_t *drm,
	struct gralloc_drm_plane_t *plane)
{
	struct gralloc_drm_bo_t *bo = NULL;
	int err;

	if (plane->handle)
		bo = gralloc_drm_bo_from_handle(plane->handle);

	// create a framebuffer if does not exist
	if (bo && bo->fb_id == 0) {
		err = gralloc_drm_bo_add_fb(bo);
		if (err) {
			ALOGE("%s: could not create drm fb, (%s)",
				__func__, strerror(-err));
			return err;
		}
	}

	err = drmModeSetPlane(drm->fd,
		plane->drm_plane->plane_id,
		drm->primary.crtc_id,
		bo ? bo->fb_id : 0,
		0, // flags
		plane->dst_x,
		plane->dst_y,
		plane->dst_w,
		plane->dst_h,
		plane->src_x << 16,
		plane->src_y << 16,
		plane->src_w << 16,
		plane->src_h << 16);

	if (err) {
		/* clear plane_mask so that this buffer won't be tried again */
		struct gralloc_drm_handle_t *drm_handle =
			(struct gralloc_drm_handle_t *) plane->handle;
		drm_handle->plane_mask = 0;

		ALOGE("drmModeSetPlane : error (%s) (plane %d crtc %d fb %d)",
			strerror(-err),
			plane->drm_plane->plane_id,
			drm->primary.crtc_id,
			bo ? bo->fb_id : 0);
	}

	if (plane->prev)
		gralloc_drm_bo_decref(plane->prev);

	if (bo)
		bo->refcount++;

	plane->prev = bo;

	return err;
}

/*
 * Returns if a particular plane is supported with the implementation
 */
static unsigned is_plane_supported(const struct gralloc_drm_t *drm,
	const struct gralloc_drm_plane_t *plane)
{
	/* Planes are only supported on primary pipe for now */
	return plane->drm_plane->possible_crtcs & (1 << drm->primary.pipe);
}

/*
 * Sets all the active planes to be displayed.
 */
static void gralloc_drm_set_planes(struct gralloc_drm_t *drm)
{
	struct gralloc_drm_plane_t *plane = drm->planes;
	unsigned int i;
	for (i = 0; i < drm->plane_resources->count_planes;
		i++, plane++) {
		/* plane is not in use at all */
		if (!plane->active && !plane->handle)
			continue;

		/* plane is active, safety check if it is supported */
		if (!is_plane_supported(drm, plane))
			ALOGE("%s: plane %d is not supported",
				 __func__, plane->drm_plane->plane_id);

		/*
		 * Disable overlay if it is not active
		 * or if there is error during setplane
		 */
		if (!plane->active)
			plane->handle = 0;

		if (gralloc_drm_bo_setplane(drm, plane))
			plane->active = 0;
	}
}

/*
 * Interface for HWC, used to reserve a plane for a layer.
 */
int gralloc_drm_reserve_plane(struct gralloc_drm_t *drm,
	buffer_handle_t handle,
	uint32_t id,
	uint32_t dst_x,
	uint32_t dst_y,
	uint32_t dst_w,
	uint32_t dst_h,
	uint32_t src_x,
	uint32_t src_y,
	uint32_t src_w,
	uint32_t src_h)
{
	int j;
	struct gralloc_drm_handle_t *drm_handle =
		gralloc_drm_handle(handle);
	int plane_count = drm->plane_resources->count_planes;
	struct gralloc_drm_plane_t *plane = drm->planes;

	/* no supported planes for this handle */
	if (!drm_handle->plane_mask) {
		ALOGE("%s: buffer %p cannot be shown on a plane\n",
			__func__, drm_handle);
		return -EINVAL;
	}

	for (j = 0; j < plane_count; j++, plane++) {

		/*
		 * handle may be suitable to be shown on a plane, in
		 * addition we need to check that this particular plane
		 * is supported by the current implementation
		 */
		if (!is_plane_supported(drm, plane))
			continue;

		/* if plane is available and can support this buffer */
		if (!plane->active &&
			drm_handle->plane_mask &
			(1U << plane->drm_plane->plane_id)) {

			plane->dst_x = dst_x;
			plane->dst_y = dst_y;
			plane->dst_w = dst_w;
			plane->dst_h = dst_h;
			plane->src_x = src_x;
			plane->src_y = src_y;
			plane->src_w = src_w;
			plane->src_h = src_h;
			plane->handle = handle;
			plane->id = id;
			plane->active = 1;

			return 0;
		}
	}

	/* no free planes available */
	return -EBUSY;
}

/*
 * Interface for HWC, used to disable all the overlays. Plane id
 * is also set to 0 as it should be mappable to a particular layer only
 * if it has been reserved with 'reserve_plane'.
 */
void gralloc_drm_disable_planes(struct gralloc_drm_t *drm)
{
	struct gralloc_drm_plane_t *plane = drm->planes;
	unsigned int i;

	for (i = 0; i < drm->plane_resources->count_planes; i++, plane++) {
		plane->active = 0;
		plane->id = 0;
	}
}

/*
 * Interface for HWC, used to change handle of a reserved plane.
 */
int gralloc_drm_set_plane_handle(struct gralloc_drm_t *drm,
	uint32_t id, buffer_handle_t handle)
{
	struct gralloc_drm_plane_t *plane = drm->planes;
	unsigned i;

	for (i = 0; i < drm->plane_resources->count_planes; i++, plane++)
		if (plane->active && plane->id == id) {
			plane->handle = handle;
			return 0;
		}

	return -EINVAL;
}

/*
 * Schedule a page flip.
 */
static int drm_kms_page_flip(struct gralloc_drm_t *drm,
		struct gralloc_drm_bo_t *bo)
{
	int ret;

	/* there is another flip pending */
	while (drm->next_front) {
		drm->waiting_flip = 1;
		drmHandleEvent(drm->fd, &drm->evctx);
		drm->waiting_flip = 0;
		if (drm->next_front) {
			/* record an error and break */
			ALOGE("drmHandleEvent returned without flipping");
			drm->current_front = drm->next_front;
			drm->next_front = NULL;
		}
	}

	if (!bo)
		return 0;

	pthread_mutex_lock(&drm->hdmi_mutex);
	if (drm->hdmi.active && drm->hdmi_mode == HDMI_CLONED && drm->hdmi.bo) {

		int dst_x1 = 0, dst_y1 = 0;

		if (drm->hdmi.bo->handle->width > bo->handle->width)
			dst_x1 = (drm->hdmi.bo->handle->width - bo->handle->width) / 2;
		if (drm->hdmi.bo->handle->height > bo->handle->height)
			dst_y1 = (drm->hdmi.bo->handle->height - bo->handle->height) / 2;

		drm->drv->blit(drm->drv, drm->hdmi.bo, bo,
			dst_x1, dst_y1,
			dst_x1 + bo->handle->width,
			dst_y1 + bo->handle->height,
			0, 0, bo->handle->width, bo->handle->height);

		ret = drmModePageFlip(drm->fd, drm->hdmi.crtc_id, drm->hdmi.bo->fb_id, 0, NULL);
		if (ret && errno != EBUSY)
			ALOGE("failed to perform page flip for hdmi (%s) (crtc %d fb %d))",
				strerror(errno), drm->hdmi.crtc_id, drm->hdmi.bo->fb_id);
	}
	pthread_mutex_unlock(&drm->hdmi_mutex);

	/* set planes to be displayed */
	gralloc_drm_set_planes(drm);

	ret = drmModePageFlip(drm->fd, drm->primary.crtc_id, bo->fb_id,
			DRM_MODE_PAGE_FLIP_EVENT, (void *) drm);
	if (ret) {
		ALOGE("failed to perform page flip for primary (%s) (crtc %d fb %d))",
			strerror(errno), drm->primary.crtc_id, bo->fb_id);
		/* try to set mode for next frame */
		if (errno != EBUSY)
			drm->first_post = 1;
	}
	else
		drm->next_front = bo;

	return ret;
}

/*
 * Wait for the next post.
 */
static void drm_kms_wait_for_post(struct gralloc_drm_t *drm, int flip)
{
	unsigned int current, target;
	drmVBlank vbl;
	int ret;

	if (drm->mode_quirk_vmwgfx)
		return;

	flip = !!flip;

	memset(&vbl, 0, sizeof(vbl));
	vbl.request.type = DRM_VBLANK_RELATIVE;
	if (drm->vblank_secondary)
		vbl.request.type |= DRM_VBLANK_SECONDARY;
	vbl.request.sequence = 0;

	/* get the current vblank */
	ret = drmWaitVBlank(drm->fd, &vbl);
	if (ret) {
		ALOGW("failed to get vblank");
		return;
	}

	current = vbl.reply.sequence;
	if (drm->first_post)
		target = current;
	else
		target = drm->last_swap + drm->swap_interval - flip;

	/* wait for vblank */
	if (current < target || !flip) {
		memset(&vbl, 0, sizeof(vbl));
		vbl.request.type = DRM_VBLANK_ABSOLUTE;
		if (drm->vblank_secondary)
			vbl.request.type |= DRM_VBLANK_SECONDARY;
		if (!flip) {
			vbl.request.type |= DRM_VBLANK_NEXTONMISS;
			if (target < current)
				target = current;
		}

		vbl.request.sequence = target;

		ret = drmWaitVBlank(drm->fd, &vbl);
		if (ret) {
			ALOGW("failed to wait vblank");
			return;
		}
	}

	drm->last_swap = vbl.reply.sequence + flip;
}

/*
 * Post a bo.  This is not thread-safe.
 */
int gralloc_drm_bo_post(struct gralloc_drm_bo_t *bo)
{
	struct gralloc_drm_t *drm = bo->drm;
	int ret;

	if (!bo->fb_id && drm->swap_mode != DRM_SWAP_COPY) {
		ALOGE("unable to post bo %p without fb", bo);
		return -EINVAL;
	}

	/* TODO spawn a thread to avoid waiting and race */

	if (drm->first_post) {
		if (drm->swap_mode == DRM_SWAP_COPY) {
			struct gralloc_drm_bo_t *dst;

			dst = (drm->next_front) ?
				drm->next_front :
				drm->current_front;
			drm->drv->blit(drm->drv, dst, bo, 0, 0,
					bo->handle->width,
					bo->handle->height,
					0, 0,
					bo->handle->width,
					bo->handle->height);
			bo = dst;
		}

		ret = drm_kms_set_crtc(drm, &drm->primary, bo->fb_id);
		if (!ret) {
			drm->first_post = 0;
			drm->current_front = bo;
			if (drm->next_front == bo)
				drm->next_front = NULL;
		}

		pthread_mutex_lock(&drm->hdmi_mutex);
		if (drm->hdmi.active && drm->hdmi_mode == HDMI_CLONED && drm->hdmi.bo)
			drm_kms_set_crtc(drm, &drm->hdmi, drm->hdmi.bo->fb_id);
		pthread_mutex_unlock(&drm->hdmi_mutex);

		return ret;
	}

	switch (drm->swap_mode) {
	case DRM_SWAP_FLIP:
		if (drm->swap_interval > 1)
			drm_kms_wait_for_post(drm, 1);
		ret = drm_kms_page_flip(drm, bo);
		if (drm->next_front) {
			/*
			 * wait if the driver says so or the current front
			 * will be written by CPU
			 */
			if (drm->mode_sync_flip ||
			    (drm->current_front->handle->usage &
			     GRALLOC_USAGE_SW_WRITE_MASK))
				drm_kms_page_flip(drm, NULL);
		}
		break;
	case DRM_SWAP_COPY:
		drm_kms_wait_for_post(drm, 0);
		drm->drv->blit(drm->drv, drm->current_front,
				bo, 0, 0,
				bo->handle->width,
				bo->handle->height,
				0, 0,
				bo->handle->width,
				bo->handle->height);
		if (drm->mode_quirk_vmwgfx)
			ret = drmModeDirtyFB(drm->fd, drm->current_front->fb_id, &drm->clip, 1);
		ret = 0;
		break;
	case DRM_SWAP_SETCRTC:
		drm_kms_wait_for_post(drm, 0);
		ret = drm_kms_set_crtc(drm, &drm->primary, bo->fb_id);

		pthread_mutex_lock(&drm->hdmi_mutex);
		if (drm->hdmi.active && drm->hdmi_mode == HDMI_CLONED && drm->hdmi.bo)
			drm_kms_set_crtc(drm, &drm->hdmi, drm->hdmi.bo->fb_id);
		pthread_mutex_unlock(&drm->hdmi_mutex);

		drm->current_front = bo;
		break;
	default:
		/* no-op */
		ret = 0;
		break;
	}

	return ret;
}

static struct gralloc_drm_t *drm_singleton;

static void on_signal(int sig)
{
	struct gralloc_drm_t *drm = drm_singleton;

	/* wait the pending flip */
	if (drm && drm->swap_mode == DRM_SWAP_FLIP && drm->next_front) {
		/* there is race, but this function is hacky enough to ignore that */
		if (drm_singleton->waiting_flip)
			usleep(100 * 1000); /* 100ms */
		else
			drm_kms_page_flip(drm_singleton, NULL);
	}

	exit(-1);
}

static void drm_kms_init_features(struct gralloc_drm_t *drm)
{
	const char *swap_mode;

	/* call to the driver here, after KMS has been initialized */
	drm->drv->init_kms_features(drm->drv, drm);

	if (drm->swap_mode == DRM_SWAP_FLIP) {
		struct sigaction act;

		memset(&drm->evctx, 0, sizeof(drm->evctx));
		drm->evctx.version = DRM_EVENT_CONTEXT_VERSION;
		drm->evctx.page_flip_handler = page_flip_handler;

		/*
		 * XXX GPU tends to freeze if the program is terminiated with a
		 * flip pending.  What is the right way to handle the
		 * situation?
		 */
		sigemptyset(&act.sa_mask);
		act.sa_handler = on_signal;
		act.sa_flags = 0;
		sigaction(SIGINT, &act, NULL);
		sigaction(SIGTERM, &act, NULL);

		drm_singleton = drm;
	}
	else if (drm->swap_mode == DRM_SWAP_COPY) {
		struct gralloc_drm_bo_t *front;
		int stride;

		/* create the real front buffer */
		front = gralloc_drm_bo_create(drm,
					      drm->primary.mode.hdisplay,
					      drm->primary.mode.vdisplay,
					      drm->primary.fb_format,
					      GRALLOC_USAGE_HW_FB);
		if (front && gralloc_drm_bo_add_fb(front)) {
			gralloc_drm_bo_decref(front);
			front = NULL;
		}

		/* abuse next_front */
		if (front)
			drm->next_front = front;
		else
			drm->swap_mode = DRM_SWAP_SETCRTC;
	}

	switch (drm->swap_mode) {
	case DRM_SWAP_FLIP:
		swap_mode = "flip";
		break;
	case DRM_SWAP_COPY:
		swap_mode = "copy";
		break;
	case DRM_SWAP_SETCRTC:
		swap_mode = "set-crtc";
		break;
	default:
		swap_mode = "no-op";
		break;
	}

	ALOGD("will use %s for fb posting", swap_mode);
}

#define MARGIN_PERCENT 1.8   /* % of active vertical image*/
#define CELL_GRAN 8.0   /* assumed character cell granularity*/
#define MIN_PORCH 1 /* minimum front porch   */
#define V_SYNC_RQD 3 /* width of vsync in lines   */
#define H_SYNC_PERCENT 8.0   /* width of hsync as % of total line */
#define MIN_VSYNC_PLUS_BP 550.0 /* min time of vsync + back porch (microsec) */
#define M 600.0 /* blanking formula gradient */
#define C 40.0  /* blanking formula offset   */
#define K 128.0 /* blanking formula scaling factor   */
#define J 20.0  /* blanking formula scaling factor   */
/* C' and M' are part of the Blanking Duty Cycle computation */
#define C_PRIME   (((C - J) * K / 256.0) + J)
#define M_PRIME   (K / 256.0 * M)

static drmModeModeInfoPtr generate_mode(int h_pixels, int v_lines, float freq)
{
	float h_pixels_rnd;
	float v_lines_rnd;
	float v_field_rate_rqd;
	float top_margin;
	float bottom_margin;
	float interlace;
	float h_period_est;
	float vsync_plus_bp;
	float v_back_porch;
	float total_v_lines;
	float v_field_rate_est;
	float h_period;
	float v_field_rate;
	float v_frame_rate;
	float left_margin;
	float right_margin;
	float total_active_pixels;
	float ideal_duty_cycle;
	float h_blank;
	float total_pixels;
	float pixel_freq;
	float h_freq;

	float h_sync;
	float h_front_porch;
	float v_odd_front_porch_lines;
	int interlaced = 0;
	int margins = 0;

	drmModeModeInfoPtr m = malloc(sizeof(drmModeModeInfo));

	h_pixels_rnd = rint((float) h_pixels / CELL_GRAN) * CELL_GRAN;
	v_lines_rnd = interlaced ? rint((float) v_lines) / 2.0 : rint((float) v_lines);
	v_field_rate_rqd = interlaced ? (freq * 2.0) : (freq);
	top_margin = margins ? rint(MARGIN_PERCENT / 100.0 * v_lines_rnd) : (0.0);
	bottom_margin = margins ? rint(MARGIN_PERCENT / 100.0 * v_lines_rnd) : (0.0);
	interlace = interlaced ? 0.5 : 0.0;
	h_period_est = (((1.0 / v_field_rate_rqd) - (MIN_VSYNC_PLUS_BP / 1000000.0)) / (v_lines_rnd + (2 * top_margin) + MIN_PORCH + interlace) * 1000000.0);
	vsync_plus_bp = rint(MIN_VSYNC_PLUS_BP / h_period_est);
	v_back_porch = vsync_plus_bp - V_SYNC_RQD;
	total_v_lines = v_lines_rnd + top_margin + bottom_margin + vsync_plus_bp + interlace + MIN_PORCH;
	v_field_rate_est = 1.0 / h_period_est / total_v_lines * 1000000.0;
	h_period = h_period_est / (v_field_rate_rqd / v_field_rate_est);
	v_field_rate = 1.0 / h_period / total_v_lines * 1000000.0;
	v_frame_rate = interlaced ? v_field_rate / 2.0 : v_field_rate;
	left_margin = margins ? rint(h_pixels_rnd * MARGIN_PERCENT / 100.0 / CELL_GRAN) * CELL_GRAN : 0.0;
	right_margin = margins ? rint(h_pixels_rnd * MARGIN_PERCENT / 100.0 / CELL_GRAN) * CELL_GRAN : 0.0;
	total_active_pixels = h_pixels_rnd + left_margin + right_margin;
	ideal_duty_cycle = C_PRIME - (M_PRIME * h_period / 1000.0);
	h_blank = rint(total_active_pixels * ideal_duty_cycle / (100.0 - ideal_duty_cycle) / (2.0 * CELL_GRAN)) * (2.0 * CELL_GRAN);
	total_pixels = total_active_pixels + h_blank;
	pixel_freq = total_pixels / h_period;
	h_freq = 1000.0 / h_period;
	h_sync = rint(H_SYNC_PERCENT / 100.0 * total_pixels / CELL_GRAN) * CELL_GRAN;
	h_front_porch = (h_blank / 2.0) - h_sync;
	v_odd_front_porch_lines = MIN_PORCH + interlace;

	m->clock = ceil(pixel_freq) * 1000;
	m->hdisplay = (int) (h_pixels_rnd);
	m->hsync_start = (int) (h_pixels_rnd + h_front_porch);
	m->hsync_end = (int) (h_pixels_rnd + h_front_porch + h_sync);
	m->htotal = (int) (total_pixels);
	m->hskew = 0;
	m->vdisplay = (int) (v_lines_rnd);
	m->vsync_start = (int) (v_lines_rnd + v_odd_front_porch_lines);
	m->vsync_end = (int) (int) (v_lines_rnd + v_odd_front_porch_lines + V_SYNC_RQD);
	m->vtotal = (int) (total_v_lines);
	m->vscan = 0;
	m->vrefresh = freq;
	m->flags = 10;
	m->type = 64;

	return (m);
}

static drmModeModeInfoPtr find_mode(drmModeConnectorPtr connector, int *bpp)
{
	char value[PROPERTY_VALUE_MAX];
	drmModeModeInfoPtr mode;
	int dist, i;
	int xres = 0, yres = 0, rate = 0;
	int forcemode = 0;

	if (property_get("debug.drm.mode", value, NULL)) {
		char *p = value, *end;

		/* parse <xres>x<yres>[@<bpp>] */
		if (sscanf(value, "%dx%d@%d", &xres, &yres, bpp) != 3) {
			*bpp = 0;
			if (sscanf(value, "%dx%d", &xres, &yres) != 2)
				xres = yres = 0;
		}

		if ((xres && yres) || *bpp) {
			ALOGI("will find the closest match for %dx%d@%d",
					xres, yres, *bpp);
		}
	} else if (property_get("debug.drm.mode.force", value, NULL)) {
		char *p = value, *end;
		*bpp = 0;

		/* parse <xres>x<yres>[@<refreshrate>] */
		if (sscanf(value, "%dx%d@%d", &xres, &yres, &rate) != 3) {
			rate = 60;
			if (sscanf(value, "%dx%d", &xres, &yres) != 2)
				xres = yres = 0;
		}

		if (xres && yres && rate) {
			ALOGI("will use %dx%d@%dHz", xres, yres, rate);
			forcemode = 1;
		}
	} else {
		*bpp = 0;
	}

	dist = INT_MAX;

	if (forcemode)
		mode = generate_mode(xres, yres, rate);
	else {
		mode = NULL;
		for (i = 0; i < connector->count_modes; i++) {
			drmModeModeInfoPtr m = &connector->modes[i];
			int tmp;

			if (xres && yres) {
				tmp = (m->hdisplay - xres) * (m->hdisplay - xres) +
					(m->vdisplay - yres) * (m->vdisplay - yres);
			}
			else {
				/* use the first preferred mode */
				tmp = (m->type & DRM_MODE_TYPE_PREFERRED) ? 0 : dist;
			}

			if (tmp < dist) {
				mode = m;
				dist = tmp;
				if (!dist)
					break;
			}
		}
	}

	/* fallback to the first mode */
	if (!mode)
		mode = &connector->modes[0];

	ALOGI("Established mode:");
	ALOGI("clock: %d, hdisplay: %d, hsync_start: %d, hsync_end: %d, htotal: %d, hskew: %d", mode->clock, mode->hdisplay, mode->hsync_start, mode->hsync_end, mode->htotal, mode->hskew);
	ALOGI("vdisplay: %d, vsync_start: %d, vsync_end: %d, vtotal: %d, vscan: %d, vrefresh: %d", mode->vdisplay, mode->vsync_start, mode->vsync_end, mode->vtotal, mode->vscan, mode->vrefresh);
	ALOGI("flags: %d, type: %d, name %s", mode->flags, mode->type, mode->name);

	*bpp /= 8;

	return mode;
}

/*
 * Initialize KMS with a connector.
 */
static int drm_kms_init_with_connector(struct gralloc_drm_t *drm,
		struct gralloc_drm_output *output, drmModeConnectorPtr connector)
{
	drmModeEncoderPtr encoder;
	drmModeModeInfoPtr mode;
	static int used_crtcs = 0;
	int bpp, i;

	if (!connector->count_modes)
		return -EINVAL;

	encoder = drmModeGetEncoder(drm->fd, connector->encoders[0]);
	if (!encoder)
		return -EINVAL;

	/* find first possible crtc which is not used yet */
	for (i = 0; i < drm->resources->count_crtcs; i++) {
		if (encoder->possible_crtcs & (1 << i) &&
			(used_crtcs & (1 << i)) != (1 << i))
			break;
	}

	used_crtcs |= (1 << i);

	drmModeFreeEncoder(encoder);
	if (i == drm->resources->count_crtcs)
		return -EINVAL;

	output->bo = NULL;
	output->crtc_id = drm->resources->crtcs[i];
	output->connector_id = connector->connector_id;
	output->pipe = i;

	/* print connector info */
	if (connector->count_modes > 1) {
		ALOGI("there are %d modes on connector 0x%x, type %d",
				connector->count_modes,
				connector->connector_id,
				connector->connector_type);
		for (i = 0; i < connector->count_modes; i++)
			ALOGI("  %s", connector->modes[i].name);
	}
	else {
		ALOGI("there is one mode on connector 0x%d: %s",
				connector->connector_id,
				connector->modes[0].name);
	}

	mode = find_mode(connector, &bpp);

	ALOGI("the best mode is %s", mode->name);

	output->mode = *mode;
	switch (bpp) {
	case 2:
		output->fb_format = HAL_PIXEL_FORMAT_RGB_565;
		break;
	case 4:
	default:
		output->fb_format = HAL_PIXEL_FORMAT_BGRA_8888;
		break;
	}

	if (connector->mmWidth && connector->mmHeight) {
		output->xdpi = (output->mode.hdisplay * 25.4 / connector->mmWidth);
		output->ydpi = (output->mode.vdisplay * 25.4 / connector->mmHeight);
	}
	else {
		output->xdpi = 75;
		output->ydpi = 75;
	}

#ifdef DRM_MODE_FEATURE_DIRTYFB
	drm->clip.x1 = 0;
	drm->clip.y1 = 0;
	drm->clip.x2 = output->mode.hdisplay;
	drm->clip.y2 = output->mode.vdisplay;
#endif

	return 0;
}


/*
 * Fetch a connector of particular type
 */
static drmModeConnectorPtr fetch_connector(struct gralloc_drm_t *drm,
	uint32_t type)
{
	int i;

	if (!drm->resources)
		return NULL;

	for (i = 0; i < drm->resources->count_connectors; i++) {
		drmModeConnectorPtr connector =
			connector = drmModeGetConnector(drm->fd,
				drm->resources->connectors[i]);
		if (connector) {
			if (connector->connector_type == type &&
				connector->connection == DRM_MODE_CONNECTED)
				return connector;
			drmModeFreeConnector(connector);
		}
	}
	return NULL;
}


/*
 * Initializes hdmi output with a connector and allocates
 * a private framebuffer for it. This is called on startup if
 * hdmi cable is connected and also on hotplug events.
 */
static void init_hdmi_output(struct gralloc_drm_t *drm,
	drmModeConnectorPtr connector)
{
	drm_kms_init_with_connector(drm, &drm->hdmi, connector);

	ALOGD("%s, allocate private buffer for hdmi [%dx%d]",
		__func__, drm->hdmi.mode.hdisplay, drm->hdmi.mode.vdisplay);

	drm->hdmi.bo = gralloc_drm_bo_create(drm,
		drm->hdmi.mode.hdisplay, drm->hdmi.mode.vdisplay,
		drm->hdmi.fb_format,
		GRALLOC_USAGE_SW_WRITE_OFTEN|GRALLOC_USAGE_HW_RENDER);

	gralloc_drm_bo_add_fb(drm->hdmi.bo);

	drm->hdmi_mode = HDMI_CLONED;
	drm->hdmi.active = 1;
}


/*
 * Thread that listens to uevents and checks if hdmi state changes
 */
static void *hdmi_observer(void *data)
{
	static char uevent_desc[4096];
	drmModeConnectorPtr hdmi;
	struct gralloc_drm_t *drm =
		(struct gralloc_drm_t *) data;

	uevent_init();

	memset(uevent_desc, 0, sizeof(uevent_desc));

	while(1) {

		/* this polls */
		int len = uevent_next_event(uevent_desc, sizeof(uevent_desc) - 2);

		if(len && strstr(uevent_desc, "devices/virtual/switch/hdmi")) {

			/* check what changed */
			const char *prop = uevent_desc + strlen(uevent_desc) + 1;

			while (*prop) {

				const char *state = strstr(prop, "SWITCH_STATE=");
				if (state) {
					unsigned int value = 0;
					state += strlen("SWITCH_STATE=");
					value = atoi(state);

					pthread_mutex_lock(&drm->hdmi_mutex);

					if (value) {
						hdmi = fetch_connector(drm, DRM_MODE_CONNECTOR_HDMIA);
						if (hdmi) {

							ALOGD("init hdmi on hotplug event");
							init_hdmi_output(drm, hdmi);

							/* will trigger modeset */
							drm->first_post = 1;

							drmModeFreeConnector(hdmi);

							pthread_mutex_unlock(&drm->hdmi_mutex);
						}
						break;
					} else {
						drm->hdmi.active = 0;

						ALOGD("destroy hdmi private buffer");
						gralloc_drm_bo_decref(drm->hdmi.bo);
						drm->hdmi.bo = NULL;

						pthread_mutex_unlock(&drm->hdmi_mutex);
						break;
					}

					pthread_mutex_unlock(&drm->hdmi_mutex);
				}

				/* next property/value pair */
				prop += strlen(prop) + 1;
				if (prop - uevent_desc >= len)
					break;
			}
		}
	}

	pthread_exit(NULL);
	return 0;
}


/*
 * Initialize KMS.
 */
int gralloc_drm_init_kms(struct gralloc_drm_t *drm)
{
	drmModeConnectorPtr lvds, hdmi;
	int i, ret;

	if (drm->resources)
		return 0;

	drm->resources = drmModeGetResources(drm->fd);
	if (!drm->resources) {
		ALOGE("failed to get modeset resources");
		return -EINVAL;
	}

	drm->plane_resources = drmModeGetPlaneResources(drm->fd);
	if (!drm->plane_resources) {
		ALOGD("no planes found from drm resources");
	} else {
		unsigned int i, j;

		ALOGD("supported drm planes and formats");
		/* fill a helper structure for hwcomposer */
		drm->planes = calloc(drm->plane_resources->count_planes,
			sizeof(struct gralloc_drm_plane_t));

		for (i = 0; i < drm->plane_resources->count_planes; i++) {
			drm->planes[i].drm_plane = drmModeGetPlane(drm->fd,
				drm->plane_resources->planes[i]);

			ALOGD("plane id %d", drm->planes[i].drm_plane->plane_id);
			for (j = 0; j < drm->planes[i].drm_plane->count_formats; j++)
				ALOGD("    format %c%c%c%c",
					(drm->planes[i].drm_plane->formats[j]),
					(drm->planes[i].drm_plane->formats[j])>>8,
					(drm->planes[i].drm_plane->formats[j])>>16,
					(drm->planes[i].drm_plane->formats[j])>>24);
		}
	}

	/* find the crtc/connector/mode to use */
	lvds = fetch_connector(drm, DRM_MODE_CONNECTOR_LVDS);
	if (lvds) {
		drm_kms_init_with_connector(drm, &drm->primary, lvds);
		drmModeFreeConnector(lvds);
		drm->primary.active = 1;
	}

	/* if still no connector, find first connected connector and try it */
	if (!drm->primary.active) {

		for (i = 0; i < drm->resources->count_connectors; i++) {
			drmModeConnectorPtr connector;

			connector = drmModeGetConnector(drm->fd,
					drm->resources->connectors[i]);
			if (connector) {
				if (connector->connection == DRM_MODE_CONNECTED) {
					if (!drm_kms_init_with_connector(drm,
							&drm->primary, connector))
						break;
				}

				drmModeFreeConnector(connector);
			}
		}
		if (i == drm->resources->count_connectors) {
			ALOGE("failed to find a valid crtc/connector/mode combination");
			drmModeFreeResources(drm->resources);
			drm->resources = NULL;

			return -EINVAL;
		}
	}


	/* check if hdmi is connected already */
	hdmi = fetch_connector(drm, DRM_MODE_CONNECTOR_HDMIA);
	if (hdmi) {

		if (hdmi->connector_id == drm->primary.connector_id) {
			/* special case: our primary connector is hdmi */
			ALOGD("hdmi is the primary connector");
			goto skip_hdmi_modes;
		}

		ALOGD("init hdmi on startup");
		init_hdmi_output(drm, hdmi);

		drmModeFreeConnector(hdmi);
	}

goto skip_hdmi_modes;
	/* launch hdmi observer thread */
	pthread_mutex_init(&drm->hdmi_mutex, NULL);
	pthread_create(&drm->hdmi_hotplug_thread, NULL, hdmi_observer, drm);

skip_hdmi_modes:

	drm_kms_init_features(drm);
	drm->first_post = 1;

	return 0;
}

void gralloc_drm_fini_kms(struct gralloc_drm_t *drm)
{
	switch (drm->swap_mode) {
	case DRM_SWAP_FLIP:
		drm_kms_page_flip(drm, NULL);
		break;
	case DRM_SWAP_COPY:
		{
			struct gralloc_drm_bo_t **bo = (drm->current_front) ?
				&drm->current_front : &drm->next_front;

			if (*bo)
				gralloc_drm_bo_decref(*bo);
			*bo = NULL;
		}
		break;
	default:
		break;
	}

	/* restore crtc? */

	if (drm->resources) {
		drmModeFreeResources(drm->resources);
		drm->resources = NULL;
	}

	if (drm->planes) {
		unsigned int i;
		for (i = 0; i < drm->plane_resources->count_planes; i++)
			drmModeFreePlane(drm->planes[i].drm_plane);
		free(drm->planes);
		drm->planes = NULL;
	}

	if (drm->plane_resources) {
		drmModeFreePlaneResources(drm->plane_resources);
		drm->plane_resources = NULL;
	}

	/* destroy private buffer of hdmi output */
	if (drm->hdmi.bo)
		gralloc_drm_bo_decref(drm->hdmi.bo);

	drm_singleton = NULL;
}

int gralloc_drm_is_kms_initialized(struct gralloc_drm_t *drm)
{
	return (drm->resources != NULL);
}

/*
 * Initialize a framebuffer device with KMS info.
 */
void gralloc_drm_get_kms_info(struct gralloc_drm_t *drm,
		struct framebuffer_device_t *fb)
{
	*((uint32_t *) &fb->flags) = 0x0;
	*((uint32_t *) &fb->width) = drm->primary.mode.hdisplay;
	*((uint32_t *) &fb->height) = drm->primary.mode.vdisplay;
	*((int *)      &fb->stride) = drm->primary.mode.hdisplay;
	*((float *)    &fb->fps) = drm->primary.mode.vrefresh;

	*((int *)      &fb->format) = drm->primary.fb_format;
	*((float *)    &fb->xdpi) = drm->primary.xdpi;
	*((float *)    &fb->ydpi) = drm->primary.ydpi;
	*((int *)      &fb->minSwapInterval) = drm->swap_interval;
	*((int *)      &fb->maxSwapInterval) = drm->swap_interval;
}

/*
 * Return true if fb posting is pipelined.
 */
int gralloc_drm_is_kms_pipelined(struct gralloc_drm_t *drm)
{
	return (drm->swap_mode != DRM_SWAP_SETCRTC);
}
