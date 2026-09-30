#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/math/vec.hpp>

namespace fuse::ecs {

/// Kinematic capsule character (GAP-PHYS-CHARACTER), moved by fuse::physics::CharacterControllerSystem in
/// the physics phase (PhysicsManager::step runs it before the rigid-body solve). The entity's Transform
/// position is the capsule centre; the capsule stands along world +Y. Game code writes the input fields;
/// the system writes the state fields. Add RigidBody + Collider + TagKinematic as well if rigid bodies
/// should be pushed out of the character's way by the solver (the controller ignores its own body).
struct CharacterController {
    static constexpr const char* component_name = "CharacterController";

    // Shape and tuning.
    f32 radius = 0.3f;
    /// Half length of the capsule's inner segment (total height = 2 * (half_height + radius)).
    f32 half_height = 0.6f;
    /// Obstacles up to this height are stepped onto.
    f32 step_height = 0.35f;
    /// Steeper ground is a wall (not walkable): the character slides down it.
    f32 max_slope_deg = 45.f;
    /// While grounded, the character is kept on ground up to this far below (walking down stairs / slopes).
    f32 snap_distance = 0.3f;
    /// Gap kept between the capsule and the world.
    f32 skin_width = 0.01f;
    /// Vertical acceleration (m/s^2).
    f32 gravity = -9.81f;
    f32 max_fall_speed = 50.f;
    /// Force (N) pushed onto dynamic bodies the character walks into.
    f32 push_force = 200.f;
    /// Collision layers the character collides with.
    u32 layer_mask = 0xFFFFFFFFu;

    // Input (written by game code).
    /// Desired horizontal velocity in world space (y ignored).
    vec3 move_velocity{};
    /// > 0: jump with this vertical speed on the next step if grounded (then cleared).
    f32 jump_speed = 0.f;

    // State (written by the system).
    vec3 velocity{};
    vec3 ground_normal{0.f, 1.f, 0.f, 0.f};
    /// Velocity inherited from the body stood on (moving platforms).
    vec3 platform_velocity{};
    EntityID ground_entity{};
    bool grounded = false;
    bool on_steep_slope = false;
    /// Bit 0: sides hit, bit 1: ceiling hit, bit 2: ground hit (last step).
    u32 collision_flags = 0;
};

} // namespace fuse::ecs
