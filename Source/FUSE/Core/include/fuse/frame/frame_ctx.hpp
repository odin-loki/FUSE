#pragma once

#include <fuse/types.hpp>

namespace fuse::frame {

/// Per-frame timing and bookkeeping passed through dimension tick/render.
struct FrameCtx {
    float dt = 0.f;
    float time = 0.f;
    u32 frameIndex = 0;
};

} // namespace fuse::frame
