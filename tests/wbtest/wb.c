#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "drm_fourcc.h"
#include "util/common.h"
#include "util_double_list.h"
#include "util_math.h"
#include "util/kms.h"

#define DRM_DEVICE "/dev/dri/card0"

#define  CLUSTER0_NAME		"Cluster0-win0"
#define  CLUSTER0_1_NAME	"Cluster0-win1"
#define  CLUSTER1_NAME		"Cluster1-win0"
#define  CLUSTER1__1_NAME	"Cluster1-win1"
#define  ESMART0_NAME		"Esmart0-win0"
#define  ESMART1_NAME		"Esmart1-win0"
#define  SMART0_NAME		"Smart0-win0"
#define  SMART1_NAME		"Smart1-win0"

#define AUX_SCREEN_PIC		"data/1920x1080-NV12-Boxlanucher.bin"
#define AUX_SCREEN_PIC1		"data/wb_1920x1080_NV12-1920x1080-RGB888-disp-err.bin"
#define MAIN_SCREEN_PIC		"data/1920x1080_yuv420-flower.bin"
//#define AFBC_PIC		"data/1920x1080-NV12-afbc-flower.bin"
#define AFBC_PIC		"data/win0_area0_1088x1920_ARGB8888_AFBC_3.bin"
#define WB_PIC			"data/wb.bin"

#define VO_DRM_PRIORITY_DIFFER_FROM_MAX_FOR_VSYNC   10
#define VO_DRM_PRIORITY_DIFFER_FROM_MAX_FOR_SPLICE  11
#define VO_DRM_PRIORITY_DIFFER_FROM_MAX_FOR_RENDER  12

#ifndef DRM_RDWR
#define DRM_RDWR O_RDWR
#endif

#ifndef DRM_MODE_FB_MODIFIERS
#define DRM_MODE_FB_MODIFIERS O_RDWR
#endif

struct  drm_backend {
	int drm_fd;

	uint32_t chipid;

	struct list_head connector_list;
	struct list_head crtc_list;
	struct list_head plane_list;
};

struct drm_property_info {
	const char *name; /**< name as string (static, not freed) */
	char name_value[DRM_PROP_NAME_LEN];
	uint32_t prop_id; /**< KMS property object ID */
	uint32_t flags;
	uint64_t value;
};

/**
 * List of properties attached to DRM connectors
 */
enum drm_connector_property {
	DRM_CONNECTOR_PROP_CRTC_ID,
	DRM_CONNECTOR_PROP_WRITEBACK_FB_ID,
	DRM_CONNECTOR_PROP_BRIGHTNESS,
	DRM_CONNECTOR_PROP_CONTRAST,
	DRM_CONNECTOR_PROP_SATURATION,
	DRM_CONNECTOR_PROP_HUE,
	DRM_CONNECTOR_PROP_HDMI_FORMAT,
	DRM_CONNECTOR_PROP_HDMI_COLORIMETRY,
	DRM_CONNECTOR_PROP_HDMI_QUANT_RANGE,
	DRM_CONNECTOR_PROP__COUNT
};

/**
 * List of properties attached to DRM crtcs
 */
enum drm_crtc_property {
	DRM_CRTC_PROP_MODE_ID = 0,
	DRM_CRTC_PROP_ACTIVE,
	DRM_CRTC_PROP_SOC_ID,
	DRM_CRTC_PROP__COUNT
};

/**
 * List of properties attached to DRM planes
 */
enum drm_plane_property {
	DRM_PLANE_PROP_TYPE = 0,
	DRM_PLANE_PROP_SRC_X,
	DRM_PLANE_PROP_SRC_Y,
	DRM_PLANE_PROP_SRC_W,
	DRM_PLANE_PROP_SRC_H,
	DRM_PLANE_PROP_CRTC_X,
	DRM_PLANE_PROP_CRTC_Y,
	DRM_PLANE_PROP_CRTC_W,
	DRM_PLANE_PROP_CRTC_H,
	DRM_PLANE_PROP_FB_ID,
	DRM_PLANE_PROP_CRTC_ID,
	DRM_PLANE_PROP_IN_FENCE_FD,
	DRM_PLANE_PROP_ZPOS,
	DRM_PLANE_PROP_NAME,
	DRM_PLANE_PROP_ASYNC_COMMIT,
	DRM_PLANE_PROP__COUNT
};

struct  drm_connector {
	struct list_head link;
	struct drm_backend *backend;
	struct drm_output *output;

	uint32_t connector_id; /* object ID to pass to DRM functions */
	uint32_t crtc_id; /* object ID to pass to DRM functions */
	drmModeConnector *conn;
	char *name;

	struct drm_property_info props[DRM_CONNECTOR_PROP__COUNT];
};

struct drm_crtc {
	struct list_head link;
	struct drm_backend *backend;
	struct drm_output *output;

	uint32_t crtc_id; /* object ID to pass to DRM functions */
	uint32_t pipe;

	struct drm_property_info props[DRM_CRTC_PROP__COUNT];

};

struct drm_plane {
	struct list_head link;
	struct drm_backend *backend;
	struct drm_output *output;
	const char *name;

	uint32_t plane_id;
	uint32_t crtc_id;
	uint32_t type;
	uint32_t zpos;

	struct drm_property_info props[DRM_PLANE_PROP__COUNT];
};

struct drm_fb {
	int prime_fd;
	int width, height;
	int vir_width, vir_height;
	uint32_t format;
	uint64_t modifier;

	uint32_t fb_id, size;
	uint32_t handles[4];
	uint32_t strides[4];
	uint32_t offsets[4];

	int drm_fd;
	/* Used by dumb fbs */
	void *map;
};

struct drm_output_vsync_callback {
	struct list_head link;
	void (*vsync_callback)(struct drm_output *output);
};

struct drm_output {
	struct drm_backend *backend;

	struct drm_connector *connector;
	struct drm_crtc *crtc;

	struct drm_plane *primary_plane; /* used for graphics surface*/
	struct drm_plane *video_plane;   /* used for video surface*/
	struct drm_plane *cursor_plane;

	struct list_head mode_list;

	uint32_t width, height;

	pthread_t vsync_thread;
	uint32_t vsync_thread_destroy;

	struct timeval vsync_timestamp;
	struct timeval vsync_timestamp_last;

	struct list_head callback_list;
	pthread_mutex_t  mutex_callbcak;

	struct list_head plane_list;
	pthread_mutex_t  mutex_plane;
};

struct drm_writeback {
	struct drm_connector *connector;
	struct drm_crtc *crtc;
};

