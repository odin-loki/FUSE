/* FUSE-authored (not an upstream volk file): the machine-checkable version pin for the vendored volk.
 * Upstream volk.h declares only VOLK_HEADER_VERSION (the Vulkan header patch it was generated from),
 * which `fuse_lint vendored-pins` cannot read as X.Y.Z. This header declares the pinned upstream tag
 * (1.4.304) in the *_VERSION_MAJOR/MINOR/PATCH form the lint checks against VERSION, and
 * Source/FUSE/Renderer/src/vk/vk_loader.cpp static_asserts VOLK_HEADER_VERSION == FUSE_VOLK_VERSION_PATCH,
 * so VERSION -> this header -> the compiled volk.h form one checked chain. Update together with VERSION. */
#ifndef FUSE_VOLK_PIN_H_
#define FUSE_VOLK_PIN_H_

#define FUSE_VOLK_VERSION_MAJOR 1
#define FUSE_VOLK_VERSION_MINOR 4
#define FUSE_VOLK_VERSION_PATCH 304

#endif
