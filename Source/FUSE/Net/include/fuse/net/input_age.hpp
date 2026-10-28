#pragma once

#include <cstdint>

namespace fuse::net {

/// True when `now` is strictly more than `window` frames after `frame`.
/// A `now` earlier than `frame` is not expired.
[[nodiscard]] inline bool input_expired(std::uint32_t frame, std::uint32_t now,
                                         std::uint32_t window) noexcept {
    if (now < frame) {
        return false;
    }
    return (now - frame) > window;
}

/// True for ages in the closed range 0..128.
[[nodiscard]] inline bool input_age_ok(int age) noexcept {
    return age >= 0 && age <= 128;
}

} // namespace fuse::net
