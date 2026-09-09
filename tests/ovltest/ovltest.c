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
#include <pthread.h>
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

#define PIC_NAME_MAX_LEN	256
#define PIC_MAX_CNT		8

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

	int error_event_fd;

	struct resources *resources;

	struct {
		unsigned int width;
		unsigned int height;

		unsigned int fb_id;
		struct bo *bo;
		struct bo *cursor_bo;
	} mode;

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
	static char mod_string[4096];

	char *modifier_name = drmGetFormatModifierName(modifier);
	char *vendor_name = drmGetFormatModifierVendor(modifier);
	memset(mod_string, 0x00, sizeof(mod_string));

	if (!modifier_name) {
		if (vendor_name)
			snprintf(mod_string, sizeof(mod_string), "%s_%s",
				 vendor_name, "UNKNOWN_MODIFIER");
		else
			snprintf(mod_string, sizeof(mod_string), "%s_%s",
				 "UNKNOWN_VENDOR", "UNKNOWN_MODIFIER");
		/* safe, as free is no-op for NULL */
		free(vendor_name);
		return mod_string;
	}

	if (modifier == DRM_FORMAT_MOD_LINEAR) {
		snprintf(mod_string, sizeof(mod_string), "%s", modifier_name);
		free(modifier_name);
		free(vendor_name);
		return mod_string;
	}

	snprintf(mod_string, sizeof(mod_string), "%s_%s",
		 vendor_name, modifier_name);

	free(modifier_name);
	free(vendor_name);
	return mod_string;
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
	drmModeModeInfo *owned_mode;
	uint32_t mode_blob_id;

	/* Is write back connector */
	bool wbc;

	/* AFBC format options for writeback connector */
	bool afbc_en;
	bool afbc_ytr_en;
	bool afbc_split_en;
	bool afbc_sparse_en;
	bool rfbc_en;  /* RFBC modifier support */
	uint32_t block_w;  /* 16=16x16, 32=32x8, 64=64x4 */
	uint32_t block_h;  /* 8=16x8, 16=16x16, 32=32x16, 4=64x4 */

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
	bool afbc_ytr_en;
	bool afbc_split_en;
	bool afbc_sparse_en;
	bool tiled_en;
	bool rfbc_en;  /* RFBC modifier support */
	bool afrc_en;  /* AFRC modifier support */
	bool afrc_scan;  /* AFRC scanline layout, otherwise rotation-optimised */
	uint32_t afrc_cu_size;  /* AFRC coding unit size: 16, 24 or 32 bytes */
	uint32_t tile_mode;  /* 0=8x8, 2=4x4_MODE0, 3=4x4_MODE1 */
	uint32_t block_h;  /* 8=16x8, 16=16x16, 32=32x16, 4=64x4 */
	uint32_t block_w;
	int32_t rotation;
	int32_t x, y;
	uint32_t w, h;
	uint32_t crtc_w, crtc_h;
	uint32_t stride;
	uint32_t zpos;
	double scale;
	unsigned int fb_id;
	unsigned int old_fb_id;
	struct bo *bo;
	struct bo *old_bo;
	char format_str[5]; /* need to leave room for terminating \0 */
	unsigned int fourcc;
};

struct fbc_format {
	bool afbc_en;
	bool rfbc_en;
	bool tiled_en;
	bool afrc_en;
	bool afrc_scan;
	uint32_t afrc_cu_size;
	bool afbc_ytr_en;
	bool afbc_split_en;
	bool afbc_sparse_en;
	uint32_t tile_mode;
	uint32_t block_w;
	uint32_t block_h;
};

struct error_event {
	struct device *dev;
	pthread_t monitor_thread;
};

static drmModeModeInfo user_mode;

static int create_custom_mode(drmModeModeInfo *mode, const char *mode_str,
	const float vrefresh)
{
	uint32_t hdisplay, vdisplay;
	uint32_t hblank, vblank;
	uint32_t hfront_porch, hsync;
	uint32_t vfront_porch, vsync;
	float refresh;

	/* Parse resolution string like "3840x2160" */
	if (sscanf(mode_str, "%u x %u", &hdisplay, &vdisplay) != 2) {
		if (sscanf(mode_str, "%ux%u", &hdisplay, &vdisplay) != 2)
			return -EINVAL;
	}

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
	mode->clock = (uint32_t)roundf(mode->htotal * mode->vtotal * refresh / 1000.0f);
	mode->vrefresh = (uint16_t)roundf(refresh);

	/* Set mode name */
	snprintf(mode->name, sizeof(mode->name), "%dx%d@%.0f", hdisplay, vdisplay, refresh);

	/* Set standard mode flags */
	mode->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
	mode->type = DRM_MODE_TYPE_USERDEF;

	return 0;
}

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
	drmModeConnector *connector;
	drmModeModeInfo *mode = NULL;
	uint32_t hdisplay, vdisplay;
	int i;

	pipe->mode = NULL;

	for (i = 0; i < (int)pipe->num_cons; i++) {

		connector = get_connector_by_id(dev, pipe->con_ids[i]);
		if (!connector)
			continue;
	        if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
			pipe->wbc = true;
			if (!pipe->owned_mode)
				pipe->owned_mode = calloc(1, sizeof(*mode));
			mode = pipe->owned_mode;
			if (!mode) {
				fprintf(stderr, "out of memory for writeback connector mode\n");
				return -ENOMEM;
			}

			if (sscanf(pipe->mode_str, "%u x %u", &hdisplay, &vdisplay) != 2 ) {
				fprintf(stderr, "scan width and height for writeback failed\n");
				return -EINVAL;
			}
			mode->hdisplay = hdisplay;
			mode->vdisplay = vdisplay;
			continue;
		}

		mode = connector_find_mode(dev, pipe->con_ids[i],
					   pipe->mode_str, pipe->vrefresh);
		if (mode == NULL) {
			fprintf(stderr,
				"failed to find mode \"%s\" for connector %s, using custom mode\n",
				pipe->mode_str, pipe->cons[i]);

			if (create_custom_mode(&user_mode, pipe->mode_str, pipe->vrefresh) < 0) {
				fprintf(stderr,
					"failed to create custom mode from \"%s\"\n",
					pipe->mode_str);
				return -EINVAL;
			}
			mode = &user_mode;
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

struct test_state {
	int encoders;
	int connectors;
	int crtcs;
	int planes;
	int framebuffers;
	int drop_master;
	int test_vsync;
	int dynamic_onoff;
	bool one_shot;
	bool error_monitor;
	char *device;
	char *module;
	struct pipe_arg *pipes;
	unsigned int pipe_count;
	struct plane_arg *plane_args;
	unsigned int plane_count;
	struct property_arg *properties;
	unsigned int property_count;
	char pictures[PIC_MAX_CNT][PIC_NAME_MAX_LEN];
	unsigned int picture_count;
	unsigned int zpos;
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
		return p->optional;
	}

	p->prop_id = props->props[i];

	ret = drmModeAtomicAddProperty(dev->req, p->obj_id, p->prop_id, p->value);

	if (ret < 0) {
		fprintf(stderr, "failed to set %s %i property %s to %" PRIu64 ": %s\n",
			obj_type, p->obj_id, p->name, p->value, strerror(-ret));
		return false;
	}

	return true;
}