const struct drm_property_info connector_props[] = {
	[DRM_CONNECTOR_PROP_CRTC_ID] = { .name = "CRTC_ID", },
	[DRM_CONNECTOR_PROP_WRITEBACK_FB_ID] = { .name = "WRITEBACK_FB_ID", },
	[DRM_CONNECTOR_PROP_BRIGHTNESS] = { .name = "brightness", },
	[DRM_CONNECTOR_PROP_CONTRAST] = { .name = "contrast", },
	[DRM_CONNECTOR_PROP_SATURATION] = { .name = "saturation", },
	[DRM_CONNECTOR_PROP_HUE] = { .name = "hue", },
	[DRM_CONNECTOR_PROP_HDMI_FORMAT] = { .name = "hdmi_output_format", },
	[DRM_CONNECTOR_PROP_HDMI_COLORIMETRY] = { .name = "hdmi_output_colorimetry", },
	[DRM_CONNECTOR_PROP_HDMI_QUANT_RANGE] = { .name = "hdmi_quant_range", },
};

const struct drm_property_info crtc_props[] = {
	[DRM_CRTC_PROP_MODE_ID] = { .name = "MODE_ID", },
	[DRM_CRTC_PROP_ACTIVE] = { .name = "ACTIVE", },
	[DRM_CRTC_PROP_SOC_ID] = { .name = "SOC_ID" },
};

const struct drm_property_info plane_props[] = {
	[DRM_PLANE_PROP_TYPE] = { .name = "type", },
	[DRM_PLANE_PROP_SRC_X] = { .name = "SRC_X", },
	[DRM_PLANE_PROP_SRC_Y] = { .name = "SRC_Y", },
	[DRM_PLANE_PROP_SRC_W] = { .name = "SRC_W", },
	[DRM_PLANE_PROP_SRC_H] = { .name = "SRC_H", },
	[DRM_PLANE_PROP_CRTC_X] = { .name = "CRTC_X", },
	[DRM_PLANE_PROP_CRTC_Y] = { .name = "CRTC_Y", },
	[DRM_PLANE_PROP_CRTC_W] = { .name = "CRTC_W", },
	[DRM_PLANE_PROP_CRTC_H] = { .name = "CRTC_H", },
	[DRM_PLANE_PROP_FB_ID] = { .name = "FB_ID", },
	[DRM_PLANE_PROP_CRTC_ID] = { .name = "CRTC_ID", },
	[DRM_PLANE_PROP_IN_FENCE_FD] = { .name = "IN_FENCE_FD", },
	[DRM_PLANE_PROP_ZPOS] = { .name = "zpos", },
	[DRM_PLANE_PROP_NAME] = { .name = "NAME", },
	[DRM_PLANE_PROP_ASYNC_COMMIT] = { .name = "ASYNC_COMMIT", },
};

static int
add_property(drmModeAtomicReqPtr preq, uint32_t obj_id, struct drm_property_info *prop, uint64_t value)
{
	int ret;

	if (!prop || !preq)
		return -1;

	//    fprintf(stderr,"%s obj_id %d property_id %d value %ld\n", __func__, obj_id, prop->prop_id, value);
	ret = drmModeAtomicAddProperty(preq, obj_id, prop->prop_id, value);
	if (ret < 0)
		fprintf(stderr,"fail to add property\n");

	prop->value = value;

	return ret;
}

static void drm_property_info_populate(struct drm_backend *b, const struct drm_property_info *src,
				       struct drm_property_info *info, unsigned int num_infos,
				       drmModeObjectProperties *props)
{
	drmModePropertyRes *prop;
	unsigned i, j;

	for (i = 0; i < num_infos; i++) {
		info[i].name = src[i].name;
		info[i].prop_id = 0;
	}

	for (i = 0; i < props->count_props; i++) {

		prop = drmModeGetProperty(b->drm_fd, props->props[i]);
		if (!prop)
			continue;

		for (j = 0; j < num_infos; j++) {
			if (!strcmp(prop->name, info[j].name))
				break;
		}

		/* We don't know/care about this property. */
		if (j == num_infos) {
			drmModeFreeProperty(prop);
			continue;
		}

		/* get the plane NAME, which is a bitmask */
		if (!strcmp(prop->name, "NAME")) {
			memcpy(info[j].name_value, prop->enums[0].name, DRM_PROP_NAME_LEN);
		}

		info[j].prop_id = props->props[i];
		info[j].value = props->prop_values[i];
		info[j].flags = prop->flags;

		drmModeFreeProperty(prop);
		//fprintf(stderr,"prop %s id %d value %lu\n",info[j].name, info[j].prop_id, info[j].value);
	}
}

static struct drm_connector *drm_connector_add(struct drm_backend *b, uint32_t connector_id)
{
	struct drm_connector *connector = NULL;
	drmModeConnector *conn;
	drmModeObjectProperties *props;

	conn = drmModeGetConnector(b->drm_fd, connector_id);
	if (!conn)
		goto ret;

	connector = (struct drm_connector *)calloc(sizeof(*connector), 1);
	if (!connector) {
		drmModeFreeConnector(conn);
		goto ret;
	}
	connector->backend = b;
	connector->connector_id = connector_id;
	connector->conn = conn;

	props = drmModeObjectGetProperties(b->drm_fd, connector_id, DRM_MODE_OBJECT_CONNECTOR);
	if (!props) {
		fprintf(stderr,"couldn't get plane properties\n");
		goto err;
	}
	drm_property_info_populate(b, connector_props, connector->props, DRM_CONNECTOR_PROP__COUNT, props);
	drmModeFreeObjectProperties(props);

	list_addtail(&connector->link, &b->connector_list);
ret:
	return connector;
err:
	free(connector);
	return NULL;
}

static void drm_connector_destroy(struct drm_connector * connector)
{
	list_del(&connector->link);
	drmModeFreeConnector(connector->conn);
	free(connector);
}

static drmModeEncoder *drm_get_encoder_by_id(int fd, drmModeRes *res, uint32_t id)
{
	drmModeEncoder *encoder;
	int i;

	for (i = 0; i < res->count_encoders; i++) {
		encoder = drmModeGetEncoder(fd, res->encoders[i]);
		if (!encoder)
			continue;

		/*		printf("%d\t%d\t%s\t0x%08x\t0x%08x\n",
				encoder->encoder_id,
				encoder->crtc_id,
				util_lookup_encoder_type_name(encoder->encoder_type),
				encoder->possible_crtcs,
				encoder->possible_clones); */

		if (encoder->encoder_id == id)
			return encoder;
	}

	return NULL;
}

