#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>
#include <optional>

namespace fuse::renderer {

/// Hit of a decal projected in projector space.
/// `u` and `v` are in [0, 1] across the projector's XY square.
/// `depth` is projector-space z: 0 at the projector, 1 at the far cap.
struct DecalHit {
    float u = 0.f;
    float v = 0.f;
    float depth = 0.f;
};

/// Project `pointProjectorSpace` onto the decal box.
///
/// The box is centered at the origin in projector space: x and y in [-1, 1] are the UV square,
/// z in [0, 1] is depth (0 at the projector, 1 at the far cap). Bounds are inclusive.
///
/// Rejects (nullopt) when:
/// - any component of the point or the normal is non-finite
/// - the point lies outside the box
/// - `normalProjectorSpace.z >= 0` (only surfaces facing the projector, normal.z < 0, receive the decal)
///
/// On a hit: u = x * 0.5 + 0.5, v = y * 0.5 + 0.5, depth = z.
inline std::optional<DecalHit> project_decal(fuse::math::Vec3 pointProjectorSpace,
                                             fuse::math::Vec3 normalProjectorSpace) {
    const bool finite = std::isfinite(pointProjectorSpace.x) && std::isfinite(pointProjectorSpace.y) &&
                        std::isfinite(pointProjectorSpace.z) && std::isfinite(normalProjectorSpace.x) &&
                        std::isfinite(normalProjectorSpace.y) && std::isfinite(normalProjectorSpace.z);
    if (!finite) {
        return std::nullopt;
    }

    const bool inside = pointProjectorSpace.x >= -1.f && pointProjectorSpace.x <= 1.f &&
                        pointProjectorSpace.y >= -1.f && pointProjectorSpace.y <= 1.f &&
                        pointProjectorSpace.z >= 0.f && pointProjectorSpace.z <= 1.f;
    if (!inside) {
        return std::nullopt;
    }

    // Projector looks down +Z. A receiving surface faces back toward it.
    if (normalProjectorSpace.z >= 0.f) {
        return std::nullopt;
    }

    DecalHit hit{};
    hit.u = pointProjectorSpace.x * 0.5f + 0.5f;
    hit.v = pointProjectorSpace.y * 0.5f + 0.5f;
    hit.depth = pointProjectorSpace.z;
    return hit;
}

} // namespace fuse::renderer
