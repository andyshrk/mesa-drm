/*
 * DRM based mode setting test program
 * Copyright 2008 Tungsten Graphics
 *   Jakob Bornecrantz <jakob@tungstengraphics.com>
 * Copyright 2008 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "drm.h"
#include "drm_fourcc.h"

#include "libdrm_macros.h"
#include "xf86drm.h"

#include "bo.h"


/* -----------------------------------------------------------------------------
 * Buffers management
 */

static struct bo *
bo_create_dumb(int fd, unsigned int width, unsigned int height, unsigned int bpp)
{
	struct drm_mode_create_dumb arg;
	struct bo *bo;
	int ret;

	bo = calloc(1, sizeof(*bo));
	if (bo == NULL) {
		fprintf(stderr, "failed to allocate buffer object\n");
		return NULL;
	}

	memset(&arg, 0, sizeof(arg));
	arg.bpp = bpp;
	arg.width = width;
	arg.height = height;

	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &arg);
	if (ret) {
		fprintf(stderr, "failed to create dumb buffer: %s\n",
			strerror(errno));
		free(bo);
		return NULL;
	}

	bo->fd = fd;
	bo->handle = arg.handle;
	bo->size = arg.size;
	bo->pitch = arg.pitch;

	return bo;
}

static int bo_map(struct bo *bo, void **out)
{
	struct drm_mode_map_dumb arg;
	void *map;
	int ret;

	memset(&arg, 0, sizeof(arg));
	arg.handle = bo->handle;

	ret = drmIoctl(bo->fd, DRM_IOCTL_MODE_MAP_DUMB, &arg);
	if (ret)
		return ret;

	map = drm_mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       bo->fd, arg.offset);
	if (map == MAP_FAILED)
		return -EINVAL;

	bo->ptr = map;
	*out = map;

	return 0;
}

static void bo_unmap(struct bo *bo)
{
	if (!bo->ptr)
		return;

	drm_munmap(bo->ptr, bo->size);
	bo->ptr = NULL;
}

#define AFBC_HEADER_SIZE                16
#define AFBC_HDR_ALIGN                  64
#define AFBC_SUPERBLK_PIXELS            256
#define AFBC_SUPERBLK_ALIGNMENT         128

static int get_afbc_size(uint32_t width, uint32_t height, uint32_t bpp)
{
	uint32_t h_alignment = 16;
	uint32_t n_blocks;
	uint32_t hdr_size;
	uint32_t size;

	height = ALIGN(height, h_alignment);
	n_blocks = width * height / AFBC_SUPERBLK_PIXELS;
	hdr_size = ALIGN(n_blocks * AFBC_HEADER_SIZE, AFBC_HDR_ALIGN);

	size = hdr_size + n_blocks * ALIGN(bpp * AFBC_SUPERBLK_PIXELS / 8, AFBC_SUPERBLK_ALIGNMENT);

	return size;
}

