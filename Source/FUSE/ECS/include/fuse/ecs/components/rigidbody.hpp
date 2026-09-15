#pragma once

#include <fuse/ecs/math/vec.hpp>

namespace fuse::ecs {

struct RigidBody {
    static constexpr const char* component_name = "RigidBody";

    vec3 velocity{};
    vec3 angular_velocity{};
    vec3 force_accumulator{};
    vec3 torque_accumulator{};
    f32 mass = 1.f;
    f32 inv_mass = 1.f;
    f32 restitution = 0.4f;
    f32 linear_damping = 0.99f;
    f32 angular_damping = 0.98f;
    bool is_static = false;
    bool is_sleeping = false;
    f32 sleep_timer = 0.f;
};

} // namespace fuse::ecs
