/*
 * VBD Writeback Test Program
 * Auto-run all VBD 3-layer composite writeback tests
 * Simulates Intel FPGA EM2130 platform with multiple overlay layers
 * Captures writeback data for validation
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <poll.h>

#include "xf86drm.h"
#include "xf86drmMode.h"
#include "drm_fourcc.h"
#include "libdrm_macros.h"

#include "util/common.h"
#include "util/format.h"
#include "util/kms.h"
#include "util/pattern.h"

#include "bo.h"

/* Test dimensions */
#define TEST_WIDTH	3840
#define TEST_HEIGHT	2160

/* Plane IDs from verification report */
#define PLANE_ID_0	59
#define PLANE_ID_1	64
#define PLANE_ID_2	69

/* User mode for custom display mode */
static drmModeModeInfo user_mode;

/* Test case structure */
struct test_case {
	const char *name;
	const char *format_str;
	const char *modifier;
	const char *resolution;  /* Resolution string like "3840x2160" */
	uint32_t crc32;          /* crc32 of the writeback data*/
	uint32_t fourcc;
	uint64_t wbc_mod;
	uint32_t plane_format[3];  /* DRM format for each of 3 layers */
	uint64_t plane_modifier[3];  /* Modifier for each of 3 layers */
	const char *file_pattern[3];
};

