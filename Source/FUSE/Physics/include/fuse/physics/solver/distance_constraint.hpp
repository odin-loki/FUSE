#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/types.hpp>

namespace fuse::physics {

struct DistanceConstraint {
    u32 bodyA = 0;
    u32 bodyB = 0;
    vec3 localAnchorA{};
    vec3 localAnchorB{};
    f32 restLength = 0.f;
    f32 compliance = 0.f;
};

} // namespace fuse::physics
