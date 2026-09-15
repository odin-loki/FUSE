#pragma once

#include <fuse/platform/thread.hpp>

namespace fuse::platform {

/// Portable GPU context threading rules (architecture-parallel §4.4).
/// v1: only the registered render thread may record or submit GPU work.

inline bool mayTouchGpuContext() {
    return isRenderThread();
}

/// Returns false when called off the render thread (non-fatal guard for stubs).
inline bool requireGpuContextThread() {
    return isRenderThread();
}

} // namespace fuse::platform
