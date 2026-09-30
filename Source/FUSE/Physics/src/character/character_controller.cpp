#include <fuse/physics/character/character_controller.hpp>

#include <fuse/ecs/components/character_controller.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/physics/physics_manager.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics {

namespace {

constexpr f32 kPi = 3.14159265358979f;

struct MoveContext {
    MoveContext(const PhysicsManager& w, const CharacterControllerDesc& d) : world(w), desc(d) {}

    const PhysicsManager& world;
    const CharacterControllerDesc& desc;
    QueryFilter filter{};
    f32 minWalkY = 0.7071f;
    std::vector<CharacterPush>* pushes = nullptr;
    u32 flags = 0;
};

narrowphase::ShapeInstance capsuleAt(const CharacterControllerDesc& desc, vec3 position) {
    narrowphase::ShapeInstance shape{};
    shape.type = CollisionShapeType::Capsule;
    shape.params = {desc.radius, desc.halfHeight, 0.f};
    shape.position = position;
    return shape;
}

bool isDynamic(const PhysicsManager& world, u32 body) {
    const RigidBodySoA& bodies = world.bodies();
    return body < bodies.count() && (bodies.flags[body] & (RB_STATIC | RB_KINEMATIC)) == 0u && bodies.invMasses[body] > 0.f;
}

/// Pushes the capsule out of whatever it overlaps (a platform moved into it, spawn inside geometry).
vec3 depenetrate(MoveContext& ctx, vec3 position) {
    std::vector<OverlapContact> contacts;
    for (u32 iteration = 0; iteration < 4u; ++iteration) {
        contacts.clear();
        ctx.world.overlapShape(capsuleAt(ctx.desc, position), contacts, ctx.filter);
        const OverlapContact* deepest = nullptr;
        for (const OverlapContact& c : contacts) {
            if (c.depth > 1e-5f && (deepest == nullptr || c.depth > deepest->depth)) {
                deepest = &c;
            }
        }
        if (deepest == nullptr) {
            break;
        }
        position += deepest->normal * (deepest->depth + 0.5f * ctx.desc.skinWidth);
    }
    return position;
}

/// Collide-and-slide along `displacement`. `wallsVertical`: surfaces steeper than the walkable limit are
/// treated as vertical walls (the character cannot walk up them). Returns the new position; `lastHit`
/// receives the last blocking surface.
vec3 slide(MoveContext& ctx, vec3 position, vec3 displacement, bool wallsVertical, u32 hitFlag, ShapeCastResult* lastHit,
           bool* blocked) {
    vec3 remaining = displacement;
    const vec3 intended = displacement;
    const f32 skin = ctx.desc.skinWidth;
    if (blocked != nullptr) {
        *blocked = false;
    }
    for (u32 iteration = 0; iteration < ctx.desc.maxSlideIterations; ++iteration) {
        const f32 length = remaining.length();
        if (length < 1e-6f) {
            break;
        }
        const vec3 dir = remaining * (1.f / length);
        ShapeCastResult result{};
        if (!ctx.world.shapeCast(capsuleAt(ctx.desc, position), dir, length + skin, result, ctx.filter)) {
            position += remaining;
            break;
        }
        const ShapeCastHit& hit = result.hit;
        if (hit.startPenetrating) {
            position += hit.normal * (hit.penetration + 0.5f * skin);
            continue;
        }
        // Stop `skin` short of the surface (measured along the normal).
        const f32 approach = std::max(-hit.normal.dot(dir), 0.05f);
        const f32 travel = std::clamp(hit.distance - skin / approach, 0.f, length);
        position += dir * travel;
        if (lastHit != nullptr) {
            *lastHit = result;
        }
        if (blocked != nullptr) {
            *blocked = true;
        }
        ctx.flags |= hitFlag;
        if (ctx.pushes != nullptr && isDynamic(ctx.world, result.body)) {
            vec3 push{-hit.normal.x, 0.f, -hit.normal.z};
            if (push.length() > 1e-6f) {
                ctx.pushes->push_back({result.entity, hit.point, push.normalized()});
            }
        }
        vec3 n = hit.normal;
        if (wallsVertical && n.y < ctx.minWalkY) {
            const vec3 flat{n.x, 0.f, n.z};
            if (flat.length() > 1e-4f) {
                n = flat.normalized();
            }
        }
        remaining = remaining - dir * travel;
        remaining = remaining - n * remaining.dot(n);
        if (remaining.dot(intended) <= 0.f) {
            break; // would turn back: stop (corners)
        }
    }
    return position;
}

/// Downward probe: anything within `distance` below.
bool probeGround(MoveContext& ctx, vec3 position, f32 distance, ShapeCastResult& ground) {
    if (!ctx.world.shapeCast(capsuleAt(ctx.desc, position), {0.f, -1.f, 0.f}, distance, ground, ctx.filter)) {
        return false;
    }
    return !ground.hit.startPenetrating || ground.hit.normal.y > 0.f;
}

/// Can the character stand on this contact? A walkable contact normal, or the rounded capsule resting on
/// the edge of a walkable surface (a step's lip): the surface under the contact point (a short ray just
/// past the contact, into the obstacle) is walkable and no higher than a step above the feet.
bool standable(MoveContext& ctx, const ShapeCastHit& hit, f32 footY) {
    if (hit.normal.y >= ctx.minWalkY) {
        return true;
    }
    if (hit.normal.y <= 0.05f) {
        return false;
    }
    vec3 into{-hit.normal.x, 0.f, -hit.normal.z};
    const f32 len = into.length();
    if (len < 1e-5f) {
        return false;
    }
    into = into * (1.f / len);
    const vec3 origin = hit.point + into * 0.02f + vec3{0.f, 0.05f, 0.f};
    ShapeCastResult surface{};
    if (!ctx.world.rayCastFiltered(origin, {0.f, -1.f, 0.f}, 0.1f, surface, ctx.filter)) {
        return false;
    }
    return surface.hit.normal.y >= ctx.minWalkY && surface.hit.point.y - footY <= ctx.desc.stepHeight + ctx.desc.skinWidth;
}

} // namespace

