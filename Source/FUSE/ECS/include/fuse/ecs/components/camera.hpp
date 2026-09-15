#pragma once

#include <fuse/ecs/math/vec.hpp>

namespace fuse::ecs {

struct Camera {
    static constexpr const char* component_name = "Camera";

    f32 fov_deg = 75.f;
    f32 near_plane = 0.1f;
    f32 far_plane = 10000.f;
    f32 aspect_ratio = 16.f / 9.f;
    bool is_active = false;

    mat4 view = mat4::identity();
    mat4 projection = mat4::identity();
    mat4 view_projection = mat4::identity();

    struct Frustum {
        vec4 planes[6]{};
    } frustum{};
};

} // namespace fuse::ecs
