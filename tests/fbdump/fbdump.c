/*
 * DRM based framebuffer dump tool
 *   Andy Yan <andyshrk@163.com>
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
#include <sys/mman.h>
#include <math.h>

#include "xf86drm.h"
#include "xf86drmMode.h"
#include "drm_fourcc.h"

#include "util/common.h"
#include "util/format.h"
#include "util/kms.h"

#include "drm_format.h"

#define VERSION "1.0.0"
#define PIC_NAME_MAX_LEN	256

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
	uint32_t crtc_id;
	char *dir;
};

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

static char *fourcc2str(uint32_t fourcc)
{
	char *name = drmGetFormatName(fourcc);

	return name;
}

static void write_fb_file(char *buffer, const char *filename, int size)
{
	int fd;

	fd = open(filename, O_WRONLY| O_TRUNC | O_CREAT, 0666);
	if (fd == -1) {
		printf("Failed to open %s: %s\n", filename, strerror(errno));
		return;
	}

	write(fd, buffer, size);
}

static int fb_handle_to_fd(int drm_fd, int handle)
{
	struct drm_prime_handle args;
	int ret;

	memset(&args, 0, sizeof(args));
	args.fd = -1;
	args.handle = handle;

	ret = drmIoctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
	if (ret)
		return ret;

	return args.fd;
}

static void dump_planes(struct device *dev)
{
	drmModeFB2Ptr fb;
	__s32 fb_fd;
	unsigned int fb_size;
	unsigned int i;
	void *data;
	char path[PIC_NAME_MAX_LEN];
	char cwd[PIC_NAME_MAX_LEN];
	char *dir;
	char *format_name;
	uint32_t bpp;


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

		if (!ovr->fb_id)
			continue;

		fb = drmModeGetFB2(dev->fd, ovr->fb_id);
		if (!fb) {
			fprintf(stderr, "drmModeGetFB2 for fb: %d failed: %s\n",
				ovr->fb_id, strerror(errno));
			continue;
		}
		bpp = drm_get_bpp(fb->pixel_format);
		fb_size = fb->pitches[0] * fb->height;
		fb_fd = fb_handle_to_fd(dev->fd, fb->handles[0]);
		if (fb_fd < 0) {
			fprintf(stderr, "Failed to get fb fd: %s\n", strerror(errno));
			continue;
		}

		data = mmap(NULL, fb_size, PROT_READ, MAP_SHARED, fb_fd, 0);
		if (data == MAP_FAILED) {
			fprintf(stderr, "Failed to mmap: %s\n", strerror(errno));
			continue;
		}

		format_name = fourcc2str(fb->pixel_format);

		getcwd(cwd, sizeof(cwd));

		if (dev->dir)
			dir = dev->dir;
		else
			dir = cwd;
		if (fb->modifier) {
			snprintf(path, sizeof(path), "%s/plane-%d-%dx%d-%s-%s.bin",
				 dir, ovr->plane_id, (fb->pitches[0] << 3) / bpp,
				 fb->height, format_name,
				 modifier_to_string(fb->modifier));
		} else  {
			snprintf(path, sizeof(path), "%s/plane-%d-%dx%d-%s.bin",
				 dir, ovr->plane_id, (fb->pitches[0] << 3) / bpp,
				 fb->height, format_name);
		}

		write_fb_file(data, path, fb_size);

		drmModeFreeFB2(fb);
		free(format_name);
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

	printf("crtcs: %d encoders: %d connectors : %d fbs %d\n",
	       res->res->count_crtcs, res->res->count_encoders,
	       res->res->count_connectors, res->res->count_fbs);
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

static void usage(char *name)
{
	fprintf(stderr, "Framebuffer dump tool by Andy, version: %s\n", VERSION);
	fprintf(stderr, "usage: %s [-cdDM]\n", name);

	fprintf(stderr, "\t-c <crtc_id>\t dump framebuffer attached to this crtc, default dump all framebuffer\n");
	fprintf(stderr, "\t-d <Directory>\t director to store the dumped file, default use the dir where you run fbdump\n");
	fprintf(stderr, "\n Generic options:\n\n");
	fprintf(stderr, "\t-M module\tuse the given driver\n");
	fprintf(stderr, "\t-D device\tuse the given device\n");

	exit(0);
}

static char optstr[] = "c:d:D:M:";

int main(int argc, char **argv)
{
	struct device dev;
	int c;
	int crtc_id = 0;
	char *device = NULL;
	char *module = NULL;
	unsigned int args = 0;
	drmVersionPtr version;
	int ret;

	memset(&dev, 0, sizeof dev);

	opterr = 0;
	while ((c = getopt(argc, argv, optstr)) != -1) {
		args++;

		switch (c) {
		case 'c':
			crtc_id = atoi(optarg);
			break;
		case 'd':
			dev.dir = optarg;
			break;
		case 'D':
			device = optarg;
			args--;
			break;
		case 'M':
			module = optarg;
			/* Preserve the default behaviour of dumping all information. */
			args--;
			break;
		default:
			usage(argv[0]);
			break;
		}
	}

	dev.fd = util_open(device, module);
	if (dev.fd < 0)
		return -1;

	dev.crtc_id = crtc_id;

	version = drmGetVersion(dev.fd);
	if(version) {
		printf("Description: %s\n", version->desc);
		printf("Name: %s\n", version->name);
		printf("Version: %d.%d.%d\n", version->version_major,
		       version->version_minor, version->version_patchlevel);
		printf("Date: %s\n", version->date);
		drmFreeVersion(version);
	}

	ret = drmSetClientCap(dev.fd, DRM_CLIENT_CAP_ATOMIC, 1);
	if (ret) {
		fprintf(stderr, "no atomic modesetting support: %s\n", strerror(errno));
		drmClose(dev.fd);
		return -1;
	}

	dev.resources = get_resources(&dev);
	if (!dev.resources) {
		drmClose(dev.fd);
		return 1;
	}

	dump_planes(&dev);

	free_resources(dev.resources);

	return 0;
}
