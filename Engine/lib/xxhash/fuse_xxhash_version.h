/* FUSE pin header for the vendored xxHash (Engine/lib/xxhash/VERSION). Not an upstream file.
 *
 * `fuse_lint vendored-pins` reads the FUSE_XXHASH_VERSION_* defines below and requires them to equal
 * the pinned `version`; the #error below requires the vendored xxhash.h to declare the same version.
 * Relight's Remix-compatible hashes need XXH3 output stability, which xxHash guarantees from 0.8.0.
 */
#ifndef FUSE_XXHASH_VERSION_H
#define FUSE_XXHASH_VERSION_H

#define FUSE_XXHASH_VERSION_MAJOR 0
#define FUSE_XXHASH_VERSION_MINOR 8
#define FUSE_XXHASH_VERSION_PATCH 3

#if defined(XXH_VERSION_MAJOR)
#  if XXH_VERSION_MAJOR != FUSE_XXHASH_VERSION_MAJOR || XXH_VERSION_MINOR != FUSE_XXHASH_VERSION_MINOR || \
      XXH_VERSION_RELEASE != FUSE_XXHASH_VERSION_PATCH
#    error "Engine/lib/xxhash/xxhash.h does not match the version pinned in Engine/lib/xxhash/VERSION"
#  endif
#endif

#endif /* FUSE_XXHASH_VERSION_H */