static int add_property(struct device *dev, uint32_t obj_id, const char *name, uint64_t value)
{
	struct property_arg property = {
		.obj_id = obj_id,
		.value = value,
	};

	strcpy(property.name, name);

	return set_property(dev, &property) ? 0 : -EINVAL;
}

static int add_property_optional(struct device *dev, uint32_t obj_id,
				 const char *name, uint64_t value)
{
	struct property_arg property = {
		.obj_id = obj_id,
		.value = value,
		.optional = true,
	};

	strcpy(property.name, name);

	return set_property(dev, &property) ? 0 : -EINVAL;
}

static int get_plane_num(unsigned int format)
{
	switch (format) {
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
		plane_bo = ovl_bo_create(dev->fd, p->fourcc, p->afbc_en, p->stride, p->h,
				     handles, pitches, offsets, file_name);

		if (plane_bo == NULL)
			return -1;

		if (p->afbc_en || p->tiled_en || p->rfbc_en || p->afrc_en) {
			uint64_t afrc_cu_size;

			if (p->afbc_en && p->block_w == 32)
				modifiers[0] = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8);
			else if (p->afbc_en && p->afbc_ytr_en && p->block_w == 16)
				modifiers[0] = DRM_FORMAT_MOD_ARM_AFBC( 1 | AFBC_FORMAT_MOD_YTR);
			else if (p->afbc_en && p->block_w == 16)
				modifiers[0] = DRM_FORMAT_MOD_ARM_AFBC(1);
			else if (p->afrc_en) {
				if (p->afrc_cu_size == 32)
					afrc_cu_size = AFRC_FORMAT_MOD_CU_SIZE_32;
				else if (p->afrc_cu_size == 24)
					afrc_cu_size = AFRC_FORMAT_MOD_CU_SIZE_24;
				else
					afrc_cu_size = AFRC_FORMAT_MOD_CU_SIZE_16;

				modifiers[0] = DRM_FORMAT_MOD_ARM_AFRC(AFRC_FORMAT_MOD_CU_SIZE_P0(afrc_cu_size));
				if (p->afrc_scan)
					modifiers[0] |= AFRC_FORMAT_MOD_LAYOUT_SCAN;
			} else if (p->tiled_en) {
				if (p->tile_mode == 2)
					modifiers[0] = DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0);
				else
					modifiers[0] = DRM_FORMAT_MOD_ROCKCHIP_TILED(1);
			} else if (p->rfbc_en) {
				modifiers[0] = DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4);
			}

			if (modifiers[0] && p->afbc_split_en)
				modifiers[0] |= AFBC_FORMAT_MOD_SPLIT;

			if (modifiers[0] && p->afbc_sparse_en)
				modifiers[0] |= AFBC_FORMAT_MOD_SPARSE;

			if (get_plane_num(p->fourcc) == 2)
				modifiers[1] = modifiers[0];

			ret = drmModeAddFB2WithModifiers(dev->fd, p->w, p->h, p->fourcc, handles, pitches,
						   offsets, modifiers, &p->fb_id, DRM_MODE_FB_MODIFIERS);
		} else {
			ret = drmModeAddFB2(dev->fd, p->w, p->h, p->fourcc,
					    handles, pitches, offsets, &p->fb_id, 0);
		}

		if (ret) {
			fprintf(stderr, "failed to add fb: %s\n", strerror(errno));
			bo_destroy(plane_bo);
			return -1;
		}
	}

	p->bo = plane_bo;

	old_fb_id = p->fb_id;
	p->old_fb_id = old_fb_id;

	if (p->rotation & (DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_270)) {
		crtc_w = p->crtc_w ? p->crtc_h : p->h * p->scale;
		crtc_h = p->crtc_h ? p->crtc_w : p->w * p->scale;
	} else {
		crtc_w = p->crtc_w ? p->crtc_w : p->w * p->scale;
		crtc_h = p->crtc_h ? p->crtc_h : p->h * p->scale;
	}

	if (crtc_w > crtc->mode->hdisplay)
		crtc_w = crtc->mode->hdisplay;

	if (crtc_h > crtc->mode->vdisplay)
		crtc_h = crtc->mode->vdisplay;

	if (!p->has_position) {
		/* Default to the middle of the screen */
		crtc_x = (crtc->mode->hdisplay - crtc_w) / 2;
		crtc_y = (crtc->mode->vdisplay - crtc_h) / 2;
	} else {
		crtc_x = p->x;
		crtc_y = p->y;
	}

	ret = add_property(dev, p->plane_id, "FB_ID", p->fb_id);
	if (!ret)
		ret = add_property(dev, p->plane_id, "CRTC_ID", p->crtc_id);
	if (!ret)
		ret = add_property(dev, p->plane_id, "SRC_X", 0);
	if (!ret)
		ret = add_property(dev, p->plane_id, "SRC_Y", 0);
	if (!ret)
		ret = add_property(dev, p->plane_id, "SRC_W", p->w << 16);
	if (!ret)
		ret = add_property(dev, p->plane_id, "SRC_H", p->h << 16);
	if (!ret)
		ret = add_property(dev, p->plane_id, "CRTC_X", crtc_x);
	if (!ret)
		ret = add_property(dev, p->plane_id, "CRTC_Y", crtc_y);
	if (!ret)
		ret = add_property(dev, p->plane_id, "CRTC_W", crtc_w);
	if (!ret)
		ret = add_property(dev, p->plane_id, "CRTC_H", crtc_h);
	if (!ret) {
		/* Default rotation is valid even without a rotation property. */
		if (p->rotation == DRM_MODE_ROTATE_0)
			ret = add_property_optional(dev, p->plane_id, "rotation", p->rotation);
		else
			ret = add_property(dev, p->plane_id, "rotation", p->rotation);
	}
	if (!ret)
		ret = add_property_optional(dev, p->plane_id, "zpos", p->zpos);

	return ret;
}

