/* FUSE pin header for the vendored TinyUSDZ subset (Engine/lib/tinyusdz/VERSION). Not an upstream file.
 *
 * `fuse_lint vendored-pins` reads the FUSE_TINYUSDZ_VERSION_* defines below and requires them to equal the
 * pinned `version` (the upstream tag v0.9.4). Upstream's own src/tinyusdz.hh still declares
 * version_major/minor/micro = 0.9.1 at that tag (the constants were not bumped for 0.9.3 / 0.9.4), so the check
 * against the vendored header is on major.minor only: a static_assert when this header is included after
 * tinyusdz.hh (Source/FUSE/Relight/mods/usd/src/layer_loader.cpp does).
 */
#ifndef FUSE_TINYUSDZ_VERSION_H
#define FUSE_TINYUSDZ_VERSION_H

#define FUSE_TINYUSDZ_VERSION_MAJOR 0
#define FUSE_TINYUSDZ_VERSION_MINOR 9
#define FUSE_TINYUSDZ_VERSION_PATCH 4

#if defined(__cplusplus) && defined(TINYUSDZ_HH_)
static_assert(tinyusdz::version_major == FUSE_TINYUSDZ_VERSION_MAJOR && tinyusdz::version_minor == FUSE_TINYUSDZ_VERSION_MINOR,
              "Engine/lib/tinyusdz/src/tinyusdz.hh does not match the version pinned in Engine/lib/tinyusdz/VERSION");
#endif

#endif /* FUSE_TINYUSDZ_VERSION_H */
