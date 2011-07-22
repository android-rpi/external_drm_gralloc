# Copyright (C) 2010 Chia-I Wu <olvaffe@gmail.com>
# Copyright (C) 2010-2011 LunarG Inc.
# 
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

# Android.mk for drm_gralloc

DRM_GPU_DRIVERS := $(BOARD_GPU_DRIVERS)

# convert board uses to DRM_GPU_DRIVERS
ifeq ($(strip $(DRM_GPU_DRIVERS)),)
ifeq ($(strip $(BOARD_USES_I915C)),true)
DRM_GPU_DRIVERS += i915c
endif
ifeq ($(strip $(BOARD_USES_I965C)),true)
DRM_GPU_DRIVERS += i965c
endif
ifeq ($(strip $(BOARD_USES_I915G)),true)
DRM_GPU_DRIVERS += i915g
endif
ifeq ($(strip $(BOARD_USES_R300G)),true)
DRM_GPU_DRIVERS += r300g
endif
ifeq ($(strip $(BOARD_USES_R600G)),true)
DRM_GPU_DRIVERS += r600g
endif
ifeq ($(strip $(BOARD_USES_NOUVEAU)),true)
DRM_GPU_DRIVERS += nouveau
endif
ifeq ($(strip $(BOARD_USES_VMWGFX)),true)
DRM_GPU_DRIVERS += vmwgfx
endif
endif # DRM_GPU_DRIVERS

intel_drivers := i915c i965c i915g
radeon_drivers := r300g r600g
nouveau_drivers := nouveau
vmwgfx_drivers := vmwgfx

valid_drivers := \
	$(intel_drivers) \
	$(radeon_drivers) \
	$(nouveau_drivers) \
	$(vmwgfx_drivers)

# warn about invalid drivers
invalid_drivers := $(filter-out $(valid_drivers), $(DRM_GPU_DRIVERS))
ifneq ($(invalid_drivers),)
$(warning invalid GPU drivers: $(invalid_drivers))
# tidy up
DRM_GPU_DRIVERS := $(filter-out $(invalid_drivers), $(DRM_GPU_DRIVERS))
endif

ifneq ($(filter $(vmwgfx_drivers), $(DRM_GPU_DRIVERS)),)
DRM_USES_PIPE := true
else
DRM_USES_PIPE := false
endif

ifneq ($(strip $(DRM_GPU_DRIVERS)),)

LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_SRC_FILES := \
	gralloc.c \
	gralloc_drm.c \
	gralloc_drm_kms.c

LOCAL_C_INCLUDES := \
	external/drm \
	external/drm/include/drm

LOCAL_SHARED_LIBRARIES := \
	libdrm \
	liblog \
	libcutils \

# for glFlush/glFinish
LOCAL_SHARED_LIBRARIES += \
	libGLESv1_CM

ifneq ($(filter $(intel_drivers), $(DRM_GPU_DRIVERS)),)
LOCAL_SRC_FILES += gralloc_drm_intel.c
LOCAL_C_INCLUDES += external/drm/intel
LOCAL_CFLAGS += -DENABLE_INTEL
LOCAL_SHARED_LIBRARIES += libdrm_intel
endif

ifneq ($(filter $(radeon_drivers), $(DRM_GPU_DRIVERS)),)
LOCAL_SRC_FILES += gralloc_drm_radeon.c
LOCAL_C_INCLUDES += external/drm/radeon
LOCAL_CFLAGS += -DENABLE_RADEON
LOCAL_SHARED_LIBRARIES += libdrm_radeon
endif

ifneq ($(filter $(nouveau_drivers), $(DRM_GPU_DRIVERS)),)
LOCAL_SRC_FILES += gralloc_drm_nouveau.c
LOCAL_C_INCLUDES += external/drm/nouveau
LOCAL_CFLAGS += -DENABLE_NOUVEAU
LOCAL_SHARED_LIBRARIES += libdrm_nouveau
endif

ifeq ($(strip $(DRM_USES_PIPE)),true)
LOCAL_SRC_FILES += gralloc_drm_pipe.c
LOCAL_CFLAGS += -DENABLE_PIPE
LOCAL_C_INCLUDES += \
	external/mesa/src/gallium/include \
	external/mesa/src/gallium/winsys \
	external/mesa/src/gallium/drivers \
	external/mesa/src/gallium/auxiliary

ifneq ($(filter r600g, $(DRM_GPU_DRIVERS)),)
LOCAL_CFLAGS += -DENABLE_PIPE_R600
LOCAL_STATIC_LIBRARIES += \
	libmesa_pipe_r600 \
	libmesa_winsys_r600
endif
ifneq ($(filter vmwgfx, $(DRM_GPU_DRIVERS)),)
LOCAL_CFLAGS += -DENABLE_PIPE_VMWGFX
LOCAL_STATIC_LIBRARIES += \
	libmesa_pipe_svga \
	libmesa_winsys_svga
endif

LOCAL_STATIC_LIBRARIES += \
	libmesa_gallium
LOCAL_SHARED_LIBRARIES += libdl
endif # DRM_USES_PIPE

LOCAL_MODULE := gralloc.$(TARGET_PRODUCT)
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw

include $(BUILD_SHARED_LIBRARY)

endif # DRM_GPU_DRIVERS