/* Test cases array - ordered by block size: 16x16 -> 32x8 -> 64x4 -> Raster */
static const struct test_case test_cases[] = {
	/* AFBC 16x16 block size */
	/* Test case 1: YUV420 AFBC 16x16 */
	{
		.name = "YUV420 AFBC 16x16",
		.format_str = "YU08",
		.modifier = "afbc16x16",
		.resolution = "3840x2160",
		.fourcc = DRM_FORMAT_YUV420_8BIT,
		.wbc_mod = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16),
		.plane_format = {DRM_FORMAT_YUV420_8BIT, DRM_FORMAT_YUV420_8BIT, DRM_FORMAT_R8},
		.plane_modifier = {DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0),
				   DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0),
				   0},
		.file_pattern = {"res/3840x2160_y420_bin", "res/3840x2160_y420_bin", "res/3840x2160_y420_bin"},
		.crc32 = 0xfb691f37
	},
	/* Test case 2: YUV422 AFBC 16x16 */
	{
		.name = "YUV422 AFBC 16x16",
		.format_str = "YUYV",
		.modifier = "afbc16x16",
		.resolution = "3840x2160",
		.fourcc = DRM_FORMAT_YUYV,
		.wbc_mod = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16),
		.plane_format = {DRM_FORMAT_YUYV, DRM_FORMAT_YUYV, DRM_FORMAT_R8},
		.plane_modifier = {DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0),
				   DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0),
			           0},
		.file_pattern = {"res/3840x2160_y422_bin", "res/3840x2160_y422_bin", "res/3840x2160_y420_bin"},
		.crc32 = 0x91ef58ff
	},
	/* Test case 3: YUV444 AFBC 16x16 */
	{
		.name = "YUV444 AFBC 16x16",
		.format_str = "VU24",
		.modifier = "afbc16x16",
		.resolution = "3840x2160",
		.fourcc = DRM_FORMAT_VUY888,
		.wbc_mod = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16),
		.plane_format = {DRM_FORMAT_VUY888, DRM_FORMAT_VUY888, DRM_FORMAT_R8},
		.plane_modifier = {DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0),
				   DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0),
				   0},
		.file_pattern = {"res/3840x2160_y444_bin", "res/3840x2160_y444_bin", "res/3840x2160_y420_bin"},
		.crc32 = 0x1451f82e
	},
	/* AFBC 32x8 block size */
	/* Test case 4: YUV444 AFBC 32x8 */
	{
		.name = "YUV444 AFBC 32x8",
		.format_str = "VU24",
		.modifier = "afbc32x8",
		.resolution = "3840x2160",
		.fourcc = DRM_FORMAT_VUY888,
		.wbc_mod = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8),
		.plane_format = {DRM_FORMAT_VUY888, DRM_FORMAT_VUY888, DRM_FORMAT_R8},
		.plane_modifier = {DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0),
				   DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0),
				   0},
		.file_pattern = {"res/3840x2160_y444_bin", "res/3840x2160_y444_bin", "res/3840x2160_y420_bin"},
		.crc32 = 0x8f6f2fb1
	},
	/* Test case 5: YUV422 AFBC 32x8 */
	{
		.name = "YUV422 AFBC 32x8",
		.format_str = "YUYV",
		.modifier = "afbc32x8",
		.resolution = "3840x2160",
		.fourcc = DRM_FORMAT_YUYV,
		.wbc_mod = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8),
		.plane_format = {DRM_FORMAT_YUYV, DRM_FORMAT_YUYV, DRM_FORMAT_R8},
		.plane_modifier = {DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0),
				   DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0),
				   0},
		.file_pattern = {"res/3840x2160_y422_bin", "res/3840x2160_y422_bin", "res/3840x2160_y420_bin"},
		.crc32 = 0x6382cbeb
	},
	/* Test case 6: YUV420 AFBC 32x8 */
	{
		.name = "YUV420 AFBC 32x8",
		.format_str = "YU08",
		.modifier = "afbc32x8",
		.resolution = "3840x2160",
		.fourcc = DRM_FORMAT_YUV420_8BIT,
		.wbc_mod = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8),
		.plane_format = {DRM_FORMAT_YUV420_8BIT, DRM_FORMAT_YUV420_8BIT, DRM_FORMAT_R8},
		.plane_modifier = {DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0),
				   DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0),
				   0},
		.file_pattern = {"res/3840x2160_y420_bin", "res/3840x2160_y420_bin", "res/3840x2160_y420_bin"},
		.crc32 = 0xcb9a33c6
	},
	/* RFBC 64x4 block size */
	/* Test case 7: YUV444 RFBC 64x4 */
	{
		.name = "YUV444 RFBC 64x4",
		.format_str = "VU24",
		.modifier = "rfbc64x4",
		.resolution = "3840x2160",
		.fourcc = DRM_FORMAT_VUY888,
		.wbc_mod = DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
		.plane_format = {DRM_FORMAT_VUY888, DRM_FORMAT_VUY888, DRM_FORMAT_R8},
		.plane_modifier = {DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
				   DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
			           0},
		.file_pattern = {"res/3840x2160_y444_bin", "res/3840x2160_y444_bin", "res/3840x2160_y420_bin"},
		.crc32 = 0x38212575
	},
	/* Test case 8: YUV422 RFBC 64x4 */
	{
		.name = "YUV422 RFBC 64x4",
		.format_str = "YUYV",
		.modifier = "rfbc64x4",
		.resolution = "3840x2160",
		.fourcc = DRM_FORMAT_YUYV,
		.wbc_mod = DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
		.plane_format = {DRM_FORMAT_YUYV, DRM_FORMAT_YUYV, DRM_FORMAT_R8},
		.plane_modifier = {DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
				   DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
			           0},
		.file_pattern = {"res/3840x2160_y422_bin", "res/3840x2160_y422_bin", "res/3840x2160_y420_bin"},
		.crc32 = 0xa41e0474

	},
	/* Test case 9: YUV420 RFBC 64x4 */
	{
		.name = "YUV420 RFBC 64x4",
		.format_str = "YU08",
		.modifier = "rfbc64x4",
		.resolution = "3840x2160",
		.fourcc = DRM_FORMAT_YUV420_8BIT,
		.wbc_mod = DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
		.plane_format = {DRM_FORMAT_YUV420_8BIT, DRM_FORMAT_YUV420_8BIT, DRM_FORMAT_R8},
		.plane_modifier = {DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
				   DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
			           0},
		.file_pattern = {"res/3840x2160_y420_bin", "res/3840x2160_y420_bin", "res/3840x2160_y420_bin"},
		.crc32 = 0xc17a82d9
	},
	/* Raster (uncompressed) */
	/* Test case 10: YUV444 Raster */
	{
		.name = "YUV444 Raster",
		.format_str = "NV24",
		.modifier = NULL,
		.resolution = "3840x2160",
		.fourcc = DRM_FORMAT_NV24,
		.wbc_mod = 0,
		.plane_format = {DRM_FORMAT_VUY888, DRM_FORMAT_VUY888, DRM_FORMAT_R8},
		.plane_modifier = {DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
				   DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
			           0},
		.file_pattern = {"res/3840x2160_y444_bin", "res/3840x2160_y444_bin", "res/3840x2160_y420_bin"},
		.crc32 = 0x3acf9b38
	},
	/* Test case 11: YUV422 Raster */
	{
		.name = "YUV422 Raster",
		.format_str = "NV16",
		.modifier = NULL,
		.resolution = "3840x2160",
		.fourcc = DRM_FORMAT_NV16,
		.wbc_mod = 0,
		.plane_format = {DRM_FORMAT_YUYV, DRM_FORMAT_YUYV, DRM_FORMAT_R8},
		.plane_modifier = {DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
				   DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
			           0},
		.file_pattern = {"res/3840x2160_y422_bin", "res/3840x2160_y422_bin", "res/3840x2160_y420_bin"},
		.crc32 = 0x6fb91620
	},
	/* Test case 12: YUV420 Raster */
	{
		.name = "YUV420 Raster",
		.format_str = "NV12",
		.modifier = NULL,
		.resolution = "3840x2160",
		.fourcc = DRM_FORMAT_NV12,
		.wbc_mod = 0,
		.plane_format = {DRM_FORMAT_YUV420_8BIT, DRM_FORMAT_YUV420_8BIT, DRM_FORMAT_R8},
		.plane_modifier = {DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
				   DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4),
			           0},
		.file_pattern = {"res/3840x2160_y420_bin", "res/3840x2160_y420_bin", "res/3840x2160_y420_bin"},
		.crc32 = 0x2ed50e1a
	}
};

