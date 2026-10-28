#pragma once

#include <cmath>

namespace fuse::compute {

/// True when the closed tile interval lies completely outside the closed range.
/// Swapped bounds are ordered first. Sharing an endpoint counts as inside.
/// Any non-finite bound rejects the tile.
[[nodiscard]] inline bool tile_outside(float tileMin, float tileMax, float rangeMin, float rangeMax) {
    if (!std::isfinite(tileMin) || !std::isfinite(tileMax) || !std::isfinite(rangeMin) ||
        !std::isfinite(rangeMax)) {
        return true;
    }
    const float tileLo = tileMin < tileMax ? tileMin : tileMax;
    const float tileHi = tileMin < tileMax ? tileMax : tileMin;
    const float rangeLo = rangeMin < rangeMax ? rangeMin : rangeMax;
    const float rangeHi = rangeMin < rangeMax ? rangeMax : rangeMin;
    return tileHi < rangeLo || tileLo > rangeHi;
}

} // namespace fuse::compute
