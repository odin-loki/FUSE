#pragma once

#include <fuse/math/aabb.hpp>
#include <fuse/math/mat.hpp>
#include <fuse/math/vec.hpp>

#include <array>
#include <cmath>

namespace fuse::math {

/// View frustum extracted from a combined view-projection matrix.
struct Frustum {
    std::array<Vec4, 6> planes{};

    static Frustum fromViewProjection(const Mat4& view_projection) {
        Frustum frustum{};
        const f32* m = view_projection.data.data();

        frustum.planes[0] = {m[3] + m[0], m[7] + m[4], m[11] + m[8], m[15] + m[12]};
        frustum.planes[1] = {m[3] - m[0], m[7] - m[4], m[11] - m[8], m[15] - m[12]};
        frustum.planes[2] = {m[3] + m[1], m[7] + m[5], m[11] + m[9], m[15] + m[13]};
        frustum.planes[3] = {m[3] - m[1], m[7] - m[5], m[11] - m[9], m[15] - m[13]};
        frustum.planes[4] = {m[3] + m[2], m[7] + m[6], m[11] + m[10], m[15] + m[14]};
        frustum.planes[5] = {m[3] - m[2], m[7] - m[6], m[11] - m[10], m[15] - m[14]};

        for (Vec4& plane : frustum.planes) {
            const f32 length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
            if (length > 0.f) {
                plane.x /= length;
                plane.y /= length;
                plane.z /= length;
                plane.w /= length;
            }
        }
        return frustum;
    }

    bool intersectsSphere(const Vec3& center, f32 radius) const {
        for (const Vec4& plane : planes) {
            const f32 distance =
                plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
            if (distance < -radius) {
                return false;
            }
        }
        return true;
    }

    bool intersectsAabb(const AABB& box) const {
        for (const Vec4& plane : planes) {
            const Vec3 positive{
                plane.x >= 0.f ? box.max.x : box.min.x,
                plane.y >= 0.f ? box.max.y : box.min.y,
                plane.z >= 0.f ? box.max.z : box.min.z,
            };
            const f32 distance =
                plane.x * positive.x + plane.y * positive.y + plane.z * positive.z + plane.w;
            if (distance < 0.f) {
                return false;
            }
        }
        return true;
    }
};

} // namespace fuse::math