/* Device structure for tracking resources */
struct device {
	int fd;
	drmModeResPtr resources;
	drmModeAtomicReqPtr req;
	uint32_t crtc_id;
	uint32_t main_conn_id;
	uint32_t wb_conn_id;
	drmModeModeInfoPtr mode;
	int writeback_fence_fd;
};

/* Property lookup helper */
static uint32_t get_property_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name)
{
	drmModeObjectPropertiesPtr props;
	uint32_t i;
	uint32_t prop_id = 0;

	props = drmModeObjectGetProperties(fd, obj_id, obj_type);
	if (!props)
		return 0;

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
		if (prop && strcmp(prop->name, name) == 0) {
			prop_id = props->props[i];
			drmModeFreeProperty(prop);
			break;
		}
		if (prop)
			drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);
	return prop_id;
}

/* Add property to atomic request */
static int add_property(struct device *dev, uint32_t obj_id,
		     const char *name, uint64_t value)
{
	uint32_t prop_id;
	int ret;

	prop_id = get_property_id(dev->fd, obj_id,
				  (obj_id == dev->crtc_id) ? DRM_MODE_OBJECT_CRTC :
				  (obj_id == dev->main_conn_id || obj_id == dev->wb_conn_id) ?
				  DRM_MODE_OBJECT_CONNECTOR : DRM_MODE_OBJECT_PLANE,
				  name);
	if (!prop_id) {
		fprintf(stderr, "Failed to find property %s\n", name);
		return -1;
	}

	ret = drmModeAtomicAddProperty(dev->req, obj_id, prop_id, value);
	if (ret < 0) {
		fprintf(stderr, "Failed to add property %s: %s\n", name, strerror(-ret));
		return -1;
	}

	return 0;
}

/* Initialize device and find resources */
static int init_device(struct device *dev)
{
	int i;

	dev->resources = drmModeGetResources(dev->fd);
	if (!dev->resources) {
		fprintf(stderr, "Failed to get DRM resources\n");
		return -1;
	}

	printf("Found %u connectors, %u encoders, %u crtcs\n",
	       dev->resources->count_connectors,
	       dev->resources->count_encoders,
	       dev->resources->count_crtcs);

	/* Find main connector (non-writeback) */
	for (i = 0; i < (int)dev->resources->count_connectors; i++) {
		drmModeConnectorPtr conn;
		conn = drmModeGetConnector(dev->fd, dev->resources->connectors[i]);
		if (conn) {
			if (conn->connector_type != DRM_MODE_CONNECTOR_WRITEBACK &&
			    conn->connection == DRM_MODE_CONNECTED) {
				dev->main_conn_id = dev->resources->connectors[i];
				dev->mode = &conn->modes[0];
				printf("Main connector: %u (%dx%d@%dHz)\n",
				       dev->main_conn_id,
				       conn->modes[0].hdisplay,
				       conn->modes[0].vdisplay,
				       conn->modes[0].vrefresh);
				drmModeFreeConnector(conn);
				break;
			}
			drmModeFreeConnector(conn);
		}
	}

	/* Find writeback connector */
	for (i = 0; i < (int)dev->resources->count_connectors; i++) {
		drmModeConnectorPtr conn;
		conn = drmModeGetConnector(dev->fd, dev->resources->connectors[i]);
		if (conn) {
			if (conn->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
				dev->wb_conn_id = dev->resources->connectors[i];
				printf("Writeback connector: %u\n", dev->wb_conn_id);
				drmModeFreeConnector(conn);
				break;
			}
			drmModeFreeConnector(conn);
		}
	}

	/* Use first available CRTC */
	if (dev->resources->count_crtcs > 0) {
		dev->crtc_id = dev->resources->crtcs[0];
		printf("Using CRTC: %u\n", dev->crtc_id);
	}

	if (!dev->main_conn_id || !dev->wb_conn_id) {
		fprintf(stderr, "Failed to find required connectors\n");
		return -1;
	}

	dev->req = drmModeAtomicAlloc();
	if (!dev->req) {
		fprintf(stderr, "Failed to allocate atomic request\n");
		return -1;
	}

	dev->writeback_fence_fd = -1;

	return 0;
}

