#include <fuse/physics/phase4_test_registry.hpp>

#include <fuse/physics/destruction/voxel_destruction.hpp>
#include <fuse/physics/softbody/cloth_simulator.hpp>

namespace fuse::physics {

namespace {

const std::vector<Phase4TestCase> kCatalog = {
    {"destruction.carve_radius", "Sphere carve removes voxels within radius", Phase4TestCategory::Destruction, true, false},
    {"destruction.debris_mass", "Debris mass proportional to voxel count", Phase4TestCategory::Destruction, true, false},
    {"softbody.cloth_gravity", "32x32 cloth simulates under gravity at dt=1/60", Phase4TestCategory::SoftBody, true, false},
    {"softbody.pinned_corners", "Pinned corners hold position exactly", Phase4TestCategory::SoftBody, true, false},
    {"integration.manager_step", "PhysicsManager::step completes without error", Phase4TestCategory::Integration, true, false},
    {"integration.collision_callbacks", "CollisionEventSystem dispatches Enter/Exit", Phase4TestCategory::Integration, true, false},
    {"integration.broad_to_manager",
     "Broadphase -> narrowphase -> PBD/CCD stubs -> PhysicsManager",
     Phase4TestCategory::Integration,
     true,
     false},
    {"broadphase.spatial_hash_10k",
     "Spatial hash finds all overlapping pairs for 10k spheres (1k parallel parity smoke in fuse_physics_broadphase_tests)",
     Phase4TestCategory::BroadPhase,
     false,
     false},
    {"narrowphase.sphere_sphere", "Sphere-sphere matches Bullet reference within 0.001f", Phase4TestCategory::NarrowPhase, false, false},
    {"solver.spring_rest_length",
     "Distance constraint holds rest length; compliant spring stretches and recovers",
     Phase4TestCategory::Solver,
     true,
     false},
    {"solver.island_partition",
     "ContactIslandGraph partitions disconnected body groups for job-safe iteration",
     Phase4TestCategory::Solver,
     true,
     false},
    {"solver.stack_stability", "Stack of 10 spheres stable after 5 seconds", Phase4TestCategory::Solver, false, false},
    {"ccd.tunneling", "High-velocity sphere does not tunnel through wall", Phase4TestCategory::Ccd, false, false},
    {"perf.manager_1000_bodies", "PhysicsManager::step < 8ms for 1000 bodies", Phase4TestCategory::Performance, false, false},
};

} // namespace

const std::vector<Phase4TestCase>& Phase4TestRegistry::catalog() {
    return kCatalog;
}

u32 Phase4TestRegistry::countByCategory(Phase4TestCategory category) {
    u32 count = 0;
    for (const Phase4TestCase& testCase : kCatalog) {
        if (testCase.category == category) {
            ++count;
        }
    }
    return count;
}

u32 Phase4TestRegistry::automatedCount() {
    u32 count = 0;
    for (const Phase4TestCase& testCase : kCatalog) {
        if (testCase.automated) {
            ++count;
        }
    }
    return count;
}

bool Phase4TestRegistry::runAutomatedSmoke() {
    VoxelMaterial material{};
    material.hardness = 10.f;
    const f32 radius = DestructionSystem::deriveCarveRadius(25.f, material);
    if (radius <= 0.f) {
        return false;
    }

    ClothSimulator cloth;
    ClothDesc desc{};
    desc.rows = 4;
    desc.cols = 4;
    cloth.init(desc, {0.f, 5.f, 0.f});
    if (!cloth.ready()) {
        return false;
    }
    cloth.step(1.f / 60.f, {0.f, -9.81f, 0.f});
    cloth.destroy();
    return true;
}

} // namespace fuse::physics
