# wbtest - VBD Writeback Test Program

wbtest is a test program for VBD (Video Block Display) writeback functionality. It automates the VBD 3-layer composite writeback tests described in `vbd_verification_report.md`.

## Overview

wbtest testing by:
- Creating multiple overlay planes with different formats and modifiers
- Compositing 3 layers together
- Capturing writeback data for validation
- Supporting AFBC (32x8, 16x16) and RFBC (64x4) formats
- Supporting Raster (uncompressed) formats

## Build

```bash
# From drm root directory
ninja -C Sbuild64

# Output binary
./Sbuild64/tests/wbtest/wbtest
```

## Test Cases

Based on `vbd_verification_report.md`:

| Format | Modifier | Description |
|--------|----------|-------------|
| YUV444 | AFBC 32x8 | AFBC compressed 32x8 block size |
| YUV422 | AFBC 32x8 | AFBC compressed 32x8 block size |
| YUV420 | AFBC 32x8 | AFBC compressed 32x8 block size |
| YUV420 | AFBC 16x16 | AFBC compressed 16x16 block size |
| YUV422 | AFBC 16x16 | AFBC compressed 16x16 block size |
| YUV444 | AFBC 16x16 | AFBC compressed 16x16 block size |
| YUV444 | RFBC 64x4 | Rockchip RFBC 64x4 block size |
| YUV422 | RFBC 64x4 | Rockchip RFBC 64x4 block size |
| YUV420 | RFBC 64x4 | Rockchip RFBC 64x4 block size |
| YUV444 | Raster | Uncompressed format |
| YUV422 | Raster | Uncompressed format |
| YUV420 | Raster | Uncompressed format |

## Architecture

### Source Files

- `wbtest.c` - Main test program logic
- `bo.c` - Buffer object management for dumb buffers
- `bo.h` - Buffer object API header

### Key Components

**Test Case Structure** (`struct test_case`):
- `name` - Test case identifier
- `format_str` - Format string (e.g., "VU24", "YUYV", "YU08")
- `modifier` - Modifier string (e.g., "afbc32x8", "rfbc64x4")
- `resolution` - Resolution string (e.g., "3840x2160")
- `fourcc` - DRM format fourcc for writeback
- `wbc_mod` - Writeback modifier
- `plane_format[3]` - DRM format for each of 3 layers
- `plane_modifier[3]` - Modifier for each layer
- `file_pattern[3]` - Pattern file for each layer

**Device Structure** (`struct device`):
- Tracks DRM resources (connectors, CRTCs, planes)
- Manages atomic requests

## Format and Modifier Support

### Plane Formats

| Format | Fourcc | Planes | Description |
|--------|--------|--------|-------------|
| YUV420 | DRM_FORMAT_YUV420 | 1 | YUV 4:2:0 planar |
| YUV420_8BIT | DRM_FORMAT_YUV420_8BIT | 1 | YUV 4:2:0 8-bit |
| YUV420_10BIT | DRM_FORMAT_YUV420_10BIT | 1 | YUV 4:2:0 10-bit |
| YVU420 | DRM_FORMAT_YVU420 | 1 | YVU 4:2:0 planar |
| YUYV | DRM_FORMAT_YUYV | 1 | YUYV packed |
| YVYU | DRM_FORMAT_YVYU | 1 | YVYU packed |
| NV12 | DRM_FORMAT_NV12 | 2 | YUV 4:2:0 semi-planar |
| NV15 | DRM_FORMAT_NV15 | 2 | YUV 4:2:0 10-bit |
| NV16 | DRM_FORMAT_NV16 | 2 | YUV 4:2:2 semi-planar |
| NV20 | DRM_FORMAT_NV20 | 2 | YUV 4:2:2 10-bit |
| NV24 | DRM_FORMAT_NV24 | 2 | YUV 4:4:4 semi-planar |
| VUY888 | DRM_FORMAT_VUY888 | 1 | VUY 8:8:8 planar |
| R8 | DRM_FORMAT_R8 | 1 | 8-bit single channel (alpha) |

### Modifiers

- `DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_32x8)` - AFBC 32x8
- `DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16)` - AFBC 16x16
- `DRM_FORMAT_MOD_ROCKCHIP_RFBC(ROCKCHIP_RFBC_BLOCK_SIZE_64x4)` - RFBC 64x4
- `DRM_FORMAT_MOD_ROCKCHIP_TILED(ROCKCHIP_TILED_BLOCK_SIZE_4x4_MODE0)` - Rockchip tiled 4x4
- `0` - No modifier (linear/raster)

## Important Implementation Details

### Modifier Handling

When creating framebuffers, use the appropriate API based on modifier:

```c
if (modifier != 0) {
    ret = drmModeAddFB2WithModifiers(fd, width, height, format,
                                    handles, pitches, offsets, modifiers,
                                    fb_id, DRM_MODE_FB_MODIFIERS);
} else {
    ret = drmModeAddFB2(fd, width, height, format,
                        handles, pitches, offsets, fb_id, 0);
}
```

### Alpha Window Extra 256 Bytes

Per the VBD verification report, the Alpha layer (using DRM_FORMAT_R8 format) reads an extra 256 bytes of data. To avoid access violations:

```c
if (fourcc == DRM_FORMAT_R8) {
    // Allocate with extra space
    plane_bos[i] = wb_bo_create(dev->fd, fourcc, false,
                     TEST_WIDTH + 1, TEST_HEIGHT,
                     handles, pitches, offsets,
                     test->file_pattern[i]);
}
```

### Plane Array Management

Each plane must have separate handles/pitches/offsets/modifiers arrays to avoid data corruption:

