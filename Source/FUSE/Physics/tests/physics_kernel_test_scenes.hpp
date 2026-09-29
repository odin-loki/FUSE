#pragma once

// Scenes shared by the physics kernel gates (test_b4_physics_kernel_gates.cpp) and the resident
// pipeline gates (test_b4_physics_resident.cpp). makeScene3D(10000, 99) with cellSize 2 is the
// 10k-body broadphase scene of the 2026-09-25 RTX 3090 run; makeSolverScene is the stacked solver scene.

#include <fuse/physics/physics_data.hpp>
#include <fuse/physics/rotation.hpp>

#include <cmath>
#include <random>
#include <vector>

namespace fuse::physics::test_scenes {

struct Scene {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
};

/// Jittered 3D grid of spheres / boxes (some rotated) + a ground plane; ~spacing 0.9 so neighbours overlap.
inline Scene makeScene3D(u32 count, u32 seed, bool withPlane = true, bool rotated = true, bool layers = false) {
    Scene s;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> jitter(-0.15f, 0.15f);
    if (withPlane) {
        const u32 ground = s.bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
        s.shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);
    }
    const u32 side = static_cast<u32>(std::ceil(std::cbrt(static_cast<double>(count))));
    for (u32 i = 0; i < count; ++i) {
        const vec3 p{static_cast<float>(i % side) * 0.9f + jitter(rng),
                     0.45f + static_cast<float>((i / side) % side) * 0.9f + jitter(rng),
                     static_cast<float>(i / (side * side)) * 0.9f + jitter(rng)};
        const u32 layer = layers ? (1u << (i % 3u)) : 1u;
        const u32 mask = layers ? (i % 5u == 0u ? 0x1u : 0xFFFFFFFFu) : 0xFFFFFFFFu;
        const u32 body = s.bodies.addBody(p, 1.f, i % 17u == 0u ? RB_STATIC : 0u, layer, mask);
        if (i % 3u == 0u) {
            s.shapes.addShape(CollisionShapeType::Box, body, {0.45f, 0.4f, 0.35f});
            if (rotated && i % 2u == 0u) {
                s.bodies.orientations[body] = quatFromAxisAngle({0.3f, 1.f, 0.2f}, 0.1f * static_cast<float>(i % 13u));
            }
        } else {
            s.shapes.addShape(CollisionShapeType::Sphere, body, {0.5f, 0.f, 0.f});
        }
    }
    return s;
}

inline Scene makeScene2D(u32 count, u32 seed) {
    Scene s;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> jitter(-0.2f, 0.2f);
    for (u32 i = 0; i < count; ++i) {
        const vec3 p{static_cast<float>(i % 40u) * 0.9f + jitter(rng), static_cast<float>(i / 40u) * 0.9f + jitter(rng),
                     0.f};
        const u32 body = s.bodies.addBody(p, 1.f);
        if (i % 3u == 0u) {
            s.shapes.addShape(CollisionShapeType::Box, body, {0.45f, 0.45f, 0.f});
        } else {
            s.shapes.addShape(CollisionShapeType::Sphere, body, {0.5f, 0.f, 0.f});
        }
    }
    return s;
}

struct SolverScene {
    RigidBodySoA bodies;
    CollisionShapeSoA shapes;
    std::vector<u32> tops;
};

/// Columns of stacked boxes and spheres on a ground plane (+ a few distance constraints).
inline SolverScene makeSolverScene(u32 columnsPerSide, u32 height) {
    SolverScene s;
    const u32 ground = s.bodies.addBody({0.f, 0.f, 0.f}, 0.f, RB_STATIC);
    s.shapes.addShape(CollisionShapeType::Plane, ground, {0.f, 1.f, 0.f}, 0.f);
    for (u32 cx = 0; cx < columnsPerSide; ++cx) {
        for (u32 cz = 0; cz < columnsPerSide; ++cz) {
            // Box columns stack; sphere "columns" are a single sphere resting on the ground (stacked
            // spheres are unstable and would roll off in either solver).
            const bool boxes = ((cx + cz) % 2u) == 0u;
            const u32 levels = boxes ? height : 1u;
            for (u32 level = 0; level < levels; ++level) {
                const vec3 p{static_cast<float>(cx) * 1.5f, 0.5f + static_cast<float>(level) * 1.001f,
                             static_cast<float>(cz) * 1.5f};
                const u32 body = s.bodies.addBody(p, 1.f);
                if (boxes) {
                    s.shapes.addShape(CollisionShapeType::Box, body, {0.5f, 0.5f, 0.5f});
                } else {
                    s.shapes.addShape(CollisionShapeType::Sphere, body, {0.5f, 0.f, 0.f});
                }
                if (boxes && level + 1u == height) {
                    s.tops.push_back(body);
                }
            }
        }
    }
    return s;
}

} // namespace fuse::physics::test_scenes
