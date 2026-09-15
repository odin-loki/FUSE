#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/detail/iteration_parity.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

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

void testEachParallelVisitsSameEntities() {
    fuse::ecs::Registry reg;
    reg.init(256);

    for (fuse::u32 i = 0; i < 64; ++i) {
        const fuse::ecs::EntityID id = reg.create();
        fuse::ecs::Transform& transform = reg.add<fuse::ecs::Transform>(id);
        transform.position.x = static_cast<float>(i);
    }

    fuse::u32 serialCount = 0;
    reg.each<fuse::ecs::Transform>([&](fuse::ecs::EntityID, fuse::ecs::Transform&) {
        ++serialCount;
    });

    std::atomic<fuse::u32> parallelCount{0};
    withScheduler(4, [&] {
        reg.each_parallel<fuse::ecs::Transform>([&](fuse::ecs::EntityID, fuse::ecs::Transform&) {
            parallelCount.fetch_add(1u, std::memory_order_relaxed);
        }, 8);
    });

    expectEq(serialCount, 64u, "serial each visits all transforms");
    expectEq(parallelCount.load(std::memory_order_relaxed), 64u, "each_parallel visits all transforms");
}

void testEachParallelStressVisitCoverage() {
    constexpr fuse::u32 kEntityCount = 256;
    constexpr fuse::u32 kIterations = 64;
    const fuse::u32 workerCounts[] = {1, 2, 4, 8};
    const fuse::u32 batchSizes[] = {1, 4, 8, 16, 32, 64};

    for (fuse::u32 workers : workerCounts) {
        withScheduler(workers, [&] {
            for (fuse::u32 batchSize : batchSizes) {
                for (fuse::u32 iteration = 0; iteration < kIterations; ++iteration) {
                    fuse::ecs::Registry reg;
                    reg.init(kEntityCount);

                    for (fuse::u32 i = 0; i < kEntityCount; ++i) {
                        const fuse::ecs::EntityID id = reg.create();
                        fuse::ecs::Transform& transform = reg.add<fuse::ecs::Transform>(id);
                        transform.position.x = static_cast<float>(i);
                    }

                    std::vector<std::atomic<bool>> visited(kEntityCount);
                    for (auto& slot : visited) {
                        slot.store(false, std::memory_order_relaxed);
                    }

                    reg.each_parallel<fuse::ecs::Transform>(
                        [&](fuse::ecs::EntityID, fuse::ecs::Transform& transform) {
                            const fuse::u32 index = static_cast<fuse::u32>(transform.position.x);
                            if (index < kEntityCount) {
                                visited[index].store(true, std::memory_order_relaxed);
                            }
                        },
                        batchSize);

                    fuse::u32 visitedCount = 0;
                    for (fuse::u32 i = 0; i < kEntityCount; ++i) {
                        if (visited[i].load(std::memory_order_relaxed)) {
                            ++visitedCount;
                        }
                    }

                    if (visitedCount != kEntityCount) {
                        std::fprintf(stderr,
                                     "FAIL: stress visit coverage workers=%u batch=%u iter=%u (expected %u, got %u)\n",
                                     workers,
                                     batchSize,
                                     iteration,
                                     kEntityCount,
                                     visitedCount);
                        ++g_failures;
                        return;
                    }
                }
            }
        });
    }
}

