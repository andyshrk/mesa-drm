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
#include <pthread.h>

#include "xf86drm.h"
#include "xf86drmMode.h"
#include "drm_fourcc.h"
#include "libdrm_macros.h"

#include "util/common.h"
#include "util/format.h"
#include "util/kms.h"
#include "util/pattern.h"

#include "bo.h"
#include "wb_props.h"

/* Plane IDs from verification report */
#define PLANE_ID_0	59
#define PLANE_ID_1	64
#define PLANE_ID_2	69

/* User mode for custom display mode */
static drmModeModeInfo user_mode;

#include "test_case_table.h"


/* Device structure for tracking resources */
struct device {
	int fd;
	drmModeResPtr resources;
	drmModeAtomicReqPtr req;
	uint32_t crtc_id;
	uint32_t main_conn_id;
	uint32_t wb_conn_id;
	drmModeModeInfoPtr mode;
	uint32_t mode_blob_id;
	int writeback_fence_fd;
	int write_file;

	/* Property caches (populated once at init) */
	struct wb_crtc_props crtc_props;
	struct wb_connector_props main_conn_props;
	struct wb_connector_props wb_conn_props;
	struct wb_plane_props plane_props[3];
};

/* Initialize property caches for all DRM objects */
static int wb_drm_props_init(struct device *dev)
{
	unsigned int i;
	uint32_t plane_ids[3] = {PLANE_ID_0, PLANE_ID_1, PLANE_ID_2};

	/* CRTC properties */
	dev->crtc_props.crtc_id = dev->crtc_id;
	wb_crtc_props_populate(dev->fd, &dev->crtc_props);

	/* Main connector properties */
	dev->main_conn_props.connector_id = dev->main_conn_id;
	wb_connector_props_populate(dev->fd, &dev->main_conn_props);

	/* Writeback connector properties */
	dev->wb_conn_props.connector_id = dev->wb_conn_id;
	wb_connector_props_populate(dev->fd, &dev->wb_conn_props);

	/* Plane properties */
	for (i = 0; i < 3; i++) {
		dev->plane_props[i].plane_id = plane_ids[i];
		wb_plane_props_populate(dev->fd, &dev->plane_props[i]);
	}

	/* Verify plane 0 has FB_ID (basic sanity check) */
	if (dev->plane_props[0].props[WB_PLANE_FB_ID].prop_id == 0) {
		fprintf(stderr, "Plane %u missing FB_ID property\n", plane_ids[0]);
		return -1;
	}

	return 0;
}

/* Initialize device and find resources */
static int wb_drm_device_init(struct device *dev)
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

