#include <fuse/spatial/frustum.hpp>

#include <cmath>

namespace fuse::spatial {

Frustum extract_frustum(const fuse::ecs::mat4& view_projection) {
    Frustum frustum{};
    const f32* m = view_projection.data.data();

    frustum.planes[0] = {m[3] + m[0], m[7] + m[4], m[11] + m[8], m[15] + m[12]};
    frustum.planes[1] = {m[3] - m[0], m[7] - m[4], m[11] - m[8], m[15] - m[12]};
    frustum.planes[2] = {m[3] + m[1], m[7] + m[5], m[11] + m[9], m[15] + m[13]};
    frustum.planes[3] = {m[3] - m[1], m[7] - m[5], m[11] - m[9], m[15] - m[13]};
    frustum.planes[4] = {m[3] + m[2], m[7] + m[6], m[11] + m[10], m[15] + m[14]};
    frustum.planes[5] = {m[3] - m[2], m[7] - m[6], m[11] - m[10], m[15] - m[14]};

    for (fuse::ecs::vec4& plane : frustum.planes) {
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

bool test_aabb_frustum(const Frustum& frustum, const fuse::ecs::vec3& aabb_min,
                       const fuse::ecs::vec3& aabb_max) {
    for (const fuse::ecs::vec4& plane : frustum.planes) {
        const fuse::ecs::vec3 positive{
            plane.x >= 0.f ? aabb_max.x : aabb_min.x,
            plane.y >= 0.f ? aabb_max.y : aabb_min.y,
            plane.z >= 0.f ? aabb_max.z : aabb_min.z,
            0.f,
        };
        const f32 distance =
            plane.x * positive.x + plane.y * positive.y + plane.z * positive.z + plane.w;
        if (distance < 0.f) {
            return false;
        }
    }
    return true;
}

bool test_sphere_frustum(const Frustum& frustum, const fuse::ecs::vec3& center, fuse::f32 radius) {
    for (const fuse::ecs::vec4& plane : frustum.planes) {
        const f32 distance =
            plane.x * center.x + plane.y * center.y + plane.z * center.z + plane.w;
        if (distance < -radius) {
            return false;
        }
    }
    return true;
}

} // namespace fuse::spatial