CharacterMoveResult CharacterController::move(const PhysicsManager& world, const CharacterControllerDesc& desc,
                                              vec3 position, vec3 displacement, bool wasGrounded,
                                              fuse::ecs::EntityID self, std::vector<CharacterPush>* pushes) {
    MoveContext ctx{world, desc};
    ctx.filter.ignore = self;
    ctx.filter.layerMask = desc.layerMask;
    ctx.minWalkY = std::cos(std::clamp(desc.maxSlopeDeg, 0.f, 89.9f) * kPi / 180.f);
    ctx.pushes = pushes;

    CharacterMoveResult result{};
    position = depenetrate(ctx, position);

    // Up (jumps / upward motion).
    if (displacement.y > 0.f) {
        bool blocked = false;
        position = slide(ctx, position, {0.f, displacement.y, 0.f}, false, 2u, nullptr, &blocked);
    }

    // Side.
    const vec3 horizontal{displacement.x, 0.f, displacement.z};
    const f32 horizontalLength = horizontal.length();
    if (horizontalLength > 1e-6f) {
        const vec3 heading = horizontal * (1.f / horizontalLength);
        bool blocked = false;
        const vec3 plain = slide(ctx, position, horizontal, true, 1u, nullptr, &blocked);
        vec3 chosen = plain;
        const f32 plainProgress = (plain - position).dot(heading);
        if (blocked && wasGrounded && desc.stepHeight > 0.f && displacement.y <= 0.f &&
            plainProgress < horizontalLength - 1e-4f) {
            // Step up, move, step down; keep it when it gets further and lands on walkable ground.
            const u32 savedFlags = ctx.flags;
            const vec3 up = slide(ctx, position, {0.f, desc.stepHeight, 0.f}, false, 0u, nullptr, nullptr);
            const f32 raised = up.y - position.y;
            if (raised > 1e-4f) {
                const vec3 across = slide(ctx, up, horizontal, true, 1u, nullptr, nullptr);
                ShapeCastResult ground{};
                const f32 footY = position.y - desc.halfHeight - desc.radius;
                const bool landed = probeGround(ctx, across, raised + desc.skinWidth * 2.f, ground) &&
                                    !ground.hit.startPenetrating && standable(ctx, ground.hit, footY);
                const f32 stepProgress = (across - position).dot(heading);
                if (landed && stepProgress > plainProgress + 1e-3f) {
                    const f32 drop = std::max(ground.hit.distance - desc.skinWidth, 0.f);
                    chosen = across - vec3{0.f, drop, 0.f};
                    result.steppedUp = chosen.y > position.y + 1e-3f;
                } else {
                    ctx.flags = savedFlags | 1u;
                }
            }
        }
        position = chosen;
    }

    // Down (gravity): slides down non-walkable slopes, stops on walkable ground. A grounded character does
    // not slide: the ground probe below snaps it straight down onto its support.
    const bool snap = wasGrounded && displacement.y <= 0.f;
    if (displacement.y < 0.f && !snap) {
        ShapeCastResult last{};
        bool blocked = false;
        position = slide(ctx, position, {0.f, displacement.y, 0.f}, false, 4u, &last, &blocked);
    }

    // Ground probe (with snapping while grounded and not moving up).
    const f32 probe = snap ? desc.snapDistance + desc.skinWidth * 2.f : desc.skinWidth * 3.f;
    ShapeCastResult ground{};
    if (probeGround(ctx, position, probe, ground)) {
        const f32 footY = position.y - desc.halfHeight - desc.radius;
        const bool walkable = standable(ctx, ground.hit, footY);
        if (walkable) {
            result.grounded = true;
            result.groundNormal = ground.hit.normal;
            result.groundEntity = ground.entity;
            result.groundPoint = ground.hit.point;
            ctx.flags |= 4u;
            if (snap && !ground.hit.startPenetrating) {
                const f32 drop = std::max(ground.hit.distance - desc.skinWidth, 0.f);
                position.y -= drop;
            }
        } else {
            result.onSteepSlope = ground.hit.distance <= desc.skinWidth * 3.f;
            result.groundNormal = ground.hit.normal;
        }
    }
    result.position = position;
    result.collisionFlags = ctx.flags;
    return result;
}

