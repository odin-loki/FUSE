#pragma once

#include <fuse/ecs/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::ecs {

enum class SDFPrimitive : u8 {
    Sphere,
    Box,
    Capsule,
    Torus,
    Cylinder,
    Custom,
};

/// Placeholder until GRIA α types land in core (B3.5).
static constexpr f32 kSdfDefaultBlendAlpha = 0.5f;

struct SDFObject {
    static constexpr const char* component_name = "SDFObject";

    SDFPrimitive type = SDFPrimitive::Sphere;
    vec3 params = {1.f, 0.f, 0.f, 0.f};
    u32 material_id = 0;
    f32 blend_alpha = kSdfDefaultBlendAlpha;
    bool casts_shadow = true;
    bool visible = true;
};

} // namespace fuse::ecs