/* Parse resolution string "WxH" into width and height */
static int parse_resolution(const char *resolution, uint32_t *width, uint32_t *height)
{
	if (sscanf(resolution, "%ux%u", width, height) != 2)
		return -EINVAL;
	return 0;
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
	uint32_t width, height;

	printf("Creating writeback framebuffer: %s Modifier: %s\n", test->format_str,
	       test->modifier ? test->modifier : "None");

	if (parse_resolution(test->resolution, &width, &height)) {
		fprintf(stderr, "Invalid resolution: %s\n", test->resolution);
		return -1;
	}

	is_compressed = (test->wbc_mod != 0);

	wb_bo = wb_bo_create(dev->fd, test->fourcc, is_compressed,
			     width, height,
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
		ret = drmModeAddFB2WithModifiers(dev->fd, width, height,
						   test->fourcc,
						   handles, pitches, offsets, modifiers,
						   fb_id, DRM_MODE_FB_MODIFIERS);
	} else {
		ret = drmModeAddFB2(dev->fd, width, height, test->fourcc,
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
	int ret;
	int num_planes;
	uint32_t width, height;

	printf("Setting up overlay planes...\n");

	if (parse_resolution(test->resolution, &width, &height)) {
		fprintf(stderr, "Invalid resolution: %s\n", test->resolution);
		return -1;
	}

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
					     width, height,
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
			offsets[1] = pitches[0] * height;
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
			ret = drmModeAddFB2WithModifiers(dev->fd, width, height, fourcc,
							  handles, pitches, offsets, modifiers,
							  &fb_ids[i], DRM_MODE_FB_MODIFIERS);
		} else {
			ret = drmModeAddFB2(dev->fd, width, height, fourcc,
					     handles, pitches, offsets, &fb_ids[i], 0);
		}

		if (ret) {
			fprintf(stderr, "Failed to add plane %u FB: %s format: %s\n",
				i, strerror(errno), util_format_info_find(fourcc)->name);
			goto planes_cleanup;
		}

		//printf("Plane %u: fourcc=0x%08x, fb_id=%u\n", i, fourcc, fb_ids[i]);
	}

	/* Add plane properties to atomic request */
	for (i = 0; i < 3; i++) {
		struct wb_plane_props *pp = &dev->plane_props[i];
		uint32_t src_w = width << 16;
		uint32_t src_h = height << 16;

		//printf("Configuring plane %u (ID: %u, fourcc: 0x%08x)\n", i,
		 //      pp->plane_id, test->plane_format[i]);

		if (wb_plane_prop_add(dev->req, pp, WB_PLANE_FB_ID, fb_ids[i]))
			goto planes_cleanup;
		if (wb_plane_prop_add(dev->req, pp, WB_PLANE_CRTC_ID, dev->crtc_id))
			goto planes_cleanup;
		if (wb_plane_prop_add(dev->req, pp, WB_PLANE_SRC_X, 0))
			goto planes_cleanup;
		if (wb_plane_prop_add(dev->req, pp, WB_PLANE_SRC_Y, 0))
			goto planes_cleanup;
		if (wb_plane_prop_add(dev->req, pp, WB_PLANE_SRC_W, src_w))
			goto planes_cleanup;
		if (wb_plane_prop_add(dev->req, pp, WB_PLANE_SRC_H, src_h))
			goto planes_cleanup;
		if (wb_plane_prop_add(dev->req, pp, WB_PLANE_CRTC_X, 0))
			goto planes_cleanup;
		if (wb_plane_prop_add(dev->req, pp, WB_PLANE_CRTC_Y, 0))
			goto planes_cleanup;
		if (wb_plane_prop_add(dev->req, pp, WB_PLANE_CRTC_W, width))
			goto planes_cleanup;
		if (wb_plane_prop_add(dev->req, pp, WB_PLANE_CRTC_H, height))
			goto planes_cleanup;
		if (wb_plane_prop_add(dev->req, pp, WB_PLANE_ZPOS, i))
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
	int ret;

	printf("Setting up display and writeback...\n");

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
	ret = drmModeCreatePropertyBlob(dev->fd, dev->mode, sizeof(*dev->mode), &dev->mode_blob_id);
	if (ret) {
		fprintf(stderr, "Failed to create mode blob: %s\n", strerror(errno));
		return -1;
	}

	/* Set CRTC properties */
	if (wb_crtc_prop_add(dev->req, &dev->crtc_props, WB_CRTC_MODE_ID, dev->mode_blob_id))
		return -1;
	if (wb_crtc_prop_add(dev->req, &dev->crtc_props, WB_CRTC_ACTIVE, 1))
		return -1;

	/* Set writeback connector properties */
	if (wb_connector_prop_add(dev->req, &dev->wb_conn_props, WB_CONNECTOR_WRITEBACK_FB_ID, wb_fb_id))
		return -1;
	if (wb_connector_prop_add(dev->req, &dev->wb_conn_props, WB_CONNECTOR_WRITEBACK_OUT_FENCE_PTR,
				  (uintptr_t)&dev->writeback_fence_fd))
		return -1;
	if (wb_connector_prop_add(dev->req, &dev->wb_conn_props, WB_CONNECTOR_CRTC_ID, dev->crtc_id))
		return -1;

	/* Set main connector CRTC_ID */
	if (wb_connector_prop_add(dev->req, &dev->main_conn_props, WB_CONNECTOR_CRTC_ID, dev->crtc_id))
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

/* CRC32 calculation (thread-safe) */
static uint32_t crc32_table[256];
static pthread_once_t crc32_once = PTHREAD_ONCE_INIT;

static void crc32_init_table(void)
{
	for (int i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int j = 0; j < 8; j++)
			c = (c >> 1) ^ (c & 1 ? 0xEDB88320 : 0);
		crc32_table[i] = c;
	}
}

static uint32_t calc_crc32(const void *data, size_t len)
{
	const unsigned char *p = data;
	uint32_t crc = 0xFFFFFFFF;
	int i;

	pthread_once(&crc32_once, crc32_init_table);

	for (i = 0; i < (int)len; i++)
		crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);

	return crc ^ 0xFFFFFFFF;
}

/* Trigger writeback capture */
struct wb_result {
	char name[128];
	int pass;
	uint32_t crc;
	uint32_t expected_crc;
};

