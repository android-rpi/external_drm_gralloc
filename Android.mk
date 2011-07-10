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

ifeq ($(strip $(BOARD_USES_DRM)),true)

LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

DRM_USES_INTEL := $(findstring true, \
	$(BOARD_USES_I915C) \
	$(BOARD_USES_I965C) \
	$(BOARD_USES_I915G) \
	$(BOARD_USES_I965G))

DRM_USES_RADEON := $(findstring true, \
	$(BOARD_USES_R300G) \
	$(BOARD_USES_R600G))

DRM_USES_NOUVEAU := $(findstring true, \
	$(BOARD_USES_NOUVEAU))

DRM_USES_PIPE := false

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

ifeq ($(strip $(DRM_USES_INTEL)),true)
LOCAL_SRC_FILES += gralloc_drm_intel.c
LOCAL_C_INCLUDES += external/drm/intel
LOCAL_CFLAGS += -DENABLE_INTEL
LOCAL_SHARED_LIBRARIES += libdrm_intel
endif # DRM_USES_INTEL

ifeq ($(strip $(DRM_USES_RADEON)),true)
LOCAL_SRC_FILES += gralloc_drm_radeon.c
LOCAL_C_INCLUDES += external/drm/radeon
LOCAL_CFLAGS += -DENABLE_RADEON
LOCAL_SHARED_LIBRARIES += libdrm_radeon
endif # DRM_USES_RADEON

ifeq ($(strip $(DRM_USES_NOUVEAU)),true)
LOCAL_SRC_FILES += gralloc_drm_nouveau.c
LOCAL_C_INCLUDES += external/drm/nouveau
LOCAL_CFLAGS += -DENABLE_NOUVEAU
LOCAL_SHARED_LIBRARIES += libdrm_nouveau
endif # DRM_USES_NOUVEAU

ifeq ($(strip $(DRM_USES_PIPE)),true)
LOCAL_SRC_FILES += gralloc_drm_pipe.c
LOCAL_CFLAGS += -DENABLE_PIPE
LOCAL_C_INCLUDES += \
	external/mesa/src/gallium/include \
	external/mesa/src/gallium/winsys \
	external/mesa/src/gallium/drivers \
	external/mesa/src/gallium/auxiliary

ifeq ($(strip $(BOARD_USES_R600G)),true)
LOCAL_CFLAGS += -DENABLE_PIPE_R600
LOCAL_STATIC_LIBRARIES += \
	libmesa_pipe_r600 \
	libmesa_winsys_r600
endif

LOCAL_STATIC_LIBRARIES += \
	libmesa_gallium
LOCAL_SHARED_LIBRARIES += libdl
endif # DRM_USES_PIPE

LOCAL_MODULE := gralloc.$(TARGET_PRODUCT)
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw

include $(BUILD_SHARED_LIBRARY)

endif # BOARD_USES_DRM