/* Cleanup device resources */
static void cleanup_device(struct device *dev)
{
	if (dev->req)
		drmModeAtomicFree(dev->req);
	if (dev->resources)
		drmModeFreeResources(dev->resources);
}

/* Get number of planes for a given format - based on ovltest implementation */
static int get_plane_num(uint32_t fourcc)
{
	switch (fourcc) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
	case DRM_FORMAT_NV24:
	case DRM_FORMAT_NV42:
	case DRM_FORMAT_NV12_10:
	case DRM_FORMAT_NV15:
	case DRM_FORMAT_NV20:
	case DRM_FORMAT_NV30:
		return 2;
	default:
		return 1;
	}
}

/* Create custom display mode from resolution string */
static int create_custom_mode(drmModeModeInfo *mode, const char *resolution,
			     float vrefresh)
{
	uint32_t hdisplay, vdisplay;
	uint32_t hblank, vblank;
	uint32_t hfront_porch, hsync;
	uint32_t vfront_porch, vsync;
	float refresh;

	/* Parse resolution string like "3840x2160" */
	if (sscanf(resolution, "%ux%u", &hdisplay, &vdisplay) != 2)
		return -EINVAL;

	/* Calculate blanking values based on typical timing parameters */
	hblank = hdisplay * 5 / 100; /* 5% horizontal blanking */
	if (hblank < 88)
		hblank = 88; /* Minimum H-blank */
	if (hblank > 200)
		hblank = 200; /* Maximum H-blank */

	vblank = vdisplay * 5 / 100; /* 5% vertical blanking */
	if (vblank < 6)
		vblank = 6; /* Minimum V-blank */
	if (vblank > 30)
		vblank = 30; /* Maximum V-blank */

	/* H-sync timings: front porch = hblank/4, sync = hblank/2 */
	hfront_porch = hblank / 4;
	hsync = hblank / 2;

	/* V-sync timings: front porch = vblank/4, sync = vblank/2 */
	vfront_porch = vblank / 4;
	vsync = vblank / 2;

	/* Fill mode structure */
	memset(mode, 0, sizeof(*mode));

	mode->hdisplay = hdisplay;
	mode->hsync_start = hdisplay + hfront_porch;
	mode->hsync_end = hdisplay + hfront_porch + hsync;
	mode->htotal = hdisplay + hblank;

	mode->vdisplay = vdisplay;
	mode->vsync_start = vdisplay + vfront_porch;
	mode->vsync_end = vdisplay + vfront_porch + vsync;
	mode->vtotal = vdisplay + vblank;

	/* Calculate clock for specified refresh rate (default 60Hz if not specified) */
	refresh = (vrefresh > 0) ? vrefresh : 60.0f;
	mode->clock = (uint32_t)((mode->htotal * mode->vtotal * (unsigned int)refresh) / 1000);
	mode->vrefresh = (uint16_t)((unsigned int)refresh);

	/* Set mode name */
	snprintf(mode->name, sizeof(mode->name), "%dx%d@%d", hdisplay, vdisplay, mode->vrefresh);

	/* Set standard mode flags */
	mode->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
	mode->type = DRM_MODE_TYPE_USERDEF;

	return 0;
}

