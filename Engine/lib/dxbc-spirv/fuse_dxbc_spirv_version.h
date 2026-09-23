/* FUSE pin record for the vendored dxbc-spirv tree (not an upstream file; FUSE contributors, MIT).
 * dxbc-spirv has no release tags; 0.1.0 is the version its meson.build declares
 * (project('dxbc-spirv', ..., version : '0.1.0')). The authoritative pin is the commit in
 * Engine/lib/dxbc-spirv/VERSION, which is DXVK v3.1.1's subprojects/dxbc-spirv gitlink.
 * Read by `fuse_lint vendored-pins` (version_header). */
#pragma once

#define FUSE_DXBC_SPIRV_VERSION_MAJOR 0
#define FUSE_DXBC_SPIRV_VERSION_MINOR 1
#define FUSE_DXBC_SPIRV_VERSION_PATCH 0
