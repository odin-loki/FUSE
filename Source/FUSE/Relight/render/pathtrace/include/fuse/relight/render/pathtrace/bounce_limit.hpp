// Path bounce budget: a bounce continues only while its index is inside a positive max-bounce count.
#pragma once

namespace fuse::relight::render::pathtrace {

/// True when `maxBounces > 0`, `bounce >= 0`, and `bounce < maxBounces`.
[[nodiscard]] inline bool bounce_continues(int bounce, int maxBounces) {
    return maxBounces > 0 && bounce >= 0 && bounce < maxBounces;
}

} // namespace fuse::relight::render::pathtrace
