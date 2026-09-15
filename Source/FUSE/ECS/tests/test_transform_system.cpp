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

void testCountDirtyTransformsEmptyRegistryGuard() {
    fuse::ecs::Registry reg;
    reg.init(8);

    expectTrue(!fuse::ecs::TransformSystem::has_any_dirty_transforms(reg),
               "has_any_dirty_transforms is false on empty registry");
    expectEq(fuse::ecs::TransformSystem::count_dirty_transforms(reg), 0u,
             "count_dirty_transforms uses empty-transform guard");
}

void testCountDirtyTransformsMixedScene() {
    fuse::ecs::Registry reg;
    reg.init(32);

    const fuse::ecs::EntityID dirtyRoot = reg.create();
    const fuse::ecs::EntityID cleanRoot = reg.create();
    const fuse::ecs::EntityID dirtyChild = reg.create();
    const fuse::ecs::EntityID cleanChild = reg.create();

    fuse::ecs::Transform dirtyRootTransform{};
    dirtyRootTransform.dirty = true;
    reg.add(dirtyRoot, dirtyRootTransform);

    fuse::ecs::Transform cleanRootTransform{};
    cleanRootTransform.dirty = false;
    reg.add(cleanRoot, cleanRootTransform);

    fuse::ecs::Transform dirtyChildTransform{};
    dirtyChildTransform.parent = dirtyRoot;
    dirtyChildTransform.dirty = true;
    reg.add(dirtyChild, dirtyChildTransform);

    fuse::ecs::Transform cleanChildTransform{};
    cleanChildTransform.parent = cleanRoot;
    cleanChildTransform.dirty = false;
    reg.add(cleanChild, cleanChildTransform);

    expectTrue(fuse::ecs::TransformSystem::has_any_dirty_transforms(reg),
               "mixed scene has dirty transforms");
    expectEq(fuse::ecs::TransformSystem::count_dirty_transforms(reg), 2u,
             "count_dirty_transforms tallies roots and children");
}

void testShouldSkipHierarchyRecomputeGuards() {
    fuse::ecs::Transform cleanRoot{};
    cleanRoot.dirty = false;

    fuse::ecs::Transform dirtyRoot{};
    dirtyRoot.dirty = true;

    fuse::ecs::Transform cleanChild{};
    cleanChild.parent = fuse::ecs::EntityID{1, 1};
    cleanChild.dirty = false;

    expectTrue(fuse::ecs::TransformSystem::should_skip_hierarchy_recompute(cleanRoot),
               "clean root skips hierarchy recompute");
    expectTrue(!fuse::ecs::TransformSystem::should_skip_hierarchy_recompute(dirtyRoot),
               "dirty root does not skip hierarchy recompute");
    expectTrue(!fuse::ecs::TransformSystem::should_skip_hierarchy_recompute(cleanChild),
               "child with parent never skips hierarchy recompute");
}

void testSubtreeHasDirtyTransformsGuards() {
    fuse::ecs::Registry reg;
    reg.init(32);

    const fuse::ecs::EntityID root = reg.create();
    const fuse::ecs::EntityID mid = reg.create();
    const fuse::ecs::EntityID leaf = reg.create();

    fuse::ecs::Transform rootTransform{};
    rootTransform.dirty = false;
    reg.add(root, rootTransform);

    fuse::ecs::Transform midTransform{};
    midTransform.parent = root;
    midTransform.dirty = false;
    reg.add(mid, midTransform);

    fuse::ecs::Transform leafTransform{};
    leafTransform.parent = mid;
    leafTransform.dirty = false;
    reg.add(leaf, leafTransform);

    expectTrue(!fuse::ecs::TransformSystem::subtree_has_dirty_transforms(reg, root),
               "clean subtree has no dirty descendants");

    fuse::ecs::Transform* dirtyLeaf = reg.get<fuse::ecs::Transform>(leaf);
    expectTrue(dirtyLeaf != nullptr, "leaf exists before dirty flag set");
    dirtyLeaf->dirty = true;

    expectTrue(fuse::ecs::TransformSystem::subtree_has_dirty_transforms(reg, root),
               "dirty leaf makes root subtree dirty");
    expectTrue(fuse::ecs::TransformSystem::subtree_has_dirty_transforms(reg, mid),
               "dirty leaf makes mid subtree dirty");
    expectTrue(fuse::ecs::TransformSystem::subtree_has_dirty_transforms(reg, leaf),
               "dirty leaf marks its own subtree dirty");
}

