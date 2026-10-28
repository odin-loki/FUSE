// FUSE Relight: frame-index wrap. modulo 0 is the linear case (no ring).
#pragma once

#include <cstdint>

namespace fuse::relight::render::frame {

/// Next index after `frame`. `modulo == 0` returns `frame`. Otherwise `(frame + 1) % modulo`.
/// The add is 64-bit so a uint32 overflow still reduces by `modulo`.
inline std::uint32_t next_frame_index(std::uint32_t frame, std::uint32_t modulo) {
    if (modulo == 0) {
        return frame;
    }
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(frame) + 1u) % modulo);
}

/// Forward distance from `earlier` to `later`.
/// `modulo == 0`: `later - earlier` when `later >= earlier`, otherwise 0.
/// Otherwise the forward distance on the ring of `modulo` slots.
inline std::uint32_t frames_between(std::uint32_t earlier, std::uint32_t later, std::uint32_t modulo) {
    if (modulo == 0) {
        return later >= earlier ? later - earlier : 0u;
    }
    const std::uint64_t m = modulo;
    const std::uint64_t e = static_cast<std::uint64_t>(earlier) % m;
    const std::uint64_t l = static_cast<std::uint64_t>(later) % m;
    return static_cast<std::uint32_t>(l >= e ? l - e : m - e + l);
}

} // namespace fuse::relight::render::frame
