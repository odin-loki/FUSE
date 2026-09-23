#pragma once

#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::ecs {

/// Local-space primitive shapes. `SDFObject::params` per shape: Sphere x = radius; Box xyz = half
/// extents; Capsule x = radius, y = half segment length (Y axis); Torus x = major radius, y = minor
/// radius (XZ plane); Cylinder x = radius, y = half height (Y axis); Custom evaluates as Sphere.
enum class SDFPrimitive : u8 {
    Sphere,
    Box,
    Capsule,
    Torus,
    Cylinder,
    Custom,
};

/// How an object combines with the scene built from every object before it (ascending
/// `csg_order`, then entity index): scene' = op(scene, d). See `fuse/ecs/sdf_csg.hpp`.
enum class SDFCsgOp : u8 {
    Union,       ///< min(scene, d)
    Subtract,    ///< max(scene, -d): carves the object's volume out of everything before it
    Intersect,   ///< max(scene, d): keeps only what lies inside the object
    SmoothUnion, ///< polynomial smooth min with blend radius `blend_radius` (hard union at 0)
};

/// Placeholder until GRIA α types land in core (B3.5).
static constexpr f32 kSdfDefaultBlendAlpha = 0.5f;

struct SDFObject {
    static constexpr const char* component_name = "SDFObject";

    SDFPrimitive type = SDFPrimitive::Sphere;
    SDFCsgOp op = SDFCsgOp::Union;
    vec3 params = {1.f, 0.f, 0.f, 0.f};
    u32 material_id = 0;
    f32 blend_alpha = kSdfDefaultBlendAlpha;
    bool casts_shadow = true;
    bool visible = true;
    /// World-space blend radius for `SDFCsgOp::SmoothUnion`.
    f32 blend_radius = 0.f;
    /// Surface displacement amplitude (sculpt Roughen); 0 = smooth primitive.
    f32 roughness = 0.f;
    /// CSG evaluation order; objects apply in ascending order (ties by entity index).
    u32 csg_order = 0;
};

} // namespace fuse::ecs
