#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/detail/iteration_parity.hpp>
#include <fuse/ecs/math/mat.hpp>
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

void testHasDirtyRootsEmptyRegistryGuard() {
    fuse::ecs::Registry reg;
    reg.init(8);

    expectTrue(!fuse::ecs::TransformSystem::has_dirty_roots(reg),
               "has_dirty_roots is false on empty registry");
    expectEq(fuse::ecs::TransformSystem::count_roots(reg), 0u,
             "count_roots is zero on empty registry");
}

void testHasDirtyRootsMixedScene() {
    fuse::ecs::Registry reg;
    reg.init(32);

    const fuse::ecs::EntityID dirtyRoot = reg.create();
    const fuse::ecs::EntityID cleanRoot = reg.create();
    const fuse::ecs::EntityID child = reg.create();

    fuse::ecs::Transform dirtyRootTransform{};
    dirtyRootTransform.position = {1.f, 0.f, 0.f, 1.f};
    dirtyRootTransform.dirty = true;
    reg.add(dirtyRoot, dirtyRootTransform);

    fuse::ecs::Transform cleanRootTransform{};
    cleanRootTransform.dirty = false;
    cleanRootTransform.local_to_world = fuse::ecs::mat4::identity();
    cleanRootTransform.world_to_local = fuse::ecs::mat4::identity();
    reg.add(cleanRoot, cleanRootTransform);

    fuse::ecs::Transform childTransform{};
    childTransform.position = {0.f, 2.f, 0.f, 1.f};
    childTransform.parent = dirtyRoot;
    childTransform.dirty = true;
    reg.add(child, childTransform);

    expectTrue(fuse::ecs::TransformSystem::has_dirty_roots(reg),
               "dirty root makes has_dirty_roots true");
    expectEq(fuse::ecs::TransformSystem::count_dirty_roots(reg), 1u,
             "only the dirty root is counted");
    expectEq(fuse::ecs::TransformSystem::count_roots(reg), 2u,
             "count_roots includes clean and dirty roots");
}

void testCountRootsIgnoresChildren() {
    fuse::ecs::Registry reg;
    reg.init(16);

    const fuse::ecs::EntityID root = reg.create();
    const fuse::ecs::EntityID child = reg.create();

    fuse::ecs::Transform rootTransform{};
    rootTransform.dirty = true;
    reg.add(root, rootTransform);

    fuse::ecs::Transform childTransform{};
    childTransform.parent = root;
    childTransform.dirty = true;
    reg.add(child, childTransform);

    expectEq(fuse::ecs::TransformSystem::count_roots(reg), 1u,
             "count_roots ignores child transforms");
    expectEq(fuse::ecs::TransformSystem::count_dirty_roots(reg), 1u,
             "dirty child does not inflate dirty-root tally");
}

void testNoDirtyRootsDirtyRootStubsEarlyOut() {
    fuse::ecs::Registry reg;
    reg.init(16);

    const fuse::ecs::EntityID root = reg.create();
    const fuse::ecs::EntityID child = reg.create();

    fuse::ecs::Transform rootTransform{};
    rootTransform.dirty = false;
    rootTransform.local_to_world = fuse::ecs::mat4::identity();
    rootTransform.world_to_local = fuse::ecs::mat4::identity();
    reg.add(root, rootTransform);

    fuse::ecs::Transform childTransform{};
    childTransform.position = {0.f, 3.f, 0.f, 1.f};
    childTransform.parent = root;
    childTransform.dirty = true;
    reg.add(child, childTransform);

    expectTrue(!fuse::ecs::TransformSystem::has_dirty_roots(reg),
               "clean root scene has no dirty roots");

    fuse::ecs::TransformSystem::update_dirty_roots_serial(reg);
    withScheduler(2, [&] {
        fuse::ecs::TransformSystem::update_dirty_roots_parallel(reg, 4);
    });

    const fuse::ecs::Transform* updatedChild = reg.get<fuse::ecs::Transform>(child);
    expectTrue(updatedChild != nullptr, "child survives no-dirty-root stub early-out");
    expectTrue(updatedChild->dirty, "dirty-root stubs leave dirty child untouched");
    expectTrue(updatedChild->local_to_world.data[13] == 0.f,
               "dirty-root stubs leave child matrix untouched when no dirty roots");
}

