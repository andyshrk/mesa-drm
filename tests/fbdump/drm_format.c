#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

#include "util/common.h"
#include "drm_fourcc.h"
#include "drm_format.h"

/**
 * struct drm_format_info - information about a DRM format
 */
struct drm_format_info {
	/** @format: 4CC format identifier (DRM_FORMAT_*) */
	uint32_t format;

	/**
	 * @depth:
	 *
	 * Color depth (number of bits per pixel excluding padding bits),
	 * valid for a subset of RGB formats only. This is a legacy field, do
	 * not use in new code and set to 0 for new formats.
	 */
	uint8_t depth;

	/** @num_planes: Number of color planes (1 to 3) */
	uint8_t num_planes;

	union {
		/**
		 * @cpp:
		 *
		 * Number of bytes per pixel (per plane), this is aliased with
		 * @char_per_block. It is deprecated in favour of using the
		 * triplet @char_per_block, @block_w, @block_h for better
		 * describing the pixel format.
		 */
		uint8_t cpp[4];

		/**
		 * @char_per_block:
		 *
		 * Number of bytes per block (per plane), where blocks are
		 * defined as a rectangle of pixels which are stored next to
		 * each other in a byte aligned memory region. Together with
		 * @block_w and @block_h this is used to properly describe tiles
		 * in tiled formats or to describe groups of pixels in packed
		 * formats for which the memory needed for a single pixel is not
		 * byte aligned.
		 *
		 * @cpp has been kept for historical reasons because there are
		 * a lot of places in drivers where it's used. In drm core for
		 * generic code paths the preferred way is to use
		 * @char_per_block, drm_format_info_block_width() and
		 * drm_format_info_block_height() which allows handling both
		 * block and non-block formats in the same way.
		 *
		 * For formats that are intended to be used only with non-linear
		 * modifiers both @cpp and @char_per_block must be 0 in the
		 * generic format table. Drivers could supply accurate
		 * information from their drm_mode_config.get_format_info hook
		 * if they want the core to be validating the pitch.
		 */
		uint8_t char_per_block[4];
	};

	/**
	 * @block_w:
	 *
	 * Block width in pixels, this is intended to be accessed through
	 * drm_format_info_block_width()
	 */
	uint8_t block_w[4];

	/**
	 * @block_h:
	 *
	 * Block height in pixels, this is intended to be accessed through
	 * drm_format_info_block_height()
	 */
	uint8_t block_h[4];

	/** @hsub: Horizontal chroma subsampling factor */
	uint8_t hsub;
	/** @vsub: Vertical chroma subsampling factor */
	uint8_t vsub;

	/** @has_alpha: Does the format embeds an alpha component? */
	bool has_alpha;

	/** @is_yuv: Is it a YUV format? */
	bool is_yuv;
};

/*
 * Internal function to query information for a given format. See
 * drm_format_info() for the public API.
 */