void CharacterControllerSystem::update(fuse::ecs::Registry& registry, PhysicsManager& world, f32 dt) {
    using fuse::ecs::CharacterController;
    using fuse::ecs::Transform;
    if (dt <= 0.f) {
        return;
    }
    m_entities.clear();
    registry.each<Transform, CharacterController>(
        [&](fuse::ecs::EntityID id, Transform&, CharacterController&) { m_entities.push_back(id); });
    if (m_entities.empty()) {
        return;
    }
    std::sort(m_entities.begin(), m_entities.end(),
              [](fuse::ecs::EntityID a, fuse::ecs::EntityID b) { return a.index < b.index; });
    for (const fuse::ecs::EntityID id : m_entities) {
        Transform* transform = registry.get<Transform>(id);
        CharacterController* cc = registry.get<CharacterController>(id);
        if (transform == nullptr || cc == nullptr) {
            continue;
        }
        CharacterControllerDesc desc{};
        desc.radius = cc->radius;
        desc.halfHeight = cc->half_height;
        desc.stepHeight = cc->step_height;
        desc.maxSlopeDeg = cc->max_slope_deg;
        desc.snapDistance = cc->snap_distance;
        desc.skinWidth = cc->skin_width;
        desc.layerMask = cc->layer_mask;

        // Vertical velocity: gravity while airborne (or on a steep slope), zero on walkable ground.
        f32 vy = cc->velocity.y;
        if (cc->grounded) {
            vy = 0.f;
        }
        if (cc->jump_speed > 0.f && cc->grounded) {
            vy = cc->jump_speed;
        }
        cc->jump_speed = 0.f;
        if (!cc->grounded || vy > 0.f) {
            vy = std::max(vy + cc->gravity * dt, -cc->max_fall_speed);
        } else {
            vy = cc->gravity * dt; // keeps probing the ground under a grounded character
        }

        // Moving platforms: the velocity of the body stood on, at the contact point.
        vec3 platform{};
        const u32 groundBody = world.bodyIndex(cc->ground_entity);
        if (cc->grounded && groundBody != 0xFFFFFFFFu) {
            const vec3 foot{transform->position.x, transform->position.y - cc->half_height - cc->radius,
                            transform->position.z};
            platform = world.pointVelocity(groundBody, foot);
        }
        const vec3 start{transform->position.x, transform->position.y, transform->position.z};
        vec3 displacement{(cc->move_velocity.x + platform.x) * dt, vy * dt, (cc->move_velocity.z + platform.z) * dt};
        if (platform.y > 0.f) {
            displacement.y += platform.y * dt; // lifting platform
        }

        m_pushes.clear();
        const CharacterMoveResult moved =
            physics::CharacterController::move(world, desc, start, displacement, cc->grounded, id, &m_pushes);

        const vec3 delta = moved.position - start;
        f32 newVy = vy;
        if (moved.grounded) {
            newVy = 0.f;
        }
        if ((moved.collisionFlags & 2u) != 0u && newVy > 0.f) {
            newVy = 0.f; // head hit a ceiling
        }
        cc->velocity = {delta.x / dt, newVy, delta.z / dt, 0.f};
        cc->grounded = moved.grounded;
        cc->on_steep_slope = moved.onSteepSlope;
        cc->ground_normal = {moved.groundNormal.x, moved.groundNormal.y, moved.groundNormal.z, 0.f};
        cc->ground_entity = moved.grounded ? moved.groundEntity : fuse::ecs::EntityID{};
        cc->platform_velocity = {platform.x, platform.y, platform.z, 0.f};
        cc->collision_flags = moved.collisionFlags;
        transform->position = {moved.position.x, moved.position.y, moved.position.z, transform->position.w};
        transform->dirty = true;

        for (const CharacterPush& push : m_pushes) {
            world.applyImpulse(push.entity, push.direction * (cc->push_force * dt), push.point);
        }
    }
}

} // namespace fuse::physics
