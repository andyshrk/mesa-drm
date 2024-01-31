/*
 * Copyright 2020 Rockchip Electronics Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <time.h>

#include "xf86drm.h"
#include "xf86drmMode.h"

#include "util/common.h"
#include "util/kms.h"

struct  drm_backend {
	int drm_fd;
};

#define DRM_EVENTS_MAX 4
#define DRM_ROCKCHIP_GET_VCNT_EVENT     0x05
#define DRM_EVENT_ROCKCHIP_CRTC_VCNT    0xf
#define DRM_ROCKCHIP_VCNT_EVENT 0x80000000

#define DRM_IOCTL_ROCKCHIP_GET_VCNT_EVENT  DRM_IOWR(DRM_COMMAND_BASE + \
		        DRM_ROCKCHIP_GET_VCNT_EVENT, union drm_wait_vblank)

static void drm_fdevent_init(int* epoll_fd) {
	int fd = epoll_create(128);

	if (fd < 0) {
		printf("epoll_create() failed");
		return;
	}

	/* mark for close-on-exec */
	fcntl(fd, F_SETFD, FD_CLOEXEC);

	*epoll_fd = fd;
}

static int drm_get_vcnt(int fd, drmVBlankPtr vbl) {
	struct timespec timeout, cur;
	int ret, type = 0;

	clock_gettime(CLOCK_MONOTONIC, &timeout);
	timeout.tv_sec++;

	do {
		ret = drmIoctl(fd, DRM_IOCTL_ROCKCHIP_GET_VCNT_EVENT, vbl);
		type &= ~DRM_VBLANK_RELATIVE;
		vbl->request.type = (drmVBlankSeqType)type;
		if (ret && errno == EINTR) {
			clock_gettime(CLOCK_MONOTONIC, &cur);
			/* Timeout after 1s */
			if (cur.tv_sec > timeout.tv_sec + 1 ||
					(cur.tv_sec == timeout.tv_sec && cur.tv_nsec >=
					 timeout.tv_nsec)) {
				errno = EBUSY;
				ret = -1;
				break;
			}
		}
	} while (ret && errno == EINTR);

	return 0;
}

static int drm_vcnt_handler(struct drm_backend *b, struct drm_event_vblank *vblank) {
	drmVBlank vbl;
	int ret = -1, type;

	/* Queue an event for frame + 1 */
	memset(&vbl, 0, sizeof(vbl));
	type = DRM_ROCKCHIP_VCNT_EVENT;
	vbl.request.type = (drmVBlankSeqType)type;
	vbl.request.sequence = 1;

	ret = drm_get_vcnt(b->drm_fd, &vbl);
	if (ret != 0) {
		printf("drm_get_vcnt failed ret: %d", ret);
	}

	printf("seq: %-8d event_time: %ld:%ld \n",
	       vbl.reply.sequence, vbl.reply.tval_sec, vbl.reply.tval_usec);

	return ret;
}

static int drm_vblank_handler(struct drm_backend *b, struct drm_event_vblank *vblank)
{
	drmVBlank vbl;
	int type, ret = -1;

	/* Queue an event for frame + 1 */
	type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
	vbl.request.type = (drmVBlankSeqType)type;
	vbl.request.sequence = 1;

	ret = drmWaitVBlank(b->drm_fd, &vbl);
	if (ret != 0) {
		printf("failed to wait vsync event %d", ret);
	}

	printf("[%d.%06d]: %d-vblank-[%ld.%06ld]: %d\n",
	       vblank->tv_sec, vblank->tv_usec, vblank->sequence,
	       vbl.reply.tval_sec, vbl.reply.tval_usec, vbl.reply.sequence);

	return ret;
}

static int drm_event_handler(struct drm_backend *b)
{
	struct drm_event *e;
	struct drm_event_vblank *vblank;
	size_t len, i;
	char buffer[1024];

	len = read(b->drm_fd, buffer, sizeof buffer);
	if (len < sizeof(*e)) {
		printf("read failed len: %zu (%s)", len, strerror(errno));
		return -1;
	}

	i = 0;
	while (i < len) {
		e = (struct drm_event *)(buffer + i);
		switch (e->type) {
			case DRM_EVENT_ROCKCHIP_CRTC_VCNT:
				vblank = (struct drm_event_vblank *) e;
				drm_vcnt_handler(b, vblank);
				break;
			case DRM_EVENT_VBLANK:
				vblank = (struct drm_event_vblank *) e;
				drm_vblank_handler(b, vblank);
				break;
			case DRM_EVENT_FLIP_COMPLETE:
				break;
			default:
				break;
		}
		i += e->length;
	}

	return 0;
}

static void drm_fdevent_add(int epoll_fd, struct drm_backend* b)
{
	struct epoll_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN;
	ev.data.fd = b->drm_fd;
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, b->drm_fd, &ev))
		printf("epoll_ctl vsync failed");
}

int main(int argc, char **argv)
{
	struct epoll_event events[DRM_EVENTS_MAX];
	struct drm_backend b;
	drmVBlank vbl;
	const char *device = NULL, *module = NULL;
	char name[32] = "drm_event_test";
	int epoll_fd = -1, n, timeout = 1000;
	int ret;

	prctl(PR_SET_NAME, name);

	b.drm_fd = util_open(device, module);
	if (b.drm_fd < 0)
		return b.drm_fd;

	drm_fdevent_init(&epoll_fd);
	drm_fdevent_add(epoll_fd, &b);

	/* Get current count first */
	vbl.request.type = DRM_VBLANK_RELATIVE;
	vbl.request.sequence = 1;
	ret = drmWaitVBlank(b.drm_fd, &vbl);
	if (ret != 0) {
		printf("drmWaitVBlank (relative) failed ret: %i\n", ret);
		return -1;
	}

	printf("[%ld.%06ld]: %d-start DRM_VBLANK_RELATIVE\n",
	       vbl.reply.tval_sec, vbl.reply.tval_usec, vbl.reply.sequence);

	/* Queue an event for frame + 1 */
	vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
	vbl.request.sequence = 1;
	ret = drmWaitVBlank(b.drm_fd, &vbl);
	if (ret != 0) {
		printf("drmWaitVBlank (relative, event) failed ret: %i\n", ret);
		return -1;
	}
	printf("[%ld.%06ld]: %d-start DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT\n",
	       vbl.reply.tval_sec, vbl.reply.tval_usec, vbl.reply.sequence);

	while (1) {
		n = epoll_wait(epoll_fd, events, DRM_EVENTS_MAX, timeout);

		if (!n) {
			printf("epoll: %d, restart vblank event\n", n);
			vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT;
			vbl.request.sequence = 1;
			ret = drmWaitVBlank(b.drm_fd, &vbl);
			if (ret != 0) {
				printf("drmWaitVBlank (relative, event) failed ret: %i\n", ret);
			}

		}

		for (int i = 0; i < n; i++) {
			struct epoll_event* ev = events + i;
			if (ev->data.fd == b.drm_fd && ev->events & EPOLLIN) {
				drm_event_handler(&b);
			}
		}

	}

	close(epoll_fd);
	printf("drm event thread exit");
}