static int atomic_set_planes(struct device *dev, struct plane_arg *p,
			     unsigned int count, const char pictures[PIC_MAX_CNT][PIC_NAME_MAX_LEN],
			     unsigned int picture_count, bool update)
{
	unsigned int i;
	int ret;

	/* set up planes */
	if (count > picture_count)
		fprintf(stderr, "no enough picture data for %d planes\n", count);

	for (i = 0; i < count; i++) {
		ret = atomic_set_plane(dev, &p[i], pictures[i], update);
		if (ret < 0) {
			fprintf(stderr, "failed to set plane %d\n", i);
			return ret;
		}
	}

	return 0;
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

static int atomic_disable_unused(struct device *dev, const struct pipe_arg *pipes,
				 unsigned int pipe_count, const struct plane_arg *planes,
				 unsigned int plane_count)
{
	struct resources *res = dev->resources;
	unsigned int i, j, k;
	uint32_t id;
	bool used;
	int ret;

	if (!res || !res->res || !res->plane_res)
		return -ENODEV;

	for (i = 0; i < res->plane_res->count_planes; i++) {
		id = res->plane_res->planes[i];
		for (j = 0; j < plane_count; j++) {
			if (planes[j].plane_id == id)
				break;
		}
		if (j < plane_count)
			continue;

		ret = add_property(dev, id, "FB_ID", 0);
		if (!ret)
			ret = add_property(dev, id, "CRTC_ID", 0);
		if (ret)
			return ret;
	}

	for (i = 0; i < (unsigned int)res->res->count_connectors; i++) {
		drmModeConnector *connector = res->connectors[i].connector;

		id = connector->connector_id;
		used = false;
		for (j = 0; j < pipe_count && !used; j++) {
			for (k = 0; k < pipes[j].num_cons; k++) {
				if (pipes[j].con_ids[k] == id) {
					used = true;
					break;
				}
			}
		}
		if (used)
			continue;

		ret = add_property(dev, id, "CRTC_ID", 0);
		if (!ret && connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
			ret = add_property(dev, id, "WRITEBACK_FB_ID", 0);
		if (ret)
			return ret;
	}

	for (i = 0; i < (unsigned int)res->res->count_crtcs; i++) {
		id = res->crtcs[i].crtc->crtc_id;
		for (j = 0; j < pipe_count; j++) {
			if (pipes[j].crtc_id == id ||
			    (pipes[j].crtc && pipes[j].crtc->crtc->crtc_id == id))
				break;
		}
		if (j < pipe_count)
			continue;

		ret = add_property(dev, id, "ACTIVE", 0);
		if (!ret)
			ret = add_property(dev, id, "MODE_ID", 0);
		if (ret)
			return ret;
	}

	return 0;
}

static void destroy_mode_blobs(struct device *dev, struct test_state *state)
{
	unsigned int i;

	for (i = 0; i < state->pipe_count; i++) {
		if (!state->pipes[i].mode_blob_id)
			continue;

		if (drmModeDestroyPropertyBlob(dev->fd, state->pipes[i].mode_blob_id))
			fprintf(stderr, "failed to destroy mode blob %u\n", state->pipes[i].mode_blob_id);
		state->pipes[i].mode_blob_id = 0;
	}
}

static void release_test_buffers(struct device *dev, struct test_state *state)
{
	unsigned int i;
	unsigned int fb_id;
	unsigned int old_fb_id;
	struct bo *bo;
	struct bo *old_bo;

	for (i = 0; i < state->plane_count; i++) {
		struct plane_arg *plane = &state->plane_args[i];

		fb_id = plane->fb_id;
		old_fb_id = plane->old_fb_id;
		bo = plane->bo;
		old_bo = plane->old_bo;

		if (fb_id)
			drmModeRmFB(dev->fd, fb_id);
		if (old_fb_id && old_fb_id != fb_id)
			drmModeRmFB(dev->fd, old_fb_id);
		if (bo)
			bo_destroy(bo);
		if (old_bo && old_bo != bo)
			bo_destroy(old_bo);

		plane->fb_id = 0;
		plane->old_fb_id = 0;
		plane->bo = NULL;
		plane->old_bo = NULL;
	}

	for (i = 0; i < state->pipe_count; i++) {
		struct pipe_arg *pipe = &state->pipes[i];

		if (!pipe->wbc)
			continue;

		fb_id = pipe->fb_id;
		old_fb_id = pipe->old_fb_id;
		bo = pipe->bo;
		old_bo = pipe->old_bo;

		if (fb_id)
			drmModeRmFB(dev->fd, fb_id);
		if (old_fb_id && old_fb_id != fb_id)
			drmModeRmFB(dev->fd, old_fb_id);
		if (bo)
			bo_destroy(bo);
		if (old_bo && old_bo != bo)
			bo_destroy(old_bo);

		pipe->fb_id = 0;
		pipe->old_fb_id = 0;
		pipe->bo = NULL;
		pipe->old_bo = NULL;
	}
}

static int atomic_add_wbc_fb(struct device *dev, struct pipe_arg *pipe)
{
	uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
	uint64_t modifiers[4] = {0, 0, 0, 0};
	uint32_t w, h;
	struct bo *pipe_bo;
	int ret;

	pipe_bo = pipe->old_bo;
	pipe->old_bo = pipe->bo;

	w = pipe->mode->hdisplay;
	h =  pipe->mode->vdisplay;
	if (!pipe_bo) {
		pipe_bo = ovl_bo_create(dev->fd, pipe->fourcc, pipe->afbc_en || pipe->rfbc_en, w, h,
					handles, pitches, offsets, NULL);

		if (pipe_bo == NULL)
			return -1;

		/* Set modifiers based on AFBC/RFBC */
		if (pipe->afbc_en) {
			if (pipe->block_w == 32)
				modifiers[0] = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8);
			else if (pipe->afbc_ytr_en && pipe->block_w == 16)
				modifiers[0] = DRM_FORMAT_MOD_ARM_AFBC(1 | AFBC_FORMAT_MOD_YTR);
			else if (pipe->block_w == 16)
				modifiers[0] = DRM_FORMAT_MOD_ARM_AFBC(1);
			else if (pipe->block_w == 64)
				modifiers[0] = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_64x4);

			if (pipe->afbc_split_en)
				modifiers[0] |= AFBC_FORMAT_MOD_SPLIT;
			if (pipe->afbc_sparse_en)
				modifiers[0] |= AFBC_FORMAT_MOD_SPARSE;
		} else if (pipe->rfbc_en) {
			modifiers[0] = DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4);
		}

		/* For YUV formats, set modifier for second plane */
		if (get_plane_num(pipe->fourcc) == 2)
			modifiers[1] = modifiers[0];

		/* Add framebuffer */
		if (pipe->afbc_en || pipe->rfbc_en) {
			ret = drmModeAddFB2WithModifiers(dev->fd, w, h, pipe->fourcc, handles, pitches,
						   offsets, modifiers, &pipe->fb_id, DRM_MODE_FB_MODIFIERS);
		} else {
			ret = drmModeAddFB2(dev->fd, w, h, pipe->fourcc,
						handles, pitches, offsets, &pipe->fb_id, 0);
		}

		if (ret) {
			fprintf(stderr, "failed to add fb: %s\n", strerror(errno));
			bo_destroy(pipe_bo);
			return -1;
		}
		pipe->bo = pipe_bo;
	}

	return 0;
}

