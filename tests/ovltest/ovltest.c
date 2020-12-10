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

/*
 * This fairly simple test program dumps output in a similar format to the
 * "xrandr" tool everyone knows & loves.  It's necessarily slightly different
 * since the kernel separates outputs into encoder and connector structures,
 * each with their own unique ID.  The program also allows test testing of the
 * memory management and mode setting APIs by allowing the user to specify a
 * connector and mode to use for mode setting.  If all works as expected, a
 * blue background should be painted on the monitor attached to the specified
 * connector after the selected mode is set.
 *
 * TODO: use cairo to write the mode info on the selected output once
 *       the mode has been programmed, along with possible test patterns.
 */

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <math.h>

#include "xf86drm.h"
#include "xf86drmMode.h"
#include "drm_fourcc.h"

#include "util/common.h"
#include "util/format.h"
#include "util/kms.h"
#include "util/pattern.h"

#include "bo.h"

#define PIC_NAME_MAX_LEN	64
#define PIC_MAX_CNT		8

/*
 * raw rgb/yuv data file name:
 * widthxheight_format.bin: 720x1280_ARGB8888.bin
 *
 */
char pic_name[PIC_MAX_CNT][PIC_NAME_MAX_LEN];

static unsigned int pic_cnt;
static unsigned int zpos;

struct crtc {
	drmModeCrtc *crtc;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	drmModeModeInfo *mode;
};

struct encoder {
	drmModeEncoder *encoder;
};

struct connector {
	drmModeConnector *connector;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	char *name;
};

struct fb {
	drmModeFB *fb;
};

struct plane {
	drmModePlane *plane;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
};

struct resources {
	drmModeRes *res;
	drmModePlaneRes *plane_res;

	struct crtc *crtcs;
	struct encoder *encoders;
	struct connector *connectors;
	struct fb *fbs;
	struct plane *planes;
};

struct device {
	int fd;

	struct resources *resources;

	struct {
		unsigned int width;
		unsigned int height;

		unsigned int fb_id;
		struct bo *bo;
		struct bo *cursor_bo;
	} mode;

	int use_atomic;
	drmModeAtomicReq *req;
};

static inline int64_t U642I64(uint64_t val)
{
	return (int64_t)*((int64_t *)&val);
}

static float mode_vrefresh(drmModeModeInfo *mode)
{
	return  mode->clock * 1000.00
			/ (mode->htotal * mode->vtotal);
}

