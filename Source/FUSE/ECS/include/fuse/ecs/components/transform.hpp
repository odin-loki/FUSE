#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>

namespace fuse::ecs {

struct Transform {
    static constexpr const char* component_name = "Transform";

    vec3 position = {0.f, 0.f, 0.f, 1.f};
    quat rotation = {0.f, 0.f, 0.f, 1.f};
    vec3 scale = {1.f, 1.f, 1.f, 0.f};
    EntityID parent = EntityID::null();

    mat4 local_to_world = mat4::identity();
    mat4 world_to_local = mat4::identity();
    bool dirty = true;
};

} // namespace fuse::ecs