```c
uint32_t plane_handles[3][4] = {{0}};
uint32_t plane_pitches[3][4] = {{0}};
uint32_t plane_offsets[3][4] = {{0}};
uint64_t plane_modifiers[3][4] = {{0}};
```

### Buffer Object Creation

`wb_bo_create()` creates dumb buffers via DRM and sets up multi-plane layouts:

- For 2-plane formats (NV12, NV16, NV24): sets handles[1], pitches[1], offsets[1]
- For 1-plane formats: only uses handles[0], pitches[0], offsets[0]

### Data Loading from Pattern Files

When wbtest runs, it reads pixel data from pre-filled pattern files and writes them to the three overlay layer framebuffers. The data loading process is as follows:

1. **Pattern File Specification**: Each test case defines three pattern files in `test_case.file_pattern[3]` array:
   - Layer 0: First pattern file (e.g., `res/3840x2160_y444_bin`)
   - Layer 1: Second pattern file
   - Layer 2: Third pattern file (typically alpha/Y420)

2. **File Reading**: In `wb_bo_create()`, if a `pic_name` is provided, the pattern data is loaded:
   ```c
   if (pic_name) {
       FILE *fp = fopen(pic_name, "rb");
       if (fp) {
           for (i = 0; i < virtual_height; i++) {
               size_t to_read = (width * bpp) >> 3;
               fread((char *)bo->ptr + i * bo->pitch, 1, to_read, fp);
           }
           fclose(fp);
       }
   }
   ```

3. **Data Format Considerations**:
   - **Compressed Data (AFBC/RFBC)**: Pattern files contain pre-compressed data matching the specified modifier. The driver handles the decompression/compression based on the modifier.
   - **Uncompressed Data (Raster)**: Pattern files contain raw pixel data in linear layout.
   - **Multi-plane Formats**: Data is read row by row based on the buffer's pitch (stride), which accounts for padding and alignment.

4. **Three-Layer Composition**: Each test case loads three separate pattern files into three different framebuffers, which are then composed together by the VBD hardware.

5. **Buffer Layout**: The data is written to the mapped buffer (`bo->ptr`) with proper pitch handling:
   - Each row starts at `bo->ptr + i * bo->pitch`
   - Row width is `(width * bpp) >> 3` bytes
   - Pitch may be larger than row width due to alignment requirements

## Reference Code

wbtest is derived from and shares code structure with the following reference programs:

### ovltest

`@tests/ovltest/ovltest.c` - Overlay/plane testing tool with AFBC support

**Similarities with wbtest**:
- Shared `bo.c/bo.h` buffer object management
- Uses `util/format.h` for format/modifier parsing
- Uses `util/kms.h` for DRM resource management
- Supports AFBC modes (afbc32x8, afbc16x16, rfbc64x4)
- Handles pattern file loading for layer data
- Command-line argument parsing for format/modifier configuration

**Key differences**:
- ovltest: Interactive command-line tool, supports dynamic plane setup
- wbtest: Automated test runner with predefined test cases

### modetest

`@tests/modetest/modetest.c` - Comprehensive DRM modesetting test tool

**Similarities with wbtest**:
- Common DRM resource discovery pattern (connectors, encoders, CRTCs, planes)
- Atomic commit handling with `drmModeAtomicReq`
- Buffer management through dumb buffers
- Property-based configuration approach

**Key differences**:
- modetest: General-purpose tool for testing all KMS features
- wbtest: Focused on VBD 3-layer composition and writeback

**Shared utility functions** (in `tests/util/`):
- `format.c` - Format and modifier parsing utilities
- `kms.c` - KMS object helpers
- `pattern.c` - Test pattern generation
- `common.h` - Common definitions

## Test Resources

Pattern files are loaded from `res/` directory:
- `res/3840x2160_y444_bin` - YUV444 pattern
- `res/3840x2160_y422_bin` - YUV422 pattern
- `res/3840x2160_y420_bin` - YUV420 pattern

## Running Tests

```bash
# Default uses /dev/dri/card0
./wbtest

# Expected output:
# VBD Writeback Test Program
# =============================
# Auto-running 12 test cases
#
# Found X connectors, Y encoders, Z crtcs
# Main connector: <id> (<resolution>@<Hz>)
# Writeback connector: <id>
# Using CRTC: <id>
# ...
# ========================================
# Test Summary
# ============
# Total: 12, Pass: X, Fail: Y
```

## Code Structure

### Main Functions

- `main()` - Entry point, iterates through all test cases
- `init_device()` - Initialize DRM device and find resources
- `cleanup_device()` - Free device resources
- `run_test_case()` - Execute a single test case
- `create_wb_fb()` - Create writeback framebuffer
- `setup_planes()` - Create and configure overlay planes
- `setup_display_and_wb()` - Configure display and writeback connector
- `trigger_writeback()` - Trigger writeback capture
- `create_custom_mode()` - Create custom display mode from resolution string

### Buffer Management Functions (bo.c/bo.h)

- `bo_create_dumb()` - Create dumb buffer via DRM
- `bo_map()` - Map buffer to userspace
- `bo_unmap()` - Unmap buffer
- `bo_destroy()` - Destroy buffer object
- `wb_bo_create()` - Create buffer with format and AFBC support

### Atomic Helpers

- `get_property_id()` - Find property ID by name
- `add_property()` - Add property to atomic request

## Coding principles
- Keep the code logic as concise as possible.

## Test
- Copy(via command like scp) the wbtest binary to the remote target board: root@172.16.12.213:~/
- Run wbtest on the target board and confirm successful execution.

