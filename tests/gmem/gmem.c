/* Copyright 2020 Rockchip Electronics Co. LTD
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
 */

#undef DBG_MOD_ID
#define DBG_MOD_ID       RK_ID_MB

#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int drm_open() {
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) {
        printf("fail to open drm device(/dev/dri/card0)\n");
    }

    return fd;
}

static int drm_close(int fd) {
    int ret = close(fd);
    if (ret < 0) {
        return -errno;
    }

    return ret;
}

static int drm_ioctl(int fd, int req, void* arg) {
    int ret = ioctl(fd, req, arg);
    if (ret < 0) {
        printf("fail to drm_ioctl(fd = %d, req =%p), error: %s\n", fd, req, strerror(errno));
        return -errno;
    }
    return ret;
}

static int drm_alloc(
        int fd,
        int len,
        int align,
        int *handle,
        int heaps) {
    int ret;
    struct drm_mode_create_dumb dmcb;

    memset(&dmcb, 0, sizeof(struct drm_mode_create_dumb));
    dmcb.bpp = 8;
    dmcb.width = (len + align - 1) & (~(align - 1));
    dmcb.height = 1;
    dmcb.flags = heaps;

    if (handle == NULL) {
        printf("illegal parameter, handler is null.\n");
        return -EINVAL;
    }

    ret = drm_ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcb);
    if (ret < 0) {
        return ret;
    }

    *handle = dmcb.handle;
    return ret;
}

static int drm_handle_to_fd(int fd, int handle, int *map_fd, int flags) {
    int ret;
    struct drm_prime_handle dph;

    memset(&dph, 0, sizeof(struct drm_prime_handle));
    dph.handle = handle;
    dph.fd = -1;
    dph.flags = flags;

    if (map_fd == NULL)
        return -EINVAL;

    ret = drm_ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
    if (ret < 0) {
        return ret;
    }

    *map_fd = dph.fd;

    if (*map_fd < 0) {
        printf("fail to handle_to_fd(fd=%d)\n", fd);
        return -EINVAL;
    }

    return ret;
}

static int drm_fd_to_handle(
        int fd,
        int map_fd,
        int *handle,
        int flags) {
    int ret;
    struct drm_prime_handle dph;

    dph.fd = map_fd;
    dph.flags = flags;

    ret = drm_ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &dph);
    if (ret < 0) {
        return ret;
    }

    *handle = dph.handle;
    return ret;
}

static int drm_get_info_from_name(
        int   fd,
        int  name,
        int *handle,
        int  *size) {
    int ret = 0;
    struct drm_gem_open req;

    req.name = name;
    ret = drm_ioctl(fd, DRM_IOCTL_GEM_OPEN, &req);
    if (ret < 0) {
        return ret;
    }

    *handle = req.handle;
    *size   = req.size;

    return ret;
}

static int drm_get_name_from_handle(int fd, int handle, int *name) {
    struct drm_gem_flink req;
    int ret = 0;

    req.handle = handle,
    ret = drm_ioctl(fd, DRM_IOCTL_GEM_FLINK, &req);

    *name = req.name;
    return ret;
}

static int drm_close_handle(int fd, int handle) {
    struct drm_gem_close req;
    int ret;

    req.handle = handle;
    ret = drm_ioctl(fd, DRM_IOCTL_GEM_CLOSE, &req);
    if (ret < 0) {
        return ret;
    }

    return ret;
}

static int drm_free(int fd, int handle) {
    struct drm_mode_destroy_dumb data = {
        .handle = (__u32)handle
    };
    return drm_ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &data);
}

