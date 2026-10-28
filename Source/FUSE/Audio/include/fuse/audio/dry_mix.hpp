#pragma once

#include <cmath>

namespace fuse::audio {

/// True when \p mix is finite and in [0, 1].
[[nodiscard]] inline bool dry_mix_ok(float mix) {
    return std::isfinite(mix) && mix >= 0.f && mix <= 1.f;
}

} // namespace fuse::audio
