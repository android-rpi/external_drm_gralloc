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

DRM_GPU_DRIVERS := $(strip $(filter-out swrast, $(BOARD_GPU_DRIVERS)))

intel_drivers := i915 i965 i915g ilo
radeon_drivers := r300g r600g
rockchip_drivers := rockchip
nouveau_drivers := nouveau
vmwgfx_drivers := vmwgfx
vc4_drivers := vc4

valid_drivers := \
	prebuilt \
	$(intel_drivers) \
	$(radeon_drivers) \
	$(rockchip_drivers) \
	$(nouveau_drivers) \
	$(vmwgfx_drivers) \
        $(vc4_drivers)

# warn about invalid drivers
invalid_drivers := $(filter-out $(valid_drivers), $(DRM_GPU_DRIVERS))
ifneq ($(invalid_drivers),)
$(warning invalid GPU drivers: $(invalid_drivers))
# tidy up
DRM_GPU_DRIVERS := $(filter-out $(invalid_drivers), $(DRM_GPU_DRIVERS))
endif

ifneq ($(filter $(vmwgfx_drivers) $(vc4_drivers), $(DRM_GPU_DRIVERS)),)
DRM_USES_PIPE := true
else
DRM_USES_PIPE := false
endif

ifneq ($(strip $(DRM_GPU_DRIVERS)),)

LOCAL_PATH := $(call my-dir)


# Use the PREBUILT libraries
ifeq ($(strip $(DRM_GPU_DRIVERS)),prebuilt)

include $(CLEAR_VARS)
LOCAL_MODULE := libgralloc_drm
LOCAL_MODULE_TAGS := optional
LOCAL_SRC_FILES := ../../$(BOARD_GPU_DRIVER_BINARY)
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := $(TARGET_SHLIB_SUFFIX)
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := gralloc.$(TARGET_PRODUCT)
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_SRC_FILES := ../../$(BOARD_GPU_DRIVER_BINARY)
LOCAL_MODULE_CLASS := SHARED_LIBRARIES
LOCAL_MODULE_SUFFIX := $(TARGET_SHLIB_SUFFIX)
include $(BUILD_PREBUILT)

# Use the sources
else

include $(CLEAR_VARS)
LOCAL_MODULE := libgralloc_drm
LOCAL_MODULE_TAGS := optional
LOCAL_PROPRIETARY_MODULE := true

LOCAL_SRC_FILES := \
	gralloc_drm.c

LOCAL_C_INCLUDES := \
	external/libdrm \
	external/libdrm/include/drm

LOCAL_SHARED_LIBRARIES := \
	libdrm \
	liblog \
	libcutils \
	libutils \
        libexpat \
        libz

ifneq ($(filter $(intel_drivers), $(DRM_GPU_DRIVERS)),)
LOCAL_SRC_FILES += gralloc_drm_intel.c
LOCAL_C_INCLUDES += external/libdrm/intel
LOCAL_CFLAGS += -DENABLE_INTEL
LOCAL_SHARED_LIBRARIES += libdrm_intel
endif

ifneq ($(filter $(radeon_drivers), $(DRM_GPU_DRIVERS)),)
LOCAL_SRC_FILES += gralloc_drm_radeon.c
LOCAL_C_INCLUDES += external/libdrm/radeon
LOCAL_CFLAGS += -DENABLE_RADEON
LOCAL_SHARED_LIBRARIES += libdrm_radeon
endif

ifneq ($(filter $(nouveau_drivers), $(DRM_GPU_DRIVERS)),)
LOCAL_SRC_FILES += gralloc_drm_nouveau.c
LOCAL_C_INCLUDES += external/libdrm/nouveau
LOCAL_CFLAGS += -DENABLE_NOUVEAU
LOCAL_SHARED_LIBRARIES += libdrm_nouveau
endif

ifneq ($(filter $(rockchip_drivers), $(DRM_GPU_DRIVERS)),)
LOCAL_SRC_FILES += gralloc_drm_rockchip.c
LOCAL_CFLAGS += -DENABLE_ROCKCHIP
LOCAL_SHARED_LIBRARIES += libdrm_rockchip
endif

ifeq ($(strip $(DRM_USES_PIPE)),true)
LOCAL_SRC_FILES += gralloc_drm_pipe.c
LOCAL_CFLAGS += -DENABLE_PIPE
LOCAL_C_INCLUDES += \
	external/mesa3d/include \
	external/mesa3d/src \
	external/mesa3d/src/gallium \
	external/mesa3d/src/gallium/include \
	external/mesa3d/src/gallium/winsys \
	external/mesa3d/src/gallium/drivers \
	external/mesa3d/src/gallium/auxiliary

ifneq ($(filter r600g, $(DRM_GPU_DRIVERS)),)
LOCAL_CFLAGS += -DENABLE_PIPE_R600
LOCAL_STATIC_LIBRARIES += \
	libmesa_pipe_r600 \
	libmesa_pipe_radeon \
	libmesa_winsys_radeon
endif
ifneq ($(filter vmwgfx, $(DRM_GPU_DRIVERS)),)
LOCAL_CFLAGS += -DENABLE_PIPE_VMWGFX
LOCAL_STATIC_LIBRARIES += \
	libmesa_pipe_svga \
	libmesa_winsys_svga
LOCAL_C_INCLUDES += \
	external/mesa/src/gallium/drivers/svga/include
endif

ifneq ($(filter vc4, $(DRM_GPU_DRIVERS)),)
LOCAL_CFLAGS += -DENABLE_PIPE_VC4
LOCAL_STATIC_LIBRARIES += \
	libmesa_winsys_vc4 \
	libmesa_pipe_vc4
endif

LOCAL_STATIC_LIBRARIES += \
	libmesa_gallium \
	libmesa_glsl \
	libmesa_glsl_utils \
        libmesa_nir \
	libmesa_util \
        libmesa_compiler

LOCAL_SHARED_LIBRARIES += libdl
endif # DRM_USES_PIPE

LOCAL_CFLAGS += \
	-Wno-unused-variable \
	-Wno-unused-parameter

include $(BUILD_SHARED_LIBRARY)


include $(CLEAR_VARS)
LOCAL_SRC_FILES := \
	gralloc.cpp

LOCAL_C_INCLUDES := \
	external/libdrm \
	external/libdrm/include/drm

LOCAL_SHARED_LIBRARIES := \
	libgralloc_drm \
	liblog \
	libutils

# for glFlush/glFinish
LOCAL_SHARED_LIBRARIES += \
	libGLESv1_CM

LOCAL_MODULE := gralloc.drm
LOCAL_MODULE_TAGS := optional
LOCAL_MODULE_RELATIVE_PATH := hw

LOCAL_CFLAGS += \
	-Wno-unused-variable \
	-Wno-unused-parameter

include $(BUILD_SHARED_LIBRARY)

endif # DRM_GPU_DRIVERS=prebuilt
endif # DRM_GPU_DRIVERS
