#ifndef __DRM_FORMAT_H
#define __DRM_FORMAT_H
#include <stdio.h>
#include "util_math.h"


/*
 * DRM modifier helper
 * from mesa3d src/panfrost/lib/pan_texture.h
 *
 * */

#define drm_is_afbc(mod)                                                       \
   ((mod >> 52) ==                                                             \
    (DRM_FORMAT_MOD_ARM_TYPE_AFBC | (DRM_FORMAT_MOD_VENDOR_ARM << 4)))

#define drm_is_afrc(mod)                                                       \
   ((mod >> 52) ==                                                             \
    (DRM_FORMAT_MOD_ARM_TYPE_AFRC | (DRM_FORMAT_MOD_VENDOR_ARM << 4)))


uint32_t drm_get_bpp(uint32_t fmt);

int drm_gem_afbc_min_size(uint32_t fmt, uint32_t widht, uint32_t height, uint64_t modifier);
#endif
