// FUSE pin header for the vendored Tracy client (not an upstream file). Tracy declares its version as
// tracy::Version::{Major,Minor,Patch} in public/common/TracyVersion.hpp; this restates it as
// *_VERSION_MAJOR/MINOR/PATCH defines so `fuse_lint vendored-pins` can read it, and the adapter
// (Source/FUSE/Core/src/profiler/tracy_adapter.cpp) static_asserts that both agree.
#pragma once

#define FUSE_TRACY_VERSION_MAJOR 0
#define FUSE_TRACY_VERSION_MINOR 14
#define FUSE_TRACY_VERSION_PATCH 1
