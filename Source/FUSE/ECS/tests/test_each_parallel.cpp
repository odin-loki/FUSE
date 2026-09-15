#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/tags.hpp>
#include <fuse/ecs/components/transform.hpp>
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

bool transformMatricesMatch(const fuse::ecs::Transform& lhs, const fuse::ecs::Transform& rhs) {
    for (fuse::u32 i = 0; i < 16; ++i) {
        if (lhs.local_to_world.data[i] != rhs.local_to_world.data[i]) {
            return false;
        }
        if (lhs.world_to_local.data[i] != rhs.world_to_local.data[i]) {
            return false;
        }
    }
    return lhs.dirty == rhs.dirty;
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
        expectTrue(transformMatricesMatch(transform, it->second),
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
    testEachQueryParallelWithWithoutSmoke();
    testTransformSystemParallelDirtyRoots();
    testTransformSystemSerialDirtyRoots();
    testTransformSystemSerialParallelParity();

    if (g_failures == 0) {
        std::printf("fuse_ecs_each_parallel_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_ecs_each_parallel_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