void testNoDirtyRootsUpdateHierarchyStillRuns() {
    fuse::ecs::Registry reg;
    reg.init(16);

    const fuse::ecs::EntityID root = reg.create();
    const fuse::ecs::EntityID child = reg.create();

    fuse::ecs::Transform rootTransform{};
    rootTransform.position = {2.f, 0.f, 0.f, 1.f};
    rootTransform.dirty = false;
    rootTransform.local_to_world =
        fuse::ecs::from_trs(rootTransform.position, rootTransform.rotation, rootTransform.scale);
    rootTransform.world_to_local = fuse::ecs::inverse_affine(rootTransform.local_to_world);
    reg.add(root, rootTransform);

    fuse::ecs::Transform childTransform{};
    childTransform.position = {0.f, 4.f, 0.f, 1.f};
    childTransform.parent = root;
    childTransform.dirty = true;
    reg.add(child, childTransform);

    withScheduler(2, [&] {
        fuse::ecs::TransformSystemOptions options{};
        options.parallelDirtyRoots = true;
        fuse::ecs::TransformSystem::update(reg, options);
    });

    const fuse::ecs::Transform* updatedChild = reg.get<fuse::ecs::Transform>(child);
    expectTrue(updatedChild != nullptr, "child exists after update with no dirty roots");
    expectTrue(updatedChild->local_to_world.data[12] == 2.f,
               "hierarchy pass updates dirty child when dirty-root pass early-outs");
    expectTrue(updatedChild->local_to_world.data[13] == 4.f,
               "hierarchy pass preserves child local Y when dirty-root pass early-outs");
    expectTrue(!updatedChild->dirty, "hierarchy pass clears child dirty flag");
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

void testHasDirtyTransformsEmptyRegistryGuard() {
    fuse::ecs::Registry reg;
    reg.init(8);

    expectTrue(!fuse::ecs::TransformSystem::has_dirty_transforms(reg),
               "has_dirty_transforms is false on empty registry");
    expectEq(fuse::ecs::TransformSystem::count_dirty_transforms(reg), 0u,
             "count_dirty_transforms is zero on empty registry");
    expectTrue(fuse::ecs::TransformSystem::should_skip_hierarchy_update(reg),
               "should_skip_hierarchy_update on empty registry");
}

void testHasDirtyTransformsMixedScene() {
    fuse::ecs::Registry reg;
    reg.init(32);

    const fuse::ecs::EntityID dirtyRoot = reg.create();
    const fuse::ecs::EntityID cleanRoot = reg.create();
    const fuse::ecs::EntityID child = reg.create();

    fuse::ecs::Transform dirtyRootTransform{};
    dirtyRootTransform.dirty = true;
    reg.add(dirtyRoot, dirtyRootTransform);

    fuse::ecs::Transform cleanRootTransform{};
    cleanRootTransform.dirty = false;
    reg.add(cleanRoot, cleanRootTransform);

    fuse::ecs::Transform childTransform{};
    childTransform.parent = cleanRoot;
    childTransform.dirty = true;
    reg.add(child, childTransform);

    expectTrue(fuse::ecs::TransformSystem::has_dirty_transforms(reg),
               "mixed scene has dirty transforms");
    expectEq(fuse::ecs::TransformSystem::count_dirty_transforms(reg), 2u,
             "count_dirty_transforms tallies dirty root and dirty child");
    expectTrue(!fuse::ecs::TransformSystem::should_skip_hierarchy_update(reg),
               "should_skip_hierarchy_update false when any transform is dirty");
}

void testShouldSkipHierarchyUpdateAllClean() {
    fuse::ecs::Registry reg;
    reg.init(16);

    const fuse::ecs::EntityID root = reg.create();
    const fuse::ecs::EntityID child = reg.create();

    fuse::ecs::Transform rootTransform{};
    rootTransform.dirty = false;
    rootTransform.local_to_world = fuse::ecs::mat4::identity();
    rootTransform.world_to_local = fuse::ecs::mat4::identity();
    reg.add(root, rootTransform);

    fuse::ecs::Transform childTransform{};
    childTransform.parent = root;
    childTransform.dirty = false;
    childTransform.local_to_world = fuse::ecs::mat4::identity();
    childTransform.world_to_local = fuse::ecs::mat4::identity();
    reg.add(child, childTransform);

    expectTrue(fuse::ecs::TransformSystem::should_skip_hierarchy_update(reg),
               "all-clean scene skips hierarchy update");
    expectTrue(!fuse::ecs::TransformSystem::subtree_has_dirty(reg, root),
               "clean root subtree has no dirty transforms");
    expectTrue(!fuse::ecs::TransformSystem::subtree_has_dirty(reg, child),
               "clean leaf subtree has no dirty transforms");
}

void testSubtreeHasDirtyDetectsNestedDirtyChild() {
    fuse::ecs::Registry reg;
    reg.init(16);

    const fuse::ecs::EntityID root = reg.create();
    const fuse::ecs::EntityID child = reg.create();

    fuse::ecs::Transform rootTransform{};
    rootTransform.dirty = false;
    reg.add(root, rootTransform);

    fuse::ecs::Transform childTransform{};
    childTransform.parent = root;
    childTransform.dirty = true;
    reg.add(child, childTransform);

    expectTrue(fuse::ecs::TransformSystem::subtree_has_dirty(reg, child),
               "dirty leaf reports dirty via its own flag");
    expectTrue(fuse::ecs::TransformSystem::subtree_has_dirty(reg, root),
               "clean root detects dirty child in subtree");
}

void testShouldRecomputeInHierarchyGuards() {
    fuse::ecs::Transform dirtyRoot{};
    dirtyRoot.dirty = true;
    expectTrue(fuse::ecs::TransformSystem::should_recompute_in_hierarchy(dirtyRoot),
               "dirty root should recompute in hierarchy");

    fuse::ecs::Transform cleanRoot{};
    cleanRoot.dirty = false;
    expectTrue(!fuse::ecs::TransformSystem::should_recompute_in_hierarchy(cleanRoot),
               "clean root without parent should not recompute in hierarchy");

    fuse::ecs::Transform cleanChild{};
    cleanChild.dirty = false;
    cleanChild.parent = fuse::ecs::EntityID{1, 1};
    expectTrue(fuse::ecs::TransformSystem::should_recompute_in_hierarchy(cleanChild),
               "child with parent should recompute when visited in hierarchy");
}

void testAllCleanUpdateLeavesMatricesUntouched() {
    fuse::ecs::Registry reg;
    reg.init(16);

    const fuse::ecs::EntityID root = reg.create();
    const fuse::ecs::EntityID child = reg.create();

    fuse::ecs::Transform rootTransform{};
    rootTransform.position = {3.f, 0.f, 0.f, 1.f};
    rootTransform.dirty = false;
    rootTransform.local_to_world =
        fuse::ecs::from_trs(rootTransform.position, rootTransform.rotation, rootTransform.scale);
    rootTransform.world_to_local = fuse::ecs::inverse_affine(rootTransform.local_to_world);
    reg.add(root, rootTransform);

    fuse::ecs::Transform childTransform{};
    childTransform.position = {0.f, 2.f, 0.f, 1.f};
    childTransform.parent = root;
    childTransform.dirty = false;
    childTransform.local_to_world =
        fuse::ecs::from_trs(childTransform.position, childTransform.rotation, childTransform.scale);
    childTransform.world_to_local = fuse::ecs::inverse_affine(childTransform.local_to_world);
    reg.add(child, childTransform);

    withScheduler(2, [&] {
        fuse::ecs::TransformSystem::update(reg);
    });

    const fuse::ecs::Transform* updatedRoot = reg.get<fuse::ecs::Transform>(root);
    const fuse::ecs::Transform* updatedChild = reg.get<fuse::ecs::Transform>(child);
    expectTrue(updatedRoot != nullptr && updatedChild != nullptr,
               "all-clean update keeps transform entities");
    expectTrue(updatedRoot->local_to_world.data[12] == 3.f,
               "all-clean hierarchy skip leaves root matrix untouched");
    expectTrue(updatedChild->local_to_world.data[13] == 2.f,
               "all-clean hierarchy skip leaves child matrix untouched");
    expectTrue(!updatedRoot->dirty && !updatedChild->dirty,
               "all-clean update leaves dirty flags clear");
}

void testSecondUpdateSkipsWhenSceneStaysClean() {
    fuse::ecs::Registry reg;
    reg.init(16);

    const fuse::ecs::EntityID root = reg.create();
    fuse::ecs::Transform rootTransform{};
    rootTransform.position = {1.f, 0.f, 0.f, 1.f};
    rootTransform.dirty = true;
    reg.add(root, rootTransform);

    fuse::ecs::TransformSystem::update(reg);

    const fuse::ecs::Transform* afterFirst = reg.get<fuse::ecs::Transform>(root);
    expectTrue(afterFirst != nullptr, "root exists after first update");
    if (afterFirst == nullptr) {
        return;
    }

    const float firstX = afterFirst->local_to_world.data[12];
    expectTrue(fuse::ecs::TransformSystem::should_skip_hierarchy_update(reg),
               "post-update scene should skip hierarchy when all transforms are clean");

    fuse::ecs::TransformSystem::update(reg);

    const fuse::ecs::Transform* afterSecond = reg.get<fuse::ecs::Transform>(root);
    expectTrue(afterSecond != nullptr, "root exists after second update");
    expectTrue(afterSecond->local_to_world.data[12] == firstX,
               "second update with clean scene preserves root matrix");
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
    testHasDirtyRootsEmptyRegistryGuard();
    testHasDirtyRootsMixedScene();
    testCountRootsIgnoresChildren();
    testNoDirtyRootsDirtyRootStubsEarlyOut();
    testNoDirtyRootsUpdateHierarchyStillRuns();
    testCountTransformsMatchesEach();
    testHasDirtyTransformsEmptyRegistryGuard();
    testHasDirtyTransformsMixedScene();
    testShouldSkipHierarchyUpdateAllClean();
    testSubtreeHasDirtyDetectsNestedDirtyChild();
    testShouldRecomputeInHierarchyGuards();
    testAllCleanUpdateLeavesMatricesUntouched();
    testSecondUpdateSkipsWhenSceneStaysClean();

    if (g_failures == 0) {
        std::printf("fuse_ecs_transform_system_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs_transform_system_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