void testEachParallelMutationParity() {
    fuse::ecs::Registry serialReg;
    fuse::ecs::Registry parallelReg;
    serialReg.init(512);
    parallelReg.init(512);

    for (fuse::u32 i = 0; i < 128; ++i) {
        const fuse::ecs::EntityID serialId = serialReg.create();
        const fuse::ecs::EntityID parallelId = parallelReg.create();

        fuse::ecs::Transform serialTransform{};
        serialTransform.position = {static_cast<float>(i), static_cast<float>(i * 2), 0.f, 1.f};
        serialTransform.dirty = (i % 3) == 0;
        serialReg.add(serialId, serialTransform);

        fuse::ecs::Transform parallelTransform = serialTransform;
        parallelReg.add(parallelId, parallelTransform);

        if ((i % 5) == 0) {
            serialReg.add<fuse::ecs::RigidBody>(serialId);
            parallelReg.add<fuse::ecs::RigidBody>(parallelId);
        }
    }

    serialReg.each<fuse::ecs::Transform>([&](fuse::ecs::EntityID, fuse::ecs::Transform& transform) {
        if (!transform.dirty) {
            return;
        }
        transform.position.x += 1.f;
        transform.position.y += 2.f;
        transform.dirty = false;
    });

    withScheduler(4, [&] {
        parallelReg.each_parallel<fuse::ecs::Transform>([&](fuse::ecs::EntityID, fuse::ecs::Transform& transform) {
            if (!transform.dirty) {
                return;
            }
            transform.position.x += 1.f;
            transform.position.y += 2.f;
            transform.dirty = false;
        }, 16);
    });

    std::unordered_map<fuse::u32, fuse::ecs::Transform> serialByIndex;
    serialReg.each<fuse::ecs::Transform>([&](fuse::ecs::EntityID id, fuse::ecs::Transform& transform) {
        serialByIndex[id.index] = transform;
    });

    parallelReg.each<fuse::ecs::Transform>([&](fuse::ecs::EntityID id, fuse::ecs::Transform& transform) {
        const auto it = serialByIndex.find(id.index);
        expectTrue(it != serialByIndex.end(), "parallel registry entity exists in serial snapshot");
        if (it == serialByIndex.end()) {
            return;
        }
        expectTrue(transform.position.x == it->second.position.x, "each_parallel mutation matches each on X");
        expectTrue(transform.position.y == it->second.position.y, "each_parallel mutation matches each on Y");
        expectTrue(transform.dirty == it->second.dirty, "each_parallel dirty flag matches each");
    });
}

void testEachParallelMultiComponent() {
    fuse::ecs::Registry reg;
    reg.init(64);

    const fuse::ecs::EntityID id = reg.create();
    fuse::ecs::Transform& transform = reg.add<fuse::ecs::Transform>(id);
    transform.position.x = 7.f;
    reg.add<fuse::ecs::RigidBody>(id);

    bool visited = false;
    withScheduler(2, [&] {
        reg.each_parallel<fuse::ecs::Transform, fuse::ecs::RigidBody>(
            [&](fuse::ecs::EntityID entityId, fuse::ecs::Transform& visitedTransform, fuse::ecs::RigidBody&) {
                visited = true;
                expectTrue(entityId.valid(), "multi-component each_parallel visits valid entity");
                expectTrue(visitedTransform.position.x == 7.f, "multi-component each_parallel reads transform");
            },
            4);
    });

    expectTrue(visited, "multi-component each_parallel visits matching archetype");
}

void testTransformSystemParallelDirtyRoots() {
    fuse::ecs::Registry registry;
    registry.init();

    const fuse::ecs::EntityID parent = registry.create();
    const fuse::ecs::EntityID child = registry.create();

    fuse::ecs::Transform parentTransform{};
    parentTransform.position = {2.f, 0.f, 0.f, 1.f};
    parentTransform.dirty = true;
    registry.add(parent, parentTransform);

    fuse::ecs::Transform childTransform{};
    childTransform.position = {0.f, 3.f, 0.f, 1.f};
    childTransform.parent = parent;
    childTransform.dirty = true;
    registry.add(child, childTransform);

    withScheduler(4, [&] {
        fuse::ecs::TransformSystemOptions options{};
        options.parallelDirtyRoots = true;
        options.batchSize = 8;
        fuse::ecs::TransformSystem::update(registry, options);
    });

    const fuse::ecs::Transform* updated = registry.get<fuse::ecs::Transform>(child);
    expectTrue(updated != nullptr, "child transform exists after parallel update");
    expectTrue(updated->local_to_world.data[12] == 2.f, "parallel dirty-root pass preserves hierarchy X");
    expectTrue(updated->local_to_world.data[13] == 3.f, "parallel dirty-root pass preserves hierarchy Y");
}