static const struct drm_format_info *__drm_format_info(uint32_t format)
{
	static const struct drm_format_info formats[] = {
		{ .format = DRM_FORMAT_C8,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGB332,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGR233,		.depth = 8,  .num_planes = 1, .cpp = { 1, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XRGB4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XBGR4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGBX4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGRX4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_ARGB4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_ABGR4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGBA4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGRA4444,	.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_XRGB1555,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XBGR1555,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGBX5551,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGRX5551,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_ARGB1555,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_ABGR1555,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGBA5551,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGRA5551,	.depth = 15, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGB565,		.depth = 16, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGR565,		.depth = 16, .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGB888,		.depth = 24, .num_planes = 1, .cpp = { 3, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGR888,		.depth = 24, .num_planes = 1, .cpp = { 3, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XRGB8888,	.depth = 24, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XBGR8888,	.depth = 24, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGBX8888,	.depth = 24, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGRX8888,	.depth = 24, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGB565_A8,	.depth = 24, .num_planes = 2, .cpp = { 2, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGR565_A8,	.depth = 24, .num_planes = 2, .cpp = { 2, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_XRGB2101010,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XBGR2101010,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_RGBX1010102,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_BGRX1010102,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_ARGB2101010,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_ABGR2101010,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGBA1010102,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGRA1010102,	.depth = 30, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_ARGB8888,	.depth = 32, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_ABGR8888,	.depth = 32, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGBA8888,	.depth = 32, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGRA8888,	.depth = 32, .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_XRGB16161616F,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_XBGR16161616F,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1 },
		{ .format = DRM_FORMAT_ARGB16161616F,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_ABGR16161616F,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_AXBXGXRX106106106106, .depth = 0, .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGB888_A8,	.depth = 32, .num_planes = 2, .cpp = { 3, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGR888_A8,	.depth = 32, .num_planes = 2, .cpp = { 3, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_XRGB8888_A8,	.depth = 32, .num_planes = 2, .cpp = { 4, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_XBGR8888_A8,	.depth = 32, .num_planes = 2, .cpp = { 4, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_RGBX8888_A8,	.depth = 32, .num_planes = 2, .cpp = { 4, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_BGRX8888_A8,	.depth = 32, .num_planes = 2, .cpp = { 4, 1, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true },
		{ .format = DRM_FORMAT_YUV410,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 4, .is_yuv = true },
		{ .format = DRM_FORMAT_YVU410,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 4, .is_yuv = true },
		{ .format = DRM_FORMAT_YUV411,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YVU411,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 4, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YUV420,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_YVU420,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_YUV422,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YVU422,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YUV444,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YVU444,		.depth = 0,  .num_planes = 3, .cpp = { 1, 1, 1 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_NV12,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_NV21,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_NV16,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_NV61,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_NV24,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_NV42,		.depth = 0,  .num_planes = 2, .cpp = { 1, 2, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YUYV,		.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_YVYU,		.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_UYVY,		.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_VYUY,		.depth = 0,  .num_planes = 1, .cpp = { 2, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_XYUV8888,	.depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_VUY888,          .depth = 0,  .num_planes = 1, .cpp = { 3, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_AYUV,		.depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true, .is_yuv = true },
		{ .format = DRM_FORMAT_Y210,            .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_Y212,            .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_Y216,            .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 2, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_Y410,            .depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true, .is_yuv = true },
		{ .format = DRM_FORMAT_Y412,            .depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true, .is_yuv = true },
		{ .format = DRM_FORMAT_Y416,            .depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .has_alpha = true, .is_yuv = true },
		{ .format = DRM_FORMAT_XVYU2101010,	.depth = 0,  .num_planes = 1, .cpp = { 4, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_XVYU12_16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_XVYU16161616,	.depth = 0,  .num_planes = 1, .cpp = { 8, 0, 0 }, .hsub = 1, .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_Y0L0,		.depth = 0,  .num_planes = 1,
		  .char_per_block = { 8, 0, 0 }, .block_w = { 2, 0, 0 }, .block_h = { 2, 0, 0 },
		  .hsub = 2, .vsub = 2, .has_alpha = true, .is_yuv = true },
		{ .format = DRM_FORMAT_X0L0,		.depth = 0,  .num_planes = 1,
		  .char_per_block = { 8, 0, 0 }, .block_w = { 2, 0, 0 }, .block_h = { 2, 0, 0 },
		  .hsub = 2, .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_Y0L2,		.depth = 0,  .num_planes = 1,
		  .char_per_block = { 8, 0, 0 }, .block_w = { 2, 0, 0 }, .block_h = { 2, 0, 0 },
		  .hsub = 2, .vsub = 2, .has_alpha = true, .is_yuv = true },
		{ .format = DRM_FORMAT_X0L2,		.depth = 0,  .num_planes = 1,
		  .char_per_block = { 8, 0, 0 }, .block_w = { 2, 0, 0 }, .block_h = { 2, 0, 0 },
		  .hsub = 2, .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_P010,            .depth = 0,  .num_planes = 2,
		  .char_per_block = { 2, 4, 0 }, .block_w = { 1, 1, 0 }, .block_h = { 1, 1, 0 },
		  .hsub = 2, .vsub = 2, .is_yuv = true},
		{ .format = DRM_FORMAT_P012,		.depth = 0,  .num_planes = 2,
		  .char_per_block = { 2, 4, 0 }, .block_w = { 1, 1, 0 }, .block_h = { 1, 1, 0 },
		   .hsub = 2, .vsub = 2, .is_yuv = true},
		{ .format = DRM_FORMAT_P016,		.depth = 0,  .num_planes = 2,
		  .char_per_block = { 2, 4, 0 }, .block_w = { 1, 1, 0 }, .block_h = { 1, 1, 0 },
		  .hsub = 2, .vsub = 2, .is_yuv = true},
		{ .format = DRM_FORMAT_P210,		.depth = 0,
		  .num_planes = 2, .char_per_block = { 2, 4, 0 },
		  .block_w = { 1, 1, 0 }, .block_h = { 1, 1, 0 }, .hsub = 2,
		  .vsub = 1, .is_yuv = true },
		{ .format = DRM_FORMAT_VUY101010,	.depth = 0,
		  .num_planes = 1, .cpp = { 0, 0, 0 }, .hsub = 1, .vsub = 1,
		  .is_yuv = true },
		{ .format = DRM_FORMAT_YUV420_8BIT,     .depth = 0,
		  .num_planes = 1, .cpp = { 0, 0, 0 }, .hsub = 2, .vsub = 2,
		  .is_yuv = true },
		{ .format = DRM_FORMAT_YUV420_10BIT,    .depth = 0,
		  .num_planes = 1, .cpp = { 0, 0, 0 }, .hsub = 2, .vsub = 2,
		  .is_yuv = true },
		{ .format = DRM_FORMAT_NV15,		.depth = 0,
		  .num_planes = 2, .char_per_block = { 5, 5, 0 },
		  .block_w = { 4, 2, 0 }, .block_h = { 1, 1, 0 }, .hsub = 2,
		  .vsub = 2, .is_yuv = true },
		{ .format = DRM_FORMAT_Q410,		.depth = 0,
		  .num_planes = 3, .char_per_block = { 2, 2, 2 },
		  .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 }, .hsub = 0,
		  .vsub = 0, .is_yuv = true },
		{ .format = DRM_FORMAT_Q401,		.depth = 0,
		  .num_planes = 3, .char_per_block = { 2, 2, 2 },
		  .block_w = { 1, 1, 1 }, .block_h = { 1, 1, 1 }, .hsub = 0,
		  .vsub = 0, .is_yuv = true },
	};

	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(formats); ++i) {
		if (formats[i].format == format)
			return &formats[i];
	}

	return NULL;
}

/**
 * drm_format_info - query information for a given format
 * @format: pixel format (DRM_FORMAT_*)
 *
 * The caller should only pass a supported pixel format to this function.
 * Unsupported pixel formats will generate a warning in the kernel log.
 *
 * Returns:
 * The instance of struct drm_format_info that describes the pixel format, or
 * NULL if the format is unsupported.
 */
static const struct drm_format_info *drm_format_info(uint32_t format)
{
	const struct drm_format_info *info;

	info = __drm_format_info(format);
	return info;
}

/**
 * drm_format_info_block_width - width in pixels of block.
 * @info: pixel format info
 * @plane: plane index
 *
 * Returns:
 * The width in pixels of a block, depending on the plane index.
 */
static unsigned int drm_format_info_block_width(const struct drm_format_info *info,
					 int plane)
{
	if (!info || plane < 0 || plane >= info->num_planes)
		return 0;

	if (!info->block_w[plane])
		return 1;
	return info->block_w[plane];
}

/**
 * drm_format_info_block_height - height in pixels of a block
 * @info: pixel format info
 * @plane: plane index
 *
 * Returns:
 * The height in pixels of a block, depending on the plane index.
 */
static unsigned int drm_format_info_block_height(const struct drm_format_info *info,
					  int plane)
{
	if (!info || plane < 0 || plane >= info->num_planes)
		return 0;

	if (!info->block_h[plane])
		return 1;
	return info->block_h[plane];
}

/**
 *  drm_format_info_bpp - number of bits per pixel
 *  @info: pixel format info
 *  @plane: plane index
 *
 *  Returns:
 *  The actual number of bits per pixel, depending on the plane index.
 */
static unsigned int drm_format_info_bpp(const struct drm_format_info *info, int plane)
{
	if (!info || plane < 0 || plane >= info->num_planes)
		return 0;

	return info->char_per_block[plane] * 8 /
		(drm_format_info_block_width(info, plane) *
		 drm_format_info_block_height(info, plane));
}

/*
 * Rertun  the actual number of bits per pixel, depending on the plane index.
 */
uint32_t drm_get_bpp(uint32_t fmt)
{
	const struct drm_format_info *format = drm_format_info(fmt);
	switch (format->format) {
	case DRM_FORMAT_YUV420_8BIT:
		return 12;
	case DRM_FORMAT_YUV420_10BIT:
		return 15;
	case DRM_FORMAT_VUY101010:
		return 30;
	default:
		return drm_format_info_bpp(format, 0);
	}
}

#define AFBC_HEADER_SIZE                16
#define AFBC_TH_LAYOUT_ALIGNMENT        8
#define AFBC_HDR_ALIGN                  64
#define AFBC_SUPERBLOCK_PIXELS          256
#define AFBC_SUPERBLOCK_ALIGNMENT       128
#define AFBC_TH_BODY_START_ALIGNMENT    4096

int drm_gem_afbc_min_size(uint32_t fmt, uint32_t width, uint32_t height, uint64_t modifier)
{
	uint32_t n_blocks, w_alignment, h_alignment, hdr_alignment;
	uint32_t block_width = 0, block_height = 0;
	uint32_t aligned_width = 0, aligned_height = 0;
	uint32_t afbc_size;
	/* remove bpp when all users properly encode cpp in drm_format_info */
	__u32 bpp;

	switch (modifier & AFBC_FORMAT_MOD_BLOCK_SIZE_MASK) {
	case AFBC_FORMAT_MOD_BLOCK_SIZE_16x16:
		block_width = 16;
		block_height = 16;
		break;
	case AFBC_FORMAT_MOD_BLOCK_SIZE_32x8:
		block_width = 32;
		block_height = 8;
		break;
	/* no user exists yet - fall through */
	case AFBC_FORMAT_MOD_BLOCK_SIZE_64x4:
	case AFBC_FORMAT_MOD_BLOCK_SIZE_32x8_64x4:
	default:
		printf("Invalid AFBC_FORMAT_MOD_BLOCK_SIZE: %ld.\n",
		       modifier & AFBC_FORMAT_MOD_BLOCK_SIZE_MASK);
		return 0;
	}

	/* tiled header afbc */
	w_alignment = block_width;
	h_alignment = block_height;
	hdr_alignment = AFBC_HDR_ALIGN;
	if (modifier & AFBC_FORMAT_MOD_TILED) {
		w_alignment *= AFBC_TH_LAYOUT_ALIGNMENT;
		h_alignment *= AFBC_TH_LAYOUT_ALIGNMENT;
		hdr_alignment = AFBC_TH_BODY_START_ALIGNMENT;
	}

	aligned_width = ALIGN(width, w_alignment);
	aligned_height = ALIGN(height, h_alignment);

	bpp = drm_get_bpp(fmt);
	if (!bpp) {
		printf("Invalid AFBC bpp value: %d\n", bpp);
		return 0;
	}

	n_blocks = (aligned_width * aligned_height) / AFBC_SUPERBLOCK_PIXELS;
	afbc_size = ALIGN(n_blocks * AFBC_HEADER_SIZE, hdr_alignment);
	afbc_size += n_blocks * ALIGN(bpp * AFBC_SUPERBLOCK_PIXELS / 8,
					       AFBC_SUPERBLOCK_ALIGNMENT);

	return afbc_size;
}