struct bo *
ovl_bo_create(int fd, unsigned int format, bool is_afbc,
	  unsigned int width, unsigned int height,
	  unsigned int handles[4], unsigned int pitches[4],
	  unsigned int offsets[4], const char *pic_name)
{
	unsigned int virtual_height;
	struct bo *bo;
	unsigned int bpp;
	void *virtual;
	int ret;
	int pic_fd;
	unsigned int afbc_size;
	unsigned int i;

	switch (format) {
	case DRM_FORMAT_C8:
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
	case DRM_FORMAT_NV24:
	case DRM_FORMAT_NV42:
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YVU420:
		bpp = 8; /* this bpp is for Y channel bits when format if YUV */
		break;
	case DRM_FORMAT_NV12_10:
	case DRM_FORMAT_NV15:
	case DRM_FORMAT_NV20:
	case DRM_FORMAT_NV30:
		bpp = 10;
		break;
	case DRM_FORMAT_YUV420_8BIT:
		bpp = 12;
		break;
	case DRM_FORMAT_YUV420_10BIT:
		bpp = 15;
		break;
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_XRGB4444:
	case DRM_FORMAT_ABGR4444:
	case DRM_FORMAT_XBGR4444:
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_RGBX4444:
	case DRM_FORMAT_BGRA4444:
	case DRM_FORMAT_BGRX4444:
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ABGR1555:
	case DRM_FORMAT_XBGR1555:
	case DRM_FORMAT_RGBA5551:
	case DRM_FORMAT_RGBX5551:
	case DRM_FORMAT_BGRA5551:
	case DRM_FORMAT_BGRX5551:
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_BGR565:
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_VYUY:
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YVYU:
		bpp = 16;
		break;

	case DRM_FORMAT_BGR888:
	case DRM_FORMAT_RGB888:
		bpp = 24;
		break;
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_BGRA1010102:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_Y210:
		bpp = 32;
		break;

	case DRM_FORMAT_XRGB16161616F:
	case DRM_FORMAT_XBGR16161616F:
	case DRM_FORMAT_ARGB16161616F:
	case DRM_FORMAT_ABGR16161616F:
		bpp = 64;
		break;

	default:
		fprintf(stderr, "unsupported format 0x%08x\n",  format);
		return NULL;
	}

	switch (format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YVU420:
	case DRM_FORMAT_NV12_10:
	case DRM_FORMAT_NV15:
	case DRM_FORMAT_YUV420_8BIT:
	case DRM_FORMAT_YUV420_10BIT:
		virtual_height = height * 3 / 2;
		break;
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
	case DRM_FORMAT_NV20:
	case DRM_FORMAT_Y210:
		virtual_height = height * 2;
		break;
	case DRM_FORMAT_NV24:
	case DRM_FORMAT_NV42:
	case DRM_FORMAT_NV30:
		virtual_height = height * 3;
		break;
	default:
		virtual_height = height;
		break;
	}

	/*
	 * A afbc buffer combined with header and payload, so it
	 * may larger than the uncomressed data.
	 */
	if (is_afbc) {
		afbc_size = get_afbc_size(width, virtual_height, bpp);
		/*
		 * The calculation of the size of the afbc buffer is relatively
		 * complex:
		 * according to mesa: panfrost_create_kms_dumb_buffer_for_resource
		 * pan_image_layout_init,
		 * The calculated size will be larger than what we actually calculate here.
		 * We will first simply do an upward PAGE alignment to keep the code simpler.
		 */
		afbc_size = ALIGN(afbc_size, 4096);
		while (afbc_size > (width * virtual_height * bpp >> 3))
			virtual_height++;
	}

	bo = bo_create_dumb(fd, width, virtual_height, bpp);
	if (!bo)
		return NULL;

	ret = bo_map(bo, &virtual);
	if (ret) {
		fprintf(stderr, "failed to map buffer: %s\n",
			strerror(-errno));
		bo_destroy(bo);
		return NULL;
	}

	/* just testing a limited # of formats to test single
	 * and multi-planar path.. would be nice to add more..
	 */
	switch (format) {
	case DRM_FORMAT_UYVY:
	case DRM_FORMAT_VYUY:
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_YVYU:
	case DRM_FORMAT_YUV420_8BIT:
	case DRM_FORMAT_YUV420_10BIT:
	case DRM_FORMAT_Y210:
		offsets[0] = 0;
		handles[0] = bo->handle;
		pitches[0] = bo->pitch;

		break;

	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
	case DRM_FORMAT_NV12_10:
	case DRM_FORMAT_NV15:
	case DRM_FORMAT_NV20:
		offsets[0] = 0;
		handles[0] = bo->handle;
		pitches[0] = bo->pitch;
		pitches[1] = pitches[0];
		offsets[1] = pitches[0] * height;
		handles[1] = bo->handle;

		break;
	case DRM_FORMAT_NV24:
	case DRM_FORMAT_NV42:
	case DRM_FORMAT_NV30:
		offsets[0] = 0;
		handles[0] = bo->handle;
		pitches[0] = bo->pitch;
		pitches[1] = pitches[0] * 2;
		offsets[1] = pitches[0] * height;
		handles[1] = bo->handle;
		break;
	case DRM_FORMAT_YUV420:
	case DRM_FORMAT_YVU420:
		offsets[0] = 0;
		handles[0] = bo->handle;
		pitches[0] = bo->pitch;
		pitches[1] = pitches[0] / 2;
		offsets[1] = pitches[0] * height;
		handles[1] = bo->handle;
		pitches[2] = pitches[1];
		offsets[2] = offsets[1] + pitches[1] * height / 2;
		handles[2] = bo->handle;

		break;

	case DRM_FORMAT_C8:
	case DRM_FORMAT_ARGB4444:
	case DRM_FORMAT_XRGB4444:
	case DRM_FORMAT_ABGR4444:
	case DRM_FORMAT_XBGR4444:
	case DRM_FORMAT_RGBA4444:
	case DRM_FORMAT_RGBX4444:
	case DRM_FORMAT_BGRA4444:
	case DRM_FORMAT_BGRX4444:
	case DRM_FORMAT_ARGB1555:
	case DRM_FORMAT_XRGB1555:
	case DRM_FORMAT_ABGR1555:
	case DRM_FORMAT_XBGR1555:
	case DRM_FORMAT_RGBA5551:
	case DRM_FORMAT_RGBX5551:
	case DRM_FORMAT_BGRA5551:
	case DRM_FORMAT_BGRX5551:
	case DRM_FORMAT_RGB565:
	case DRM_FORMAT_BGR565:
	case DRM_FORMAT_BGR888:
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_BGRA1010102:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_XRGB16161616F:
	case DRM_FORMAT_XBGR16161616F:
	case DRM_FORMAT_ARGB16161616F:
	case DRM_FORMAT_ABGR16161616F:
		offsets[0] = 0;
		handles[0] = bo->handle;
		pitches[0] = bo->pitch;

		break;
	}

	/*
	 * when no pic, the buffer is for write back;
	 */
	if (pic_name) {
		pic_fd = open(pic_name, O_RDONLY);

		if (pic_fd > 0) {
			/* 
			 * take care of stride >= act_width
			 * For a afbc buffer, there maybe gap between header and playlod,
			 * This means that the payload will be scattered over an area larger
			 * than the actual linear format buffer.
			 */
			for(i = 0; i < virtual_height; i++)
				read(pic_fd, virtual + i * pitches[0], width * bpp >> 3);
			if (format == DRM_FORMAT_NV12 || format == DRM_FORMAT_NV21 || format == DRM_FORMAT_NV15) {
				for(i = 0; i < virtual_height / 2; i++)
					read(pic_fd, virtual + offsets[1] + i * pitches[1], width * bpp >> 3);
			} else if (format == DRM_FORMAT_NV16 || format == DRM_FORMAT_NV61 || format == DRM_FORMAT_NV20) {
				for(i = 0; i < virtual_height; i++)
					read(pic_fd, virtual + offsets[1] + i * pitches[1], width * bpp >> 3);
			} else if (format == DRM_FORMAT_NV24 || format == DRM_FORMAT_NV42 || format == DRM_FORMAT_NV30) {
				for(i = 0; i < virtual_height; i++)
					read(pic_fd, virtual + offsets[1] + i * pitches[1], (width * bpp >> 3) * 2);
			}
		} else {
			fprintf(stderr, "failed to open %s: %s\n", pic_name, strerror(errno));
		}

		bo_unmap(bo);
	}

	return bo;
}

void bo_destroy(struct bo *bo)
{
	struct drm_mode_destroy_dumb arg;
	int ret;

	memset(&arg, 0, sizeof(arg));
	arg.handle = bo->handle;

	ret = drmIoctl(bo->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &arg);
	if (ret)
		fprintf(stderr, "failed to destroy dumb buffer: %s\n",
			strerror(errno));

	free(bo);
}