void testFullyCleanSceneUpdateEarlyOut() {
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
        fuse::ecs::from_trs({3.f, 2.f, 0.f, 1.f}, childTransform.rotation, childTransform.scale);
    childTransform.world_to_local = fuse::ecs::inverse_affine(childTransform.local_to_world);
    reg.add(child, childTransform);

    expectTrue(!fuse::ecs::TransformSystem::has_any_dirty_transforms(reg),
               "fully clean scene has no dirty transforms");

    withScheduler(2, [&] {
        fuse::ecs::TransformSystemOptions options{};
        options.parallelDirtyRoots = true;
        fuse::ecs::TransformSystem::update(reg, options);
    });

    const fuse::ecs::Transform* updatedChild = reg.get<fuse::ecs::Transform>(child);
    expectTrue(updatedChild != nullptr, "child survives fully clean update early-out");
    expectTrue(updatedChild->local_to_world.data[12] == 3.f,
               "fully clean hierarchy skip leaves child X unchanged");
    expectTrue(updatedChild->local_to_world.data[13] == 2.f,
               "fully clean hierarchy skip leaves child Y unchanged");
}

void testCleanSubtreeHierarchySkipPreservesMatrices() {
    fuse::ecs::Registry reg;
    reg.init(32);

    const fuse::ecs::EntityID dirtyRoot = reg.create();
    const fuse::ecs::EntityID cleanBranch = reg.create();
    const fuse::ecs::EntityID cleanLeaf = reg.create();
    const fuse::ecs::EntityID dirtyChild = reg.create();

    fuse::ecs::Transform dirtyRootTransform{};
    dirtyRootTransform.position = {1.f, 0.f, 0.f, 1.f};
    dirtyRootTransform.dirty = true;
    reg.add(dirtyRoot, dirtyRootTransform);

    fuse::ecs::Transform cleanBranchTransform{};
    cleanBranchTransform.parent = dirtyRoot;
    cleanBranchTransform.position = {0.f, 5.f, 0.f, 1.f};
    cleanBranchTransform.dirty = false;
    cleanBranchTransform.local_to_world =
        fuse::ecs::from_trs({1.f, 5.f, 0.f, 1.f}, cleanBranchTransform.rotation, cleanBranchTransform.scale);
    cleanBranchTransform.world_to_local = fuse::ecs::inverse_affine(cleanBranchTransform.local_to_world);
    reg.add(cleanBranch, cleanBranchTransform);

    fuse::ecs::Transform cleanLeafTransform{};
    cleanLeafTransform.parent = cleanBranch;
    cleanLeafTransform.position = {0.f, 1.f, 0.f, 1.f};
    cleanLeafTransform.dirty = false;
    cleanLeafTransform.local_to_world =
        fuse::ecs::from_trs({1.f, 6.f, 0.f, 1.f}, cleanLeafTransform.rotation, cleanLeafTransform.scale);
    cleanLeafTransform.world_to_local = fuse::ecs::inverse_affine(cleanLeafTransform.local_to_world);
    reg.add(cleanLeaf, cleanLeafTransform);

    fuse::ecs::Transform dirtyChildTransform{};
    dirtyChildTransform.parent = dirtyRoot;
    dirtyChildTransform.position = {0.f, 3.f, 0.f, 1.f};
    dirtyChildTransform.dirty = true;
    reg.add(dirtyChild, dirtyChildTransform);

    withScheduler(2, [&] {
        fuse::ecs::TransformSystem::update(reg);
    });

    const fuse::ecs::Transform* updatedBranch = reg.get<fuse::ecs::Transform>(cleanBranch);
    const fuse::ecs::Transform* updatedLeaf = reg.get<fuse::ecs::Transform>(cleanLeaf);
    const fuse::ecs::Transform* updatedDirtyChild = reg.get<fuse::ecs::Transform>(dirtyChild);

    expectTrue(updatedBranch != nullptr && updatedLeaf != nullptr && updatedDirtyChild != nullptr,
               "mixed hierarchy entities survive update");
    expectTrue(updatedBranch->local_to_world.data[12] == 1.f &&
                   updatedBranch->local_to_world.data[13] == 5.f,
               "clean subtree skip preserves branch world matrix");
    expectTrue(updatedLeaf->local_to_world.data[12] == 1.f &&
                   updatedLeaf->local_to_world.data[13] == 6.f,
               "clean subtree skip preserves leaf world matrix");
    expectTrue(updatedDirtyChild->local_to_world.data[12] == 1.f &&
                   updatedDirtyChild->local_to_world.data[13] == 3.f,
               "dirty sibling still recomputes under dirty root");
    expectTrue(!updatedDirtyChild->dirty, "dirty sibling clears dirty flag after update");
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
    testHasDirtyRootsEmptyRegistryGuard();
    testHasDirtyRootsMixedScene();
    testCountRootsIgnoresChildren();
    testNoDirtyRootsDirtyRootStubsEarlyOut();
    testNoDirtyRootsUpdateHierarchyStillRuns();
    testCountDirtyTransformsEmptyRegistryGuard();
    testCountDirtyTransformsMixedScene();
    testShouldSkipHierarchyRecomputeGuards();
    testSubtreeHasDirtyTransformsGuards();
    testFullyCleanSceneUpdateEarlyOut();
    testCleanSubtreeHierarchySkipPreservesMatrices();
    testCountTransformsMatchesEach();

    if (g_failures == 0) {
        std::printf("fuse_ecs_transform_system_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs_transform_system_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
