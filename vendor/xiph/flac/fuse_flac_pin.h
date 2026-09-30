/* FUSE-authored pin header (not upstream code). It restates the version of the vendored
 * libFLAC source archive as *_VERSION_MAJOR/MINOR/PATCH defines so `fuse_lint vendored-pins` can check
 * it against VERSION; cmake/FuseXiph.cmake builds the archive and the runtime check in
 * Source/FUSE/Audio/tests/test_audio_output.cpp compares the library's own version string. */
#ifndef FUSE_XIPH_FLAC_PIN_H
#define FUSE_XIPH_FLAC_PIN_H
#define FUSE_XIPH_FLAC_VERSION_MAJOR 1
#define FUSE_XIPH_FLAC_VERSION_MINOR 5
#define FUSE_XIPH_FLAC_VERSION_PATCH 0
#endif
