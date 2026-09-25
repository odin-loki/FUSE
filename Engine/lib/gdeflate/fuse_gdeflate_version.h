/* FUSE pin header for the vendored GDeflate codec (Engine/lib/gdeflate/VERSION). Not an upstream file.
 *
 * The codec is NVIDIA's GDeflate branch of libdeflate, whose libdeflate.h declares only
 * LIBDEFLATE_VERSION_MAJOR / _MINOR (1.8). `fuse_lint vendored-pins` reads the FUSE_GDEFLATE_VERSION_*
 * defines below and requires them to equal the pinned `version`; the #error below requires the vendored
 * libdeflate.h to declare the same major.minor. Included by Source/FUSE/Relight/mods/assets/src/gdeflate.cpp
 * after libdeflate.h.
 */
#ifndef FUSE_GDEFLATE_VERSION_H
#define FUSE_GDEFLATE_VERSION_H

#define FUSE_GDEFLATE_VERSION_MAJOR 1
#define FUSE_GDEFLATE_VERSION_MINOR 8
#define FUSE_GDEFLATE_VERSION_PATCH 0

#if defined(LIBDEFLATE_VERSION_MAJOR)
#  if LIBDEFLATE_VERSION_MAJOR != FUSE_GDEFLATE_VERSION_MAJOR || LIBDEFLATE_VERSION_MINOR != FUSE_GDEFLATE_VERSION_MINOR
#    error "Engine/lib/gdeflate/libdeflate/libdeflate.h does not match the version pinned in Engine/lib/gdeflate/VERSION"
#  endif
#endif

#endif /* FUSE_GDEFLATE_VERSION_H */