static void write_wb_file(struct pipe_arg *pipes, unsigned int count)
{
	unsigned int i;
	int fd;
	char filename[256] = "";
	char afbc_str[64] = "";

	for (i = 0; i < count; i++) {
		struct pipe_arg *pipe = &pipes[i];

		if (pipe->wbc) {
			/*
			 * wait for writeback complete.
			 */
			sleep(1);

			/* Build AFBC/RFBC info string */
			if (pipe->afbc_en) {
				snprintf(afbc_str, sizeof(afbc_str), "_afbc%dx%d",
					 (int)pipe->block_w, (int)pipe->block_h);
				if (pipe->afbc_split_en)
					strcat(afbc_str, "_split");
				if (pipe->afbc_sparse_en)
					strcat(afbc_str, "_sparse");
			} else if (pipe->rfbc_en) {
				snprintf(afbc_str, sizeof(afbc_str), "_rfbc%dx%d",
					 (int)pipe->block_w, (int)pipe->block_h);
			}

			/* Build filename with all info */
			snprintf(filename, sizeof(filename), "/data/wb_%dx%d_%s%s.bin",
				 pipe->mode->hdisplay, pipe->mode->vdisplay,
				 pipe->format_str, afbc_str);

			fd = open(filename, O_WRONLY| O_TRUNC | O_CREAT, 0666);
			if (fd == -1) {
				printf("Failed to open wb file : %s\n", strerror(errno));
				return;
			}
			printf("write data to %s ...\n", filename);
			write(fd, pipe->bo->ptr, pipe->bo->size);
			printf("done\n");
			close(fd);
		}
	}

}

enum rockchip_drm_error_event_type {
	ROCKCHIP_DRM_ERROR_IOMMU_FAULT    = (1 << 0),
	ROCKCHIP_DRM_ERROR_POST_BUF_EMPTY = (1 << 1),
};

static int drm_error_event_handler(int fd, drmEventContextPtr evctx)
{
	struct drm_event *e;
	struct drm_event_vblank *vblank;
	struct timespec time = { 0 };
	unsigned long diff_usec;
	char buffer[1024];
	int len, i, err;

	err = lseek(fd, 0, SEEK_SET);
	if (err < 0) {
		fprintf(stderr, "failed to seeking to vcnt: %s\n", strerror(errno));
		return -1;
	}

	len = read(fd, buffer, sizeof buffer);
	if (len < (int)sizeof *e) {
		fprintf(stderr, "read failed len: %d polled_time:%lu:%06lu %s\n", len, time.tv_sec, time.tv_nsec / 1000, strerror(errno));
		return -1;
	}
	clock_gettime(CLOCK_MONOTONIC, &time);

	i = 0;

	while (i < len) {
		e = (struct drm_event *)(buffer + i);
		fprintf(stderr, "e->length: %d read len: %d\n", e->length, len);

		switch (e->type) {
		case ROCKCHIP_DRM_ERROR_IOMMU_FAULT:
			vblank = (struct drm_event_vblank *) e;
			diff_usec = (time.tv_sec * 1000000 +  time.tv_nsec/1000) - (vblank->tv_sec * 1000000 + vblank->tv_usec);
			fprintf(stderr, "err_iommu_falut count: %-8d event_time: %d:%06d polled_time: %lu:%06lu diff: %06lu\n",
				vblank->sequence, vblank->tv_sec, vblank->tv_usec, time.tv_sec, time.tv_nsec/1000, diff_usec);
			break;
		case ROCKCHIP_DRM_ERROR_POST_BUF_EMPTY:
			vblank = (struct drm_event_vblank *) e;
			diff_usec = (time.tv_sec * 1000000 +  time.tv_nsec/1000) - (vblank->tv_sec * 1000000 + vblank->tv_usec);
			fprintf(stderr, "err_iommu_falut count: %-8d event_time: %d:%06d polled_time: %lu:%06lu diff: %06lu\n",
				vblank->sequence, vblank->tv_sec, vblank->tv_usec, time.tv_sec, time.tv_nsec/1000, diff_usec);
			break;
		default:
			break;
		}

		i += e->length ? e->length : sizeof(struct drm_event_vblank);
	}

	return 0;
}