static int drm_backend_discover_connectors(struct drm_backend *b, drmModeRes *resources)
{
	struct drm_connector *connector;
	int i;

	for (i = 0; i < resources->count_connectors; i++) {
		uint32_t connector_id = resources->connectors[i];
		drmModeEncoder *encoder;

		connector = drm_connector_add(b, connector_id);
		if (!connector)
			continue;
		encoder = drm_get_encoder_by_id(b->drm_fd, resources, connector->conn->encoder_id);
		if (encoder) {
			connector->crtc_id = encoder->crtc_id;
			asprintf(&connector->name, "%s-%u",
					util_lookup_connector_type_name(connector->conn->connector_type),
					connector->conn->connector_type_id);
		}

	}

	return 0;
};

static struct drm_crtc *drm_crtc_create(struct drm_backend *b, uint32_t crtc_id, uint32_t pipe)
{
	struct drm_crtc *crtc;
	drmModeObjectProperties *props;

	crtc = (struct drm_crtc *)calloc(sizeof(*crtc), 1);
	if (!crtc)
		goto ret;

	crtc->backend = b;
	crtc->crtc_id = crtc_id;
	crtc->pipe = pipe;

	props = drmModeObjectGetProperties(b->drm_fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC);
	if (!props) {
		fprintf(stderr,"couldn't get plane properties\n");
		goto err;
	}
	drm_property_info_populate(b, crtc_props, crtc->props, DRM_CRTC_PROP__COUNT, props);
	drmModeFreeObjectProperties(props);

	if (b->chipid == 0)
		b->chipid = crtc->props[DRM_CRTC_PROP_SOC_ID].value;
	/* Add it to the last position of the DRM-backend CRTC list */
	list_addtail(&crtc->link, &b->crtc_list);
ret:
	return crtc;
err:
	free(crtc);
	return NULL;
}

static void drm_crtc_destroy(struct drm_crtc *crtc)
{
	list_del(&crtc->link);
	free(crtc);
}

static int drm_backend_create_crtc_list(struct drm_backend *b, drmModeRes *resources)
{
	struct drm_crtc *crtc, *tmp;
	int i;

	/* Iterate through all CRTCs */
	for (i = 0; i < resources->count_crtcs; i++) {

		/* Let's create an object for the CRTC and add it to the list */
		crtc = drm_crtc_create(b, resources->crtcs[i], i);
		if (!crtc)
			goto err;
	}

	return 0;

err:
	LIST_FOR_EACH_ENTRY_SAFE(crtc, tmp, &b->crtc_list, link)
		drm_crtc_destroy(crtc);
	return -1;
}

static int drm_plane_set_commit_mode(struct drm_plane *plane, bool async)
{
	int ret;
	drmModeAtomicReqPtr preq;

	preq = drmModeAtomicAlloc();
	if (!preq) {
		fprintf(stderr,"%s outof memory\n", __func__);
		return -1;
	}

	add_property(preq, plane->plane_id, &plane->props[DRM_PLANE_PROP_ASYNC_COMMIT], async);

	ret = drmModeAtomicCommit(plane->backend->drm_fd, preq, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret)
		fprintf(stderr,"%s Atomic Commit failed %s\n", __func__, strerror(errno));

	drmModeAtomicFree(preq);

	return ret;
}

static struct drm_plane *drm_plane_create(struct drm_backend *b, drmModePlane *plane)
{
	struct drm_plane *drm_plane = NULL;
	drmModeObjectProperties *props;

	drm_plane = (struct drm_plane *)calloc(sizeof(*drm_plane), 1);
	if(!drm_plane)
		goto ret;

	drm_plane->backend = b;
	drm_plane->crtc_id = plane->crtc_id;
	drm_plane->plane_id = plane->plane_id;

