#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer {

/// TAA tuning parameters (B5.9 — P5 §5.9).
struct TAAParams {
    f32 blend_factor = 0.1f;
    f32 velocity_rejection = 0.9f;
    f32 depth_rejection = 0.1f;
    f32 clamp_gamma = 1.25f;
    bool use_catmull_rom = true;
};

/// Halton sub-pixel jitter sequence configuration.
struct TaaJitterDesc {
    u32 sequence_length = 8;
};

/// Ping-pong history buffer allocation parameters.
struct TaaHistoryBufferDesc {
    u32 width = 1;
    u32 height = 1;
};

/// CUDA surface inputs for the resolve pass (opaque until B2.6 interop wiring).
struct TaaResolveSurfaces {
    void* current_frame = nullptr;
    void* velocity_buffer = nullptr;
    void* depth_buffer = nullptr;
    void* output = nullptr;
};

/// Per-frame resolve request — consumed by `TaaResolve` / `TaaPass`.
struct TaaResolveDesc {
    TaaResolveSurfaces surfaces{};
    TAAParams params{};
    u32 width = 0;
    u32 height = 0;
};

/// Resolve bookkeeping returned by the stub backend.
struct TaaResolveStats {
    bool resolved = false;
    u32 width = 0;
    u32 height = 0;
    f32 last_blend = 0.f;
    bool history_swapped = false;
};

} // namespace fuse::renderer
