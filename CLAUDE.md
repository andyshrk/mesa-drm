# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System

This is libdrm, a userspace library for accessing DRM (Direct Rendering Manager) on Linux. It uses Meson as the build system.
If you need to analyze the implementation of the corresponding ioctl calls in the Linux kernel, you can refer to the Linux kernel source code in the following directory: @../linux/.

### Cross-Compilation

To cross-compile for ARM:
```bash
# For aarch64 (64-bit ARM)
meson setup  --default-library static  --cross-file  cross_aarch64 Sbuild64/

# For arm (32-bit ARM)
meson setup --default-library static --cross-file cross_arm  Sbuild 
```
The cross files define toolchain paths and host machine properties (arch, endian).

### Build Commands

# Build
ninja -C Sbuild64/

## Architecture

### Core Library

The main libdrm library is built from:
- `xf86drm.c` - Core DRM ioctl wrappers
- `xf86drmMode.c` - Modesetting interface (KMS)
- `xf86drmHash.c`, `xf86drmRandom.c`, `xf86drmSL.c` - Utility structures

Key public headers:
- `xf86drm.h` - Core DRM API
- `xf86drmMode.h` - Kernel Mode Setting (KMS) API
- `include/drm/*.h` - Kernel DRM headers synced from Linux UAPI
- `include/drm/drm_fourcc.h` - DRM FORMAT definition

### Test Programs

The `tests/` directory contains test and debugging tools:

Core test utilities (in `tests/util/`):
- `libutil` static library - Shared utilities for tests (format.c, kms.c, pattern.c)
- `format.c` - Format utilities including AFBC (Arm Frame Buffer Compression) support

Test programs:
- `modetest/` - Test DRM modesetting, display modes, and planes (supports hcolorbar/vcolorbar patterns)
- `ovltest/` - Overlay/plane testing tool with AFBC mode support (split, sparse flags)
- `proptest/` - Property testing tool
- `modeprint/` - Print DRM mode information
- `cursor/` - Cursor testing
- `wbtest/` - Writeback testing with AFBC/RFBC support
- `fbdump/` - Framebuffer dump utility (`fbdump_drm`) with AFBC support, includes split/sparse flags in filename
- `fps/` - FPS measurement (supports DRM lease)
- `vbltest/` - VBlank testing
- `event/` - DRM event handling
- `drmdevice.c` - List DRM devices
- `hash.c`, `drmsl.c` - Hash and DRM string list tests

Running individual test programs:
```bash
# After building in Sbuild64/
./Sbuild64/tests/modetest/modetest
./Sbuild64/tests/ovltest/ovltest
./Sbuild64/tests/proptest/proptest
./Sbuild64/tests/fbdump/fbdump_drm
```

Note: Most test programs are statically linked (`-static` linker flag) for easier deployment to target devices.

### Pixel formats  
- Pixel formats in DRM(DRM_FORMAT_RGB888/XRGB8888/YUV420_10BIT) are defined in include/drm/drm_fourcc.h 
- Format abbreviations such as YU08, AR24, and RG24 correspond to the strings of fourcc_code defined
  in the drm_fourcc.h header file.

### AFBC/RFBC Support

This codebase has extensive customization for AFBC (Arm Frame Buffer Compression):
- Custom fourcc formats in `include/drm/drm_fourcc.h` (YUV420_8BIT, YUV420_10BIT, DRM_FORMAT_YUYV, DRM_FORMAT_VUY888)
- AFBC buffer utilities in `tests/util/format.c`
- Test resources in `res/` directory for AFBC testing
- `fbdump` and `ovltest` support AFBC-specific features like split and sparse flags
- Identification of non-linear/compressed(AFBC/RFBC/TILE) formats in DRM programming: DRM_FORMAT_xxx + modifier.
  When allocate for a framebuffer, these two parameters must be passed to either drmModeAddFB2WithModifiers or drmModeAddFB2, depending on whether the requested format is compressed/TILE or non-compressed.

### Tools Directory

`tools/` contains helper scripts like `fps_diagnose.py` for performance diagnostics.

## Key Concepts

### DRM Objects

The KMS API works with these primary objects:
- **Connectors** - Physical display outputs (HDMI, DP, etc.)
- **Encoders** - Signal encoders that drive connectors
- **CRTCs** - Scanout engines that produce pixel data
- **Planes** - Hardware composition layers (primary, overlay, cursor)
- **Framebuffers** - Memory objects containing pixel data

### Format Modifiers

The library uses format modifiers (from `drm_fourcc.h`) to describe non-linear/tiling/compression(AFBC/RFBC/TILE) formats.
Simple modifiers are auto-generated via `gen_table_fourcc.py` into `generated_static_table_fourcc.h`.

## Android Build

This codebase also supports Android build via `Android.bp` and `Android.sources.bp` files.

## Additional Customization

### Resources Directory

`res/` contains test resource files including AFBC test binaries and reference data for testing.

## Important Notes

- When working with APIs provided by libdrm, if the exact behavior of an API is unclear, refer to its implementation code. If you encounter ioctl-related calls within that implementation, consult the @../linux source code to further analyze the behavior of the ioctl.
