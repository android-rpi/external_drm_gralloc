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

#define LOG_TAG "GRALLOC-DRM"

#include <cutils/log.h>
#include <cutils/atomic.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "gralloc_drm.h"
#include "gralloc_drm_priv.h"

#define unlikely(x) __builtin_expect(!!(x), 0)

#define GRALLOC_DRM_DEVICE "/dev/dri/card0"

static int32_t gralloc_drm_pid = 0;

/*
 * Return the pid of the process.
 */
static int gralloc_drm_get_pid(void)
{
	if (unlikely(!gralloc_drm_pid))
		android_atomic_write((int32_t) getpid(), &gralloc_drm_pid);

	return gralloc_drm_pid;
}

/*
 * Create the driver for a DRM fd.
 */
static struct gralloc_drm_drv_t *
init_drv_from_fd(int fd)
{
	struct gralloc_drm_drv_t *drv = NULL;
	drmVersionPtr version;

	/* get the kernel module name */
	version = drmGetVersion(fd);
	if (!version) {
		LOGE("invalid DRM fd");
		return NULL;
	}

	if (version->name) {
#ifdef ENABLE_PIPE
		drv = gralloc_drm_drv_create_for_pipe(fd, version->name);
#endif

#ifdef ENABLE_INTEL
		if (!drv && !strcmp(version->name, "i915"))
			drv = gralloc_drm_drv_create_for_intel(fd);
#endif
#ifdef ENABLE_RADEON
		if (!drv && !strcmp(version->name, "radeon"))
			drv = gralloc_drm_drv_create_for_radeon(fd);
#endif
#ifdef ENABLE_NOUVEAU
		if (!drv && !strcmp(version->name, "nouveau"))
			drv = gralloc_drm_drv_create_for_nouveau(fd);
#endif
	}

	if (!drv) {
		LOGE("unsupported driver: %s", (version->name) ?
				version->name : "NULL");
	}

	drmFreeVersion(version);

	return drv;
}

/*
 * Create a DRM device object.
 */
struct gralloc_drm_t *gralloc_drm_create(void)
{
	struct gralloc_drm_t *drm;
	int err;

	drm = calloc(1, sizeof(*drm));
	if (!drm)
		return NULL;

	drm->fd = open(GRALLOC_DRM_DEVICE, O_RDWR);
	if (drm->fd < 0) {
		LOGE("failed to open %s", GRALLOC_DRM_DEVICE);
		return NULL;
	}

	drm->drv = init_drv_from_fd(drm->fd);
	if (!drm->drv) {
		close(drm->fd);
		free(drm);
		return NULL;
	}

	return drm;
}

/*
 * Destroy a DRM device object.
 */
void gralloc_drm_destroy(struct gralloc_drm_t *drm)
{
	if (drm->drv)
		drm->drv->destroy(drm->drv);
	close(drm->fd);
	free(drm);
}

/*
 * Get the file descriptor of a DRM device object.
 */
int gralloc_drm_get_fd(struct gralloc_drm_t *drm)
{
	return drm->fd;
}

/*
 * Get the magic for authentication.
 */
int gralloc_drm_get_magic(struct gralloc_drm_t *drm, int32_t *magic)
{
	return drmGetMagic(drm->fd, (drm_magic_t *) magic);
}

/*
 * Authenticate a magic.
 */
int gralloc_drm_auth_magic(struct gralloc_drm_t *drm, int32_t magic)
{
	return drmAuthMagic(drm->fd, (drm_magic_t) magic);
}

/*
 * Set as the master of a DRM device.
 */
int gralloc_drm_set_master(struct gralloc_drm_t *drm)
{
	LOGD("set master");
	drmSetMaster(drm->fd);
	drm->first_post = 1;

	return 0;
}

/*
 * Drop from the master of a DRM device.
 */
void gralloc_drm_drop_master(struct gralloc_drm_t *drm)
{
	drmDropMaster(drm->fd);
}

/*
 * Create a buffer handle.
 */
static struct gralloc_drm_handle_t *create_bo_handle(int width,
		int height, int format, int usage)
{
	struct gralloc_drm_handle_t *handle;

	handle = calloc(1, sizeof(*handle));
	if (!handle)
		return NULL;

	handle->base.version = sizeof(handle->base);
	handle->base.numInts = GRALLOC_DRM_HANDLE_NUM_INTS;
	handle->base.numFds = GRALLOC_DRM_HANDLE_NUM_FDS;

	handle->magic = GRALLOC_DRM_HANDLE_MAGIC;
	handle->width = width;
	handle->height = height;
	handle->format = format;
	handle->usage = usage;

	return handle;
}