/* Create writeback framebuffer */
static int create_wb_fb(struct device *dev, const struct test_case *test,
		      uint32_t *fb_id, struct bo **wb_bo_out)
{
	uint32_t handles[4] = {0};
	uint32_t pitches[4] = {0};
	uint32_t offsets[4] = {0};
	uint64_t modifiers[4] = {0};
	int ret;
	int num_planes;
	struct bo *wb_bo;
	bool is_compressed;

	printf("Creating writeback framebuffer: %s %s\n", test->format_str,
	       test->modifier ? test->modifier : "None");

	is_compressed = (test->wbc_mod != 0);

	wb_bo = wb_bo_create(dev->fd, test->fourcc, is_compressed,
			     TEST_WIDTH, TEST_HEIGHT,
			     handles, pitches, offsets, NULL);
	if (!wb_bo) {
		fprintf(stderr, "Failed to create writeback buffer\n");
		return -1;
	}

	/* Clear writeback buffer before use */
	memset(wb_bo->ptr, 0, wb_bo->size);

	/* drmModeAddFB2WithModifiers() requires modifiers only for planes actually used by format.
	 * Setting non-zero modifier for unused planes causes kernel error. */
	num_planes = get_plane_num(test->fourcc);
	modifiers[0] = test->wbc_mod;
	if (num_planes == 2)
		modifiers[1] = test->wbc_mod;

	if (test->wbc_mod) {
		ret = drmModeAddFB2WithModifiers(dev->fd, TEST_WIDTH, TEST_HEIGHT,
						   test->fourcc,
						   handles, pitches, offsets, modifiers,
						   fb_id, DRM_MODE_FB_MODIFIERS);
	} else {
		ret = drmModeAddFB2(dev->fd, TEST_WIDTH, TEST_HEIGHT, test->fourcc,
				     handles, pitches, offsets, fb_id, 0);
	}

	if (ret) {
		fprintf(stderr, "Failed to add framebuffer: %s\n", strerror(errno));
		bo_destroy(wb_bo);
		return -1;
	}

	printf("Created WB FB ID: %u (size: %zu, pitch: %zu)\n",
		*fb_id, wb_bo->size, wb_bo->pitch);
	*wb_bo_out = wb_bo;
	return 0;
}

