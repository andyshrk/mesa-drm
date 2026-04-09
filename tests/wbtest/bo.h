/*
 * Buffer Object Management for wbtest
 * Manages DRM dumb buffers for overlay and writeback testing
 */

#ifndef __BO_H__
#define __BO_H__

#include <stdbool.h>
#include <stdint.h>
#include "util_math.h"

/* AFBC header size and alignment */
#define AFBC_HEADER_SIZE		16
#define AFBC_HDR_ALIGN		64
#define AFBC_SUPERBLK_PIXELS	256
#define AFBC_SUPERBLK_ALIGNMENT	128

/* Buffer object structure */
struct bo {
	int fd;
	void *ptr;
	size_t size;
	size_t offset;
	size_t pitch;
	unsigned handle;
	uint32_t width;
	uint32_t height;
};

/**
 * Create dumb buffer via DRM
 * @param fd: DRM device file descriptor
 * @param width: Buffer width
 * @param height: Buffer height
 * @param bpp: Bits per pixel
 * @return: Buffer object on success, NULL on failure
 */
struct bo *bo_create_dumb(int fd, unsigned int width, unsigned int height,
			     unsigned int bpp);

/**
 * Map buffer into userspace
 * @param bo: Buffer object
 * @return: 0 on success, -1 on failure
 */
int bo_map(struct bo *bo);

/**
 * Unmap buffer
 * @param bo: Buffer object
 */
void bo_unmap(struct bo *bo);

/**
 * Destroy buffer object
 * @param bo: Buffer object
 */
void bo_destroy(struct bo *bo);

/**
 * Create buffer with format support for wbtest
 * @param fd: DRM device file descriptor
 * @param format: DRM format fourcc
 * @param is_afbc: Whether to use AFBC compression
 * @param width: Buffer width
 * @param height: Buffer height
 * @param handles: Output handles array
 * @param pitches: Output pitches array
 * @param offsets: Output offsets array
 * @param pic_name: Optional picture file to load
 * @return: Buffer object on success, NULL on failure
 */
struct bo *wb_bo_create(int fd, uint32_t format, bool is_afbc,
		       unsigned int width, unsigned int height,
		       uint32_t handles[4], uint32_t pitches[4],
		       uint32_t offsets[4], const char *pic_name);

#endif /* __BO_H__ */