	props = drmModeObjectGetProperties(b->drm_fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
	if (!props) {
		fprintf(stderr,"couldn't get plane properties\n");
		goto err;
	}
	drm_property_info_populate(b, plane_props, drm_plane->props, DRM_PLANE_PROP__COUNT, props);
	drmModeFreeObjectProperties(props);
	drm_plane->name = drm_plane->props[DRM_PLANE_PROP_NAME].name_value;
	drm_plane->type = drm_plane->props[DRM_PLANE_PROP_TYPE].value;
	drm_plane->zpos = drm_plane->props[DRM_PLANE_PROP_ZPOS].value;
	/* drmModeSetPlane work in async mode */
	if (drm_plane_set_commit_mode(drm_plane, 1))
		goto err;

	/* Disable plane */
	if (drmModeSetPlane(b->drm_fd, plane->plane_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) < 0)
		fprintf(stderr,"failed to set plane mode: %s\n", strerror(errno));

	list_addtail(&drm_plane->link, &b->plane_list);
ret:
	return drm_plane;
err:
	free(drm_plane);
	return NULL;
}

static void drm_plane_destroy(struct drm_plane *plane)
{
	list_del(&plane->link);
	free(plane);
}

static int drm_backend_create_plane_list(struct drm_backend *b)
{
	drmModePlaneRes *plane_res;
	drmModePlane *plane;
	struct drm_plane *drm_plane;
	uint32_t i;

	plane_res = drmModeGetPlaneResources(b->drm_fd);
	if (!plane_res) {
		fprintf(stderr,"drmModeGetPlaneResources failed: %s\n", strerror(errno));
		return -1;
	}
	for (i = 0; i < plane_res->count_planes; i++) {
		plane = drmModeGetPlane(b->drm_fd, plane_res->planes[i]);
		if (!plane)
			continue;

		drm_plane = drm_plane_create(b, plane);
		drmModeFreePlane(plane);
		if (!drm_plane)
			continue;
	}

	drmModeFreePlaneResources(plane_res);

	return 0;
}

static int drm_backend_get_resouces(struct drm_backend *b)
{
	drmModeRes * res;

	drmSetClientCap(b->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	res = drmModeGetResources(b->drm_fd);
	if (!res) {
		fprintf(stderr,"drmModeGetResources failed: %s\n", strerror(errno));
		return -1;
	}

	drm_backend_discover_connectors(b, res);
	drm_backend_create_crtc_list(b, res);
	drm_backend_create_plane_list(b);

	drmModeFreeResources(res);

	return 0;
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

static uint32_t get_virtual_height_for_afbc(uint32_t width, uint32_t height, uint32_t bpp)
{
	uint32_t afbc_size = get_afbc_size(width, height, bpp);
	uint32_t virtual_height = height;

	while (afbc_size > (width * virtual_height * bpp >> 3))
		virtual_height++;

	return virtual_height;
}

static uint32_t pixel_format_get_dump_info(struct drm_mode_create_dumb *create_arg,
		int width, int height, uint32_t format, uint64_t modifier)
{
	uint32_t virtual_height;

	create_arg->width = width;
	switch(format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_BGRX8888:
		create_arg->bpp = 32;
		virtual_height = height;
		if (modifier != DRM_FORMAT_MOD_INVALID)
			virtual_height = get_virtual_height_for_afbc(width, height, create_arg->bpp);
		create_arg->height = virtual_height;
		break;
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_BGR888:
		create_arg->bpp = 24;
		virtual_height = height;
		if (modifier != DRM_FORMAT_MOD_INVALID)
			virtual_height = get_virtual_height_for_afbc(width, height, create_arg->bpp);
		create_arg->height = virtual_height;
		break;
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
		create_arg->bpp = 8;
		virtual_height = height * 3 / 2;
		if (modifier != DRM_FORMAT_MOD_INVALID)
			virtual_height = get_virtual_height_for_afbc(width, height, create_arg->bpp);
		create_arg->height = virtual_height;

		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static struct drm_fb *drm_backend_fb_create_dump(struct drm_backend *b, int width, int height,
		uint32_t format, uint64_t modifier)
{
	int ret;
	uint32_t i;
	struct drm_fb *fb;
	uint64_t mods[4] = { };
	struct drm_mode_create_dumb create_arg;
	struct drm_mode_destroy_dumb destroy_arg;
	struct drm_mode_map_dumb map_arg;

	fb = (struct drm_fb*)calloc(sizeof(*fb), 1);
	if (!fb)
		return NULL;

	memset(&create_arg, 0, sizeof create_arg);
	ret = pixel_format_get_dump_info(&create_arg, width, height, format, modifier);
	if (ret) {
		fprintf(stderr,"failed to support format 0x%lx modifier %llx\n", (unsigned long) format, modifier);
		goto err_fb;
	}

	ret = drmIoctl(b->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_arg);
	if (ret) {
		fprintf(stderr,"%s DRM_IOCTL_MODE_CREATE_DUMB failed\n", __func__);
		goto err_fb;
	}

	switch (format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
		fb->handles[0] = create_arg.handle;
		fb->strides[0] = create_arg.pitch;
		fb->offsets[0] = 0;

		fb->handles[1] = create_arg.handle;
		fb->strides[1] = fb->strides[0];
		fb->offsets[1] = fb->strides[0] * height;
		break;
	default:
		fb->handles[0] = create_arg.handle;
		fb->strides[0] = create_arg.pitch;
		break;
	}

	fb->size = create_arg.size;
	fb->width = width;
	fb->height = height;
	fb->vir_width = create_arg.pitch * 8 / create_arg.bpp;
	fb->vir_height = height;
	fb->drm_fd = b->drm_fd;
	fb->format = format;
	fb->modifier = modifier;

	ret = drmPrimeHandleToFD(b->drm_fd, fb->handles[0], DRM_CLOEXEC | DRM_RDWR, &fb->prime_fd);
	if (ret != 0) {
		fprintf(stderr,"failed to get buff fd: %s\n", strerror(errno));
		goto err_bo;
	}

	if (modifier != DRM_FORMAT_MOD_INVALID) {
		/* KMS demands that if a modifier is set, it must be the same for all planes. */
		for (i = 0; i < ARRAY_SIZE(mods) && fb->handles[i]; i++)
			mods[i] = modifier;
		ret = drmModeAddFB2WithModifiers(fb->drm_fd, fb->width, fb->height,
				format, fb->handles, fb->strides,
				fb->offsets, mods, &fb->fb_id,
				DRM_MODE_FB_MODIFIERS);
	} else {
		ret = drmModeAddFB2(b->drm_fd, fb->width, fb->height, format, fb->handles,
				fb->strides,  fb->offsets, &fb->fb_id, 0);
	}
	if (ret != 0) {
		fprintf(stderr,"failed to create kms fb: %s\n", strerror(errno));
		goto err_bo;
	}

	memset(&map_arg, 0, sizeof map_arg);
	map_arg.handle = fb->handles[0];
	ret = drmIoctl(b->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_arg);
	if (ret) {
		fprintf(stderr,"DRM_IOCTL_MODE_MAP_DUMB failed\n");
		goto err_add_fb;
	}

	fb->map = mmap64(NULL, fb->size, PROT_WRITE, MAP_SHARED, b->drm_fd, map_arg.offset);
	if (fb->map == MAP_FAILED) {
		fprintf(stderr,"%s map failed\n", __func__);
		goto err_add_fb;
	}

	return fb;

err_add_fb:
	drmModeRmFB(b->drm_fd, fb->fb_id);
err_bo:
	memset(&destroy_arg, 0, sizeof(destroy_arg));
	destroy_arg.handle = create_arg.handle;
	drmIoctl(b->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);
err_fb:
	free(fb);
	return NULL;
}

static void drm_backend_fb_destroy_dumb(struct drm_fb *fb)
{
	struct drm_mode_destroy_dumb destroy_arg;

	//	assert(fb->type == BUFFER_PIXMAN_DUMB);

	if (fb->map && fb->size > 0)
		munmap(fb->map, fb->size);

	memset(&destroy_arg, 0, sizeof(destroy_arg));
	destroy_arg.handle = fb->handles[0];
	drmIoctl(fb->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_arg);

	if (fb->fb_id != 0)
		drmModeRmFB(fb->drm_fd, fb->fb_id);
	free(fb);
}

static struct drm_backend *drm_backend_create(void)
{
	struct drm_backend *b = NULL;
	int fd;

	fd = open(DRM_DEVICE, O_RDWR, 0);
	if (fd < 0) {
		fprintf(stderr, "open %s failed: %s\n", DRM_DEVICE, strerror(errno));;
		return NULL;
	}

	/* enable atomic operation */
	if(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		fprintf(stderr,"no atomic modesetting support: %s\n", strerror(errno));
		drmClose(fd);
		return NULL;
	}

	drmSetClientCap(fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);

	b = (struct drm_backend *)calloc(sizeof(*b), 1);
	if (!b)
		goto end;

	b->drm_fd = fd;
	LIST_INITHEAD(&b->connector_list);
	LIST_INITHEAD(&b->crtc_list);
	LIST_INITHEAD(&b->plane_list);

	drm_backend_get_resouces(b);
end:
	return b;
}

static void drm_backend_destroy(struct drm_backend *b)
{
	struct drm_plane *plane, *ptmp;
	struct drm_crtc *crtc, *ctmp;
	struct drm_connector *connector, *conn_tmp;

	drmClose(b->drm_fd);
	LIST_FOR_EACH_ENTRY_SAFE(plane, ptmp, &b->plane_list, link)
		drm_plane_destroy(plane);

	LIST_FOR_EACH_ENTRY_SAFE(crtc, ctmp, &b->crtc_list, link)
		drm_crtc_destroy(crtc);

	LIST_FOR_EACH_ENTRY_SAFE(connector, conn_tmp, &b->connector_list, link)
		drm_connector_destroy(connector);
	free(b);
}

static void drm_backend_dump(struct drm_backend *b)
{
	struct drm_plane *plane, *tmp;
	struct drm_crtc *crtc, *ctmp;
	struct drm_connector *connector, *conn_tmp;


	LIST_FOR_EACH_ENTRY_SAFE(plane, tmp, &b->plane_list, link)
		fprintf(stderr, "%s(%d)\n", plane->name, plane->plane_id);

	LIST_FOR_EACH_ENTRY_SAFE(crtc, ctmp, &b->crtc_list, link)
		fprintf(stderr, "crtc id: %d\n", crtc->crtc_id);

	LIST_FOR_EACH_ENTRY_SAFE(connector, conn_tmp, &b->connector_list, link)
		fprintf(stderr, "%s(%d)\n", connector->name, connector->connector_id);
}

static float mode_vrefresh(drmModeModeInfo *mode)
{
	return  mode->clock * 1000.00
		/ (mode->htotal * mode->vtotal);
}

static drmModeModeInfo *connector_find_mode(drmModeConnector *connector, const char *mode_str, const float vrefresh)
{
	drmModeModeInfo *mode;
	int i;

	if (!connector || !connector->count_modes)
		return NULL;

	/* Pick by Index */
	if (mode_str[0] == '#') {
		int index = atoi(mode_str + 1);

		if (index >= connector->count_modes || index < 0)
			return NULL;
		return &connector->modes[index];
	}

	/* Pick by Name */
	for (i = 0; i < connector->count_modes; i++) {
		mode = &connector->modes[i];
		if (!strcmp(mode->name, mode_str)) {
			/* If the vertical refresh frequency is not specified
			 * then return the first mode that match with the name.
			 * Else, return the mode that match the name and
			 * the specified vertical refresh frequency.
			 */
			if (vrefresh == 0)
				return mode;
			else if (fabs(mode_vrefresh(mode) - vrefresh) < 0.005)
				return mode;
		}
	}

	return NULL;
}

static int drm_set_mode(struct drm_output *output, const char *mode_str)
{
	drmModeConnector *conn = output->connector->conn;
	drmModeModeInfo *mode;
	drmModeAtomicReqPtr preq;
	uint32_t blob_id;
	int ret;

	preq = drmModeAtomicAlloc();
	if (!preq) {
		fprintf(stderr,"%s outof memory\n", __func__);
		return -1;
	}

	mode = connector_find_mode(conn, mode_str, 0);
	if (!mode) {
		fprintf(stderr, "faild to find mode %s for %s\n", mode_str, output->connector->name);
		return -1;
	}

	printf("setting mode %s-%.2fHz for %s\n", mode->name, mode_vrefresh(mode), output->connector->name);

	drmModeCreatePropertyBlob(output->backend->drm_fd, mode, sizeof(*mode), &blob_id);
	add_property(preq, output->connector->connector_id,
			&output->connector->props[DRM_CONNECTOR_PROP_CRTC_ID],
			output->crtc->crtc_id);
	add_property(preq, output->crtc->crtc_id, &output->crtc->props[DRM_CRTC_PROP_MODE_ID], blob_id);
	add_property(preq, output->crtc->crtc_id, &output->crtc->props[DRM_CRTC_PROP_ACTIVE], 1);

	ret = drmModeAtomicCommit(output->backend->drm_fd, preq, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(preq);
	if (ret) {
		fprintf(stderr,"%s Atomic Commit failed %s\n", __func__, strerror(errno));
		return -1;
	} else {
		output->width = mode->hdisplay;
		output->height = mode->vdisplay;
	}

	return ret;
}

static int drm_set_plane(struct drm_output *output,
		struct drm_fb *fb, uint32_t plane_id)
{

	int32_t crtc_x = 0;
	int32_t crtc_y = 0;
	uint32_t crtc_w = 0;
	uint32_t crtc_h = 0;
	uint32_t src_x = 0;
	uint32_t src_y = 0;
	uint32_t src_w = 0;
	uint32_t src_h = 0;
	uint32_t fb_id = 0;
	int ret;

	if (fb) {
		crtc_w = output->width;
		crtc_h = output->height;
		src_w = fb->width;
		src_h = fb->height;
		fb_id = fb->fb_id;
	}

	if (crtc_x + crtc_w > output->width ||
			crtc_y + crtc_h > output->height) {
		fprintf(stderr,"%s dst size [%d %d %d %d] out of output[%d %d]\n",
				__func__, crtc_x, crtc_y, crtc_w, crtc_h,
				output->width, output->height);
		return -1;
	}

	ret = drmModeSetPlane(output->backend->drm_fd, plane_id,
			output->crtc->crtc_id,fb_id, 0, crtc_x, crtc_y,
			crtc_w, crtc_h, src_x << 16, src_y << 16,
			src_w << 16, src_h << 16);
	if (ret < 0)
		fprintf(stderr,"failed to set plane mode: %s\n", strerror(errno));

	return ret;
}

static void *drm_output_vsync_thread(void *data)
{
	struct drm_output *output = (struct drm_output *)data;
	struct sched_param thread_param;
	int thread_policy = SCHED_RR;
	drmVBlank vbl;
	double t, refresh;
	int ret, request_type, count = 0;
	struct drm_output_vsync_callback *vsync_callback, *tmp;
	printf("drm_output_vsync_thread for %s\n", output->connector->name);
	thread_param.sched_priority = sched_get_priority_max(SCHED_RR) - VO_DRM_PRIORITY_DIFFER_FROM_MAX_FOR_VSYNC;
	pthread_setschedparam(output->vsync_thread, thread_policy, &thread_param);
	pthread_getschedparam(output->vsync_thread, &thread_policy, &thread_param);
	printf("drmwaitVblank...\n");
	while (0) {
		request_type= DRM_VBLANK_RELATIVE;
		if (output->crtc->pipe)
			request_type |= (output->crtc->pipe << DRM_VBLANK_HIGH_CRTC_SHIFT) & DRM_VBLANK_HIGH_CRTC_MASK;
		vbl.request.type = (drmVBlankSeqType) request_type;
		vbl.request.sequence = 1;

		ret = drmWaitVBlank(output->backend->drm_fd, &vbl);
		if (ret != 0) {
			//fprintf(stderr,"%s drmWaitVBlank (relative) failed ret: %i\n", __func__, ret);
			continue;
		}
		if (output->vsync_thread_destroy)
			break;
		output->vsync_timestamp_last = output->vsync_timestamp;

		output->vsync_timestamp.tv_sec = vbl.reply.tval_sec;
		output->vsync_timestamp.tv_usec = vbl.reply.tval_usec;

		t = output->vsync_timestamp.tv_sec + output->vsync_timestamp.tv_usec * 1e-6 -
			(output->vsync_timestamp_last.tv_sec + output->vsync_timestamp_last.tv_usec * 1e-6);

		count++;
		if (count == 2)
			count = 0;
		if (t)
			refresh = 1 / t;

		if (count % 2 && refresh > 49)
			continue;

		pthread_mutex_lock(&output->mutex_callbcak);
		LIST_FOR_EACH_ENTRY_SAFE(vsync_callback, tmp, &output->callback_list, link) {
			if(vsync_callback->vsync_callback)
				vsync_callback->vsync_callback(output);
		}
		pthread_mutex_unlock(&output->mutex_callbcak);

	}
	pthread_exit(NULL);
}

static struct drm_output * drm_output_create(struct drm_backend *b, uint32_t type)
{
	struct drm_output *output = NULL;
	struct drm_connector *connector, *tmp;
	struct drm_crtc *crtc, *ctmp;
	int ret;

	output = (struct drm_output *)calloc(sizeof(*output), 1);
	if (!output)
		goto ret;

	LIST_FOR_EACH_ENTRY_SAFE(connector, tmp, &b->connector_list, link) {
		if (connector->conn->connector_type == type) {
			output->connector = connector;
			break;
		}
	}

	if(!output->connector) {
		fprintf(stderr,"%s not found connector %d\n", __func__, type);
		goto fail;
	}
	/* pick a crtc */
	LIST_FOR_EACH_ENTRY_SAFE(crtc, ctmp, &b->crtc_list, link) {
		if (crtc->crtc_id == output->connector->crtc_id) {
			fprintf(stderr,"%s connect to crtc(id=%d)\n", output->connector->name, crtc->crtc_id);
			output->crtc = crtc;
			crtc->output = output;
			break;
		}
	}

	if (!output->crtc) {
		fprintf(stderr,"no crtc fount for %s\n", output->connector->name);
		goto fail;
	}

	output->backend = b;

	ret = pthread_create(&output->vsync_thread, NULL, drm_output_vsync_thread, (void *)output);
	if (ret < 0) {
		fprintf(stderr,"%s couldn't create vsync thread\n", __func__);
		goto fail;
	}

	LIST_INITHEAD(&output->callback_list);
	LIST_INITHEAD(&output->plane_list);
	LIST_INITHEAD(&output->mode_list);

	pthread_mutex_init(&output->mutex_callbcak, NULL);
	pthread_mutex_init(&output->mutex_plane, NULL);
ret:
	return output;
fail:
	free(output);
	return NULL;
}

static void drm_output_destroy(struct drm_output *output)
{
	int ret;
	void *retarg = NULL;

	/* wait thread exit */
	output->vsync_thread_destroy = 1;
	pthread_cancel(output->vsync_thread);
	pthread_join(output->vsync_thread, &retarg);

	if (output->cursor_plane) {
		ret = drmModeSetCursor(output->backend->drm_fd, output->crtc->crtc_id, 0, 0, 0);
		if (ret)
			fprintf(stderr,"drmModeSetCursor failed disable: %s\n",
					strerror(errno));
	}

	if (output->video_plane) {
		ret = drmModeSetPlane(output->backend->drm_fd,
				output->video_plane->plane_id,
				output->crtc->crtc_id, 0, 0, 0, 0,
				0, 0, 0, 0, 0, 0);
		if (ret)
			fprintf(stderr,"drmModeSetPlane failed disable: %s\n",
					strerror(errno));
	}

	ret = drmModeSetCrtc(output->backend->drm_fd,
			output->crtc->crtc_id, 0, 0, 0,
			NULL, 0, NULL);

	if (ret)
		fprintf(stderr,"drmModeSetCrtc failed disabling: %s\n", strerror(errno));

	free(output);
}

static int drm_output_register_callback(struct drm_output *output, void (*callback)(struct drm_output *output))
{
	struct drm_output_vsync_callback *vsysc_callback;

	if (!output || !callback)
		return -1;

	vsysc_callback = (struct drm_output_vsync_callback *)calloc(sizeof(*vsysc_callback), 1);
	if (!vsysc_callback)
		return -1;

	vsysc_callback->vsync_callback = callback;
	pthread_mutex_lock(&output->mutex_callbcak);
	list_addtail(&vsysc_callback->link, &output->callback_list);
	pthread_mutex_unlock(&output->mutex_callbcak);

	return 0;
}

static int drm_output_unregister_callback(struct drm_output *output, void (*callback)(struct drm_output *output))
{
	struct drm_output_vsync_callback *vsync_callback, *tmp;
	if (!output || !callback)
		return -1;

	pthread_mutex_lock(&output->mutex_callbcak);
	LIST_FOR_EACH_ENTRY_SAFE(vsync_callback, tmp, &output->callback_list, link) {
		if(vsync_callback->vsync_callback == callback) {
			list_del(&vsync_callback->link);
			break;
		}
	}
	pthread_mutex_unlock(&output->mutex_callbcak);

	return 0;
}

static int drm_output_update_plane_zpos(struct drm_output *output)
{
	struct drm_plane *plane, *tmp;
	drmModeAtomicReqPtr preq;
	int ret = 0;

	preq = drmModeAtomicAlloc();
	if (!preq) {
		fprintf(stderr,"%s out of memory\n", __func__);
		return -1;
	}

	pthread_mutex_lock(&output->mutex_plane);
	LIST_FOR_EACH_ENTRY_SAFE(plane, tmp, &output->plane_list, link)

		add_property(preq, plane->plane_id, &plane->props[DRM_PLANE_PROP_ZPOS],
				plane->zpos);
	pthread_mutex_unlock(&output->mutex_plane);

	// if(output->primary_plane)
	//     add_property(preq, output->primary_plane->plane_id,
	//                  output->primary_plane->prop_zpos_id, output->primary_plane->zpos);
	// if(output->video_plane)
	//     add_property(preq, output->video_plane->plane_id,
	//                  output->video_plane->prop_zpos_id, output->video_plane->zpos);
	// if(output->cursor_plane)
	//     add_property(preq, output->cursor_plane->plane_id,
	//                  output->cursor_plane->prop_zpos_id, output->cursor_plane->zpos);

	ret = drmModeAtomicCommit(output->backend->drm_fd, preq, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret)
		fprintf(stderr,"%s Atomic Commit failed %s\n", __func__, strerror(errno));

	drmModeAtomicFree(preq);

	return ret;
}

static struct drm_writeback *drm_writeback_create(struct drm_output *output)
{
	struct drm_writeback *wb;
	struct drm_connector *connector, *tmp;

	wb = (struct drm_writeback *)calloc(sizeof(*wb), 1);
	if (!wb)
		return NULL;

	LIST_FOR_EACH_ENTRY_SAFE(connector, tmp, &output->backend->connector_list, link) {
		if (connector->conn->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
			wb->connector = connector;
			break;
		}
	}

	if (!wb->connector) {
		fprintf(stderr,"[%s] not find write back device\n", __func__);
		free(wb);
		return NULL;
	}

	wb->crtc = output->crtc;

	return wb;
}

static int drm_writeback_set_fb(struct drm_writeback *wb, struct drm_fb *fb)
{
	drmModeAtomicReqPtr preq;
	int ret;

	preq = drmModeAtomicAlloc();
	if (!preq) {
		fprintf(stderr,"%s out of memory\n", __func__);
		return -1;
	}

	add_property(preq, wb->connector->connector_id,
			&wb->connector->props[DRM_CONNECTOR_PROP_WRITEBACK_FB_ID],
			fb->fb_id);

	add_property(preq, wb->connector->connector_id,
			&wb->connector->props[DRM_CONNECTOR_PROP_CRTC_ID],
			wb->crtc->crtc_id);

	ret = drmModeAtomicCommit(wb->connector->backend->drm_fd, preq, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret)
		fprintf(stderr,"%s Atomic Commit failed %s\n", __func__, strerror(errno));

	drmModeAtomicFree(preq);

	return ret;
}

static int drm_writeback_destroy(struct drm_writeback *wb)
{
	drmModeAtomicReqPtr preq;
	int ret;

	preq = drmModeAtomicAlloc();
	if (!preq) {
		fprintf(stderr,"%s out of memory\n", __func__);
		return -1;
	}

	add_property(preq, wb->connector->connector_id,
			&wb->connector->props[DRM_CONNECTOR_PROP_WRITEBACK_FB_ID],
			0);
	add_property(preq, wb->connector->connector_id,
			&wb->connector->props[DRM_CONNECTOR_PROP_CRTC_ID],
			0);
	ret = drmModeAtomicCommit(wb->connector->backend->drm_fd, preq, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret)
		fprintf(stderr,"%s Atomic Commit failed %s\n", __func__, strerror(errno));

	drmModeAtomicFree(preq);

	free(wb);

	return 0;
}



static void vsync_callback(struct drm_output *output)
{
	struct timeval *timestamp = &output->vsync_timestamp;
	//printf("time stamp %u %u\n", timestamp->tv_sec, timestamp->tv_usec);
}

static void filecpoy(void *buf, const char *file)
{
	int fd = open(file, O_RDONLY);
	u_int32_t size;

	if (fd < 0) {
		printf("open %s failed\n", file);
		return;
	}
	size = lseek(fd, 0, SEEK_END);
	//printf("%s size is %d\n", file, size);
	lseek(fd, 0, SEEK_SET);

	read(fd, buf, size);
	close(fd);
}

static void filecpoy_for_fb(struct drm_fb *fb, const char *file)
{
	int fd = open(file, O_RDONLY);
	u_int32_t size, bpp;

	if (fd < 0) {
		printf("open file %s failed\n", file);
		return;
	}
	size = lseek(fd, 0, SEEK_END);
	printf("%s size is %d\n", file, size);
	lseek(fd, 0, SEEK_SET);

	switch(fb->format) {
	case DRM_FORMAT_RGB888:
	case DRM_FORMAT_BGR888:
		bpp = 3;
		break;
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_ABGR8888:
		bpp = 4;
		break;
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
	default:
		bpp = 1;
	}

	for(int i = 0; i < fb->height; i++) {
		read(fd, fb->map + i * fb->strides[0], fb->width * bpp);
	}
	if (fb->format == DRM_FORMAT_NV12 || fb->format == DRM_FORMAT_NV21) {
		for(int i = 0; i < fb->height/2; i++)
			read(fd, fb->map + fb->offsets[1] + i * fb->strides[0], fb->width);
	}
	close(fd);
}

static void filewrite(void *buf, uint32_t size, const char *file)
{
	int fd = open(file, O_WRONLY | O_CREAT);

	if (fd < 0) {
		printf("cat not open file\n");
		return;
	}

	write(fd, buf, size);
	close(fd);
}

static void parse_mode(const char *arg, uint32_t *w, uint32_t *h)
{
	sscanf(arg, "%u x %u", w, h);
}

static char optstr[] = "m:a:c:s:h";

static void usage(char *name)
{
	fprintf(stderr, "wrteback test tool v1.0\n");
	fprintf(stderr, "usage: %s [-macs]\n", name);

	fprintf(stderr, "\n Query options:\n\n");
	fprintf(stderr, "\t-c\tafbc enable\n");
	fprintf(stderr, "\t-m w x h\tmain screen display mode\n");
	fprintf(stderr, "\t-a w x h\taux display screen mode\n");
	fprintf(stderr, "\t-s\tskip writeback\n");

	exit(0);
}

int main(int argc, char** argv)
{
	struct drm_backend *b;
	struct drm_writeback *wb;
	struct drm_fb *wb_fb;
	struct drm_fb *wb_fb1;
	struct drm_fb *main_fb, *aux_fb, *afbc_fb;
	struct drm_output *main_output = NULL, *aux_output = NULL;
	uint32_t main_output_type = DRM_MODE_CONNECTOR_HDMIA;
	uint32_t aux_output_type = DRM_MODE_CONNECTOR_eDP;
	uint32_t main_output_w = 1920;
	uint32_t main_output_h = 1080;
	uint32_t main_fb_w = 1920;
	uint32_t main_fb_h = 1080;
	uint32_t aux_output_w = 1280;
	uint32_t aux_output_h = 720;
	uint32_t aux_fb_w = 1920;
	uint32_t aux_fb_h = 1080;
	char main_mode_str[64];
	char aux_mode_str[64];
	struct drm_plane *plane, *tmp;
	drmModeCrtcPtr crtc_info;
	int count;
	bool skip_wb = false;
	bool afbc_en = false;
	int c;

	opterr = 0;
	while ((c = getopt(argc, argv, optstr)) != -1) {

		switch (c) {
		case 'c':
			afbc_en = true;
			break;
		case 'm':
			parse_mode(optarg, &main_output_w, &main_output_h);
			break;
		case 'a':
			parse_mode(optarg, &aux_output_w, &aux_output_h);
			break;
		case 's':
			skip_wb = true;
			break;
		default:
			usage(argv[0]);
			break;
		}
	}

	b = drm_backend_create();
	if (!b) {
		printf("backend create failed\n");
		return -1;
	}

	sprintf(main_mode_str, "%dx%d", main_output_w, main_output_h);
	sprintf(aux_mode_str, "%dx%d", aux_output_w, aux_output_h);
	main_mode_str[9] = '\0';
	aux_mode_str[9] = '\0';
	main_output = drm_output_create(b, main_output_type);
	if (!main_output) {
		printf("main output create failed\n");
		return -1;
	}

	LIST_FOR_EACH_ENTRY_SAFE(plane, tmp, &b->plane_list, link) {
		if (!strcmp(plane->name,  CLUSTER0_NAME)) {
			main_output->video_plane = plane;
			break;
		}
	}

	if (!main_output->video_plane) {
		printf("faild to find %s for main video_plane\n",  CLUSTER0_NAME);
		return -1;
	}

	LIST_FOR_EACH_ENTRY_SAFE(plane, tmp, &b->plane_list, link) {
		if (!strcmp(plane->name,  ESMART0_NAME)) {
			main_output->primary_plane = plane;
			break;
		}
	}

	if (!main_output->primary_plane) {
		printf("faild to find %s for main primary_plane\n",  ESMART0_NAME);
		return -1;
	}

	//printf("primary plane zpos %d video plane zpos %d\n", main_output->primary_plane->zpos, main_output->video_plane->zpos);
	count = main_output->primary_plane->zpos;
	main_output->primary_plane->zpos = main_output->video_plane->zpos;
	main_output->video_plane->zpos = count;
	//printf("new primary plane zpos %d video plane zpos %d\n", main_output->primary_plane->zpos, main_output->video_plane->zpos);
	drm_output_update_plane_zpos(main_output);
	count = 0;
	main_fb = drm_backend_fb_create_dump(b, main_fb_w, main_fb_h, DRM_FORMAT_NV12, DRM_FORMAT_MOD_INVALID);
	aux_fb = drm_backend_fb_create_dump(b, aux_fb_w, aux_fb_h, DRM_FORMAT_NV12, DRM_FORMAT_MOD_INVALID);
	//main_fb = drm_backend_fb_create_dump(b, 1920, 1080, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID);
	afbc_fb = drm_backend_fb_create_dump(b, main_fb_w, main_fb_h, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16));

	filecpoy_for_fb(aux_fb, AUX_SCREEN_PIC);

	filecpoy(main_fb->map, MAIN_SCREEN_PIC);
	filecpoy(afbc_fb->map, AFBC_PIC);


	crtc_info = drmModeGetCrtc(b->drm_fd, main_output->crtc->crtc_id);
	drmModeFreeCrtc(crtc_info);

	drm_set_mode(main_output, main_mode_str);

	aux_output = drm_output_create(b, aux_output_type);
	if (aux_output) {
		/* bind primary plane to aux_output */
		LIST_FOR_EACH_ENTRY_SAFE(plane, tmp, &b->plane_list, link) {
			if (!strcmp(plane->name,  CLUSTER1_NAME)) {
				aux_output->video_plane = plane;
				break;
			}
		}

		if (!aux_output->video_plane) {
			printf("faild to find %s for aux video_plane\n",  CLUSTER1_NAME);
			return -1;
		}

		LIST_FOR_EACH_ENTRY_SAFE(plane, tmp, &b->plane_list, link) {
			if (!strcmp(plane->name,  ESMART1_NAME)) {
				aux_output->primary_plane = plane;
				break;
			}
		}

		if (!aux_output->primary_plane) {
			printf("faild to find %s for aux primary_plane\n",  ESMART1_NAME);
			return -1;
		}

		drm_set_mode(aux_output, aux_mode_str);
		// drm_set_plane(aux_output,
		//                     aux_output->primary_plane->plane_id, 0,
		//                     0, 0, 0, 0,
		//                     0, 0, 0, 0);
	}
	sleep(1);


	drm_set_plane(main_output, NULL, main_output->primary_plane->plane_id);

	drm_output_register_callback(main_output, vsync_callback);

	printf("main %d x %d stride %d\n", main_fb->width, main_fb->height, main_fb->strides[0]);
	printf("aux  %d x %d stride %d\n", aux_fb->width, aux_fb->height, aux_fb->strides[0]);
	if (main_output) {
#if 0
		char *buf = main_fb->map;
		for (int i = 0; i < 1920 * 1080 * 4; i = i + 4) {
			buf[i] = 0xff;          //b
			buf[i + 1] = 0x00;      //g
			buf[i + 2] = 0x00;      //r
			buf[i + 3] = 0x40;      //a
		}
#endif
		/*	drm_set_plane(main_output,
			main_output->primary_plane->plane_id, main_fb->fb_id,
			0, 0, main_output->width, main_output->height,
			0, 0, main_fb->width, main_fb->height); */
		drm_set_plane(main_output, main_fb, main_output->primary_plane->plane_id);
	}

	if (aux_output)
		drm_set_plane(aux_output, aux_fb, aux_output->primary_plane->plane_id);
	wb = drm_writeback_create(main_output);
	wb_fb = drm_backend_fb_create_dump(b, main_output->width, main_output->height, DRM_FORMAT_BGR888, DRM_FORMAT_MOD_INVALID);
	wb_fb1 = drm_backend_fb_create_dump(b, main_output->width, main_output->height, DRM_FORMAT_NV12, DRM_FORMAT_MOD_INVALID);

	filecpoy(wb_fb->map, AUX_SCREEN_PIC1);
	getchar();
	if (wb && !skip_wb)
		drm_writeback_set_fb(wb, wb_fb);

	getchar();
	printf("update aux plane with %s \n", skip_wb ? AUX_SCREEN_PIC1 : "wirteback0");
	drm_set_plane(aux_output, wb_fb, aux_output->primary_plane->plane_id);
	getchar();
	printf("set 2nd wb fb\n");
	if (wb)
		drm_writeback_set_fb(wb, wb_fb1);
	getchar();
	printf("update aux plane with writeback1\n");
	drm_set_plane(aux_output, wb_fb1, aux_output->primary_plane->plane_id);
	getchar();
	printf("disabled writeback\n");
	drm_writeback_destroy(wb);
	printf("save write back data %s\n", WB_PIC);
	filewrite(wb_fb->map, wb_fb->size, WB_PIC);

	drm_backend_fb_destroy_dumb(main_fb);
	drm_backend_destroy(b);

	return 0;
}
