#pragma once

#include <fuse/physics/math.hpp>

#include <cmath>

namespace fuse::physics {

/// Raycast wheel suspension coefficients. `restLength` is the unloaded ray length.
struct SuspensionParams {
    f32 restLength = 0.f;
    f32 spring = 0.f;
    f32 damper = 0.f;
    f32 maxTravel = 0.f;
};

/// Force on the chassis along wheel-up. `compression` is how far the spring is squeezed.
struct SuspensionForce {
    vec3 force{};
    f32 compression = 0.f;
    bool grounded = false;
};

/// Spring-damper force for one raycast wheel.
///
/// compression = clamp(restLength - hitDistance, 0, maxTravel).
/// A negative `hitDistance` is a miss. The force along the normalized wheel-up axis is
/// spring * compression + damper * (prevCompression - compression) / dt.
/// `dt` <= 0 omits the damper. Negative spring and damper coefficients are zero.
inline SuspensionForce evaluate_suspension(vec3 up, f32 hitDistance, f32 prevHitDistance, f32 dt,
                                           const SuspensionParams& params) {
    const auto compressionOf = [&params](f32 distance) -> f32 {
        if (!(distance >= 0.f)) {
            return 0.f;
        }
        const f32 raw = params.restLength - distance;
        if (!(raw > 0.f)) {
            return 0.f;
        }
        return raw > params.maxTravel ? params.maxTravel : raw;
    };

    if (!std::isfinite(up.x) || !std::isfinite(up.y) || !std::isfinite(up.z)) {
        return {};
    }
    const f32 lenSq = up.dot(up);
    if (!(lenSq > 0.f) || !std::isfinite(lenSq)) {
        return {};
    }
    const f32 invLen = 1.f / std::sqrt(lenSq);
    if (!std::isfinite(invLen)) {
        return {};
    }
    const vec3 upAxis = up * invLen;

    if (!(params.restLength > 0.f) || !(params.maxTravel > 0.f) || !(hitDistance >= 0.f)) {
        return {};
    }

    const f32 compression = compressionOf(hitDistance);
    const f32 spring = params.spring > 0.f ? params.spring : 0.f;
    f32 along = spring * compression;
    if (dt > 0.f) {
        const f32 damper = params.damper > 0.f ? params.damper : 0.f;
        along += damper * (compressionOf(prevHitDistance) - compression) / dt;
    }

    SuspensionForce result;
    result.force = upAxis * along;
    result.compression = compression;
    result.grounded = true;
    return result;
}

} // namespace fuse::physics