static void *drm_error_monitor_thread(void *data) {
	struct error_event *evt = (struct error_event *)data;
	struct device *dev = evt->dev;
	struct pollfd poll_fds[1];
	char buffer[1024];
	int ret, fd;
	int i = 0;

	fd = dev->error_event_fd;
	poll_fds[0].fd = fd;
	poll_fds[0].events = POLLPRI;

	/*
	 * reference:
	 * tools/leds/led_hw_brightness_mon.c
	 * tools/testing/selftests/thermal/intel/power_floor/power_floor_test.c
	 * Documentation/driver-api/usb/usb.rst
	 */
	ret = read(fd, buffer, sizeof(buffer));
	fprintf(stderr, "read %d bytes before poll\n", ret);

	while (1) {
		fprintf(stderr, "start poll error event(%d)", ++i);
		poll_fds[0].revents = 0;
		ret = poll(poll_fds, 1, -1);
		fprintf(stderr, "polled error event(%d), ret: %d revents: 0x%08x\n",
			i, ret, poll_fds[0].revents);

		if (ret < 0) {
			fprintf(stderr, "error poll vcnt(ret=%d) : %s\n", ret, strerror(errno));
			continue;
		}

		//if (poll_fds[0].revents & POLLERR)
		//	continue;

		if (poll_fds[0].revents & (POLLPRI | POLLRDBAND))
			drm_error_event_handler(fd, NULL);
	}

	return NULL;
}

static void drm_create_error_monitor_thread(struct device *dev)
{
	struct error_event *evt;
	struct sched_param param;
	int policy;
	int ret;

	evt = calloc(1, sizeof(*evt));
	evt->dev = dev;

	ret = pthread_create(&evt->monitor_thread, NULL, drm_error_monitor_thread, (void *)evt);
	if (ret < 0)
		fprintf(stderr,"%s couldn't create display errr monitor thread\n", __func__);

	pthread_getschedparam(evt->monitor_thread, &policy, &param);
	param.sched_priority = sched_get_priority_max(SCHED_FIFO) ;
	pthread_setschedparam(evt->monitor_thread, SCHED_FIFO, &param);
}

