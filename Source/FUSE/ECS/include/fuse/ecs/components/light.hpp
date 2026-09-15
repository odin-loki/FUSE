#pragma once

#include <fuse/ecs/math/vec.hpp>

namespace fuse::ecs {

struct DirectionalLight {
    static constexpr const char* component_name = "DirectionalLight";

    vec3 color = {1.f, 1.f, 1.f, 0.f};
    f32 intensity = 1.f;
};

struct PointLight {
    static constexpr const char* component_name = "PointLight";

    vec3 color = {1.f, 1.f, 1.f, 0.f};
    f32 intensity = 1.f;
    f32 radius = 10.f;
};

struct SpotLight {
    static constexpr const char* component_name = "SpotLight";

    vec3 color = {1.f, 1.f, 1.f, 0.f};
    f32 intensity = 1.f;
    f32 inner_cone_deg = 15.f;
    f32 outer_cone_deg = 30.f;
    f32 radius = 20.f;
};

} // namespace fuse::ecs
