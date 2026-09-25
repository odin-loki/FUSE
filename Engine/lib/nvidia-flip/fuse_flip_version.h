/* FUSE pin record for the vendored NVIDIA FLIP header (not an upstream file; FUSE contributors, MIT).
 * NVlabs/flip publishes no git tags; 1.7.0 is the version the README title and pyproject.toml
 * declare ("v1.7", version = "1.7") at the pinned commit. The authoritative pin is the commit in
 * Engine/lib/nvidia-flip/VERSION. Read by `fuse_lint vendored-pins` (version_header). */
#pragma once

#define FUSE_NVIDIA_FLIP_VERSION_MAJOR 1
#define FUSE_NVIDIA_FLIP_VERSION_MINOR 7
#define FUSE_NVIDIA_FLIP_VERSION_PATCH 0
