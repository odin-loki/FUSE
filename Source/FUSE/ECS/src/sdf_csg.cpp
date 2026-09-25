#include <fuse/ecs/sdf_csg.hpp>

#include <fuse/ecs/registry.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::ecs {

namespace {

f32 length3(f32 x, f32 y, f32 z) {
    return std::sqrt(x * x + y * y + z * z);
}

f32 length2(f32 x, f32 y) {
    return std::sqrt(x * x + y * y);
}

/// Rotates v by the inverse of unit quaternion q.
vec3 rotate_inverse(const quat& q, const vec3& v) {
    const f32 qx = -q.x;
    const f32 qy = -q.y;
    const f32 qz = -q.z;
    const f32 qw = q.w;
    // t = 2 * cross(q.xyz, v); v' = v + w * t + cross(q.xyz, t)
    const f32 tx = 2.f * (qy * v.z - qz * v.y);
    const f32 ty = 2.f * (qz * v.x - qx * v.z);
    const f32 tz = 2.f * (qx * v.y - qy * v.x);
    return {v.x + qw * tx + (qy * tz - qz * ty), v.y + qw * ty + (qz * tx - qx * tz),
            v.z + qw * tz + (qx * ty - qy * tx), 0.f};
}

} // namespace

f32 sdf_primitive_distance(SDFPrimitive type, const vec3& params, const vec3& p) {
    switch (type) {
    case SDFPrimitive::Box: {
        const f32 qx = std::fabs(p.x) - params.x;
        const f32 qy = std::fabs(p.y) - params.y;
        const f32 qz = std::fabs(p.z) - params.z;
        const f32 outside = length3(std::max(qx, 0.f), std::max(qy, 0.f), std::max(qz, 0.f));
        const f32 inside = std::min(std::max(qx, std::max(qy, qz)), 0.f);
        return outside + inside;
    }
    case SDFPrimitive::Capsule: {
        const f32 y = p.y - std::clamp(p.y, -params.y, params.y);
        return length3(p.x, y, p.z) - params.x;
    }
    case SDFPrimitive::Torus: {
        const f32 qx = length2(p.x, p.z) - params.x;
        return length2(qx, p.y) - params.y;
    }
    case SDFPrimitive::Cylinder: {
        const f32 dx = length2(p.x, p.z) - params.x;
        const f32 dy = std::fabs(p.y) - params.y;
        return std::min(std::max(dx, dy), 0.f) + length2(std::max(dx, 0.f), std::max(dy, 0.f));
    }
    case SDFPrimitive::Sphere:
    case SDFPrimitive::Custom:
    default:
        return length3(p.x, p.y, p.z) - params.x;
    }
}

f32 sdf_roughness_noise(const vec3& p) {
    return std::sin(7.1f * p.x + 1.3f) * std::sin(6.3f * p.y + 2.1f) * std::sin(5.7f * p.z + 0.7f);
}

f32 sdf_object_distance(const SDFObject& sdf, const Transform& transform, const vec3& world) {
    const vec3 offset{world.x - transform.position.x, world.y - transform.position.y,
                      world.z - transform.position.z, 0.f};
    vec3 local = rotate_inverse(transform.rotation, offset);
    const f32 sx = transform.scale.x != 0.f ? transform.scale.x : 1.f;
    const f32 sy = transform.scale.y != 0.f ? transform.scale.y : 1.f;
    const f32 sz = transform.scale.z != 0.f ? transform.scale.z : 1.f;
    local.x /= sx;
    local.y /= sy;
    local.z /= sz;
    const f32 minScale = std::min(std::fabs(sx), std::min(std::fabs(sy), std::fabs(sz)));
    f32 d = sdf_primitive_distance(sdf.type, sdf.params, local) * minScale;
    if (sdf.roughness != 0.f) {
        d += sdf.roughness * sdf_roughness_noise(local);
    }
    return d;
}

f32 sdf_smooth_min(f32 a, f32 b, f32 k) {
    if (!(k > 0.f)) {
        return std::min(a, b);
    }
    const f32 h = std::max(k - std::fabs(a - b), 0.f) / k;
    return std::min(a, b) - h * h * k * 0.25f;
}

f32 sdf_csg_apply(SDFCsgOp op, f32 scene, f32 d, f32 blend_radius) {
    switch (op) {
    case SDFCsgOp::Subtract:
        return std::max(scene, -d);
    case SDFCsgOp::Intersect:
        return std::max(scene, d);
    case SDFCsgOp::SmoothUnion:
        return sdf_smooth_min(scene, d, blend_radius);
    case SDFCsgOp::Union:
    default:
        return std::min(scene, d);
    }
}

f32 sdf_effective_blend_radius(const SDFObject& sdf) {
    if (sdf.op != SDFCsgOp::SmoothUnion || !(sdf.blend_radius > 0.f)) {
        return 0.f;
    }
    const f32 alpha = sdf.blend_alpha > 0.f ? std::min(sdf.blend_alpha, 1.f) : 0.f;
    return sdf.blend_radius * (alpha / kSdfDefaultBlendAlpha);
}

f32 sdf_bounding_radius(const SDFObject& sdf) {
    f32 r = 0.f;
    switch (sdf.type) {
    case SDFPrimitive::Box:
        r = length3(sdf.params.x, sdf.params.y, sdf.params.z);
        break;
    case SDFPrimitive::Capsule:
        r = sdf.params.x + sdf.params.y;
        break;
    case SDFPrimitive::Torus:
        r = sdf.params.x + sdf.params.y;
        break;
    case SDFPrimitive::Cylinder:
        r = length2(sdf.params.x, sdf.params.y);
        break;
    case SDFPrimitive::Sphere:
    case SDFPrimitive::Custom:
    default:
        r = sdf.params.x;
        break;
    }
    r += std::fabs(sdf.roughness);
    r += 0.25f * sdf_effective_blend_radius(sdf);
    return r;
}

void SdfCsgScene::build(Registry& registry) {
    m_entries.clear();
    registry.each<SDFObject, Transform>([&](EntityID id, SDFObject& sdf, Transform& transform) {
        if (sdf.visible) {
            m_entries.push_back({id, sdf, transform});
        }
    });
    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) {
        if (a.sdf.csg_order != b.sdf.csg_order) {
            return a.sdf.csg_order < b.sdf.csg_order;
        }
        return a.entity.index < b.entity.index;
    });
}

SdfCsgSample SdfCsgScene::sample(const vec3& world) const {
    SdfCsgSample result{};
    for (const Entry& entry : m_entries) {
        const f32 d = sdf_object_distance(entry.sdf, entry.transform, world);
        const f32 before = result.distance;
        result.distance = sdf_csg_apply(entry.sdf.op, before, d, sdf_effective_blend_radius(entry.sdf));
        // The surface belongs to this object when it now defines the value: for union ops when its
        // own distance is the nearer one, for subtract/intersect when it raised the value.
        bool owns = false;
        switch (entry.sdf.op) {
        case SDFCsgOp::Union:
        case SDFCsgOp::SmoothUnion:
            owns = d < before;
            break;
        case SDFCsgOp::Subtract:
            owns = -d > before;
            break;
        case SDFCsgOp::Intersect:
            owns = d > before;
            break;
        }
        if (owns) {
            result.material_id = entry.sdf.material_id;
            result.entity = entry.entity;
        }
    }
    return result;
}

u32 SdfCsgScene::next_csg_order() const {
    u32 next = 0;
    for (const Entry& entry : m_entries) {
        next = std::max(next, entry.sdf.csg_order + 1u);
    }
    return next;
}

} // namespace fuse::ecs
