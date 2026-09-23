#include <fuse/compute/ray_march.hpp>
#include <fuse/math/sdf.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::compute {

namespace {

f32 clampedRounding(const SdfObject& obj) {
    const f32 smallest = std::min(obj.params.x, std::min(obj.params.y, obj.params.z));
    return std::clamp(obj.rounding, 0.f, std::max(smallest, 0.f));
}

/// Distance of one primitive in its local frame; writes the analytic gradient to `*grad` when non-null.
f32 objectSdf(const SdfObject& obj, math::Vec3 local, f32 fallback, math::Vec3* grad) {
    switch (static_cast<SdfPrimitiveType>(obj.type)) {
    case SdfPrimitiveType::Sphere:
        if (grad != nullptr) {
            *grad = math::SDF::sphereGradient(local);
        }
        return math::SDF::sphere(local, obj.params.x);
    case SdfPrimitiveType::Box: {
        const f32 r = clampedRounding(obj);
        const math::Vec3 inner{obj.params.x - r, obj.params.y - r, obj.params.z - r};
        if (grad != nullptr) {
            *grad = math::SDF::boxGradient(local, inner);
        }
        return math::SDF::box(local, inner) - r;
    }
    case SdfPrimitiveType::Capsule: {
        const f32 h = std::max(obj.params.y, 0.f);
        const math::Vec3 v{local.x, local.y - std::clamp(local.y, -h, h), local.z};
        if (grad != nullptr) {
            *grad = math::SDF::sphereGradient(v);
        }
        return v.length() - obj.params.x;
    }
    case SdfPrimitiveType::Torus: {
        const f32 radial = std::sqrt(local.x * local.x + local.z * local.z);
        const f32 ring = radial - obj.params.x;
        const f32 tube = std::sqrt(ring * ring + local.y * local.y);
        if (grad != nullptr) {
            if (tube < 1e-8f) {
                *grad = {1.f, 0.f, 0.f};
            } else {
                const f32 rx = radial > 1e-8f ? local.x / radial : 1.f;
                const f32 rz = radial > 1e-8f ? local.z / radial : 0.f;
                const f32 s = ring / tube;
                *grad = {s * rx, local.y / tube, s * rz};
            }
        }
        return tube - obj.params.y;
    }
    default:
        if (grad != nullptr) {
            *grad = {};
        }
        return fallback;
    }
}

/// Scene distance (hard / smooth union, in object order) and, when `grad` is non-null, its analytic gradient.
/// Smooth union d = min(a, b) - h^2 k / 4 with h = max(k - |a - b|, 0) / k has, for a <= b,
/// grad d = (1 - h/2) grad a + (h/2) grad b (continuous across a = b where both weights are 1/2).
f32 sceneEval(const RayMarchParams& params, math::Vec3 position, math::Vec3* grad) {
    f32 distance = params.max_dist;
    math::Vec3 gradient{};

    for (u32 i = 0; i < params.object_count; ++i) {
        const SdfObject& obj = params.objects[i];
        math::Vec3 objectGrad{};
        const f32 objectDistance =
            objectSdf(obj, position - obj.position, params.max_dist, grad != nullptr ? &objectGrad : nullptr);
        // A zero blend width would divide by zero in opSmoothUnion — treat it as a hard union.
        if (obj.alpha > 0.f) {
            if (grad != nullptr) {
                const f32 h = std::max(obj.alpha - std::abs(distance - objectDistance), 0.f) / obj.alpha;
                const f32 wMin = 1.f - 0.5f * h;
                const f32 wMax = 0.5f * h;
                gradient = distance <= objectDistance ? gradient * wMin + objectGrad * wMax
                                                      : gradient * wMax + objectGrad * wMin;
            }
            distance = math::SDF::opSmoothUnion(distance, objectDistance, obj.alpha);
        } else {
            if (objectDistance < distance) {
                gradient = objectGrad;
            }
            distance = std::min(distance, objectDistance);
        }
    }

    if (grad != nullptr) {
        *grad = gradient;
    }
    return distance;
}

} // namespace

f32 ray_march_scene_distance(const RayMarchParams& params, const math::Vec3& position) {
    return sceneEval(params, position, nullptr);
}

math::Vec3 ray_march_scene_normal(const RayMarchParams& params, const math::Vec3& position) {
    math::Vec3 gradient{};
    (void)sceneEval(params, position, &gradient);
    const f32 len = gradient.length();
    if (!(len > 1e-12f)) {
        return {0.f, 1.f, 0.f};
    }
    return gradient * (1.f / len);
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
                output[idx] = t >= 0.f ? math::Vec4{ray_march_scene_normal(params, params.cam_pos + direction * t), 1.f}
                                       : math::Vec4{};
            }
        }
    }
    return true;
}

} // namespace fuse::compute