void testTransformSystemSerialDirtyRoots() {
    fuse::ecs::Registry registry;
    registry.init();

    const fuse::ecs::EntityID root = registry.create();
    fuse::ecs::Transform rootTransform{};
    rootTransform.position = {5.f, 1.f, 0.f, 1.f};
    rootTransform.dirty = true;
    registry.add(root, rootTransform);

    fuse::ecs::TransformSystemOptions options{};
    options.parallelDirtyRoots = false;
    fuse::ecs::TransformSystem::update(registry, options);

    const fuse::ecs::Transform* updated = registry.get<fuse::ecs::Transform>(root);
    expectTrue(updated != nullptr, "root transform exists after serial update");
    expectTrue(updated->local_to_world.data[12] == 5.f, "serial dirty-root pass updates root translation on X");
    expectTrue(updated->local_to_world.data[13] == 1.f, "serial dirty-root pass updates root translation on Y");
    expectTrue(!updated->dirty, "serial dirty-root pass clears dirty flag");
}

void populateTransformParityScene(fuse::ecs::Registry& reg) {
    reg.init(128);

    const fuse::ecs::EntityID rootA = reg.create();
    const fuse::ecs::EntityID rootB = reg.create();
    const fuse::ecs::EntityID child = reg.create();

    fuse::ecs::Transform rootATransform{};
    rootATransform.position = {1.f, 0.f, 0.f, 1.f};
    rootATransform.dirty = true;
    reg.add(rootA, rootATransform);

    fuse::ecs::Transform rootBTransform{};
    rootBTransform.position = {0.f, 4.f, 0.f, 1.f};
    rootBTransform.dirty = true;
    reg.add(rootB, rootBTransform);

    fuse::ecs::Transform childTransform{};
    childTransform.position = {0.f, 2.f, 0.f, 1.f};
    childTransform.parent = rootA;
    childTransform.dirty = true;
    reg.add(child, childTransform);
}

void testEachParallelVisitCountParityHelper() {
    fuse::ecs::Registry reg;
    reg.init(64);

    for (fuse::u32 i = 0; i < 32; ++i) {
        const fuse::ecs::EntityID id = reg.create();
        reg.add<fuse::ecs::Transform>(id);
    }

    withScheduler(4, [&] {
        expectTrue(fuse::ecs::detail::each_parallel_matches_serial<fuse::ecs::Transform>(reg, 0),
                   "each parity helper: batchSize=0 matches serial visit count");
        expectTrue(fuse::ecs::detail::each_parallel_matches_serial<fuse::ecs::Transform>(reg, 8),
                   "each parity helper: batchSize=8 matches serial visit count");
        expectTrue(fuse::ecs::detail::each_parallel_matches_serial<fuse::ecs::Transform>(reg, 1024),
                   "each parity helper: oversized batch matches serial visit count");
        expectTrue(fuse::ecs::detail::each_query_parallel_matches_serial<fuse::ecs::Transform>(reg, 0),
                   "each_query parity helper: batchSize=0 matches serial visit count");
        expectTrue(fuse::ecs::detail::each_query_parallel_matches_serial<fuse::ecs::Transform>(reg, 8),
                   "each_query parity helper: batchSize=8 matches serial visit count");
        expectTrue(fuse::ecs::detail::each_query_parallel_matches_serial<fuse::ecs::Transform>(reg, 1024),
                   "each_query parity helper: oversized batch matches serial visit count");
    });
}

void testEachParallelEmptyRegistryParityHelper() {
    fuse::ecs::Registry reg;
    reg.init(8);

    withScheduler(2, [&] {
        expectTrue(fuse::ecs::detail::each_parallel_matches_serial<fuse::ecs::Transform>(reg, 1),
                   "each parity helper: empty registry yields zero serial and parallel visits");
        expectTrue(fuse::ecs::detail::each_query_parallel_matches_serial<fuse::ecs::Transform>(reg, 1),
                   "each_query parity helper: empty registry yields zero serial and parallel visits");
    });
}

