#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/detail/iteration_parity.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        ++g_failures;
    }
}

template <typename Body>
void withScheduler(fuse::u32 workers, Body&& body) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);
    body();
    scheduler.shutdown();
}

void testHasAnyTransformsEmptyRegistry() {
    fuse::ecs::Registry reg;
    reg.init(8);

    expectTrue(!fuse::ecs::TransformSystem::has_any_transforms(reg),
               "empty registry has no transforms");
    expectEq(fuse::ecs::TransformSystem::count_transforms(reg), 0u,
             "count_transforms is zero on empty registry");
    expectEq(fuse::ecs::TransformSystem::count_dirty_roots(reg), 0u,
             "count_dirty_roots uses empty-transform guard");
}

void testHasAnyTransformsNonTransformEntities() {
    fuse::ecs::Registry reg;
    reg.init(8);

    const fuse::ecs::EntityID id = reg.create();
    reg.add<fuse::ecs::RigidBody>(id);

    expectTrue(!fuse::ecs::TransformSystem::has_any_transforms(reg),
               "entities without Transform do not satisfy has_any_transforms");
    expectEq(fuse::ecs::TransformSystem::count_transforms(reg), 0u,
             "count_transforms ignores non-transform entities");
    expectEq(fuse::ecs::TransformSystem::count_dirty_roots(reg), 0u,
             "count_dirty_roots ignores non-transform entities");
}

void testEmptyRegistryDirtyRootStubsNoOp() {
    fuse::ecs::Registry reg;
    reg.init(8);

    fuse::ecs::TransformSystem::update_dirty_roots_serial(reg);
    withScheduler(2, [&] {
        fuse::ecs::TransformSystem::update_dirty_roots_parallel(reg, 4);
    });

    expectEq(fuse::ecs::TransformSystem::count_dirty_roots(reg), 0u,
             "empty registry dirty-root count stays zero after stub passes");
}

void testEmptyRegistryDirtyRootSerialParallelParity() {
    fuse::ecs::Registry serialReg;
    fuse::ecs::Registry parallelReg;
    serialReg.init(8);
    parallelReg.init(8);

    withScheduler(4, [&] {
        expectTrue(fuse::ecs::detail::transform_dirty_roots_serial_parallel_match(serialReg, parallelReg, 8),
                   "empty registry dirty-root stubs match serial/parallel");
    });
}

void testEmptyRegistryUpdatePaths() {
    fuse::ecs::Registry reg;
    reg.init(8);

    fuse::ecs::TransformSystemOptions serialOptions{};
    serialOptions.parallelDirtyRoots = false;
    fuse::ecs::TransformSystem::update(reg, serialOptions);

    withScheduler(2, [&] {
        fuse::ecs::TransformSystemOptions parallelOptions{};
        parallelOptions.parallelDirtyRoots = true;
        parallelOptions.batchSize = 1;
        fuse::ecs::TransformSystem::update(reg, parallelOptions);
    });

    expectTrue(!fuse::ecs::TransformSystem::has_any_transforms(reg),
               "empty registry update leaves no transforms");
    expectEq(fuse::ecs::TransformSystem::count_transforms(reg), 0u,
             "empty registry update does not create transforms");
}

void testNonTransformRegistryUpdateNoOp() {
    fuse::ecs::Registry reg;
    reg.init(8);

    const fuse::ecs::EntityID id = reg.create();
    reg.add<fuse::ecs::RigidBody>(id);

    withScheduler(2, [&] {
        fuse::ecs::TransformSystemOptions options{};
        options.parallelDirtyRoots = true;
        fuse::ecs::TransformSystem::update(reg, options);
    });

    expectTrue(!fuse::ecs::TransformSystem::has_any_transforms(reg),
               "update no-ops when registry has no Transform components");
}