struct wb_io_work {
	struct bo *wb_bo;
	char filename[256];
	uint32_t expected_crc;
	char name[128];
	struct wb_result *result;
	pthread_t thread;
	struct device *dev;
};

#define IO_CONCURRENCY 64

static void *wb_io_thread(void *arg)
{
	struct wb_io_work *work = arg;
	uint32_t crc;

	/* Write data to file */
	if (work->dev->write_file) {
		int fd = open(work->filename, O_WRONLY | O_TRUNC | O_CREAT, 0666);
		if (fd >= 0) {
			write(fd, work->wb_bo->ptr, work->wb_bo->size);
			close(fd);
		}
	}

	/* Calculate CRC32 */
	crc = calc_crc32(work->wb_bo->ptr, work->wb_bo->size);

	/* Store result */
	snprintf(work->result->name, sizeof(work->result->name), "%s", work->name);
	work->result->crc = crc;
	work->result->expected_crc = work->expected_crc;
	work->result->pass = (!work->expected_crc || crc == work->expected_crc) ? 1 : 0;

	if (work->dev->write_file)
		printf("  IO done: %s CRC32: %08x (expected: %08x) %s\n",
		       work->filename, crc, work->expected_crc,
		       work->result->pass ? "[PASS]" : "[FAIL]");
	else
		printf("  %s CRC32: %08x (expected: %08x) %s\n",
		       work->name, crc, work->expected_crc,
		       work->result->pass ? "[PASS]" : "[FAIL]");

	bo_destroy(work->wb_bo);
	return NULL;
}

static int trigger_writeback(struct device *dev)
{
	int ret;
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
		fprintf(stderr, "Poll for writeback fence timeout: %d, elapase: %ld ms\n", ret, elapsed_ms);
		return -1;
	}

	printf("Writeback completed in %ld ms\n", elapsed_ms);
	return 0;
}

static struct wb_io_work *spawn_io_thread(struct bo *wb_bo,
					  const struct test_case *test,
					  int test_idx,
					  struct wb_result *result,
					  struct device *dev)
{
	char afbc_str[64] = "";
	struct wb_io_work *work;

	if (test->modifier)
		snprintf(afbc_str, sizeof(afbc_str), "_%s", test->modifier);

	work = calloc(1, sizeof(*work));
	work->wb_bo = wb_bo;
	work->expected_crc = test->crc32;
	work->result = result;
	work->dev = dev;
	snprintf(work->filename, sizeof(work->filename), "/data/wb_%s_%s%s_%03d.bin",
		 test->format_str, test->resolution, afbc_str, test_idx);
	snprintf(work->name, sizeof(work->name), "%s", test->name);
	pthread_create(&work->thread, NULL, wb_io_thread, work);
	return work;
}

/* Run single test case */
static int run_test_case(struct device *dev, const struct test_case *test,
		       int test_idx, struct wb_io_work **work_out,
		       struct wb_result *result)
{
	int ret = -1;
	uint32_t wb_fb_id;
	uint32_t plane_fb_ids[3] = {0};
	struct bo *wb_bo = NULL;

	printf("\n========================================\n");
	printf("Running: %s\n", test->name);
	printf("Format: %s, Modifier: %s\n", test->format_str,
	       test->modifier ? test->modifier : "None");

	/* Allocate fresh atomic request for this test case */
	dev->req = drmModeAtomicAlloc();
	if (!dev->req) {
		fprintf(stderr, "Failed to allocate atomic request\n");
		return -1;
	}

	/* Create writeback framebuffer */
	if (create_wb_fb(dev, test, &wb_fb_id, &wb_bo))
		goto cleanup;

	/* Setup planes */
	if (setup_planes(dev, test, plane_fb_ids))
		goto cleanup;

	/* Setup display and writeback via atomic */
	if (setup_display_and_wb(dev, test, wb_fb_id))
		goto cleanup;

	/* Trigger writeback */
	ret = trigger_writeback(dev);
	if (ret)
		goto cleanup;

	/* Spawn async IO thread, transfer wb_bo ownership */
	*work_out = spawn_io_thread(wb_bo, test, test_idx, result, dev);
	wb_bo = NULL;
	ret = 0;
cleanup:
	drmModeAtomicFree(dev->req);
	dev->req = NULL;

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