void testTransformDirtyPropagationToCleanChild() {
    fuse::ecs::Registry registry;
    registry.init();

    const fuse::ecs::EntityID parent = registry.create();
    const fuse::ecs::EntityID child = registry.create();

    fuse::ecs::Transform parentTransform{};
    parentTransform.position = {0.f, 0.f, 0.f, 1.f};
    parentTransform.dirty = true;
    registry.add(parent, parentTransform);

    fuse::ecs::Transform childTransform{};
    childTransform.position = {0.f, 1.f, 0.f, 1.f};
    childTransform.parent = parent;
    childTransform.dirty = true;
    registry.add(child, childTransform);

    fuse::ecs::TransformSystem::update(registry);

    fuse::ecs::Transform* parentAfterFirst = registry.get<fuse::ecs::Transform>(parent);
    fuse::ecs::Transform* childAfterFirst = registry.get<fuse::ecs::Transform>(child);
    expectTrue(parentAfterFirst != nullptr && childAfterFirst != nullptr, "parent/child exist after first update");
    if (parentAfterFirst == nullptr || childAfterFirst == nullptr) {
        return;
    }

    parentAfterFirst->position.x = 10.f;
    parentAfterFirst->dirty = true;
    childAfterFirst->dirty = false;

    fuse::ecs::TransformSystem::update(registry);

    const fuse::ecs::Transform* updatedChild = registry.get<fuse::ecs::Transform>(child);
    expectTrue(updatedChild != nullptr, "child exists after dirty parent move");
    expectTrue(updatedChild->local_to_world.data[12] == 10.f, "dirty parent propagates X to clean child");
    expectTrue(updatedChild->local_to_world.data[13] == 1.f, "clean child keeps local Y after parent move");
    expectTrue(!updatedChild->dirty, "hierarchy pass clears child dirty flag after recompute");
}

void populateDirtyRootParityScene(fuse::ecs::Registry& reg) {
    for (fuse::u32 i = 0; i < 16; ++i) {
        const fuse::ecs::EntityID id = reg.create();

        fuse::ecs::Transform transform{};
        transform.position = {static_cast<float>(i), static_cast<float>(i * 2), 0.f, 1.f};
        transform.dirty = (i % 2) == 0;
        reg.add(id, transform);
    }
}

void testDirtyRootStubSerialParallelParity() {
    fuse::ecs::Registry serialReg;
    fuse::ecs::Registry parallelReg;
    serialReg.init(64);
    parallelReg.init(64);
    populateDirtyRootParityScene(serialReg);
    populateDirtyRootParityScene(parallelReg);

    withScheduler(4, [&] {
        expectTrue(fuse::ecs::detail::transform_dirty_roots_serial_parallel_match(serialReg, parallelReg, 4),
                   "dirty-root parity helper: serial/parallel stubs produce identical matrices");
    });
}

void testCountDirtyRoots() {
    fuse::ecs::Registry reg;
    reg.init(32);
    populateDirtyRootParityScene(reg);
    expectEq(fuse::ecs::TransformSystem::count_dirty_roots(reg), 8u,
             "count_dirty_roots tallies dirty roots only");
}

