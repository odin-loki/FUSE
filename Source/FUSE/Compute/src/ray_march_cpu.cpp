#include <fuse/compute/ray_march.hpp>
#include <fuse/math/sdf.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::compute {

namespace {

/// Capsule: segment from (0, -halfLength, 0) to (0, halfLength, 0) inflated by `radius`.
f32 capsuleSdf(math::Vec3 p, f32 radius, f32 halfLength) {
    const f32 h = std::max(halfLength, 0.f);
    const f32 y = p.y - std::clamp(p.y, -h, h);
    return math::Vec3{p.x, y, p.z}.length() - radius;
}

/// Torus around local Y: ring of `majorRadius` in the XZ plane, tube of `minorRadius`.
f32 torusSdf(math::Vec3 p, f32 majorRadius, f32 minorRadius) {
    const f32 ring = std::sqrt(p.x * p.x + p.z * p.z) - majorRadius;
    return std::sqrt(ring * ring + p.y * p.y) - minorRadius;
}

f32 objectSdf(const SdfObject& obj, math::Vec3 local, f32 fallback) {
    switch (static_cast<SdfPrimitiveType>(obj.type)) {
    case SdfPrimitiveType::Sphere:
        return math::SDF::sphere(local, obj.params.x);
    case SdfPrimitiveType::Box:
        return math::SDF::box(local, obj.params);
    case SdfPrimitiveType::Capsule:
        return capsuleSdf(local, obj.params.x, obj.params.y);
    case SdfPrimitiveType::Torus:
        return torusSdf(local, obj.params.x, obj.params.y);
    default:
        return fallback;
    }
}

math::Vec3 sceneNormal(const RayMarchParams& params, math::Vec3 p, f32 t) {
    const f32 h = std::max(params.min_dist, 1e-4f * std::max(t, 1.f));
    return math::SDF::finiteDifferenceNormal(
        [&params](math::Vec3 q) { return ray_march_scene_distance(params, q); }, p, h);
}

} // namespace

f32 ray_march_scene_distance(const RayMarchParams& params, const math::Vec3& position) {
    f32 distance = params.max_dist;

    for (u32 i = 0; i < params.object_count; ++i) {
        const SdfObject& obj = params.objects[i];
        const f32 objectDistance = objectSdf(obj, position - obj.position, params.max_dist);
        // A zero blend width would divide by zero in opSmoothUnion — treat it as a hard union.
        distance = obj.alpha > 0.f ? math::SDF::opSmoothUnion(distance, objectDistance, obj.alpha)
                                   : std::min(distance, objectDistance);
    }

    return distance;
}

f32 ray_march_hit_distance(const RayMarchParams& params, const math::Vec3& origin, const math::Vec3& direction) {
    const math::Vec3 rayDirection = direction.normalized();
    if (rayDirection.length() <= 0.f) {
        return -1.f;
    }

    f32 t = 0.f;
    for (u32 step = 0; step < params.max_steps && t < params.max_dist; ++step) {
        const math::Vec3 sample = origin + rayDirection * t;
        const f32 distance = ray_march_scene_distance(params, sample);
        if (distance < params.min_dist) {
            return t;
        }
        t += distance;
    }

    return -1.f;
}

f32 ray_march_center_hit_distance(const RayMarchParams& params) {
    return ray_march_hit_distance(params, params.cam_pos, params.cam_forward);
}

bool launch_ray_march_cpu(const RayMarchParams& params) {
    if (params.width == 0 || params.height == 0 || params.max_steps == 0 || !(params.min_dist > 0.f) ||
        !(params.max_dist > params.min_dist) || !(params.fov_rad > 0.f) || params.fov_rad >= 3.14159265f ||
        (params.object_count > 0 && params.objects == nullptr)) {
        return false;
    }

    auto* depth = static_cast<f32*>(params.depth_surface);
    auto* output = static_cast<math::Vec4*>(params.output_surface);
    const math::Vec3 forward = params.cam_forward.normalized();
    const math::Vec3 right = params.cam_right.normalized();
    const math::Vec3 up = params.cam_up.normalized();
    const f32 tanHalf = std::tan(0.5f * params.fov_rad);
    const f32 aspect = static_cast<f32>(params.width) / static_cast<f32>(params.height);

    for (u32 y = 0; y < params.height; ++y) {
        for (u32 x = 0; x < params.width; ++x) {
            const f32 ndcX = (2.f * (static_cast<f32>(x) + 0.5f) / static_cast<f32>(params.width) - 1.f);
            const f32 ndcY = (1.f - 2.f * (static_cast<f32>(y) + 0.5f) / static_cast<f32>(params.height));
            const math::Vec3 direction =
                (forward + right * (ndcX * tanHalf * aspect) + up * (ndcY * tanHalf)).normalized();
            const f32 t = ray_march_hit_distance(params, params.cam_pos, direction);
            const u32 idx = y * params.width + x;
            if (depth != nullptr) {
                depth[idx] = t;
            }
            if (output != nullptr) {
                output[idx] = t >= 0.f ? math::Vec4{sceneNormal(params, params.cam_pos + direction * t, t), 1.f}
                                       : math::Vec4{};
            }
        }
    }
    return true;
}

} // namespace fuse::compute
