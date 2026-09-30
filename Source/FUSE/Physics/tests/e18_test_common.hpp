#pragma once

// Shared helpers of the E18 gates (convex hull / triangle mesh / character / voxel collision).

#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/physics/physics_manager.hpp>
#include <fuse/physics/rotation.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace e18 {

using fuse::f32;
using fuse::s32;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::ecs::EntityID;
using fuse::ecs::Registry;
using namespace fuse::physics;

inline int g_failures = 0;

inline void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

inline int finish(const char* name) {
    if (g_failures == 0) {
        std::printf("%s: all checks passed\n", name);
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "%s: %d failure(s)\n", name, g_failures);
    return EXIT_FAILURE;
}

/// Deterministic xorshift generator (no <random> distribution differences across standard libraries).
struct Rng {
    u64 state = 0x9E3779B97F4A7C15ull;
    explicit Rng(u64 seed) : state(seed * 0x9E3779B97F4A7C15ull + 1ull) {}
    u32 next() {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        return static_cast<u32>(state >> 32u);
    }
    f32 uniform() { return static_cast<f32>(next() >> 8u) * (1.f / 16777216.f); }
    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * uniform(); }
    vec3 inBox(f32 h) { return {range(-h, h), range(-h, h), range(-h, h)}; }
    vec3 onSphere() {
        for (;;) {
            const vec3 v = inBox(1.f);
            const f32 len = v.length();
            if (len > 0.1f && len <= 1.f) {
                return v * (1.f / len);
            }
        }
    }
    quat rotation() { return quatFromAxisAngle(onSphere(), range(-3.14159f, 3.14159f)); }
};

inline EntityID spawnBody(Registry& reg, vec3 position, u32 shape, fuse::ecs::vec3 params, bool isStatic,
                          f32 mass = 1.f, quat rotation = {}) {
    const EntityID id = reg.create();
    fuse::ecs::Transform t{};
    t.position = {position.x, position.y, position.z, 1.f};
    t.rotation = {rotation.x, rotation.y, rotation.z, rotation.w};
    reg.add(id, t);
    fuse::ecs::RigidBody rb{};
    rb.is_static = isStatic;
    rb.mass = mass;
    rb.inv_mass = isStatic ? 0.f : 1.f / mass;
    rb.restitution = 0.f;
    reg.add(id, rb);
    fuse::ecs::Collider c{};
    c.shape = shape;
    c.params = params;
    reg.add(id, c);
    return id;
}

inline EntityID spawnPooled(Registry& reg, vec3 position, u32 shape, u32 ref, bool isStatic, f32 mass = 1.f,
                            quat rotation = {}) {
    const EntityID id = spawnBody(reg, position, shape, {}, isStatic, mass, rotation);
    reg.get<fuse::ecs::Collider>(id)->shape_ref = ref;
    return id;
}

inline EntityID spawnGroundPlane(Registry& reg, f32 height = 0.f) {
    const EntityID id = spawnBody(reg, {}, fuse::ecs::Collider::Plane, {0.f, 1.f, 0.f, 0.f}, true);
    reg.get<fuse::ecs::Collider>(id)->scalar = height;
    return id;
}

inline vec3 positionOf(Registry& reg, EntityID id) {
    const fuse::ecs::Transform* t = reg.get<fuse::ecs::Transform>(id);
    return {t->position.x, t->position.y, t->position.z};
}

inline vec3 velocityOf(Registry& reg, EntityID id) {
    const fuse::ecs::RigidBody* rb = reg.get<fuse::ecs::RigidBody>(id);
    return {rb->velocity.x, rb->velocity.y, rb->velocity.z};
}

/// Flat grid of `cells` x `cells` quads (two triangles each) over [-half, half]^2 at height y.
inline void gridMesh(u32 cells, f32 half, f32 y, std::vector<vec3>& vertices, std::vector<u32>& indices,
                     f32 (*height)(f32, f32) = nullptr) {
    vertices.clear();
    indices.clear();
    const f32 step = 2.f * half / static_cast<f32>(cells);
    for (u32 j = 0; j <= cells; ++j) {
        for (u32 i = 0; i <= cells; ++i) {
            const f32 x = -half + step * static_cast<f32>(i);
            const f32 z = -half + step * static_cast<f32>(j);
            vertices.push_back({x, y + (height != nullptr ? height(x, z) : 0.f), z});
        }
    }
    const u32 row = cells + 1u;
    for (u32 j = 0; j < cells; ++j) {
        for (u32 i = 0; i < cells; ++i) {
            const u32 a = j * row + i;
            const u32 b = a + 1u;
            const u32 c = a + row;
            const u32 d = c + 1u;
            // Counter-clockwise seen from +Y.
            indices.insert(indices.end(), {a, c, b, b, c, d});
        }
    }
}

} // namespace e18