	/* Destroy mode blob */
	if (dev->mode_blob_id) {
		drmModeDestroyPropertyBlob(dev->fd, dev->mode_blob_id);
		dev->mode_blob_id = 0;
	}

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
	const char *device = NULL;
	const char *module = NULL;
	struct wb_result *results;
	struct wb_io_work *pending[IO_CONCURRENCY] = {NULL};
	int pend_cnt = 0, pend_head = 0, pend_tail = 0;
	unsigned int round = 0;

	num_tests = sizeof(test_cases) / sizeof(test_cases[0]);
	results = calloc(num_tests, sizeof(*results));

	printf("VBD Writeback Test Program\n");
	printf("=============================\n");
	printf("Auto-running %d test cases\n\n", num_tests);

	/* Parse command line arguments for device/module selection */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-w") == 0)
			dev.write_file = 1;
		else if (strcmp(argv[i], "-M") == 0)
			device = argv[++i];
		else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-m") == 0)
			module = argv[++i];
	}

	/* Open DRM device using util_open for better compatibility */
	dev.fd = util_open(device, module);
	if (dev.fd < 0) {
		fprintf(stderr, "Failed to open DRM device: %s\n", device);
		goto err_free;
	}

	/* drmSetClientCap() with DRM_CLIENT_CAP_ATOMIC enables atomic KMS API
	 * which allows setting all display properties in a single transaction */
	if (drmSetClientCap(dev.fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		fprintf(stderr, "Failed to enable atomic modesetting: %s\n", strerror(errno));
		goto err_close_fd;
	}
	/* drmSetClientCap() with DRM_CLIENT_CAP_WRITEBACK_CONNECTORS exposes
	 * writeback connectors in the connector list for writeback functionality */
	if (drmSetClientCap(dev.fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1)) {
		fprintf(stderr, "Failed to enable writeback connectors: %s\n", strerror(errno));
		goto err_close_fd;
	}

	/* Initialize device and find resources */
	if (wb_drm_device_init(&dev))
		goto err_close_fd;

	/* Cache property IDs for all DRM objects (one-time) */
	if (wb_drm_props_init(&dev))
		goto err_cleanup;

	/* Run test rounds - continue if all pass, stop on any failure */
	while (1) {
		for (i = 0; i < num_tests; i++) {
			struct wb_io_work *work = NULL;

			if (run_test_case(&dev, &test_cases[i], i, &work, &results[i]) < 0)
				goto join_pending;
			if (work) {
				/* If queue full, join oldest to make room */
				if (pend_cnt == IO_CONCURRENCY) {
					pthread_join(pending[pend_head]->thread, NULL);
					free(pending[pend_head]);
					pending[pend_head] = NULL;
					pend_head = (pend_head + 1) % IO_CONCURRENCY;
					pend_cnt--;
				}
				pending[pend_tail] = work;
				pend_tail = (pend_tail + 1) % IO_CONCURRENCY;
				pend_cnt++;
			}
		}

		/* Drain remaining IO threads at round end */
		while (pend_cnt > 0) {
			pthread_join(pending[pend_head]->thread, NULL);
			free(pending[pend_head]);
			pending[pend_head] = NULL;
			pend_head = (pend_head + 1) % IO_CONCURRENCY;
			pend_cnt--;
		}

		/* Check CRC results */
		fail = 0;
		for (int j = 0; j < num_tests; j++) {
			if (!results[j].pass)
				fail++;
		}
		if (fail)
			break;

		round++;
		printf("\nCompleted round %u\n", round);
	}

join_pending:
	while (pend_cnt > 0) {
		pthread_join(pending[pend_head]->thread, NULL);
		free(pending[pend_head]);
		pending[pend_head] = NULL;
		pend_head = (pend_head + 1) % IO_CONCURRENCY;
		pend_cnt--;
	}

	cleanup_device(&dev);
err_cleanup:
err_close_fd:
	close(dev.fd);
err_free:
	free(results);

	/* Summary */
	pass = num_tests * round + (fail ? i : num_tests) - fail;
	fail = fail > 0 ? fail : 0;
	printf("\n========================================\n");
	printf("Test Summary\n");
	printf("============\n");
	printf("Rounds: %u, Total: %d, Pass: %d, Fail: %d\n",
	       round + (fail > 0), num_tests, pass, fail);
	if (fail) {
		printf("Failed tests:\n");
		for (int j = 0; j < num_tests; j++) {
			if (!results[j].pass)
				printf("  %s (CRC: %08x, expected: %08x)\n",
				       results[j].name, results[j].crc,
				       results[j].expected_crc);
		}
	}
	printf("========================================\n");

	return (fail == 0) ? 0 : 1;
}