static int atomic_set_mode(struct device *dev, struct pipe_arg *pipes,
			   unsigned int count, bool strict)
{
	unsigned int i;
	unsigned int j;
	int ret;
	int failed_pipes = 0;

	for (i = 0; i < count; i++) {
		struct pipe_arg *pipe = &pipes[i];

		ret = pipe_find_crtc_and_mode(dev, pipe);
		if (strict && (ret < 0 || !pipe->mode)) {
			fprintf(stderr, "failed to prepare pipe %u\n", i);
			return ret < 0 ? ret : -EINVAL;
		}
		if (ret < 0) {
			fprintf(stderr, "failed to find CRTC and mode for pipe %d\n", i);
			failed_pipes++;
			continue;
		}
	}

	if ((unsigned int)failed_pipes == count) {
		fprintf(stderr, "all pipes failed to find CRTC and mode\n");
		return -1;
	}

	for (i = 0; i < count; i++) {
		struct pipe_arg *pipe = &pipes[i];
		uint32_t blob_id;

		if (!pipe->mode)
			continue;

		if (!pipe->wbc)
			printf("setting mode %s-%.2fHz on connectors ",
				pipe->mode->name, mode_vrefresh(pipe->mode));
		for (j = 0; j < pipe->num_cons; ++j) {
			if (pipe->wbc)
				printf("writeback connector %s, ", pipe->cons[j]);
			else
				printf("%s, ", pipe->cons[j]);
			ret = add_property(dev, pipe->con_ids[j], "CRTC_ID",
					   pipe->crtc->crtc->crtc_id);
			if (ret)
				return ret;
		}
		printf("crtc %d\n", pipe->crtc->crtc->crtc_id);
		if (pipe->wbc) {
			ret = atomic_add_wbc_fb(dev, pipe);
			if (ret < 0) {
				fprintf(stderr, "failed to add writeback fb\n");
				return -1;
			}
			printf("write back %d x %d to fb_id :%d\n",
			       pipe->mode->hdisplay, pipe->mode->vdisplay, pipe->fb_id);
			ret = add_property(dev, pipe->con_ids[0], "WRITEBACK_FB_ID", pipe->fb_id);
			if (ret)
				return ret;
		} else {
			ret = drmModeCreatePropertyBlob(dev->fd, pipe->mode, sizeof(*pipe->mode), &blob_id);
			if (ret) {
				fprintf(stderr, "failed to create mode blob: %s\n", strerror(-ret));
				return ret;
			}
			pipe->mode_blob_id = blob_id;
			ret = add_property(dev, pipe->crtc->crtc->crtc_id, "MODE_ID", blob_id);
			if (!ret)
				ret = add_property(dev, pipe->crtc->crtc->crtc_id, "ACTIVE", 1);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int atomic_clear_mode(struct device *dev, struct pipe_arg *pipes, unsigned int count)
{
	unsigned int i;
	unsigned int j;
	int ret;
	int error = 0;

	for (i = 0; i < count; i++) {
		struct pipe_arg *pipe = &pipes[i];

		if (pipe->mode == NULL)
			continue;

		/*
		 * we only set fb and CRTC_ID to null to disable a writeback connector to
		 * avoid triger crtc disable/enable on the writeback display path.
		 */
		if (pipe->wbc) {
			ret = add_property(dev, pipe->con_ids[0], "WRITEBACK_FB_ID", 0);
			if (ret && !error)
				error = ret;
			ret = add_property(dev, pipe->con_ids[0], "CRTC_ID", 0);
			if (ret && !error)
				error = ret;
			continue;
		}

		for (j = 0; j < pipe->num_cons; ++j) {
			ret = add_property(dev, pipe->con_ids[j], "CRTC_ID", 0);
			if (ret && !error)
				error = ret;
		}

		ret = add_property(dev, pipe->crtc->crtc->crtc_id, "MODE_ID", 0);
		if (ret && !error)
			error = ret;
		ret = add_property(dev, pipe->crtc->crtc->crtc_id, "ACTIVE", 0);
		if (ret && !error)
			error = ret;
	}

	return error;
}

#define min(a, b)	((a) < (b) ? (a) : (b))

static void parse_format_options(struct fbc_format *format, const char *p,
				 bool parse_tile)
{
	if (strstr(p, "@afbc16x16")) {
		format->afbc_en = true;
		format->block_w = 16;
		format->block_h = 16;
	} else if (strstr(p, "@afbc32x8sparse")) {
		format->afbc_en = true;
		format->afbc_sparse_en = true;
		format->block_w = 32;
		format->block_h = 8;
	} else if (strstr(p, "@afbc32x8split")) {
		format->afbc_en = true;
		format->afbc_split_en = true;
		format->block_w = 32;
		format->block_h = 8;
	} else if (strstr(p, "@afbc32x8")) {
		format->afbc_en = true;
		format->block_w = 32;
		format->block_h = 8;
	} else if (strstr(p, "@afbc64x4")) {
		format->afbc_en = true;
		format->block_w = 64;
		format->block_h = 4;
	} else if (strstr(p, "@afbcsplitsparse")) {
		format->afbc_en = true;
		format->afbc_split_en = true;
		format->afbc_sparse_en = true;
		format->block_w = 16;
		format->block_h = 16;
	} else if (strstr(p, "@afbcsplit")) {
		format->afbc_en = true;
		format->afbc_split_en = true;
		format->block_w = 16;
		format->block_h = 16;
	} else if (strstr(p, "@afbcytr")) {
		format->afbc_en = true;
		format->afbc_ytr_en = true;
		format->block_w = 16;
		format->block_h = 16;
	} else if (strstr(p, "@afbc")) {
		format->afbc_en = true;
		format->block_w = 16;
		format->block_h = 16;
	} else if (parse_tile && strstr(p, "@tile4x4m0")) {
		format->tiled_en = true;
		format->tile_mode = 2;
	} else if (parse_tile && strstr(p, "@tile4x4m1")) {
		format->tiled_en = true;
		format->tile_mode = 3;
	} else if (parse_tile && strstr(p, "@tile")) {
		format->tiled_en = true;
		format->tile_mode = 1;
	} else if (strstr(p, "@rfbc64x4")) {
		format->rfbc_en = true;
		format->block_w = 64;
		format->block_h = 4;
	} else if (parse_tile && strstr(p, "@afrc16scan")) {
		format->afrc_en = true;
		format->afrc_scan = true;
		format->afrc_cu_size = 16;
	} else if (parse_tile && strstr(p, "@afrc24scan")) {
		format->afrc_en = true;
		format->afrc_scan = true;
		format->afrc_cu_size = 24;
	} else if (parse_tile && strstr(p, "@afrc32scan")) {
		format->afrc_en = true;
		format->afrc_scan = true;
		format->afrc_cu_size = 32;
	} else if (parse_tile && strstr(p, "@afrc16")) {
		format->afrc_en = true;
		format->afrc_cu_size = 16;
	} else if (parse_tile && strstr(p, "@afrc24")) {
		format->afrc_en = true;
		format->afrc_cu_size = 24;
	} else if (parse_tile && strstr(p, "@afrc32")) {
		format->afrc_en = true;
		format->afrc_cu_size = 32;
	}
}

static int parse_connector(struct pipe_arg *pipe, const char *arg)
{
	unsigned int len;
	unsigned int i;
	const char *p;
	char *endp;
	struct fbc_format format = {0};

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
		parse_format_options(&format, p, false);
		pipe->afbc_en = format.afbc_en;
		pipe->afbc_ytr_en = format.afbc_ytr_en;
		pipe->afbc_split_en = format.afbc_split_en;
		pipe->afbc_sparse_en = format.afbc_sparse_en;
		pipe->rfbc_en = format.rfbc_en;
		pipe->block_w = format.block_w;
		pipe->block_h = format.block_h;
	}

	pipe->fourcc = util_format_fourcc(pipe->format_str);
	if (pipe->fourcc == 0)  {
		fprintf(stderr, "unknown format %s\n", pipe->format_str);
		return -1;
	}

	return 0;
}

static int parse_plane(struct plane_arg *plane, const char *p, unsigned int *zpos)
{
	char *end;
	struct fbc_format format = {0};

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

	if (*end == ':') {
		p = end + 1;
		plane->crtc_w = strtoul(p, &end, 10);
		if (*end != 'x') {
			fprintf(stderr, "invalid crtc_w/h argument\n");
			return -EINVAL;
		}

		p = end + 1;
		plane->crtc_h = strtoul(p, &end, 10);
	} else {
		plane->crtc_w = plane->w;
		plane->crtc_h = plane->h;
	}

	if (!strncmp(end, "@stride:", 8)) {
		p = end + 8;
		plane->stride = strtoul(p, &end, 10);
	}

	if (!plane->stride)
		plane->stride = plane->w;

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
		parse_format_options(&format, end + 5, true);
		plane->afbc_en = format.afbc_en;
		plane->afbc_ytr_en = format.afbc_ytr_en;
		plane->afbc_split_en = format.afbc_split_en;
		plane->afbc_sparse_en = format.afbc_sparse_en;
		plane->tiled_en = format.tiled_en;
		plane->rfbc_en = format.rfbc_en;
		plane->afrc_en = format.afrc_en;
		plane->afrc_scan = format.afrc_scan;
		plane->afrc_cu_size = format.afrc_cu_size;
		plane->tile_mode = format.tile_mode;
		plane->block_w = format.block_w;
		plane->block_h = format.block_h;

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

	plane->zpos = (*zpos)++;

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

static int parse_pictures(struct test_state *state, const char *arg)
{
	char *copy;
	char *saveptr = NULL;
	char *name;

	copy = strdup(arg);
	if (!copy)
		return -ENOMEM;

	name = strtok_r(copy, ",", &saveptr);

	while (name) {
		if (state->picture_count >= PIC_MAX_CNT) {
			fprintf(stderr, "max picture number: %d\n", PIC_MAX_CNT);
			free(copy);
			return -EINVAL;
		}
		if (strlen(name) >= PIC_NAME_MAX_LEN) {
			fprintf(stderr, "picture name is too long\n");
			free(copy);
			return -EINVAL;
		}
		strcpy(state->pictures[state->picture_count++], name);
		name = strtok_r(NULL, ",", &saveptr);
	}

	free(copy);
	return 0;
}

static void free_test_state(struct test_state *state)
{
	unsigned int i;
	unsigned int j;

	for (i = 0; i < state->pipe_count; i++) {
		if (state->pipes[i].cons) {
			for (j = 0; j < state->pipes[i].num_cons; j++)
				free((void *)state->pipes[i].cons[j]);
		}
		free(state->pipes[i].cons);
		free(state->pipes[i].con_ids);
		free(state->pipes[i].owned_mode);
	}
	free(state->pipes);
	free(state->plane_args);
	free(state->properties);
	memset(state, 0, sizeof(*state));
}

static const char optstr[] = "acdD:efF:M:P:ps:Cvw:otE";

static int parse_test_options(int argc, char **argv, struct test_state *state)
{
	unsigned int args = 0;
	int c;
	struct plane_arg *plane_args;
	struct pipe_arg *pipes;
	struct property_arg *properties;
	size_t size;

	optind = 1;
	opterr = 0;

	while ((c = getopt(argc, argv, optstr)) != -1) {
		args++;

		switch (c) {
		case 'a':
			args--;
			break;
		case 'c':
			state->connectors = 1;
			break;
		case 'D':
			state->device = optarg;
			args--;
			break;
		case 'd':
			state->drop_master = 1;
			break;
		case 'e':
			state->encoders = 1;
			break;
		case 'f':
			state->framebuffers = 1;
			break;
		case 'F':
			if (parse_pictures(state, optarg))
				return -EINVAL;
			break;
		case 'M':
			state->module = optarg;
			/* Preserve the default behaviour of dumping all information. */
			args--;
			break;
		case 'o':
			state->dynamic_onoff = 1;
			break;
		case 'P':
			size = (state->plane_count + 1) * sizeof(*plane_args);
			plane_args = realloc(state->plane_args, size);
			if (!plane_args)
				return -ENOMEM;
			state->plane_args = plane_args;
			memset(&state->plane_args[state->plane_count], 0, sizeof(*state->plane_args));
			state->plane_count++;

			if (parse_plane(&state->plane_args[state->plane_count - 1], optarg, &state->zpos) < 0)
				return -EINVAL;
			break;
		case 'p':
			state->crtcs = 1;
			state->planes = 1;
			break;
		case 's':
			size = (state->pipe_count + 1) * sizeof(*pipes);
			pipes = realloc(state->pipes, size);
			if (!pipes)
				return -ENOMEM;
			state->pipes = pipes;
			memset(&state->pipes[state->pipe_count], 0, sizeof(*state->pipes));
			state->pipe_count++;

			if (parse_connector(&state->pipes[state->pipe_count - 1], optarg) < 0)
				return -EINVAL;
			break;
		case 't':
			state->one_shot = true;
			break;
		case 'v':
			state->test_vsync = 1;
			break;
		case 'w':
			size = (state->property_count + 1) * sizeof(*properties);
			properties = realloc(state->properties, size);
			if (!properties)
				return -ENOMEM;
			state->properties = properties;
			memset(&state->properties[state->property_count], 0, sizeof(*state->properties));
			state->property_count++;

			if (parse_property(&state->properties[state->property_count - 1], optarg) < 0)
				return -EINVAL;
			break;
		case 'E':
			state->error_monitor = true;
			break;
		default:
			return -EINVAL;
		}
	}

	if (!args)
		state->encoders = state->connectors = state->crtcs =
			state->planes = state->framebuffers = 1;

	return 0;
}

static void usage(char *name)
{
	fprintf(stderr, "overlay test by Andy, libdrm version: 2.4.101\n");
	fprintf(stderr, "usage: %s [-acDdefMPpsCvw]\n", name);

	fprintf(stderr, "\n Query options:\n\n");
	fprintf(stderr, "\t-c\tlist connectors\n");
	fprintf(stderr, "\t-e\tlist encoders\n");
	fprintf(stderr, "\t-f\tlist framebuffers\n");
	fprintf(stderr, "\t-p\tlist CRTCs and planes (pipes)\n");


	fprintf(stderr, "\n Test options:\n\n");
	fprintf(stderr, "\t-P <plane_id>@<crtc_id>:<w>x<h>[:<crtc_w>x<crtc_h>][@stride:vir_w][+<x>+<y>][*<scale>][@<format>][@afbc][@afbc16x16][@afbc32x8][@afbc64x4][@afbcsplit][@afbcytr][@tile][@tile4x4][@rfbc64x4][@afrc16/24/32][@afrc16/24/32scan][@rotatex/y/90/270]\tset a plane\n");
	fprintf(stderr, "\t-s <connector_id>[,<connector_id>][@<crtc_id>]:[#<mode index>]<mode>[-<vrefresh>][@<format>][@afbc][@afbc16x16][@afbc32x8][@afbc64x4][@afbcsplit][@afbcytr][@rfbc64x4]\tset a mode\n");
	fprintf(stderr, "\t-C\ttest hw cursor\n");
	fprintf(stderr, "\t-v\ttest vsynced page flipping\n");
	fprintf(stderr, "\t-o\ttest dynamic turn on off plane one by one, run with -v mode\n");
	fprintf(stderr, "\t-w <obj_id>:<prop_name>:<value>\tset property\n");
	fprintf(stderr, "\t-a\tcompatibility option; atomic modesetting is always used\n");
	fprintf(stderr, "\t-F pattern1,pattern2\tspecify fill patterns\n");

	fprintf(stderr, "\n Generic options:\n\n");
	fprintf(stderr, "\t-d\tdrop master after mode set\n");
	fprintf(stderr, "\t-M module\tuse the given driver\n");
	fprintf(stderr, "\t-D device\tuse the given device\n");
	fprintf(stderr, "\t-t \t oneshot test, show one frame then exit\n");


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

/*
 * Open the DRM device, require atomic modesetting, and load its resources.
 */
static int setup_device(struct device *dev, const char *device,
			const char *module)
{
	int ret;

	dev->fd = util_open(device, module);
	if (dev->fd < 0)
		return dev->fd;

	ret = drmSetClientCap(dev->fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		fprintf(stderr, "no atomic modesetting support: %s\n", strerror(errno));
		return ret;
	}

	dev->resources = get_resources(dev);
	if (!dev->resources)
		return -ENODEV;

	return 0;
}

int main(int argc, char **argv)
{
	struct device dev;
	struct test_state state = {};
	unsigned int i;
	struct plane_arg *c_plane_args = NULL;
	unsigned int c_plane_count = 0;
	unsigned int c_count = 0;
	bool c_increase_mode;
	drmVersionPtr version;
	int ret;
	int exit_code = 0;

	memset(&dev, 0, sizeof dev);

	ret = parse_test_options(argc, argv, &state);
	if (ret) {
		usage(argv[0]);
		exit_code = 1;
		goto cleanup;
	}

	ret = setup_device(&dev, state.device, state.module);
	if (ret) {
		exit_code = 1;
		goto cleanup;
	}

	version = drmGetVersion(dev.fd);
	if(version) {
		printf("Description: %s\n", version->desc);
		printf("Name: %s\n", version->name);
		printf("Version: %d.%d.%d\n", version->version_major,
		       version->version_minor, version->version_patchlevel);
		printf("Date: %s\n", version->date);
		drmFreeVersion(version);

	}

	if (state.test_vsync && !state.pipe_count) {
		fprintf(stderr, "page flipping requires at least one -s option.\n");
		exit_code = -1;
		goto cleanup;
	}

	for (i = 0; i < state.pipe_count; i++) {
		if (pipe_resolve_connectors(&dev, &state.pipes[i]) < 0) {
			exit_code = 1;
			goto cleanup;
		}
	}

#define dump_resource(dev, state, res) if ((state).res) dump_##res(dev)

	dump_resource(&dev, state, encoders);
	dump_resource(&dev, state, connectors);
	dump_resource(&dev, state, crtcs);
	dump_resource(&dev, state, planes);
	dump_resource(&dev, state, framebuffers);

	for (i = 0; i < state.property_count; ++i)
		set_property(&dev, &state.properties[i]);

	dev.req = drmModeAtomicAlloc();

	if (state.error_monitor) {
		dev.error_event_fd = open("/sys/devices/platform/display-subsystem/error_event", O_RDONLY);

		if (dev.error_event_fd < 0) {
			fprintf(stderr, "failed to open error_event_event node: %s\n", strerror(errno));
			dev.error_event_fd = 0;
		} else {
			drm_create_error_monitor_thread(&dev);
		}
	}

	if (state.pipe_count) {
		uint64_t cap = 0;

		ret = drmGetCap(dev.fd, DRM_CAP_DUMB_BUFFER, &cap);
		if (ret || cap == 0) {
			fprintf(stderr, "driver doesn't support the dumb buffer API\n");
			exit_code = 1;
			goto cleanup;
		}

		ret = atomic_set_mode(&dev, state.pipes, state.pipe_count, false);
		if (ret) {
			fprintf(stderr, "atomic_set_mode failed\n");
			exit_code = 1;
			goto cleanup;
		}

		ret = atomic_set_planes(&dev, state.plane_args, state.plane_count, state.pictures,
					state.picture_count, false);
		if (ret) {
			fprintf(stderr, "atomic_set_planes failed\n");
			exit_code = 1;
			goto cleanup;
		}

		ret = atomic_disable_unused(&dev, state.pipes, state.pipe_count,
					    state.plane_args, state.plane_count);
		if (ret) {
			fprintf(stderr, "failed to disable unused objects\n");
			exit_code = 1;
			goto cleanup;
		}

		ret = drmModeAtomicCommit(dev.fd, dev.req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (ret) {
			fprintf(stderr, "Atomic Commit failed [1]\n");
			exit_code = 1;
			goto cleanup;
		}

		gettimeofday(&state.pipes[0].start, NULL);
		state.pipes[0].swap_count = 0;
		write_wb_file(state.pipes, state.pipe_count);

		if (state.test_vsync) {
			size_t size = state.plane_count * sizeof(*c_plane_args);

			c_plane_args = calloc(1, size);
			if (c_plane_args == NULL) {
				fprintf(stderr, "memory allocation for commit plane args failed\n");
				exit_code = 1;
				goto cleanup;
			}
			c_plane_count = 1;
			c_increase_mode = true;
		}

		while (state.test_vsync) {
			drmModeAtomicFree(dev.req);
			dev.req = drmModeAtomicAlloc();
			if (state.dynamic_onoff) {
				memcpy(c_plane_args, state.plane_args, sizeof(*c_plane_args));
				ret = atomic_set_planes(&dev, state.plane_args, c_plane_count,
							state.pictures,
							state.picture_count, true);
				if (ret) {
					fprintf(stderr, "atomic_set_planes failed in vsync loop\n");
					exit_code = 1;
					goto cleanup;
				}
			} else {
				ret = atomic_set_planes(&dev, state.plane_args, state.plane_count,
							state.pictures,
							state.picture_count, true);
				if (ret) {
					fprintf(stderr, "atomic_set_planes failed in vsync loop\n");
					exit_code = 1;
					goto cleanup;
				}
			}
			ret = atomic_disable_unused(&dev, state.pipes, state.pipe_count,
						    state.plane_args, state.dynamic_onoff ?
						    c_plane_count : state.plane_count);
			if (ret) {
				fprintf(stderr, "failed to disable unused objects\n");
				exit_code = 1;
				goto cleanup;
			}

			ret = drmModeAtomicCommit(dev.fd, dev.req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
			if (ret) {
				fprintf(stderr, "Atomic Commit failed [2]\n");
				exit_code = 1;
				goto cleanup;
			}

			state.pipes[0].swap_count++;
			if (state.pipes[0].swap_count == 60) {
				struct timeval end;
				double t;

				gettimeofday(&end, NULL);
				t = end.tv_sec + end.tv_usec * 1e-6 -
				    (state.pipes[0].start.tv_sec +
				     state.pipes[0].start.tv_usec * 1e-6);
				fprintf(stderr, "freq: %.02fHz\n", state.pipes[0].swap_count / t);
				state.pipes[0].swap_count = 0;
				state.pipes[0].start = end;

				c_count++;
				/* turn on or off plane one by one every 30s */
				if (c_count == 1) {
					c_count = 0;
					if (c_increase_mode)
						c_plane_count++;
					else
						c_plane_count--;

					if (c_plane_count >= state.plane_count)
						c_increase_mode = false; /* decrease plane one by one*/

					if (c_plane_count == 1)
						c_increase_mode = true;
				}
			}
		}

		if (state.drop_master)
			drmDropMaster(dev.fd);

		if (!state.one_shot)
			getchar();
		else
			sleep(3);

		drmModeAtomicFree(dev.req);
		dev.req = drmModeAtomicAlloc();

		atomic_clear_mode(&dev, state.pipes, state.pipe_count);
		atomic_clear_planes(&dev, state.plane_args, state.plane_count);
		ret = drmModeAtomicCommit(dev.fd, dev.req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (ret)
			fprintf(stderr, "Atomic Commit failed: %s\n", strerror(errno));

		destroy_mode_blobs(&dev, &state);
	}

	if (state.error_monitor)
		getchar();

cleanup:
	destroy_mode_blobs(&dev, &state);
	release_test_buffers(&dev, &state);

	if (dev.req)
		drmModeAtomicFree(dev.req);

	if (dev.resources)
		free_resources(dev.resources);

	if (dev.fd >= 0)
		drmClose(dev.fd);

	free_test_state(&state);

	return exit_code;
}