#define bit_name_fn(res)					\
const char * res##_str(int type) {				\
	unsigned int i;						\
	const char *sep = "";					\
	for (i = 0; i < ARRAY_SIZE(res##_names); i++) {		\
		if (type & (1 << i)) {				\
			printf("%s%s", sep, res##_names[i]);	\
			sep = ", ";				\
		}						\
	}							\
	return NULL;						\
}

static const char *mode_type_names[] = {
	"builtin",
	"clock_c",
	"crtc_c",
	"preferred",
	"default",
	"userdef",
	"driver",
};

static bit_name_fn(mode_type)

static const char *mode_flag_names[] = {
	"phsync",
	"nhsync",
	"pvsync",
	"nvsync",
	"interlace",
	"dblscan",
	"csync",
	"pcsync",
	"ncsync",
	"hskew",
	"bcast",
	"pixmux",
	"dblclk",
	"clkdiv2"
};

static bit_name_fn(mode_flag)

static void dump_fourcc(uint32_t fourcc)
{
	printf(" %c%c%c%c",
		fourcc,
		fourcc >> 8,
		fourcc >> 16,
		fourcc >> 24);
}

static void dump_encoders(struct device *dev)
{
	drmModeEncoder *encoder;
	int i;

	printf("Encoders:\n");
	printf("id\tcrtc\ttype\tpossible crtcs\tpossible clones\t\n");
	for (i = 0; i < dev->resources->res->count_encoders; i++) {
		encoder = dev->resources->encoders[i].encoder;
		if (!encoder)
			continue;

		printf("%d\t%d\t%s\t0x%08x\t0x%08x\n",
		       encoder->encoder_id,
		       encoder->crtc_id,
		       util_lookup_encoder_type_name(encoder->encoder_type),
		       encoder->possible_crtcs,
		       encoder->possible_clones);
	}
	printf("\n");
}

static void dump_mode(drmModeModeInfo *mode, int index)
{
	printf("  #%i %s %.2f %d %d %d %d %d %d %d %d %d",
	       index,
	       mode->name,
	       mode_vrefresh(mode),
	       mode->hdisplay,
	       mode->hsync_start,
	       mode->hsync_end,
	       mode->htotal,
	       mode->vdisplay,
	       mode->vsync_start,
	       mode->vsync_end,
	       mode->vtotal,
	       mode->clock);

	printf(" flags: ");
	mode_flag_str(mode->flags);
	printf("; type: ");
	mode_type_str(mode->type);
	printf("\n");
}

static void dump_blob(struct device *dev, uint32_t blob_id)
{
	uint32_t i;
	unsigned char *blob_data;
	drmModePropertyBlobPtr blob;

	blob = drmModeGetPropertyBlob(dev->fd, blob_id);
	if (!blob) {
		printf("\n");
		return;
	}

	blob_data = blob->data;

	for (i = 0; i < blob->length; i++) {
		if (i % 16 == 0)
			printf("\n\t\t\t");
		printf("%.2hhx", blob_data[i]);
	}
	printf("\n");

	drmModeFreePropertyBlob(blob);
}

static const char *modifier_to_string(uint64_t modifier)
{
	switch (modifier) {
	case DRM_FORMAT_MOD_INVALID:
		return "INVALID";
	case DRM_FORMAT_MOD_LINEAR:
		return "LINEAR";
	case I915_FORMAT_MOD_X_TILED:
		return "X_TILED";
	case I915_FORMAT_MOD_Y_TILED:
		return "Y_TILED";
	case I915_FORMAT_MOD_Yf_TILED:
		return "Yf_TILED";
	case I915_FORMAT_MOD_Y_TILED_CCS:
		return "Y_TILED_CCS";
	case I915_FORMAT_MOD_Yf_TILED_CCS:
		return "Yf_TILED_CCS";
	case DRM_FORMAT_MOD_SAMSUNG_64_32_TILE:
		return "SAMSUNG_64_32_TILE";
	case DRM_FORMAT_MOD_VIVANTE_TILED:
		return "VIVANTE_TILED";
	case DRM_FORMAT_MOD_VIVANTE_SUPER_TILED:
		return "VIVANTE_SUPER_TILED";
	case DRM_FORMAT_MOD_VIVANTE_SPLIT_TILED:
		return "VIVANTE_SPLIT_TILED";
	case DRM_FORMAT_MOD_VIVANTE_SPLIT_SUPER_TILED:
		return "VIVANTE_SPLIT_SUPER_TILED";
	case DRM_FORMAT_MOD_NVIDIA_TEGRA_TILED:
		return "NVIDIA_TEGRA_TILED";
	case DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(0):
		return "NVIDIA_16BX2_BLOCK(0)";
	case DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(1):
		return "NVIDIA_16BX2_BLOCK(1)";
	case DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(2):
		return "NVIDIA_16BX2_BLOCK(2)";
	case DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(3):
		return "NVIDIA_16BX2_BLOCK(3)";
	case DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(4):
		return "NVIDIA_16BX2_BLOCK(4)";
	case DRM_FORMAT_MOD_NVIDIA_16BX2_BLOCK(5):
		return "NVIDIA_16BX2_BLOCK(5)";
	case DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED:
		return "MOD_BROADCOM_VC4_T_TILED";
	case DRM_FORMAT_MOD_QCOM_COMPRESSED:
		return "QCOM_COMPRESSED";
	default:
		return "(UNKNOWN MODIFIER)";
	}
}

static void dump_in_formats(struct device *dev, uint32_t blob_id)
{
	uint32_t i, j;
	drmModePropertyBlobPtr blob;
	struct drm_format_modifier_blob *header;
	uint32_t *formats;
	struct drm_format_modifier *modifiers;

	printf("\t\tin_formats blob decoded:\n");
	blob = drmModeGetPropertyBlob(dev->fd, blob_id);
	if (!blob) {
		printf("\n");
		return;
	}

	header = blob->data;
	formats = (uint32_t *) ((char *) header + header->formats_offset);
	modifiers = (struct drm_format_modifier *)
		((char *) header + header->modifiers_offset);

	for (i = 0; i < header->count_formats; i++) {
		printf("\t\t\t");
		dump_fourcc(formats[i]);
		printf(": ");
		for (j = 0; j < header->count_modifiers; j++) {
			uint64_t mask = 1ULL << i;
			if (modifiers[j].formats & mask)
				printf(" %s", modifier_to_string(modifiers[j].modifier));
		}
		printf("\n");
	}

	drmModeFreePropertyBlob(blob);
}

static void dump_prop(struct device *dev, drmModePropertyPtr prop,
		      uint32_t prop_id, uint64_t value)
{
	int i;
	printf("\t%d", prop_id);
	if (!prop) {
		printf("\n");
		return;
	}

	printf(" %s:\n", prop->name);

	printf("\t\tflags:");
	if (prop->flags & DRM_MODE_PROP_PENDING)
		printf(" pending");
	if (prop->flags & DRM_MODE_PROP_IMMUTABLE)
		printf(" immutable");
	if (drm_property_type_is(prop, DRM_MODE_PROP_SIGNED_RANGE))
		printf(" signed range");
	if (drm_property_type_is(prop, DRM_MODE_PROP_RANGE))
		printf(" range");
	if (drm_property_type_is(prop, DRM_MODE_PROP_ENUM))
		printf(" enum");
	if (drm_property_type_is(prop, DRM_MODE_PROP_BITMASK))
		printf(" bitmask");
	if (drm_property_type_is(prop, DRM_MODE_PROP_BLOB))
		printf(" blob");
	if (drm_property_type_is(prop, DRM_MODE_PROP_OBJECT))
		printf(" object");
	printf("\n");

	if (drm_property_type_is(prop, DRM_MODE_PROP_SIGNED_RANGE)) {
		printf("\t\tvalues:");
		for (i = 0; i < prop->count_values; i++)
			printf(" %"PRId64, U642I64(prop->values[i]));
		printf("\n");
	}

	if (drm_property_type_is(prop, DRM_MODE_PROP_RANGE)) {
		printf("\t\tvalues:");
		for (i = 0; i < prop->count_values; i++)
			printf(" %"PRIu64, prop->values[i]);
		printf("\n");
	}

	if (drm_property_type_is(prop, DRM_MODE_PROP_ENUM)) {
		printf("\t\tenums:");
		for (i = 0; i < prop->count_enums; i++)
			printf(" %s=%llu", prop->enums[i].name,
			       prop->enums[i].value);
		printf("\n");
	} else if (drm_property_type_is(prop, DRM_MODE_PROP_BITMASK)) {
		printf("\t\tvalues:");
		for (i = 0; i < prop->count_enums; i++)
			printf(" %s=0x%llx", prop->enums[i].name,
			       (1LL << prop->enums[i].value));
		printf("\n");
	} else {
		assert(prop->count_enums == 0);
	}

	if (drm_property_type_is(prop, DRM_MODE_PROP_BLOB)) {
		printf("\t\tblobs:\n");
		for (i = 0; i < prop->count_blobs; i++)
			dump_blob(dev, prop->blob_ids[i]);
		printf("\n");
	} else {
		assert(prop->count_blobs == 0);
	}

	printf("\t\tvalue:");
	if (drm_property_type_is(prop, DRM_MODE_PROP_BLOB))
		dump_blob(dev, value);
	else if (drm_property_type_is(prop, DRM_MODE_PROP_SIGNED_RANGE))
		printf(" %"PRId64"\n", value);
	else
		printf(" %"PRIu64"\n", value);

	if (strcmp(prop->name, "IN_FORMATS") == 0)
		dump_in_formats(dev, value);
}

static void dump_connectors(struct device *dev)
{
	int i, j;

	printf("Connectors:\n");
	printf("id\tencoder\tstatus\t\tname\t\tsize (mm)\tmodes\tencoders\n");
	for (i = 0; i < dev->resources->res->count_connectors; i++) {
		struct connector *_connector = &dev->resources->connectors[i];
		drmModeConnector *connector = _connector->connector;
		if (!connector)
			continue;

		printf("%d\t%d\t%s\t%-15s\t%dx%d\t\t%d\t",
		       connector->connector_id,
		       connector->encoder_id,
		       util_lookup_connector_status_name(connector->connection),
		       _connector->name,
		       connector->mmWidth, connector->mmHeight,
		       connector->count_modes);

		for (j = 0; j < connector->count_encoders; j++)
			printf("%s%d", j > 0 ? ", " : "", connector->encoders[j]);
		printf("\n");

		if (connector->count_modes) {
			printf("  modes:\n");
			printf("\tindex name refresh (Hz) hdisp hss hse htot vdisp "
			       "vss vse vtot)\n");
			for (j = 0; j < connector->count_modes; j++)
				dump_mode(&connector->modes[j], j);
		}

		if (_connector->props) {
			printf("  props:\n");
			for (j = 0; j < (int)_connector->props->count_props; j++)
				dump_prop(dev, _connector->props_info[j],
					  _connector->props->props[j],
					  _connector->props->prop_values[j]);
		}
	}
	printf("\n");
}

static void dump_crtcs(struct device *dev)
{
	int i;
	uint32_t j;

	printf("CRTCs:\n");
	printf("id\tfb\tpos\tsize\n");
	for (i = 0; i < dev->resources->res->count_crtcs; i++) {
		struct crtc *_crtc = &dev->resources->crtcs[i];
		drmModeCrtc *crtc = _crtc->crtc;
		if (!crtc)
			continue;

		printf("%d\t%d\t(%d,%d)\t(%dx%d)\n",
		       crtc->crtc_id,
		       crtc->buffer_id,
		       crtc->x, crtc->y,
		       crtc->width, crtc->height);
		dump_mode(&crtc->mode, 0);

		if (_crtc->props) {
			printf("  props:\n");
			for (j = 0; j < _crtc->props->count_props; j++)
				dump_prop(dev, _crtc->props_info[j],
					  _crtc->props->props[j],
					  _crtc->props->prop_values[j]);
		} else {
			printf("  no properties found\n");
		}
	}
	printf("\n");
}

static void dump_framebuffers(struct device *dev)
{
	drmModeFB *fb;
	int i;

	printf("Frame buffers:\n");
	printf("id\tsize\tpitch\n");
	for (i = 0; i < dev->resources->res->count_fbs; i++) {
		fb = dev->resources->fbs[i].fb;
		if (!fb)
			continue;

		printf("%u\t(%ux%u)\t%u\n",
		       fb->fb_id,
		       fb->width, fb->height,
		       fb->pitch);
	}
	printf("\n");
}

static void dump_planes(struct device *dev)
{
	unsigned int i, j;

	printf("Planes:\n");
	printf("id\tcrtc\tfb\tCRTC x,y\tx,y\tgamma size\tpossible crtcs\n");

	if (!dev->resources->plane_res)
		return;

	for (i = 0; i < dev->resources->plane_res->count_planes; i++) {
		struct plane *plane = &dev->resources->planes[i];
		drmModePlane *ovr = plane->plane;
		if (!ovr)
			continue;

		printf("%d\t%d\t%d\t%d,%d\t\t%d,%d\t%-8d\t0x%08x\n",
		       ovr->plane_id, ovr->crtc_id, ovr->fb_id,
		       ovr->crtc_x, ovr->crtc_y, ovr->x, ovr->y,
		       ovr->gamma_size, ovr->possible_crtcs);

		if (!ovr->count_formats)
			continue;

		printf("  formats:");
		for (j = 0; j < ovr->count_formats; j++)
			dump_fourcc(ovr->formats[j]);
		printf("\n");

		if (plane->props) {
			printf("  props:\n");
			for (j = 0; j < plane->props->count_props; j++)
				dump_prop(dev, plane->props_info[j],
					  plane->props->props[j],
					  plane->props->prop_values[j]);
		} else {
			printf("  no properties found\n");
		}
	}
	printf("\n");

	return;
}

static void free_resources(struct resources *res)
{
	int i;

	if (!res)
		return;

#define free_resource(_res, __res, type, Type)					\
	do {									\
		if (!(_res)->type##s)						\
			break;							\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			if (!(_res)->type##s[i].type)				\
				break;						\
			drmModeFree##Type((_res)->type##s[i].type);		\
		}								\
		free((_res)->type##s);						\
	} while (0)

#define free_properties(_res, __res, type)					\
	do {									\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			drmModeFreeObjectProperties(res->type##s[i].props);	\
			free(res->type##s[i].props_info);			\
		}								\
	} while (0)

	if (res->res) {
		free_properties(res, res, crtc);

		free_resource(res, res, crtc, Crtc);
		free_resource(res, res, encoder, Encoder);

		for (i = 0; i < res->res->count_connectors; i++)
			free(res->connectors[i].name);

		free_resource(res, res, connector, Connector);
		free_resource(res, res, fb, FB);

		drmModeFreeResources(res->res);
	}

	if (res->plane_res) {
		free_properties(res, plane_res, plane);

		free_resource(res, plane_res, plane, Plane);

		drmModeFreePlaneResources(res->plane_res);
	}

	free(res);
}

static struct resources *get_resources(struct device *dev)
{
	struct resources *res;
	int i;

	res = calloc(1, sizeof(*res));
	if (res == 0)
		return NULL;

	drmSetClientCap(dev->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	drmSetClientCap(dev->fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);

	res->res = drmModeGetResources(dev->fd);
	if (!res->res) {
		fprintf(stderr, "drmModeGetResources failed: %s\n",
			strerror(errno));
		goto error;
	}

	res->crtcs = calloc(res->res->count_crtcs, sizeof(*res->crtcs));
	res->encoders = calloc(res->res->count_encoders, sizeof(*res->encoders));
	res->connectors = calloc(res->res->count_connectors, sizeof(*res->connectors));
	res->fbs = calloc(res->res->count_fbs, sizeof(*res->fbs));

	if (!res->crtcs || !res->encoders || !res->connectors || !res->fbs)
		goto error;

#define get_resource(_res, __res, type, Type)					\
	do {									\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			(_res)->type##s[i].type =				\
				drmModeGet##Type(dev->fd, (_res)->__res->type##s[i]); \
			if (!(_res)->type##s[i].type)				\
				fprintf(stderr, "could not get %s %i: %s\n",	\
					#type, (_res)->__res->type##s[i],	\
					strerror(errno));			\
		}								\
	} while (0)

	get_resource(res, res, crtc, Crtc);
	get_resource(res, res, encoder, Encoder);
	get_resource(res, res, connector, Connector);
	get_resource(res, res, fb, FB);

	/* Set the name of all connectors based on the type name and the per-type ID. */
	for (i = 0; i < res->res->count_connectors; i++) {
		struct connector *connector = &res->connectors[i];
		drmModeConnector *conn = connector->connector;
		int num;

		num = asprintf(&connector->name, "%s-%u",
			 drmModeGetConnectorTypeName(conn->connector_type),
			 conn->connector_type_id);
		if (num < 0)
			goto error;
	}

#define get_properties(_res, __res, type, Type)					\
	do {									\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			struct type *obj = &res->type##s[i];			\
			unsigned int j;						\
			obj->props =						\
				drmModeObjectGetProperties(dev->fd, obj->type->type##_id, \
							   DRM_MODE_OBJECT_##Type); \
			if (!obj->props) {					\
				fprintf(stderr,					\
					"could not get %s %i properties: %s\n", \
					#type, obj->type->type##_id,		\
					strerror(errno));			\
				continue;					\
			}							\
			obj->props_info = calloc(obj->props->count_props,	\
						 sizeof(*obj->props_info));	\
			if (!obj->props_info)					\
				continue;					\
			for (j = 0; j < obj->props->count_props; ++j)		\
				obj->props_info[j] =				\
					drmModeGetProperty(dev->fd, obj->props->props[j]); \
		}								\
	} while (0)

	get_properties(res, res, crtc, CRTC);
	get_properties(res, res, connector, CONNECTOR);

	for (i = 0; i < res->res->count_crtcs; ++i)
		res->crtcs[i].mode = &res->crtcs[i].crtc->mode;

	res->plane_res = drmModeGetPlaneResources(dev->fd);
	if (!res->plane_res) {
		fprintf(stderr, "drmModeGetPlaneResources failed: %s\n",
			strerror(errno));
		return res;
	}

	res->planes = calloc(res->plane_res->count_planes, sizeof(*res->planes));
	if (!res->planes)
		goto error;

	get_resource(res, plane_res, plane, Plane);
	get_properties(res, plane_res, plane, PLANE);

	return res;

error:
	free_resources(res);
	return NULL;
}

static int get_crtc_index(struct device *dev, uint32_t id)
{
	int i;

	for (i = 0; i < dev->resources->res->count_crtcs; ++i) {
		drmModeCrtc *crtc = dev->resources->crtcs[i].crtc;
		if (crtc && crtc->crtc_id == id)
			return i;
	}

	return -1;
}

static drmModeConnector *get_connector_by_name(struct device *dev, const char *name)
{
	struct connector *connector;
	int i;

	for (i = 0; i < dev->resources->res->count_connectors; i++) {
		connector = &dev->resources->connectors[i];

		if (strcmp(connector->name, name) == 0)
			return connector->connector;
	}

	return NULL;
}

static drmModeConnector *get_connector_by_id(struct device *dev, uint32_t id)
{
	drmModeConnector *connector;
	int i;

	for (i = 0; i < dev->resources->res->count_connectors; i++) {
		connector = dev->resources->connectors[i].connector;
		if (connector && connector->connector_id == id)
			return connector;
	}

	return NULL;
}

static drmModeEncoder *get_encoder_by_id(struct device *dev, uint32_t id)
{
	drmModeEncoder *encoder;
	int i;

	for (i = 0; i < dev->resources->res->count_encoders; i++) {
		encoder = dev->resources->encoders[i].encoder;
		if (encoder && encoder->encoder_id == id)
			return encoder;
	}

	return NULL;
}

/* -----------------------------------------------------------------------------
 * Pipes and planes
 */

/*
 * Mode setting with the kernel interfaces is a bit of a chore.
 * First you have to find the connector in question and make sure the
 * requested mode is available.
 * Then you need to find the encoder attached to that connector so you
 * can bind it with a free crtc.
 */
struct pipe_arg {
	const char **cons;
	uint32_t *con_ids;
	unsigned int num_cons;
	uint32_t crtc_id;
	char mode_str[64];
	char format_str[5];
	float vrefresh;
	unsigned int fourcc;
	drmModeModeInfo *mode;
	struct crtc *crtc;

	/* Is write back connector */
	bool wbc;
	struct bo *bo;
	struct bo *old_bo;
	unsigned int fb_id, old_fb_id;
	struct timeval start;

	int swap_count;
};

struct plane_arg {
	uint32_t plane_id;  /* the id of plane to use */
	uint32_t crtc_id;  /* the id of CRTC to bind to */
	bool has_position;
	bool afbc_en;
	int32_t rotation;
	int32_t x, y;
	uint32_t w, h;
	uint32_t zpos;
	double scale;
	unsigned int fb_id;
	unsigned int old_fb_id;
	struct bo *bo;
	struct bo *old_bo;
	char format_str[5]; /* need to leave room for terminating \0 */
	unsigned int fourcc;
};

static drmModeModeInfo *
connector_find_mode(struct device *dev, uint32_t con_id, const char *mode_str,
	const float vrefresh)
{
	drmModeConnector *connector;
	drmModeModeInfo *mode;
	int i;

	connector = get_connector_by_id(dev, con_id);
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

static struct crtc *pipe_find_crtc(struct device *dev, struct pipe_arg *pipe)
{
	uint32_t possible_crtcs = ~0;
	uint32_t active_crtcs = 0;
	unsigned int crtc_idx;
	unsigned int i;
	int j;

	for (i = 0; i < pipe->num_cons; ++i) {
		uint32_t crtcs_for_connector = 0;
		drmModeConnector *connector;
		drmModeEncoder *encoder;
		int idx;

		connector = get_connector_by_id(dev, pipe->con_ids[i]);
		if (!connector)
			return NULL;

		for (j = 0; j < connector->count_encoders; ++j) {
			encoder = get_encoder_by_id(dev, connector->encoders[j]);
			if (!encoder)
				continue;

			crtcs_for_connector |= encoder->possible_crtcs;

			idx = get_crtc_index(dev, encoder->crtc_id);
			if (idx >= 0)
				active_crtcs |= 1 << idx;
		}

		possible_crtcs &= crtcs_for_connector;
	}

	if (!possible_crtcs)
		return NULL;

	/* Return the first possible and active CRTC if one exists, or the first
	 * possible CRTC otherwise.
	 */
	if (possible_crtcs & active_crtcs)
		crtc_idx = ffs(possible_crtcs & active_crtcs);
	else
		crtc_idx = ffs(possible_crtcs);

	return &dev->resources->crtcs[crtc_idx - 1];
}

static int pipe_find_crtc_and_mode(struct device *dev, struct pipe_arg *pipe)
{
	drmModeModeInfo *mode = NULL;
	int i;

	pipe->mode = NULL;

	for (i = 0; i < (int)pipe->num_cons; i++) {
	//	if (pipe->wbc)
	//		continue;
		mode = connector_find_mode(dev, pipe->con_ids[i],
					   pipe->mode_str, pipe->vrefresh);
		if (mode == NULL) {
			if (pipe->vrefresh)
				fprintf(stderr,
				"failed to find mode "
				"\"%s-%.2fHz\" for connector %s\n",
				pipe->mode_str, pipe->vrefresh, pipe->cons[i]);
			else
				fprintf(stderr,
				"failed to find mode \"%s\" for connector %s\n",
				pipe->mode_str, pipe->cons[i]);
			return -EINVAL;
		}
	}

	/* If the CRTC ID was specified, get the corresponding CRTC. Otherwise
	 * locate a CRTC that can be attached to all the connectors.
	 */
	if (pipe->crtc_id != (uint32_t)-1) {
		for (i = 0; i < dev->resources->res->count_crtcs; i++) {
			struct crtc *crtc = &dev->resources->crtcs[i];

			if (pipe->crtc_id == crtc->crtc->crtc_id) {
				pipe->crtc = crtc;
				break;
			}
		}
	} else {
		pipe->crtc = pipe_find_crtc(dev, pipe);
	}

	if (!pipe->crtc) {
		fprintf(stderr, "failed to find CRTC for pipe\n");
		return -EINVAL;
	}

	pipe->mode = mode;
	if (!pipe->wbc)
		pipe->crtc->mode = mode;

	return 0;
}

/* -----------------------------------------------------------------------------
 * Properties
 */

struct property_arg {
	uint32_t obj_id;
	uint32_t obj_type;
	char name[DRM_PROP_NAME_LEN+1];
	uint32_t prop_id;
	uint64_t value;
	bool optional;
};

static bool set_property(struct device *dev, struct property_arg *p)
{
	drmModeObjectProperties *props = NULL;
	drmModePropertyRes **props_info = NULL;
	const char *obj_type;
	int ret;
	int i;

	p->obj_type = 0;
	p->prop_id = 0;

#define find_object(_res, __res, type, Type)					\
	do {									\
		for (i = 0; i < (int)(_res)->__res->count_##type##s; ++i) {	\
			struct type *obj = &(_res)->type##s[i];			\
			if (obj->type->type##_id != p->obj_id)			\
				continue;					\
			p->obj_type = DRM_MODE_OBJECT_##Type;			\
			obj_type = #Type;					\
			props = obj->props;					\
			props_info = obj->props_info;				\
		}								\
	} while(0)								\

	find_object(dev->resources, res, crtc, CRTC);
	if (p->obj_type == 0)
		find_object(dev->resources, res, connector, CONNECTOR);
	if (p->obj_type == 0)
		find_object(dev->resources, plane_res, plane, PLANE);
	if (p->obj_type == 0) {
		fprintf(stderr, "Object %i not found, can't set property\n",
			p->obj_id);
		return false;
	}

	if (!props) {
		fprintf(stderr, "%s %i has no properties\n",
			obj_type, p->obj_id);
		return false;
	}

	for (i = 0; i < (int)props->count_props; ++i) {
		if (!props_info[i])
			continue;
		if (strcmp(props_info[i]->name, p->name) == 0)
			break;
	}

	if (i == (int)props->count_props) {
		if (!p->optional)
			fprintf(stderr, "%s %i has no %s property\n",
				obj_type, p->obj_id, p->name);
		return false;
	}

	p->prop_id = props->props[i];

	if (!dev->use_atomic)
		ret = drmModeObjectSetProperty(dev->fd, p->obj_id, p->obj_type,
									   p->prop_id, p->value);
	else
		ret = drmModeAtomicAddProperty(dev->req, p->obj_id, p->prop_id, p->value);

	if (ret < 0)
		fprintf(stderr, "failed to set %s %i property %s to %" PRIu64 ": %s\n",
			obj_type, p->obj_id, p->name, p->value, strerror(errno));

	return true;
}

static void add_property(struct device *dev, uint32_t obj_id,
			       const char *name, uint64_t value)
{
	struct property_arg p;

	p.obj_id = obj_id;
	strcpy(p.name, name);
	p.value = value;

	set_property(dev, &p);
}

static int get_plane_num(unsigned int format)
{
	switch (format) {
	case DRM_FORMAT_NV12:
	case DRM_FORMAT_NV21:
	case DRM_FORMAT_NV16:
	case DRM_FORMAT_NV61:
	case DRM_FORMAT_NV12_10:
		return 2;
		break;
	default:
		return 1;
		break;
	};
}

static int atomic_set_plane(struct device *dev, struct plane_arg *p, const char *file_name, bool update)
{
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
	struct bo *plane_bo;
	int crtc_x, crtc_y, crtc_w, crtc_h;
	struct crtc *crtc = NULL;
	unsigned int i;
	unsigned int old_fb_id;
	uint64_t modifiers[4] = {0, 0, 0, 0};
	int ret;

	/* Find an unused plane which can be connected to our CRTC. Find the
	 * CRTC index first, then iterate over available planes.
	 */
	for (i = 0; i < (unsigned int)dev->resources->res->count_crtcs; i++) {
		if (p->crtc_id == dev->resources->res->crtcs[i]) {
			crtc = &dev->resources->crtcs[i];
			break;
		}
	}

	if (!crtc) {
		fprintf(stderr, "CRTC %u not found\n", p->crtc_id);
		return -1;
	}

	if (!update)
		fprintf(stderr, "testing %dx%d@%s on plane %u, crtc %u\n",
			p->w, p->h, p->format_str, p->plane_id, p->crtc_id);

	plane_bo = p->old_bo;
	p->old_bo = p->bo;

	if (!plane_bo) {
		plane_bo = ovl_bo_create(dev->fd, p->fourcc, p->afbc_en, p->w, p->h,
				     handles, pitches, offsets, file_name);

		if (plane_bo == NULL)
			return -1;

		if (p->afbc_en) {
			modifiers[0] = DRM_FORMAT_MOD_ARM_AFBC(1);
			if (get_plane_num(p->fourcc) == 2)
				modifiers[1] = DRM_FORMAT_MOD_ARM_AFBC(1);
			ret = drmModeAddFB2WithModifiers(dev->fd, p->w, p->h, p->fourcc, handles, pitches,
						   offsets, modifiers, &p->fb_id, DRM_MODE_FB_MODIFIERS);
		} else {
			ret = drmModeAddFB2(dev->fd, p->w, p->h, p->fourcc,
					    handles, pitches, offsets, &p->fb_id, 0);
		}

		if (ret) {
			fprintf(stderr, "failed to add fb: %s\n", strerror(errno));
			return -1;
		}
	}

	p->bo = plane_bo;

	old_fb_id = p->fb_id;
	p->old_fb_id = old_fb_id;

	if (p->rotation & (DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_270)) {
		crtc_w = p->h * p->scale;
		crtc_h = p->w * p->scale;
	} else {
		crtc_w = p->w * p->scale;
		crtc_h = p->h * p->scale;
	}
	if (!p->has_position) {
		/* Default to the middle of the screen */
		crtc_x = (crtc->mode->hdisplay - crtc_w) / 2;
		crtc_y = (crtc->mode->vdisplay - crtc_h) / 2;
	} else {
		crtc_x = p->x;
		crtc_y = p->y;
	}

	add_property(dev, p->plane_id, "FB_ID", p->fb_id);
	add_property(dev, p->plane_id, "CRTC_ID", p->crtc_id);
	add_property(dev, p->plane_id, "SRC_X", 0);
	add_property(dev, p->plane_id, "SRC_Y", 0);
	add_property(dev, p->plane_id, "SRC_W", p->w << 16);
	add_property(dev, p->plane_id, "SRC_H", p->h << 16);
	add_property(dev, p->plane_id, "CRTC_X", crtc_x);
	add_property(dev, p->plane_id, "CRTC_Y", crtc_y);
	add_property(dev, p->plane_id, "CRTC_W", crtc_w);
	add_property(dev, p->plane_id, "CRTC_H", crtc_h);
	add_property(dev, p->plane_id, "rotation", p->rotation);
	add_property(dev, p->plane_id, "zpos", p->zpos);

	return 0;
}

static void atomic_set_planes(struct device *dev, struct plane_arg *p,
			      unsigned int count, bool update)
{
	unsigned int i;

	/* set up planes */
	if (count > pic_cnt)
		fprintf(stderr, "no enough picture data for %d planes\n", count);

	for (i = 0; i < count; i++) {
		if (atomic_set_plane(dev, &p[i], pic_name[i], update))
			return;
	}
}

static void atomic_clear_planes(struct device *dev, struct plane_arg *p, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		add_property(dev, p[i].plane_id, "FB_ID", 0);
		add_property(dev, p[i].plane_id, "CRTC_ID", 0);
		add_property(dev, p[i].plane_id, "SRC_X", 0);
		add_property(dev, p[i].plane_id, "SRC_Y", 0);
		add_property(dev, p[i].plane_id, "SRC_W", 0);
		add_property(dev, p[i].plane_id, "SRC_H", 0);
		add_property(dev, p[i].plane_id, "CRTC_X", 0);
		add_property(dev, p[i].plane_id, "CRTC_Y", 0);
		add_property(dev, p[i].plane_id, "CRTC_W", 0);
		add_property(dev, p[i].plane_id, "CRTC_H", 0);
	}
}

static void atomic_clear_FB(struct device *dev, struct plane_arg *p, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (p[i].fb_id) {
			drmModeRmFB(dev->fd, p[i].fb_id);
			p[i].fb_id = 0;
		}
		if (p[i].old_fb_id) {
			drmModeRmFB(dev->fd, p[i].old_fb_id);
			p[i].old_fb_id = 0;
		}
		if (p[i].bo) {
			bo_destroy(p[i].bo);
			p[i].bo = NULL;
		}
		if (p[i].old_bo) {
			bo_destroy(p[i].old_bo);
			p[i].old_bo = NULL;
		}

	}
}

static int atomic_add_wbc_fb(struct device *dev, struct pipe_arg *pipe)
{
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
	uint32_t w, h;
	struct bo *pipe_bo;
	int ret;

	pipe_bo = pipe->old_bo;
	pipe->old_bo = pipe->bo;

	w = pipe->mode->hdisplay;
	h =  pipe->mode->vdisplay;
	if (!pipe_bo) {

		pipe_bo = ovl_bo_create(dev->fd, pipe->fourcc, false, w, h,
					handles, pitches, offsets, NULL);

		if (pipe_bo == NULL)
			return -1;

		ret = drmModeAddFB2(dev->fd, w, h, pipe->fourcc,
					handles, pitches, offsets, &pipe->fb_id, 0);

		if (ret) {
			fprintf(stderr, "failed to add fb: %s\n", strerror(errno));
			return -1;
		}
		pipe->bo = pipe_bo;
	}

	return 0;

}

static int get_bpp(int fourcc)
{
	int bpp = 0;

	switch (fourcc)
	{
	case DRM_FORMAT_NV12:
		bpp = 12;
		break;
	case DRM_FORMAT_RGB565:
		bpp = 16;
		break;
	case DRM_FORMAT_RGB888:
		bpp = 24;
		break;
	case DRM_FORMAT_ARGB8888:
		bpp = 32;
		break;
	default:
		fprintf(stderr, "unsupported format: %x\n", fourcc);
	}
	return bpp;
}

static void write_wb_file(struct pipe_arg *pipes, unsigned int count)
{
	unsigned int i;
	unsigned int w, h;
	int fd;

	for (i = 0; i < count; i++) {
		struct pipe_arg *pipe = &pipes[i];
		w = pipe->mode->hdisplay;
		h =  pipe->mode->vdisplay;

		if (pipe->wbc) {
			/*
			 * wait for writeback complete.
			 */
			sleep(1);
			fd = open("/data/wb.bin", O_WRONLY| O_TRUNC | O_CREAT, 0666);
			if (fd == -1) {
				printf("Failed to open wb file : %s\n", strerror(errno));
				return;
			}
			printf("write data to /data/wb.bin ...");
			write(fd, pipe->bo->ptr, w * h * get_bpp(pipe->fourcc)>>3);
			printf("done\n");
		}
	}

}

static void atomic_set_mode(struct device *dev, struct pipe_arg *pipes, unsigned int count)
{
	unsigned int i;
	unsigned int j;
	int ret;

	for (i = 0; i < count; i++) {
		struct pipe_arg *pipe = &pipes[i];

		ret = pipe_find_crtc_and_mode(dev, pipe);
		if (ret < 0)
			continue;
	}

	for (i = 0; i < count; i++) {
		struct pipe_arg *pipe = &pipes[i];
		uint32_t blob_id;

		if (pipe->mode == NULL)
			continue;

		if (!pipe->wbc)
			printf("setting mode %s-%.2fHz on connectors ",
				pipe->mode->name, mode_vrefresh(pipe->mode));
		for (j = 0; j < pipe->num_cons; ++j) {
			add_property(dev, pipe->con_ids[j], "CRTC_ID", pipe->crtc->crtc->crtc_id);
		}
		printf("crtc %d\n", pipe->crtc->crtc->crtc_id);
		if (pipe->wbc) {
			atomic_add_wbc_fb(dev, pipe);
			printf("write back connector fb_id :%d\n", pipe->fb_id);
			add_property(dev, pipe->con_ids[0], "WRITEBACK_FB_ID", pipe->fb_id);
		} else {

			drmModeCreatePropertyBlob(dev->fd, pipe->mode, sizeof(*pipe->mode), &blob_id);
			add_property(dev, pipe->crtc->crtc->crtc_id, "MODE_ID", blob_id);
			add_property(dev, pipe->crtc->crtc->crtc_id, "ACTIVE", 1);
		}
	}
}

static void atomic_clear_mode(struct device *dev, struct pipe_arg *pipes, unsigned int count)
{
	unsigned int i;
	unsigned int j;

	for (i = 0; i < count; i++) {
		struct pipe_arg *pipe = &pipes[i];

		if (pipe->mode == NULL)
			continue;

		for (j = 0; j < pipe->num_cons; ++j)
			add_property(dev, pipe->con_ids[j], "CRTC_ID",0);

		add_property(dev, pipe->crtc->crtc->crtc_id, "MODE_ID", 0);
		add_property(dev, pipe->crtc->crtc->crtc_id, "ACTIVE", 0);
	}
}

#define min(a, b)	((a) < (b) ? (a) : (b))

static int parse_connector(struct pipe_arg *pipe, const char *arg)
{
	unsigned int len;
	unsigned int i;
	const char *p;
	char *endp;

	pipe->vrefresh = 0;
	pipe->crtc_id = (uint32_t)-1;
	strcpy(pipe->format_str, "XR24");

	/* Count the number of connectors and allocate them. */
	pipe->num_cons = 1;
	for (p = arg; *p && *p != ':' && *p != '@'; ++p) {
		if (*p == ',')
			pipe->num_cons++;
	}

	pipe->con_ids = calloc(pipe->num_cons, sizeof(*pipe->con_ids));
	pipe->cons = calloc(pipe->num_cons, sizeof(*pipe->cons));
	if (pipe->con_ids == NULL || pipe->cons == NULL)
		return -1;

	/* Parse the connectors. */
	for (i = 0, p = arg; i < pipe->num_cons; ++i, p = endp + 1) {
		endp = strpbrk(p, ",@:");
		if (!endp)
			break;

		pipe->cons[i] = strndup(p, endp - p);

		if (*endp != ',')
			break;
	}

	if (i != pipe->num_cons - 1)
		return -1;

	/* Parse the remaining parameters. */
	if (!endp)
		return -1;
	if (*endp == '@') {
		arg = endp + 1;
		pipe->crtc_id = strtoul(arg, &endp, 10);
	}
	if (*endp != ':')
		return -1;

	arg = endp + 1;

	/* Search for the vertical refresh or the format. */
	p = strpbrk(arg, "-@");
	if (p == NULL)
		p = arg + strlen(arg);
	len = min(sizeof pipe->mode_str - 1, (unsigned int)(p - arg));
	strncpy(pipe->mode_str, arg, len);
	pipe->mode_str[len] = '\0';

	if (*p == '-') {
		pipe->vrefresh = strtof(p + 1, &endp);
		p = endp;
	}

	if (*p == '@') {
		strncpy(pipe->format_str, p + 1, 4);
		pipe->format_str[4] = '\0';
		if (strstr(p + 5, "@WBC"))
			pipe->wbc = true;

	}

	pipe->fourcc = util_format_fourcc(pipe->format_str);
	if (pipe->fourcc == 0)  {
		fprintf(stderr, "unknown format %s\n", pipe->format_str);
		return -1;
	}

	return 0;
}

static int parse_plane(struct plane_arg *plane, const char *p)
{
	char *end;

	plane->plane_id = strtoul(p, &end, 10);
	if (*end != '@')
		return -EINVAL;

	p = end + 1;
	plane->crtc_id = strtoul(p, &end, 10);

	if (*end != ':')
		return -EINVAL;

	p = end + 1;
	plane->w = strtoul(p, &end, 10);
	if (*end != 'x')
		return -EINVAL;

	p = end + 1;
	plane->h = strtoul(p, &end, 10);

	if (*end == '+' || *end == '-') {
		plane->x = strtol(end, &end, 10);
		if (*end != '+' && *end != '-')
			return -EINVAL;
		plane->y = strtol(end, &end, 10);

		plane->has_position = true;
	}

	if (*end == '*') {
		p = end + 1;
		plane->scale = strtod(p, &end);
		if (plane->scale <= 0.0)
			return -EINVAL;
	} else {
		plane->scale = 1.0;
	}

	if (*end == '@') {
		strncpy(plane->format_str, end + 1, 4);
		plane->format_str[4] = '\0';
		if (strstr(end + 5, "@afbc"))
			plane->afbc_en = true;

	} else {
		strcpy(plane->format_str, "XR24");
	}

	if (strstr(end, "@rotatex"))
		plane->rotation |= DRM_MODE_REFLECT_X;
	if (strstr(end, "@rotatey"))
		plane->rotation |= DRM_MODE_REFLECT_Y;
	if (strstr(end, "@rotate90"))
		plane->rotation |= DRM_MODE_ROTATE_90;
	else if (strstr(end, "@rotate270"))
		plane->rotation |= DRM_MODE_ROTATE_270;
	else
		plane->rotation |= DRM_MODE_ROTATE_0;

	plane->zpos = zpos++;

	plane->fourcc = util_format_fourcc(plane->format_str);
	if (plane->fourcc == 0) {
		fprintf(stderr, "unknown format %s\n", plane->format_str);
		return -EINVAL;
	}

	return 0;
}

static int parse_property(struct property_arg *p, const char *arg)
{
	if (sscanf(arg, "%d:%32[^:]:%" SCNu64, &p->obj_id, p->name, &p->value) != 3)
		return -1;

	p->obj_type = 0;
	p->name[DRM_PROP_NAME_LEN] = '\0';

	return 0;
}

static void parse_pictures(char *arg)
{
	char *name = strtok(arg, ",");

	while (name) {
		strcpy(pic_name[pic_cnt], name);
		if ((++pic_cnt) >= PIC_MAX_CNT) {
			fprintf(stderr, "max picture number: %d\n", PIC_MAX_CNT);
			break;
		}
		name = strtok(NULL, ",");
	}

}

static void usage(char *name)
{
	fprintf(stderr, "overlay test, libdrm version: 2.4.101\n");
	fprintf(stderr, "usage: %s [-acDdefMPpsCvw]\n", name);

	fprintf(stderr, "\n Query options:\n\n");
	fprintf(stderr, "\t-c\tlist connectors\n");
	fprintf(stderr, "\t-e\tlist encoders\n");
	fprintf(stderr, "\t-f\tlist framebuffers\n");
	fprintf(stderr, "\t-p\tlist CRTCs and planes (pipes)\n");

	fprintf(stderr, "\n Test options:\n\n");
	fprintf(stderr, "\t-P <plane_id>@<crtc_id>:<w>x<h>[+<x>+<y>][*<scale>][@<format>][@afbc][@rotatex/y/90/270]\tset a plane\n");
	fprintf(stderr, "\t-s <connector_id>[,<connector_id>][@<crtc_id>]:[#<mode index>]<mode>[-<vrefresh>][@<format>]\tset a mode\n");
	fprintf(stderr, "\t-C\ttest hw cursor\n");
	fprintf(stderr, "\t-v\ttest vsynced page flipping\n");
	fprintf(stderr, "\t-o\ttest dynamic turn on off plane one by one, run with -v mode\n");
	fprintf(stderr, "\t-w <obj_id>:<prop_name>:<value>\tset property\n");
	fprintf(stderr, "\t-a \tuse atomic API\n");
	fprintf(stderr, "\t-F pattern1,pattern2\tspecify fill patterns\n");

	fprintf(stderr, "\n Generic options:\n\n");
	fprintf(stderr, "\t-d\tdrop master after mode set\n");
	fprintf(stderr, "\t-M module\tuse the given driver\n");
	fprintf(stderr, "\t-D device\tuse the given device\n");

	fprintf(stderr, "\n\tDefault is to dump all info.\n");
	exit(0);
}

static int pipe_resolve_connectors(struct device *dev, struct pipe_arg *pipe)
{
	drmModeConnector *connector;
	unsigned int i;
	uint32_t id;
	char *endp;

	for (i = 0; i < pipe->num_cons; i++) {
		id = strtoul(pipe->cons[i], &endp, 10);
		if (endp == pipe->cons[i]) {
			connector = get_connector_by_name(dev, pipe->cons[i]);
			if (!connector) {
				fprintf(stderr, "no connector named '%s'\n",
					pipe->cons[i]);
				return -ENODEV;
			}

			id = connector->connector_id;
		}

		pipe->con_ids[i] = id;
	}

	return 0;
}

static char optstr[] = "acdD:efF:M:P:ps:Cvw:o";

int main(int argc, char **argv)
{
	struct device dev;

	int c;
	int encoders = 0, connectors = 0, crtcs = 0, planes = 0, framebuffers = 0;
	int drop_master = 0;
	int test_vsync = 0;
	int use_atomic = 0;
	int dynamic_onoff = 0;
	char *device = NULL;
	char *module = NULL;
	unsigned int i;
	unsigned int count = 0, plane_count = 0;
	unsigned int prop_count = 0;
	struct pipe_arg *pipe_args = NULL;
	struct plane_arg *plane_args = NULL;
	struct plane_arg *c_plane_args = NULL;
	struct property_arg *prop_args = NULL;
	unsigned int args = 0;
	unsigned int c_plane_count = 0;
	unsigned int c_count = 0;
	bool c_increase_mode;
	int ret;

	memset(&dev, 0, sizeof dev);

	opterr = 0;
	while ((c = getopt(argc, argv, optstr)) != -1) {
		args++;

		switch (c) {
		case 'a':
			use_atomic = 1;
			break;
		case 'c':
			connectors = 1;
			break;
		case 'D':
			device = optarg;
			args--;
			break;
		case 'd':
			drop_master = 1;
			break;
		case 'e':
			encoders = 1;
			break;
		case 'f':
			framebuffers = 1;
			break;
		case 'F':
			parse_pictures(optarg);
			break;
		case 'M':
			module = optarg;
			/* Preserve the default behaviour of dumping all information. */
			args--;
			break;
		case 'o':
			dynamic_onoff = 1;
			break;
		case 'P':
			plane_args = realloc(plane_args,
					     (plane_count + 1) * sizeof *plane_args);
			if (plane_args == NULL) {
				fprintf(stderr, "memory allocation failed\n");
				return 1;
			}
			memset(&plane_args[plane_count], 0, sizeof(*plane_args));

			if (parse_plane(&plane_args[plane_count], optarg) < 0)
				usage(argv[0]);

			plane_count++;
			break;
		case 'p':
			crtcs = 1;
			planes = 1;
			break;
		case 's':
			pipe_args = realloc(pipe_args,
					    (count + 1) * sizeof *pipe_args);
			if (pipe_args == NULL) {
				fprintf(stderr, "memory allocation failed\n");
				return 1;
			}
			memset(&pipe_args[count], 0, sizeof(*pipe_args));

			if (parse_connector(&pipe_args[count], optarg) < 0)
				usage(argv[0]);

			count++;
			break;
		case 'v':
			test_vsync = 1;
			break;
		case 'w':
			prop_args = realloc(prop_args,
					   (prop_count + 1) * sizeof *prop_args);
			if (prop_args == NULL) {
				fprintf(stderr, "memory allocation failed\n");
				return 1;
			}
			memset(&prop_args[prop_count], 0, sizeof(*prop_args));

			if (parse_property(&prop_args[prop_count], optarg) < 0)
				usage(argv[0]);

			prop_count++;
			break;
		default:
			usage(argv[0]);
			break;
		}
	}

	if (!args || (args == 1 && use_atomic))
		encoders = connectors = crtcs = planes = framebuffers = 1;

	dev.fd = util_open(device, module);
	if (dev.fd < 0)
		return -1;

	ret = drmSetClientCap(dev.fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret && use_atomic) {
		fprintf(stderr, "no atomic modesetting support: %s\n", strerror(errno));
		drmClose(dev.fd);
		return -1;
	}

	dev.use_atomic = 1;

	if (test_vsync && !count) {
		fprintf(stderr, "page flipping requires at least one -s option.\n");
		return -1;
	}

	dev.resources = get_resources(&dev);
	if (!dev.resources) {
		drmClose(dev.fd);
		return 1;
	}

	for (i = 0; i < count; i++) {
		if (pipe_resolve_connectors(&dev, &pipe_args[i]) < 0) {
			free_resources(dev.resources);
			drmClose(dev.fd);
			return 1;
		}
	}

#define dump_resource(dev, res) if (res) dump_##res(dev)

	dump_resource(&dev, encoders);
	dump_resource(&dev, connectors);
	dump_resource(&dev, crtcs);
	dump_resource(&dev, planes);
	dump_resource(&dev, framebuffers);

	for (i = 0; i < prop_count; ++i)
		set_property(&dev, &prop_args[i]);

	dev.req = drmModeAtomicAlloc();

	if (count && plane_count) {
		uint64_t cap = 0;

		ret = drmGetCap(dev.fd, DRM_CAP_DUMB_BUFFER, &cap);
		if (ret || cap == 0) {
			fprintf(stderr, "driver doesn't support the dumb buffer API\n");
			return 1;
		}

		atomic_set_mode(&dev, pipe_args, count);
		atomic_set_planes(&dev, plane_args, plane_count, false);

		ret = drmModeAtomicCommit(dev.fd, dev.req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (ret) {
			fprintf(stderr, "Atomic Commit failed [1]\n");
			return 1;
		}

		gettimeofday(&pipe_args->start, NULL);
		pipe_args->swap_count = 0;
		write_wb_file(pipe_args, count);

		if (test_vsync) {
			c_plane_args = calloc(1, plane_count * sizeof(*c_plane_args));
			if (c_plane_args == NULL) {
				fprintf(stderr, "memory allocation for commit plane args failed\n");
				return 1;
			}
			c_plane_count = 1;
			c_increase_mode = true;
		}

		while (test_vsync) {
			drmModeAtomicFree(dev.req);
			dev.req = drmModeAtomicAlloc();
			if (dynamic_onoff) {
				memcpy(c_plane_args, plane_args, sizeof(*c_plane_args));
				atomic_set_planes(&dev, plane_args, c_plane_count, true);
				atomic_clear_planes(&dev, &plane_args[c_plane_count], plane_count - c_plane_count);
			} else {
				atomic_set_planes(&dev, plane_args, plane_count, true);
			}
			ret = drmModeAtomicCommit(dev.fd, dev.req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
			if (ret) {
				fprintf(stderr, "Atomic Commit failed [2]\n");
				return 1;
			}

			pipe_args->swap_count++;
			if (pipe_args->swap_count == 60) {
				struct timeval end;
				double t;

				gettimeofday(&end, NULL);
				t = end.tv_sec + end.tv_usec * 1e-6 -
			    (pipe_args->start.tv_sec + pipe_args->start.tv_usec * 1e-6);
				fprintf(stderr, "freq: %.02fHz\n", pipe_args->swap_count / t);
				pipe_args->swap_count = 0;
				pipe_args->start = end;

				c_count++;
				/* turn on or off plane one by one every 30s */
				if (c_count == 1) {
					c_count = 0;
					if (c_increase_mode)
						c_plane_count++;
					else
						c_plane_count--;

					if (c_plane_count >= plane_count) {
						c_increase_mode = false; /* decrease plane one by one*/
					}

					if (c_plane_count == 1)
						c_increase_mode = true;
				}
			}
		}

		if (drop_master)
			drmDropMaster(dev.fd);

		getchar();

		drmModeAtomicFree(dev.req);
		dev.req = drmModeAtomicAlloc();

		atomic_clear_mode(&dev, pipe_args, count);
		atomic_clear_planes(&dev, plane_args, plane_count);
		ret = drmModeAtomicCommit(dev.fd, dev.req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (ret) {
			fprintf(stderr, "Atomic Commit failed\n");
			return 1;
		}

		atomic_clear_FB(&dev, plane_args, plane_count);
	}

	drmModeAtomicFree(dev.req);

	free_resources(dev.resources);

	return 0;
}
