#pragma once

#include <fuse/math/aabb.hpp>
#include <fuse/math/vec.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::math {

/// Half-space classification relative to a plane `n·p + d = 0` (normalized `n`).
enum class PlaneSide : s32 {
    Behind = -1,
    On = 0,
    InFront = 1,
    Straddling = 2,
};

inline f32 planeSignedDistance(const Vec4& plane, const Vec3& point) {
    return plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w;
}

inline bool isDegeneratePlane(const Vec4& plane, f32 epsilon = 1e-8f) {
    const f32 lenSq = plane.x * plane.x + plane.y * plane.y + plane.z * plane.z;
    return lenSq < epsilon * epsilon;
}

inline PlaneSide classifyPoint(const Vec4& plane, const Vec3& point, f32 epsilon = 1e-5f) {
    if (isDegeneratePlane(plane, epsilon)) {
        return PlaneSide::On;
    }

    const f32 distance = planeSignedDistance(plane, point);
    if (distance > epsilon) {
        return PlaneSide::InFront;
    }
    if (distance < -epsilon) {
        return PlaneSide::Behind;
    }
    return PlaneSide::On;
}

/// Positive-vertex test for an AABB against a plane (frustum culling convention).
inline PlaneSide classifyAabb(const Vec4& plane, const AABB& box) {
    if (box.isEmpty()) {
        return PlaneSide::Behind;
    }
    if (isDegeneratePlane(plane)) {
        return PlaneSide::Straddling;
    }

    const Vec3 positive{
        plane.x >= 0.f ? box.max.x : box.min.x,
        plane.y >= 0.f ? box.max.y : box.min.y,
        plane.z >= 0.f ? box.max.z : box.min.z,
    };
    const Vec3 negative{
        plane.x >= 0.f ? box.min.x : box.max.x,
        plane.y >= 0.f ? box.min.y : box.max.y,
        plane.z >= 0.f ? box.min.z : box.max.z,
    };

    const f32 positiveDistance = planeSignedDistance(plane, positive);
    const f32 negativeDistance = planeSignedDistance(plane, negative);

    if (positiveDistance < 0.f) {
        return PlaneSide::Behind;
    }
    if (negativeDistance > 0.f) {
        return PlaneSide::InFront;
    }
    return PlaneSide::Straddling;
}

/// Clips a segment `[a, b]` against the positive half-space of `plane`.
/// Returns false when the segment is fully behind the plane.
inline bool clipSegmentAgainstPlane(const Vec4& plane, Vec3& a, Vec3& b, f32 epsilon = 1e-5f) {
    if (isDegeneratePlane(plane, epsilon)) {
        return true;
    }

    const f32 da = planeSignedDistance(plane, a);
    const f32 db = planeSignedDistance(plane, b);

    const bool aIn = da >= -epsilon;
    const bool bIn = db >= -epsilon;

    if (!aIn && !bIn) {
        return false;
    }
    if (aIn && bIn) {
        return true;
    }

    if (std::fabs(da - db) <= epsilon) {
        return aIn;
    }

    const f32 t = da / (da - db);
    const Vec3 intersection{
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
    };

    if (aIn) {
        b = intersection;
    } else {
        a = intersection;
    }
    return true;
}

/// Sutherland–Hodgman clip of a convex polygon against one plane (CPU stub).
/// Returns the clipped vertex count (0 when fully culled).
inline u32 clipPolygonAgainstPlane(const Vec4& plane, const Vec3* input, u32 inputCount, Vec3* output,
                                   u32 maxOutput, f32 epsilon = 1e-5f) {
    if (inputCount == 0 || maxOutput == 0) {
        return 0;
    }
    if (isDegeneratePlane(plane, epsilon)) {
        if (inputCount > maxOutput) {
            return 0;
        }
        for (u32 i = 0; i < inputCount; ++i) {
            output[i] = input[i];
        }
        return inputCount;
    }

    u32 outCount = 0;
    Vec3 previous = input[inputCount - 1];
    bool previousInside = planeSignedDistance(plane, previous) >= -epsilon;

    for (u32 i = 0; i < inputCount; ++i) {
        const Vec3 current = input[i];
        const bool currentInside = planeSignedDistance(plane, current) >= -epsilon;

        if (currentInside) {
            if (!previousInside) {
                if (outCount >= maxOutput) {
                    return 0;
                }
                Vec3 clippedPrevious = previous;
                Vec3 clippedCurrent = current;
                if (!clipSegmentAgainstPlane(plane, clippedPrevious, clippedCurrent, epsilon)) {
                    return 0;
                }
                output[outCount++] = clippedPrevious;
            }
            if (outCount >= maxOutput) {
                return 0;
            }
            output[outCount++] = current;
        } else if (previousInside) {
            if (outCount >= maxOutput) {
                return 0;
            }
            Vec3 clippedPrevious = previous;
            Vec3 clippedCurrent = current;
            if (!clipSegmentAgainstPlane(plane, clippedPrevious, clippedCurrent, epsilon)) {
                return 0;
            }
            output[outCount++] = clippedCurrent;
        }

        previous = current;
        previousInside = currentInside;
    }

    return outCount;
}

} // namespace fuse::math
