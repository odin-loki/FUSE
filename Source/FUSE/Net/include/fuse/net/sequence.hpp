#pragma once

#include <cstdint>

namespace fuse::net {

/// Forward distance `(newer - older)` modulo 65536.
[[nodiscard]] inline std::uint16_t sequence_delta(std::uint16_t newer, std::uint16_t older) noexcept {
    return static_cast<std::uint16_t>(newer - older);
}

/// True only when `sequence_delta(newer, older)` is in `1..32767`. Equal and a delta of 32768 are false.
[[nodiscard]] inline bool sequence_ahead(std::uint16_t newer, std::uint16_t older) noexcept {
    const std::uint16_t delta = sequence_delta(newer, older);
    return delta >= 1 && delta <= 32767;
}

} // namespace fuse::net