/*
 * Create a bo.
 */
struct gralloc_drm_bo_t *gralloc_drm_bo_create(struct gralloc_drm_t *drm,
		int width, int height, int format, int usage)
{
	struct gralloc_drm_bo_t *bo;
	struct gralloc_drm_handle_t *handle;

	handle = create_bo_handle(width, height, format, usage);
	if (!handle)
		return NULL;

	bo = drm->drv->alloc(drm->drv, handle);
	if (!bo) {
		free(handle);
		return NULL;
	}

	bo->drm = drm;
	bo->imported = 0;
	bo->handle = handle;

	handle->data_owner = gralloc_drm_get_pid();
	handle->data = (int) bo;

	return bo;
}

/*
 * Destroy a bo.
 */
void gralloc_drm_bo_destroy(struct gralloc_drm_bo_t *bo)
{
	struct gralloc_drm_handle_t *handle = bo->handle;
	int imported = bo->imported;

	bo->drm->drv->free(bo->drm->drv, bo);
	if (imported) {
		handle->data_owner = 0;
		handle->data = 0;
	}
	else {
		free(handle);
	}
}

/*
 * Register a buffer handle and return the associated bo.
 */
struct gralloc_drm_bo_t *gralloc_drm_bo_register(struct gralloc_drm_t *drm,
		buffer_handle_t _handle, int create)
{
	struct gralloc_drm_handle_t *handle = gralloc_drm_handle(_handle);

	if (!handle)
		return NULL;

	/* the buffer handle is passed to a new process */
	if (unlikely(handle->data_owner != gralloc_drm_pid)) {
		struct gralloc_drm_bo_t *bo;

		if (!create)
			return NULL;

		/* create the struct gralloc_drm_bo_t locally */
		if (handle->name)
			bo = drm->drv->alloc(drm->drv, handle);
		else /* an invalid handle */
			bo = NULL;
		if (bo) {
			bo->drm = drm;
			bo->imported = 1;
			bo->handle = handle;
		}

		handle->data_owner = gralloc_drm_get_pid();
		handle->data = (int) bo;
	}

	return (struct gralloc_drm_bo_t *) handle->data;
}

/*
 * Unregister a bo.  It is no-op for bo created locally.
 */
void gralloc_drm_bo_unregister(struct gralloc_drm_bo_t *bo)
{
	if (bo->imported)
		gralloc_drm_bo_destroy(bo);
}

/*
 * Lock a bo.  XXX thread-safety?
 */
int gralloc_drm_bo_lock(struct gralloc_drm_bo_t *bo,
		int usage, int x, int y, int w, int h,
		void **addr)
{
	if ((bo->handle->usage & usage) != usage) {
		/* make FB special for testing software renderer with */
		if (!(bo->handle->usage & GRALLOC_USAGE_HW_FB))
			return -EINVAL;
	}

	/* allow multiple locks with compatible usages */
	if (bo->lock_count && (bo->locked_for & usage) != usage)
		return -EINVAL;

	usage |= bo->locked_for;

	if (usage & (GRALLOC_USAGE_SW_WRITE_MASK |
		     GRALLOC_USAGE_SW_READ_MASK)) {
		/* the driver is supposed to wait for the bo */
		int write = !!(usage & GRALLOC_USAGE_SW_WRITE_MASK);
		int err = bo->drm->drv->map(bo->drm->drv, bo,
				x, y, w, h, write, addr);
		if (err)
			return err;
	}
	else {
		/* kernel handles the synchronization here */
	}

	bo->lock_count++;
	bo->locked_for |= usage;

	return 0;
}

/*
 * Unlock a bo.
 */
void gralloc_drm_bo_unlock(struct gralloc_drm_bo_t *bo)
{
	int mapped = bo->locked_for &
		(GRALLOC_USAGE_SW_WRITE_MASK | GRALLOC_USAGE_SW_READ_MASK);

	if (!bo->lock_count)
		return;

	if (mapped)
		bo->drm->drv->unmap(bo->drm->drv, bo);

	bo->lock_count--;
	if (!bo->lock_count)
		bo->locked_for = 0;
}

/*
 * Get the buffer handle and stride of a bo.
 */
buffer_handle_t gralloc_drm_bo_get_handle(struct gralloc_drm_bo_t *bo, int *stride)
{
	if (stride)
		*stride = bo->handle->stride;
	return &bo->handle->base;
}
