#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <xf86drm.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "util/kms.h"

struct crtc {
	drmModeCrtc *crtc;
	drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
	drmModeModeInfo *mode;
};

struct resources {
	struct crtc *crtcs;
	uint32_t count_crtcs;
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

};

#define CURSOR_WIDTH 64
#define CURSOR_HEIGHT 64

static struct resources *get_resources(struct device *dev)
{
	drmModeRes *_res;
	struct resources *res;
	uint32_t i;

	res = calloc(1, sizeof(*res));
	if (res == 0)
		return NULL;

	_res = drmModeGetResources(dev->fd);
	if (!_res) {
		fprintf(stderr, "drmModeGetResources failed: %s\n", strerror(errno));
		free(res);
		return NULL;
	}

	res->count_crtcs = _res->count_crtcs;

	res->crtcs = calloc(res->count_crtcs, sizeof(*res->crtcs));

	if (!res->crtcs) {
	    drmModeFreeResources(_res);
		goto error;
    }

#define get_resource(_res, __res, type, Type)					\
	do {									\
		for (i = 0; i < (_res)->count_##type##s; ++i) {	\
			uint32_t type##id = (__res)->type##s[i];			\
			(_res)->type##s[i].type =							\
				drmModeGet##Type(dev->fd, type##id);			\
			if (!(_res)->type##s[i].type)						\
				fprintf(stderr, "could not get %s %i: %s\n",	\
					#type, type##id,							\
					strerror(errno));			\
		}								\
	} while (0)

	get_resource(res, _res, crtc, Crtc);

	drmModeFreeResources(_res);


	for (i = 0; i < res->count_crtcs; ++i)
		res->crtcs[i].mode = &res->crtcs[i].crtc->mode;

	return res;

error:
	//free_resources(res);
	return NULL;
}

int main(int argc, const char **argv)
{
	uint32_t width = CURSOR_WIDTH;
	uint32_t height = CURSOR_HEIGHT;
	uint32_t crtc_id = 0;
	uint32_t delay = 16;
	uint32_t x = 0;
	uint32_t y = 200;
	uint32_t step = 8;
	uint32_t hdisplay = 0;
	uint32_t vdisplay = 0;
	uint32_t handle;
	uint32_t size;
	uint32_t i;
	int *ptr;
	char *device = NULL;
	char *module = NULL;
	struct drm_mode_map_dumb map_arg;
	struct device dev;

	struct drm_mode_create_dumb create_arg = {
		.width = width,
		.height = height,
		.bpp = 32,
	};

	memset(&dev, 0, sizeof dev);

	dev.fd = util_open(device, module);
	if (dev.fd < 0)
		return -1;

	dev.resources = get_resources(&dev);
	if (!dev.resources) {
		drmClose(dev.fd);
		return 1;
	}

	drmIoctl(dev.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_arg);
	handle = create_arg.handle;
	size = create_arg.size;

	map_arg.handle = handle,

	drmIoctl(dev.fd, DRM_IOCTL_MODE_MAP_DUMB, &map_arg);

	ptr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, dev.fd, map_arg.offset);

	for (i = 0; i < width * height; i++)
		ptr[i] = 0x4F000000 | (i % width) * 2 << 16 | (i / height) << 8;

	if (argc > 1)
		crtc_id = atoi(argv[1]);

	if (argc > 2)
		setenv("DRM_CURSOR_PREFER_PLANE", argv[2], 1);

	if (argc > 3)
		delay = atoi(argv[3]);

	for (i = 0; i < dev.resources->count_crtcs; ++i) {
		if (dev.resources->crtcs[i].crtc->crtc_id == crtc_id) {
			hdisplay = dev.resources->crtcs[i].crtc->mode.hdisplay;
			vdisplay = dev.resources->crtcs[i].crtc->mode.vdisplay;
		}
	}

	printf("Run cursor test on crtc:%d  mode:%dx%d\n", crtc_id, hdisplay, vdisplay);

	drmModeSetCursor(dev.fd, crtc_id, handle, width, height);

	while (hdisplay) {
		if (x >= hdisplay) {
			x = hdisplay;
			step = -8;
		}

		if (x <= 0) {
			x = 0;
			step = 8;
		}

		drmModeMoveCursor(dev.fd, crtc_id, x, y);

		x += step;

		usleep(delay*1000);
	}

	return 0;
}