/* Setup overlay planes */
static int setup_planes(struct device *dev, const struct test_case *test,
		     uint32_t *fb_ids)
{
	unsigned int i;
	/* Separate arrays for each plane to avoid overwriting */
	uint32_t plane_handles[3][4] = {{0}};
	uint32_t plane_pitches[3][4] = {{0}};
	uint32_t plane_offsets[3][4] = {{0}};
	uint64_t plane_modifiers[3][4] = {{0}};
	struct bo *plane_bos[3] = {0};
	uint32_t plane_ids[3] = {PLANE_ID_0, PLANE_ID_1, PLANE_ID_2};
	int ret;
	int num_planes;

	printf("Setting up overlay planes...\n");

	/* Create plane buffers - each with separate arrays */
	for (i = 0; i < 3; i++) {
		uint32_t fourcc = test->plane_format[i];
		uint64_t modifier = test->plane_modifier[i];
		uint32_t *handles = plane_handles[i];
		uint32_t *pitches = plane_pitches[i];
		uint32_t *offsets = plane_offsets[i];
		uint64_t *modifiers = plane_modifiers[i];

		/* Alpha plane (R8) needs extra 256 bytes - handled in bo.c by increasing virtual_height */
		plane_bos[i] = wb_bo_create(dev->fd, fourcc, false,
					     TEST_WIDTH, TEST_HEIGHT,
					     handles, pitches, offsets,
					     test->file_pattern[i]);
		if (!plane_bos[i]) {
			fprintf(stderr, "Failed to create plane %u buffer\n", i);
			ret = -1;
			goto planes_cleanup;
		}

		/* Setup handles/pitches/offsets based on format */
		handles[0] = plane_bos[i]->handle;
		pitches[0] = plane_bos[i]->pitch;
		offsets[0] = 0;

		/* Ensure unused planes are zero for kernel validation */
		for (int j = 1; j < 4; j++)
			handles[j] = 0;

		/* NV15, NV24, NV12, NV16 are 2-plane formats */
		if (fourcc == DRM_FORMAT_NV24 ||
		    fourcc == DRM_FORMAT_NV12 ||
		    fourcc == DRM_FORMAT_NV16) {
			pitches[1] = pitches[0];
			offsets[1] = pitches[0] * TEST_HEIGHT;
			handles[1] = handles[0];
		}

		/* drmModeAddFB2WithModifiers() requires modifiers only for planes actually used by format */
		num_planes = get_plane_num(fourcc);
		modifiers[0] = modifier;
		if (num_planes == 2)
			modifiers[1] = modifier;
	}

	/* Create all framebuffers */
	for (i = 0; i < 3; i++) {
		uint32_t fourcc = test->plane_format[i];
		uint64_t modifier = test->plane_modifier[i];
		uint32_t *handles = plane_handles[i];
		uint32_t *pitches = plane_pitches[i];
		uint32_t *offsets = plane_offsets[i];
		uint64_t *modifiers = plane_modifiers[i];

		/* Use AddFB2WithModifiers if modifier is non-zero, otherwise use AddFB2 */
		if (modifier != 0) {
			ret = drmModeAddFB2WithModifiers(dev->fd, TEST_WIDTH, TEST_HEIGHT, fourcc,
							  handles, pitches, offsets, modifiers,
							  &fb_ids[i], DRM_MODE_FB_MODIFIERS);
		} else {
			ret = drmModeAddFB2(dev->fd, TEST_WIDTH, TEST_HEIGHT, fourcc,
					     handles, pitches, offsets, &fb_ids[i], 0);
		}

		if (ret) {
			fprintf(stderr, "Failed to add plane %u FB: %s format: %s\n",
				i, strerror(errno), util_format_info_find(fourcc)->name);
			goto planes_cleanup;
		}

		printf("Plane %u: fourcc=0x%08x, fb_id=%u\n", i, fourcc, fb_ids[i]);
	}

	/* Add plane properties to atomic request */
	for (i = 0; i < 3; i++) {
		uint32_t plane_id = plane_ids[i];
		uint32_t src_w = TEST_WIDTH << 16;
		uint32_t src_h = TEST_HEIGHT << 16;

		/* Check if plane supports needed properties */
		if (!get_property_id(dev->fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID")) {
			fprintf(stderr, "Plane %u (ID: %u) not found or missing FB_ID property\n", i, plane_id);
			goto planes_cleanup;
		}
		printf("Configuring plane %u (ID: %u, fourcc: 0x%08x)\n", i, plane_id,
		       test->plane_format[i]);

		/* Set plane FB_ID */
		if (add_property(dev, plane_id, "FB_ID", fb_ids[i]))
			goto planes_cleanup;

		/* Set plane CRTC_ID */
		if (add_property(dev, plane_id, "CRTC_ID", dev->crtc_id))
			goto planes_cleanup;

		/* Set source position and size */
		if (add_property(dev, plane_id, "SRC_X", 0))
			goto planes_cleanup;
		if (add_property(dev, plane_id, "SRC_Y", 0))
			goto planes_cleanup;
		if (add_property(dev, plane_id, "SRC_W", src_w))
			goto planes_cleanup;
		if (add_property(dev, plane_id, "SRC_H", src_h))
			goto planes_cleanup;

		/* Set CRTC position and size */
		if (add_property(dev, plane_id, "CRTC_X", 0))
			goto planes_cleanup;
		if (add_property(dev, plane_id, "CRTC_Y", 0))
			goto planes_cleanup;
		if (add_property(dev, plane_id, "CRTC_W", TEST_WIDTH))
			goto planes_cleanup;
		if (add_property(dev, plane_id, "CRTC_H", TEST_HEIGHT))
			goto planes_cleanup;

		/* Set zpos for layer ordering */
		if (add_property(dev, plane_id, "zpos", i))
			goto planes_cleanup;
	}

	ret = 0;

planes_cleanup:
	for (i = 0; i < 3; i++) {
		if (plane_bos[i])
			bo_destroy(plane_bos[i]);
	}
	return ret;
}

/* Setup display and writeback connector via atomic commit */
static int setup_display_and_wb(struct device *dev, const struct test_case *test,
			     uint32_t wb_fb_id)
{
	uint32_t blob_id = 0;
	int ret;

	printf("Setting up display and writeback...\n");

	/* List writeback connector properties */
	{
		drmModeObjectPropertiesPtr props;
		unsigned int j;
		props = drmModeObjectGetProperties(dev->fd, dev->wb_conn_id, DRM_MODE_OBJECT_CONNECTOR);
		if (props) {
			printf("Writeback connector %u properties:\n", dev->wb_conn_id);
			for (j = 0; j < props->count_props; j++) {
				drmModePropertyPtr prop = drmModeGetProperty(dev->fd, props->props[j]);
				if (prop) {
					printf("  %u: %s (id=%u)\n", j,
					       prop->name, prop->prop_id);
					drmModeFreeProperty(prop);
				}
			}
			drmModeFreeObjectProperties(props);
		}
	}

	/* Create custom mode from resolution */
	ret = create_custom_mode(&user_mode, test->resolution, 0);
	if (ret < 0) {
		fprintf(stderr, "Failed to create custom mode from \"%s\"\n", test->resolution);
		return -1;
	}

	dev->mode = &user_mode;
	printf("Using custom mode: %s (%dx%d@%dHz)\n",
	       dev->mode->name, dev->mode->hdisplay,
	       dev->mode->vdisplay, dev->mode->vrefresh);

	/* Create mode blob for CRTC */
	ret = drmModeCreatePropertyBlob(dev->fd, dev->mode, sizeof(*dev->mode), &blob_id);
	if (ret) {
		fprintf(stderr, "Failed to create mode blob: %s\n", strerror(errno));
		return -1;
	}

	/* Set CRTC properties */
	if (add_property(dev, dev->crtc_id, "MODE_ID", blob_id))
		return -1;
	if (add_property(dev, dev->crtc_id, "ACTIVE", 1))
		return -1;

	/* Set writeback connector properties */
	if (add_property(dev, dev->wb_conn_id, "WRITEBACK_FB_ID", wb_fb_id))
		return -1;
	if (add_property(dev, dev->wb_conn_id, "WRITEBACK_OUT_FENCE_PTR",
			 (uintptr_t)&dev->writeback_fence_fd))
		return -1;
	if (add_property(dev, dev->wb_conn_id, "CRTC_ID", dev->crtc_id))
		return -1;

	/* Set main connector CRTC_ID */
	if (add_property(dev, dev->main_conn_id, "CRTC_ID", dev->crtc_id))
		return -1;

	printf("Display and writeback configured\n");
	return 0;
}

/* Poll writeback fence */
static int poll_writeback_fence(int fd, int timeout)
{
	struct pollfd fds = { fd, POLLIN };
	int ret;

	if (fd < 0)
		return -EINVAL;

	do {
		ret = poll(&fds, 1, timeout);
		if (ret > 0) {
			if (fds.revents & (POLLERR | POLLNVAL))
				return -EINVAL;
			return 0;
		} else if (ret == 0) {
			return -ETIMEDOUT;
		} else {
			ret = -errno;
			if (ret == -EINTR || ret == -EAGAIN)
				continue;
			return ret;
		}
	} while (1);
}

/* CRC32 calculation */
static uint32_t calc_crc32(const void *data, size_t len)
{
	const unsigned char *p = data;
	uint32_t crc = 0xFFFFFFFF;
	static uint32_t table[256];
	static int inited = 0;
	int i;

	if (!inited) {
		for (i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int j = 0; j < 8; j++)
				c = (c >> 1) ^ (c & 1 ? 0xEDB88320 : 0);
			table[i] = c;
		}
		inited = 1;
	}

	for (i = 0; i < (int)len; i++)
		crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);

	return crc ^ 0xFFFFFFFF;
}