void testDirtyRootPassSkipsChildrenAndCleanRoots() {
    fuse::ecs::Registry reg;
    reg.init(16);

    const fuse::ecs::EntityID root = reg.create();
    const fuse::ecs::EntityID child = reg.create();

    fuse::ecs::Transform rootTransform{};
    rootTransform.position = {1.f, 0.f, 0.f, 1.f};
    rootTransform.dirty = false;
    rootTransform.local_to_world = fuse::ecs::mat4::identity();
    rootTransform.world_to_local = fuse::ecs::mat4::identity();
    reg.add(root, rootTransform);

    fuse::ecs::Transform childTransform{};
    childTransform.position = {0.f, 2.f, 0.f, 1.f};
    childTransform.parent = root;
    childTransform.dirty = true;
    reg.add(child, childTransform);

    expectEq(fuse::ecs::TransformSystem::count_dirty_roots(reg), 0u,
             "child dirty flag does not count as dirty root");

    fuse::ecs::TransformSystem::update_dirty_roots_serial(reg);

    const fuse::ecs::Transform* updatedChild = reg.get<fuse::ecs::Transform>(child);
    expectTrue(updatedChild != nullptr, "child exists after dirty-root serial pass");
    expectTrue(updatedChild->dirty, "dirty-root pass skips child entities");
    expectTrue(updatedChild->local_to_world.data[12] == 0.f,
               "dirty-root pass does not touch child world matrix");
}

void testTransformSystemEmptyRegistryUpdate() {
    fuse::ecs::Registry reg;
    reg.init(8);

    withScheduler(2, [&] {
        fuse::ecs::TransformSystemOptions options{};
        options.parallelDirtyRoots = true;
        fuse::ecs::TransformSystem::update(reg, options);
    });

    expectEq(fuse::ecs::TransformSystem::count_dirty_roots(reg), 0u,
             "empty registry has zero dirty roots");
    expectTrue(fuse::ecs::detail::each_parallel_matches_serial<fuse::ecs::Transform>(reg, 1),
               "empty registry each_parallel parity after TransformSystem::update");
}

void testTransformDirtyPropagationDeepHierarchy() {
    fuse::ecs::Registry registry;
    registry.init();

    const fuse::ecs::EntityID root = registry.create();
    const fuse::ecs::EntityID mid = registry.create();
    const fuse::ecs::EntityID leaf = registry.create();

    fuse::ecs::Transform rootTransform{};
    rootTransform.position = {0.f, 0.f, 0.f, 1.f};
    rootTransform.dirty = true;
    registry.add(root, rootTransform);

    fuse::ecs::Transform midTransform{};
    midTransform.position = {0.f, 1.f, 0.f, 1.f};
    midTransform.parent = root;
    midTransform.dirty = true;
    registry.add(mid, midTransform);

    fuse::ecs::Transform leafTransform{};
    leafTransform.position = {0.f, 0.f, 1.f, 1.f};
    leafTransform.parent = mid;
    leafTransform.dirty = true;
    registry.add(leaf, leafTransform);

    fuse::ecs::TransformSystem::update(registry);

    fuse::ecs::Transform* rootAfter = registry.get<fuse::ecs::Transform>(root);
    fuse::ecs::Transform* midAfter = registry.get<fuse::ecs::Transform>(mid);
    fuse::ecs::Transform* leafAfter = registry.get<fuse::ecs::Transform>(leaf);
    expectTrue(rootAfter != nullptr && midAfter != nullptr && leafAfter != nullptr,
               "deep hierarchy entities exist after first update");
    if (rootAfter == nullptr || midAfter == nullptr || leafAfter == nullptr) {
        return;
    }

    rootAfter->position.x = 5.f;
    rootAfter->dirty = true;
    midAfter->dirty = false;
    leafAfter->dirty = false;

    fuse::ecs::TransformSystem::update(registry);

    const fuse::ecs::Transform* updatedMid = registry.get<fuse::ecs::Transform>(mid);
    const fuse::ecs::Transform* updatedLeaf = registry.get<fuse::ecs::Transform>(leaf);
    expectTrue(updatedMid != nullptr && updatedLeaf != nullptr, "deep hierarchy entities exist after parent move");
    expectTrue(updatedMid->local_to_world.data[12] == 5.f, "dirty root propagates X to clean mid child");
    expectTrue(updatedMid->local_to_world.data[13] == 1.f, "clean mid child keeps local Y after parent move");
    expectTrue(updatedLeaf->local_to_world.data[12] == 5.f, "dirty root propagates X to clean leaf grandchild");
    expectTrue(updatedLeaf->local_to_world.data[14] == 1.f, "clean leaf keeps local Z after parent move");
}

