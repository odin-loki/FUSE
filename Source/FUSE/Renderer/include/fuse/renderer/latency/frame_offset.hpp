#pragma once

// CPU frame-index offset: the simulation frame shown by a present, from the present
// index and how many frames are in flight. Indices only. No timestamps.

#include <cstdint>

namespace fuse::renderer::latency {

/// `framesInFlight` is clamped to 0..8 before the subtract. Returns 0 when
/// `presentFrame` is below that clamped depth.
[[nodiscard]] inline constexpr uint64_t simulation_frame(uint64_t presentFrame, uint32_t framesInFlight) {
    constexpr uint32_t kMaxFramesInFlight = 8u;
    const uint32_t depth = framesInFlight > kMaxFramesInFlight ? kMaxFramesInFlight : framesInFlight;
    if (presentFrame < depth) {
        return 0u;
    }
    return presentFrame - depth;
}

} // namespace fuse::renderer::latency
