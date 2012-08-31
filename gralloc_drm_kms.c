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
#include "gralloc_drm.h"
#include "gralloc_drm_priv.h"

#include <drm_fourcc.h>

/*
 * Return true if a bo needs fb.
 */
int gralloc_drm_bo_need_fb(const struct gralloc_drm_bo_t *bo)
{
	return ((bo->handle->usage & GRALLOC_USAGE_HW_FB) &&
		bo->drm->swap_mode != DRM_SWAP_COPY);
}

/*
 * Modify pitches, offsets and handles according to
 * the format and return corresponding drm format value
 */
static int resolve_drm_format(struct gralloc_drm_bo_t *bo,
	uint32_t *pitches, uint32_t *offsets, uint32_t *handles)
{
	memset(pitches, 0, 4 * sizeof(uint32_t));
	memset(offsets, 0, 4 * sizeof(uint32_t));
	memset(handles, 0, 4 * sizeof(uint32_t));

	pitches[0] = bo->handle->stride;
	handles[0] = bo->fb_handle;

	switch(bo->handle->format) {
		case HAL_PIXEL_FORMAT_RGB_888:
		case HAL_PIXEL_FORMAT_RGBX_8888:
		case HAL_PIXEL_FORMAT_BGRA_8888:
			return DRM_FORMAT_ARGB8888;
		case HAL_PIXEL_FORMAT_RGBA_8888:
			return DRM_FORMAT_RGBA8888;
		case HAL_PIXEL_FORMAT_RGB_565:
			return DRM_FORMAT_RGB565;
		case HAL_PIXEL_FORMAT_YV12:

			// U and V stride are half of Y plane
			pitches[2] = pitches[0]/2;
			pitches[1] = pitches[0]/2;

			// like I420 but U and V are in reverse order
			offsets[2] = offsets[0] +
				pitches[0] * bo->handle->height;
			offsets[1] = offsets[2] +
				pitches[2] * bo->handle->height/2;

			handles[1] = handles[2] = handles[0];

			return DRM_FORMAT_YUV420;
		default:
			return 0;
	}
	return 0;
}

/*
 * Add a fb object for a bo.
 */
int gralloc_drm_bo_add_fb(struct gralloc_drm_bo_t *bo)
{
	uint32_t pitches[4];
	uint32_t offsets[4];
	uint32_t handles[4];

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
static int drm_kms_set_crtc(struct gralloc_drm_t *drm, int fb_id)
{
	int ret;

	ret = drmModeSetCrtc(drm->fd, drm->crtc_id, fb_id,
			0, 0, &drm->connector_id, 1, &drm->mode);
	if (ret) {
		ALOGE("failed to set crtc");
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

	ret = drmModePageFlip(drm->fd, drm->crtc_id, bo->fb_id,
			DRM_MODE_PAGE_FLIP_EVENT, (void *) drm);
	if (ret)
		ALOGE("failed to perform page flip");
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
			drm->drv->copy(drm->drv, dst, bo, 0, 0,
					bo->handle->width,
					bo->handle->height);
			bo = dst;
		}

		ret = drm_kms_set_crtc(drm, bo->fb_id);
		if (!ret) {
			drm->first_post = 0;
			drm->current_front = bo;
			if (drm->next_front == bo)
				drm->next_front = NULL;
		}

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
		drm->drv->copy(drm->drv, drm->current_front,
				bo, 0, 0,
				bo->handle->width,
				bo->handle->height);
		if (drm->mode_quirk_vmwgfx)
			ret = drmModeDirtyFB(drm->fd, drm->current_front->fb_id, &drm->clip, 1);
		ret = 0;
		break;
	case DRM_SWAP_SETCRTC:
		drm_kms_wait_for_post(drm, 0);
		ret = drm_kms_set_crtc(drm, bo->fb_id);
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
					      drm->mode.hdisplay,
					      drm->mode.vdisplay,
					      drm->fb_format,
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

static drmModeModeInfoPtr find_mode(drmModeConnectorPtr connector, int *bpp)
{
	char value[PROPERTY_VALUE_MAX];
	drmModeModeInfoPtr mode;
	int dist, i;
	int xres = 0, yres = 0;

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
	}
	else {
		*bpp = 0;
	}

	mode = NULL;
	dist = INT_MAX;
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

	/* fallback to the first mode */
	if (!mode)
		mode = &connector->modes[0];

	*bpp /= 8;

	return mode;
}

/*
 * Initialize KMS with a connector.
 */
static int drm_kms_init_with_connector(struct gralloc_drm_t *drm,
		drmModeConnectorPtr connector)
{
	drmModeEncoderPtr encoder;
	drmModeModeInfoPtr mode;
	int bpp, i;

	if (!connector->count_modes)
		return -EINVAL;

	encoder = drmModeGetEncoder(drm->fd, connector->encoders[0]);
	if (!encoder)
		return -EINVAL;

	for (i = 0; i < drm->resources->count_crtcs; i++) {
		if (encoder->possible_crtcs & (1 << i))
			break;
	}
	drmModeFreeEncoder(encoder);
	if (i == drm->resources->count_crtcs)
		return -EINVAL;

	drm->crtc_id = drm->resources->crtcs[i];
	drm->connector_id = connector->connector_id;

	/* print connector info */
	if (connector->count_modes > 1) {
		ALOGI("there are %d modes on connector 0x%x",
				connector->count_modes,
				connector->connector_id);
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

	drm->mode = *mode;
	switch (bpp) {
	case 2:
		drm->fb_format = HAL_PIXEL_FORMAT_RGB_565;
		break;
	case 4:
	default:
		drm->fb_format = HAL_PIXEL_FORMAT_BGRA_8888;
		break;
	}

	if (connector->mmWidth && connector->mmHeight) {
		drm->xdpi = (drm->mode.hdisplay * 25.4 / connector->mmWidth);
		drm->ydpi = (drm->mode.vdisplay * 25.4 / connector->mmHeight);
	}
	else {
		drm->xdpi = 75;
		drm->ydpi = 75;
	}

#ifdef DRM_MODE_FEATURE_DIRTYFB
	drm->clip.x1 = 0;
	drm->clip.y1 = 0;
	drm->clip.x2 = drm->mode.hdisplay;
	drm->clip.y2 = drm->mode.vdisplay;
#endif

	return 0;
}

/*
 * Initialize KMS.
 */
int gralloc_drm_init_kms(struct gralloc_drm_t *drm)
{
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
		ALOGD("supported drm planes and formats");
		/* fill a helper structure for hwcomposer */
		drm->planes = calloc(drm->plane_resources->count_planes,
			sizeof(struct gralloc_drm_plane_t));

		for (i = 0; i < drm->plane_resources->count_planes; i++) {

			unsigned int j;

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
	for (i = 0; i < drm->resources->count_connectors; i++) {
		drmModeConnectorPtr connector;

		connector = drmModeGetConnector(drm->fd,
				drm->resources->connectors[i]);
		if (connector) {
			if (connector->connection == DRM_MODE_CONNECTED) {
				if (!drm_kms_init_with_connector(drm,
							connector))
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
	*((uint32_t *) &fb->width) = drm->mode.hdisplay;
	*((uint32_t *) &fb->height) = drm->mode.vdisplay;
	*((int *)      &fb->stride) = drm->mode.hdisplay;
	*((float *)    &fb->fps) = drm->mode.vrefresh;

	*((int *)      &fb->format) = drm->fb_format;
	*((float *)    &fb->xdpi) = drm->xdpi;
	*((float *)    &fb->ydpi) = drm->ydpi;
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