void testTransformSystemSerialParallelParityMultiBatch() {
    const fuse::u32 batchSizes[] = {1, 4, 16, 64, 256, 0, 1024};
    const fuse::u32 workerCounts[] = {1, 2, 4};

    for (fuse::u32 workers : workerCounts) {
        withScheduler(workers, [&] {
            for (fuse::u32 batchSize : batchSizes) {
                fuse::ecs::Registry serialReg;
                fuse::ecs::Registry parallelReg;
                populateTransformParityScene(serialReg);
                populateTransformParityScene(parallelReg);

                fuse::ecs::TransformSystemOptions serialOptions{};
                serialOptions.parallelDirtyRoots = false;
                fuse::ecs::TransformSystem::update(serialReg, serialOptions);

                fuse::ecs::TransformSystemOptions parallelOptions{};
                parallelOptions.parallelDirtyRoots = true;
                parallelOptions.batchSize = batchSize;
                fuse::ecs::TransformSystem::update(parallelReg, parallelOptions);

                expectTrue(fuse::ecs::detail::transform_registries_match(serialReg, parallelReg),
                           "TransformSystem serial/parallel parity across batch sizes and workers");
            }
        });
    }
}

void testTransformSystemSerialParallelParity() {
    fuse::ecs::Registry serialReg;
    fuse::ecs::Registry parallelReg;
    populateTransformParityScene(serialReg);
    populateTransformParityScene(parallelReg);

    fuse::ecs::TransformSystemOptions serialOptions{};
    serialOptions.parallelDirtyRoots = false;
    fuse::ecs::TransformSystem::update(serialReg, serialOptions);

    withScheduler(4, [&] {
        fuse::ecs::TransformSystemOptions parallelOptions{};
        parallelOptions.parallelDirtyRoots = true;
        parallelOptions.batchSize = 4;
        fuse::ecs::TransformSystem::update(parallelReg, parallelOptions);
    });

    std::unordered_map<fuse::u32, fuse::ecs::Transform> serialByIndex;
    serialReg.each<fuse::ecs::Transform>([&](fuse::ecs::EntityID id, fuse::ecs::Transform& transform) {
        serialByIndex[id.index] = transform;
    });

    parallelReg.each<fuse::ecs::Transform>([&](fuse::ecs::EntityID id, fuse::ecs::Transform& transform) {
        const auto it = serialByIndex.find(id.index);
        expectTrue(it != serialByIndex.end(), "parallel registry entity exists in serial snapshot");
        if (it == serialByIndex.end()) {
            return;
        }
        expectTrue(fuse::ecs::detail::transform_matrices_equal(transform, it->second),
                   "TransformSystem serial/parallel paths produce identical matrices");
    });
}

void testEachParallelBatchSizeZero() {
    fuse::ecs::Registry reg;
    reg.init(16);

    for (fuse::u32 i = 0; i < 8; ++i) {
        const fuse::ecs::EntityID id = reg.create();
        reg.add<fuse::ecs::Transform>(id);
    }

    std::atomic<fuse::u32> visitCount{0};
    withScheduler(2, [&] {
        reg.each_parallel<fuse::ecs::Transform>([&](fuse::ecs::EntityID, fuse::ecs::Transform&) {
            visitCount.fetch_add(1u, std::memory_order_relaxed);
        }, 0);
    });

    expectEq(visitCount.load(std::memory_order_relaxed), 8u, "batchSize=0 still visits all entities");
}

void testEachParallelBatchSizeExceedsEntityCount() {
    fuse::ecs::Registry reg;
    reg.init(8);

    for (fuse::u32 i = 0; i < 3; ++i) {
        const fuse::ecs::EntityID id = reg.create();
        reg.add<fuse::ecs::Transform>(id);
    }

    std::atomic<fuse::u32> visitCount{0};
    withScheduler(4, [&] {
        reg.each_parallel<fuse::ecs::Transform>([&](fuse::ecs::EntityID, fuse::ecs::Transform&) {
            visitCount.fetch_add(1u, std::memory_order_relaxed);
        }, 1024);
    });

    expectEq(visitCount.load(std::memory_order_relaxed), 3u, "batchSize larger than count visits all entities");
}

