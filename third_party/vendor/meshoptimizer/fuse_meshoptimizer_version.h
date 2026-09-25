/* FUSE pin header for the vendored meshoptimizer (Engine/lib/meshoptimizer/VERSION). Not an upstream file.
 *
 * meshoptimizer.h declares only MESHOPTIMIZER_VERSION (major * 1000 + minor * 10; patch releases keep it),
 * which `fuse_lint vendored-pins` cannot read as X.Y.Z. The FUSE_MESHOPTIMIZER_VERSION_* defines below
 * restate the pinned tag; the #error requires the vendored meshoptimizer.h to agree on major.minor.
 * Include it after meshoptimizer.h (Source/FUSE/Renderer/geometry does, WP-1.2).
 */
#ifndef FUSE_MESHOPTIMIZER_VERSION_H
#define FUSE_MESHOPTIMIZER_VERSION_H

#define FUSE_MESHOPTIMIZER_VERSION_MAJOR 1
#define FUSE_MESHOPTIMIZER_VERSION_MINOR 1
#define FUSE_MESHOPTIMIZER_VERSION_PATCH 1

#if defined(MESHOPTIMIZER_VERSION)
#  if MESHOPTIMIZER_VERSION != (FUSE_MESHOPTIMIZER_VERSION_MAJOR * 1000 + FUSE_MESHOPTIMIZER_VERSION_MINOR * 10)
#    error "Engine/lib/meshoptimizer/src/meshoptimizer.h does not match the version pinned in Engine/lib/meshoptimizer/VERSION"
#  endif
#endif

#endif /* FUSE_MESHOPTIMIZER_VERSION_H */
