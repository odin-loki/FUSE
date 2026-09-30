#pragma once

// Kinematic capsule character controller (GAP-PHYS-CHARACTER). Movement is a sequence of shape casts
// against the PhysicsManager's world (PhysicsManager::shapeCast, every shape type incl. hulls, meshes,
// voxels and SDFs), never a rigid-body solve:
//
//   depenetrate   push out of anything the capsule starts inside (overlap query)
//   up            jump / upward motion
//   side          collide-and-slide along the horizontal motion; non-walkable surfaces act as vertical
//                 walls (no climbing slopes steeper than max_slope). When blocked while grounded, a step
//                 up by step_height -> side -> step down is tried and kept when it gets further and lands
//                 on walkable ground (stairs, curbs).
//   down          gravity; sliding down non-walkable slopes
//   ground probe  walkable ground within the skin (or within snap_distance when it was grounded and not
//                 jumping: stay glued to slopes / stairs going down)
//
// Casts stop at the first contact of the swept capsule, so no speed tunnels through thin geometry. Dynamic
// bodies walked into receive push_force; the velocity of the body stood on (moving platforms) is inherited.
// CharacterControllerSystem runs every entity with Transform + CharacterController; PhysicsManager::step
// runs it first in the physics phase (before the rigid-body solve).

#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/physics/math.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::physics {

class PhysicsManager;

struct CharacterControllerDesc {
    f32 radius = 0.3f;
    f32 halfHeight = 0.6f;
    f32 stepHeight = 0.35f;
    f32 maxSlopeDeg = 45.f;
    f32 snapDistance = 0.3f;
    f32 skinWidth = 0.01f;
    u32 layerMask = 0xFFFFFFFFu;
    u32 maxSlideIterations = 4;
};

/// A dynamic body the character walked into during a move.
struct CharacterPush {
    fuse::ecs::EntityID entity{};
    vec3 point{};
    /// Horizontal push direction (unit).
    vec3 direction{};
};

struct CharacterMoveResult {
    vec3 position{};
    bool grounded = false;
    bool onSteepSlope = false;
    vec3 groundNormal{0.f, 1.f, 0.f};
    fuse::ecs::EntityID groundEntity{};
    vec3 groundPoint{};
    bool steppedUp = false;
    /// Bit 0 sides, bit 1 ceiling, bit 2 ground.
    u32 collisionFlags = 0;
};

class CharacterController {
public:
    /// Moves the capsule centred at `position` by `displacement` through `world`, ignoring `self`.
    /// `wasGrounded` enables stepping and ground snapping. Pushes onto dynamic bodies are appended to
    /// `pushes` when given.
    static CharacterMoveResult move(const PhysicsManager& world, const CharacterControllerDesc& desc, vec3 position,
                                    vec3 displacement, bool wasGrounded, fuse::ecs::EntityID self,
                                    std::vector<CharacterPush>* pushes = nullptr);
};

class CharacterControllerSystem {
public:
    /// One step for every entity with Transform + CharacterController (entity order: deterministic).
    void update(fuse::ecs::Registry& registry, PhysicsManager& world, f32 dt);

private:
    std::vector<CharacterPush> m_pushes;
    std::vector<fuse::ecs::EntityID> m_entities;
};

} // namespace fuse::physics