int main(int argc, const char **argv) {
    int s32DrmFd = 0;
    int s32Ret = 0;
    int s32Handle1MB = 0;
    int s32Fd1MB = 0;
    int s32Name1MB = 0;
    int s32Handle4MB = 0;
    int s32Fd4MB = 0;
    int s32Name4MB = 0;

    s32DrmFd = drm_open();

    // alloc 1M drm buffer
    s32Ret = drm_alloc(s32DrmFd, 1 * 1024 * 1024, 4096, &s32Handle1MB, 0);
    if (s32Ret != 0) {
        return s32Ret;
    }
	printf("1M handle = %d\n", s32Handle1MB);
    s32Ret = drm_handle_to_fd(s32DrmFd, s32Handle1MB, &s32Fd1MB, O_CLOEXEC | O_RDWR);
    if (s32Ret != 0) {
        return s32Ret;
    }
	printf("1M handle=%d fd=%d\n", s32Handle1MB, s32Fd1MB);
    s32Ret = drm_get_name_from_handle(s32DrmFd, s32Handle1MB, &s32Name1MB);
    if (s32Ret != 0) {
        return s32Ret;
    }
	printf("1M handle=%d fd=%d name=%d\n", s32Handle1MB, s32Fd1MB, s32Name1MB);
	
	
    int tmpHandle1MB = 0;
    int tmpSize = 0;
    int tmpFd = 0;
    s32Ret = drm_get_info_from_name(s32DrmFd, s32Name1MB, &tmpHandle1MB, &tmpSize);
    if (s32Ret != 0) {
        return s32Ret;
    }
    s32Ret = drm_handle_to_fd(s32DrmFd, tmpHandle1MB, &tmpFd, O_CLOEXEC | O_RDWR);
    if (s32Ret != 0) {
        return s32Ret;
    }
    printf("alloc 1M handle %d, name %d, fd %d tmpHandle %d tmpSize %d\n",
			s32Handle1MB, s32Name1MB, s32Fd1MB, tmpHandle1MB, tmpSize);
	
	
	
	
    int tmp1Handle1MB = 0;
    int tmp1Size = 0;
    int tmp1Fd = 0;
    s32Ret = drm_get_info_from_name(s32DrmFd, s32Name1MB, &tmp1Handle1MB, &tmp1Size);
    if (s32Ret != 0) {
        return s32Ret;
    }
    s32Ret = drm_handle_to_fd(s32DrmFd, tmp1Handle1MB, &tmp1Fd, O_CLOEXEC | O_RDWR);
    if (s32Ret != 0) {
        return s32Ret;
    }
    s32Ret = drm_close_handle(s32DrmFd, tmp1Handle1MB);
    if (s32Ret != 0) {
        return s32Ret;
    }
	close(tmp1Fd);

    printf("alloc 1M handle %d, name %d, fd %d tmp1Handle %d tmp1Size %d\n",
                s32Handle1MB, s32Name1MB, s32Fd1MB, tmp1Handle1MB, tmp1Size);

    s32Ret = drm_alloc(s32DrmFd, 4 * 1024 * 1024, 4096, &s32Handle4MB, 0);
    if (s32Ret != 0) {
        return s32Ret;
    }
    s32Ret = drm_handle_to_fd(s32DrmFd, s32Handle4MB, &s32Fd4MB, O_CLOEXEC | O_RDWR);
    if (s32Ret != 0) {
        return s32Ret;
    }
    int tmpHandle4MB2 = 0;
    s32Ret = drm_fd_to_handle(s32DrmFd, s32Fd4MB, &tmpHandle4MB2, 0);
    if (s32Ret != 0) {
        return s32Ret;
    }
    s32Ret = drm_get_name_from_handle(s32DrmFd, tmpHandle4MB2, &s32Name4MB);
    if (s32Ret != 0) {
        return s32Ret;
    }
    int tmpHandle4MB3 = 0;
    int tmpSize4MB3 = 0;
    s32Ret = drm_get_info_from_name(s32DrmFd, s32Name4MB, &tmpHandle4MB3, &tmpSize4MB3);
    if (s32Ret != 0) {
        return s32Ret;
    }
    printf("4MB handle %d fd %d tmpHandle %d name %d size %d\n",
            s32Handle4MB, s32Fd4MB, tmpHandle4MB2, s32Name4MB, tmpSize4MB3);

    if (s32DrmFd > 0) {
        drm_close(s32DrmFd);
    }

    return 0;
}