void testEachParallelEmptyRegistry() {
    fuse::ecs::Registry reg;
    reg.init(8);

    std::atomic<fuse::u32> visitCount{0};
    withScheduler(2, [&] {
        reg.each_parallel<fuse::ecs::Transform>([&](fuse::ecs::EntityID, fuse::ecs::Transform&) {
            visitCount.fetch_add(1u, std::memory_order_relaxed);
        }, 1);
    });

    expectEq(visitCount.load(std::memory_order_relaxed), 0u, "empty registry produces zero parallel visits");
}

void testEachQueryParallelWithWithoutSmoke() {
    fuse::ecs::Registry reg;
    reg.init(64);

    const fuse::ecs::EntityID dynamicBody = reg.create();
    const fuse::ecs::EntityID staticBody = reg.create();

    reg.add<fuse::ecs::Transform>(dynamicBody);
    reg.add<fuse::ecs::RigidBody>(dynamicBody);
    reg.add<fuse::ecs::Transform>(staticBody);
    reg.add<fuse::ecs::RigidBody>(staticBody);
    reg.add<fuse::ecs::TagStatic>(staticBody);

    std::atomic<fuse::u32> serialCount{0};
    reg.each_query<fuse::ecs::Transform, fuse::ecs::RigidBody>(
        [&](fuse::ecs::EntityID id, fuse::ecs::Transform&, fuse::ecs::RigidBody&) {
            expectTrue(id == dynamicBody, "serial each_query With/Without skips static bodies");
            serialCount.fetch_add(1u, std::memory_order_relaxed);
        },
        fuse::ecs::Without<fuse::ecs::TagStatic>{});

    std::atomic<fuse::u32> parallelCount{0};
    withScheduler(4, [&] {
        reg.each_query_parallel<fuse::ecs::Transform, fuse::ecs::RigidBody>(
            [&](fuse::ecs::EntityID id, fuse::ecs::Transform&, fuse::ecs::RigidBody&) {
                expectTrue(id == dynamicBody, "parallel each_query With/Without skips static bodies");
                parallelCount.fetch_add(1u, std::memory_order_relaxed);
            },
            fuse::ecs::Without<fuse::ecs::TagStatic>{},
            2);
    });

    expectEq(serialCount.load(std::memory_order_relaxed), 1u, "serial With/Without smoke visits one entity");
    expectEq(parallelCount.load(std::memory_order_relaxed), 1u, "parallel With/Without smoke visits one entity");
}

} // namespace

int main() {
    testEachParallelVisitsSameEntities();
    testEachParallelStressVisitCoverage();
    testEachParallelMutationParity();
    testEachParallelMultiComponent();
    testEachParallelBatchSizeZero();
    testEachParallelBatchSizeExceedsEntityCount();
    testEachParallelEmptyRegistry();
    testEachParallelVisitCountParityHelper();
    testEachParallelEmptyRegistryParityHelper();
    testEachQueryParallelWithWithoutSmoke();
    testTransformDirtyPropagationToCleanChild();
    testTransformDirtyPropagationDeepHierarchy();
    testCountDirtyRoots();
    testDirtyRootPassSkipsChildrenAndCleanRoots();
    testTransformSystemEmptyRegistryUpdate();
    testDirtyRootStubSerialParallelParity();
    testTransformSystemParallelDirtyRoots();
    testTransformSystemSerialDirtyRoots();
    testTransformSystemSerialParallelParity();
    testTransformSystemSerialParallelParityMultiBatch();

    if (g_failures == 0) {
        std::printf("fuse_ecs_each_parallel_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs_each_parallel_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
