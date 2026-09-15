#include <fuse/compute/ray_march.hpp>
#include <fuse/math/sdf.hpp>

namespace fuse::compute {

namespace {

f32 sceneSdf(const RayMarchParams& params, math::Vec3 pos) {
    f32 distance = params.max_dist;

    for (u32 i = 0; i < params.object_count; ++i) {
        const SdfObject& obj = params.objects[i];
        const math::Vec3 local = pos - obj.position;
        f32 objectDistance = params.max_dist;

        switch (static_cast<SdfPrimitiveType>(obj.type)) {
        case SdfPrimitiveType::Sphere:
            objectDistance = math::SDF::sphere(local, obj.params.x);
            break;
        case SdfPrimitiveType::Box:
            objectDistance = math::SDF::box(local, obj.params);
            break;
        case SdfPrimitiveType::Capsule:
        case SdfPrimitiveType::Torus:
        default:
            objectDistance = params.max_dist;
            break;
        }

        distance = math::SDF::opSmoothUnion(distance, objectDistance, obj.alpha);
    }

    return distance;
}

} // namespace

f32 ray_march_center_hit_distance(const RayMarchParams& params) {
    const math::Vec3 rayDirection = params.cam_forward.normalized();
    const math::Vec3 rayOrigin = params.cam_pos;

    f32 t = 0.f;
    for (u32 step = 0; step < params.max_steps && t < params.max_dist; ++step) {
        const math::Vec3 sample = rayOrigin + rayDirection * t;
        const f32 distance = sceneSdf(params, sample);
        if (distance < params.min_dist) {
            return t;
        }
        t += distance;
    }

    return -1.f;
}

bool launch_ray_march_cpu(const RayMarchParams& params) {
    (void)params;
    return true;
}

} // namespace fuse::compute
