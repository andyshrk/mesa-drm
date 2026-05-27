# Repository Guidelines

## Project Structure & Module Organization

This repository is libdrm, a C userspace library for the DRM ioctl interface.
Core library sources live at the repository root, including `xf86drm.c`,
`xf86drmMode.c`, and public headers such as `xf86drm.h`. Driver-specific
libraries are grouped by directory: `amdgpu/`, `intel/`, `radeon/`, `nouveau/`,
`freedreno/`, `etnaviv/`, `exynos/`, `omap/`, `tegra/`, and `vc4/`. Do not
modify Canonical DRM UAPI headers under `include/drm/`; they must stay aligned
with the upstream kernel headers. Tests and small utilities live in `tests/`,
developer tools in `tools/`, installed data in `data/`, and man pages in `man/`.

## Build

    ninja -C Sbuild64/

## Coding Style & Naming Conventions

The project is C11 and loosely follows Linux kernel coding style. Match nearby
code: tabs for indentation in C blocks, kernel-style braces, lowercase function
names, and driver prefixes for driver APIs, for example `amdgpu_*` or
`drmMode*`. Keep public ABI changes minimal and intentional. For Meson files,
follow existing two-space indentation and option naming in `meson_options.txt`.

## Testing Guidelines

Add or update tests under the relevant `tests/<area>/` directory, then register
new test executables in that directory's `meson.build`. Prefer focused tests
that can run through `meson test -C builddir/`. Hardware-dependent tools should
remain opt-in or gracefully skip when devices are unavailable.

## Commit & Pull Request Guidelines

Commit subjects use an area prefix followed by a short imperative summary, for
example `tests/wbtest: Add -w flag for optional file writeback` or
`amdgpu: Use uint32_t for buffer lookup index`. Explain what changed and why in
the body when behavior or ABI is affected.

## Important Notes
- When working with APIs provided by libdrm, if the exact behavior of an API is unclear, refer to its implementation code. If you encounter ioctl-related calls within that implementation, consult the @../linux source code to further analyze the behavior of the ioctl.
- Always use the -s option when making git commits