void testCleanRootsDirtyRootStubsNoOp() {
    fuse::ecs::Registry reg;
    reg.init(16);

    const fuse::ecs::EntityID root = reg.create();
    fuse::ecs::Transform rootTransform{};
    rootTransform.dirty = false;
    rootTransform.local_to_world = fuse::ecs::mat4::identity();
    rootTransform.world_to_local = fuse::ecs::mat4::identity();
    reg.add(root, rootTransform);

    expectEq(fuse::ecs::TransformSystem::count_transforms(reg), 1u, "scene has one transform");
    expectEq(fuse::ecs::TransformSystem::count_dirty_roots(reg), 0u, "clean root is not a dirty root");

    fuse::ecs::TransformSystem::update_dirty_roots_serial(reg);
    withScheduler(2, [&] {
        fuse::ecs::TransformSystem::update_dirty_roots_parallel(reg, 4);
    });

    const fuse::ecs::Transform* updated = reg.get<fuse::ecs::Transform>(root);
    expectTrue(updated != nullptr, "clean root survives dirty-root passes");
    expectTrue(!updated->dirty, "clean root stays clean after dirty-root passes");
    expectTrue(updated->local_to_world.data[12] == 0.f,
               "clean root matrix unchanged by dirty-root passes");
}

void testDirtyRootSerialParallelParityManyRoots() {
    fuse::ecs::Registry serialReg;
    fuse::ecs::Registry parallelReg;
    serialReg.init(128);
    parallelReg.init(128);

    for (fuse::u32 i = 0; i < 32; ++i) {
        const fuse::ecs::EntityID serialId = serialReg.create();
        const fuse::ecs::EntityID parallelId = parallelReg.create();

        fuse::ecs::Transform transform{};
        transform.position = {static_cast<float>(i), 1.f, 0.f, 1.f};
        transform.dirty = true;
        serialReg.add(serialId, transform);
        parallelReg.add(parallelId, transform);
    }

    expectEq(fuse::ecs::TransformSystem::count_dirty_roots(serialReg), 32u,
             "many-root scene has expected dirty-root tally");

    withScheduler(4, [&] {
        expectTrue(fuse::ecs::detail::transform_dirty_roots_serial_parallel_match(serialReg, parallelReg, 0),
                   "dirty-root serial/parallel parity with batchSize=0");
        expectTrue(fuse::ecs::detail::transform_dirty_roots_serial_parallel_match(serialReg, parallelReg, 17),
                   "dirty-root serial/parallel parity with prime batch size");
    });
}

void testCountTransformsMatchesEach() {
    fuse::ecs::Registry reg;
    reg.init(64);

    for (fuse::u32 i = 0; i < 12; ++i) {
        const fuse::ecs::EntityID id = reg.create();
        reg.add<fuse::ecs::Transform>(id);
        if ((i % 4) == 0) {
            reg.add<fuse::ecs::RigidBody>(id);
        }
    }

    fuse::u32 eachCount = 0;
    reg.each<fuse::ecs::Transform>([&](fuse::ecs::EntityID, fuse::ecs::Transform&) { ++eachCount; });

    expectEq(fuse::ecs::TransformSystem::count_transforms(reg), eachCount,
             "count_transforms matches serial each visit count");
    expectTrue(fuse::ecs::TransformSystem::has_any_transforms(reg),
               "has_any_transforms true when transforms exist");
}

} // namespace

int main() {
    testHasAnyTransformsEmptyRegistry();
    testHasAnyTransformsNonTransformEntities();
    testEmptyRegistryDirtyRootStubsNoOp();
    testEmptyRegistryDirtyRootSerialParallelParity();
    testEmptyRegistryUpdatePaths();
    testNonTransformRegistryUpdateNoOp();
    testCleanRootsDirtyRootStubsNoOp();
    testDirtyRootSerialParallelParityManyRoots();
    testCountTransformsMatchesEach();

    if (g_failures == 0) {
        std::printf("fuse_ecs_transform_system_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs_transform_system_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