/* Trigger writeback capture */
static int trigger_writeback(struct device *dev, uint32_t wb_fb_id,
			  struct bo *wb_bo, const struct test_case *test)
{
	int ret, fd;
	char filename[256];
	char afbc_str[64] = "";
	uint32_t crc;
	struct timespec ts_start, ts_end;
	long elapsed_ms;

	printf("Triggering writeback...\n");

	/* Commit to trigger writeback */
	ret = drmModeAtomicCommit(dev->fd, dev->req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret) {
		fprintf(stderr, "Atomic commit failed (trigger): %s\n", strerror(errno));
		return -1;
	}

	/* Wait for writeback to complete */
	clock_gettime(CLOCK_MONOTONIC, &ts_start);
	ret = poll_writeback_fence(dev->writeback_fence_fd, 1000);
	clock_gettime(CLOCK_MONOTONIC, &ts_end);
	elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000 +
		     (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;
	if (ret) {
		fprintf(stderr, "Poll for writeback fence error: %d\n", ret);
		return -1;
	}

	/* Build AFBC/RFBC info string */
	if (test->modifier) {
		snprintf(afbc_str, sizeof(afbc_str), "_%s", test->modifier);
	}

	/* Generate output filename */
	snprintf(filename, sizeof(filename), "/data/wb_%s%s.bin",
		 test->format_str, afbc_str);
	fd = open(filename, O_WRONLY | O_TRUNC | O_CREAT, 0666);
	if (fd == -1) {
		fprintf(stderr, "Failed to open writeback file: %s\n", strerror(errno));
		return -1;
	}

	if (write(fd, wb_bo->ptr, wb_bo->size) != (ssize_t)wb_bo->size) {
		fprintf(stderr, "Failed to write writeback data: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);

	/* Calculate CRC32 checksum */
	crc = calc_crc32(wb_bo->ptr, wb_bo->size);
	printf("Writeback completed with %ld ms, saved: %s(%zu bytes), CRC32: %08x (expected: %08x)\n",
	       elapsed_ms, filename, wb_bo->size, crc, test->crc32);

	/* Compare with expected CRC */
	if (crc != test->crc32) {
		fprintf(stderr, "CRC mismatch! calculated=%08x, expected=%08x\n",
			crc, test->crc32);
		return -1;
	}

	return 0;
}

/* Run single test case */
static int run_test_case(struct device *dev, const struct test_case *test)
{
	int ret = -1;
	uint32_t wb_fb_id;
	uint32_t plane_fb_ids[3] = {0};
	struct bo *wb_bo = NULL;

	printf("\n========================================\n");
	printf("Running: %s\n", test->name);
	printf("Format: %s, Modifier: %s\n", test->format_str,
	       test->modifier ? test->modifier : "None");

	/* Create writeback framebuffer */
	if (create_wb_fb(dev, test, &wb_fb_id, &wb_bo))
		return -1;

	/* Setup planes */
	if (setup_planes(dev, test, plane_fb_ids))
		goto cleanup;

	/* Setup display and writeback via atomic */
	if (setup_display_and_wb(dev, test, wb_fb_id))
		goto cleanup;

	/* Trigger writeback */
	ret = trigger_writeback(dev, wb_fb_id, wb_bo, test);

cleanup:
	/* Cleanup plane FBs */
	for (unsigned int i = 0; i < 3; i++) {
		if (plane_fb_ids[i])
			drmModeRmFB(dev->fd, plane_fb_ids[i]);
	}

	/* Close writeback fence fd */
	if (dev->writeback_fence_fd >= 0) {
		close(dev->writeback_fence_fd);
		dev->writeback_fence_fd = -1;
	}

	/* Cleanup writeback FB and buffer */
	if (wb_bo)
		bo_destroy(wb_bo);
	if (wb_fb_id)
		drmModeRmFB(dev->fd, wb_fb_id);

	return ret;
}

int main(int argc, char **argv)
{
	struct device dev = {0};
	int i, pass = 0, fail = 0;
	int num_tests;
	unsigned int round = 0;
	const char *device = NULL;
	const char *module = NULL;

	num_tests = sizeof(test_cases) / sizeof(test_cases[0]);

	printf("VBD Writeback Test Program\n");
	printf("=============================\n");
	printf("Auto-running %d test cases\n\n", num_tests);

	/* Parse command line arguments for device/module selection */
	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-')
			continue;

		if (strcmp(argv[i], "-M") == 0)
			device = argv[++i];
		else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-m") == 0)
			module = argv[++i];
	}

	/* Open DRM device using util_open for better compatibility */
	dev.fd = util_open(device, module);
	if (dev.fd < 0) {
		fprintf(stderr, "Failed to open DRM device: %s\n", device);
		return 1;
	}

	/* drmSetClientCap() with DRM_CLIENT_CAP_ATOMIC enables atomic KMS API
	 * which allows setting all display properties in a single transaction */
	if (drmSetClientCap(dev.fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		fprintf(stderr, "Failed to enable atomic modesetting: %s\n", strerror(errno));
		close(dev.fd);
		return 1;
	}
	/* drmSetClientCap() with DRM_CLIENT_CAP_WRITEBACK_CONNECTORS exposes
	 * writeback connectors in the connector list for writeback functionality */
	if (drmSetClientCap(dev.fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1)) {
		fprintf(stderr, "Failed to enable writeback connectors: %s\n", strerror(errno));
		close(dev.fd);
		return 1;
	}

	/* Initialize device and find resources */
	if (init_device(&dev)) {
		close(dev.fd);
		return 1;
	}

	/* Run all test cases in a loop - only exit on error */
	i = 0;
	while (1) {
		if (run_test_case(&dev, &test_cases[i]) == 0) {
			pass++;
		} else {
			fail++;
			fprintf(stderr, "Test case %d (%s) failed at round %u, exiting\n",
				i, test_cases[i].name, round);
			goto cleanup;
		}

		i++;
		if (i >= num_tests) {
			i = 0;
			round++;
			printf("Completed round %u\n", round);
		}
	}

cleanup:
	cleanup_device(&dev);
	close(dev.fd);

	printf("\n========================================\n");
	printf("Test Summary\n");
	printf("============\n");
	printf("Rounds: %u, Total: %d, Pass: %d, Fail: %d\n", round, num_tests, pass, fail);

	return (fail == 0) ? 0 : 1;
}
