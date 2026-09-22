// B3.9 ECS gate rows (master plan):
//  - Registry creates and destroys 1M entities, no leaks, generations invalidate stale handles
//  - Archetype storage groups entities by component set (checked after add/remove)
//  - each<T> iterates exactly the right entities
//  - each_parallel<T> matches each<T> across 100 randomised cases
//  - add/remove migrates entities between archetypes with data preserved
//  - 100k Transform+Mesh+RigidBody iteration throughput (500M/s workstation baseline; CI floor)
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/jobs/job_scheduler.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

using fuse::ecs::EntityID;
using fuse::ecs::Mesh;
using fuse::ecs::Registry;
using fuse::ecs::RigidBody;
using fuse::ecs::Transform;

void testMillionEntitiesAndStaleHandles() {
    constexpr fuse::u32 kCount = 1'000'000;
    Registry reg;
    reg.init(fuse::ecs::kMaxEntities);

    std::vector<EntityID> ids;
    ids.reserve(kCount);
    for (fuse::u32 i = 0; i < kCount; ++i) {
        ids.push_back(reg.create());
    }
    bool allValid = true;
    for (const EntityID id : ids) {
        allValid = allValid && id.valid() && reg.alive(id);
    }
    expectTrue(allValid, "1M entities created and alive");
    expectTrue(reg.count() == kCount, "count() == 1M");

    for (const EntityID id : ids) {
        reg.destroy_entity(id);
    }
    expectTrue(reg.count() == 0u, "all 1M destroyed, no leaks in count");

    bool anyStaleAlive = false;
    for (const EntityID id : ids) {
        anyStaleAlive = anyStaleAlive || reg.alive(id);
    }
    expectTrue(!anyStaleAlive, "stale handles are not alive after destroy");

    // Reuse: recycled slots must carry a new generation, so old handles stay dead.
    const EntityID reused = reg.create();
    bool reuseDistinct = true;
    for (fuse::u32 i = 0; i < 16u; ++i) {
        if (ids[i].index == reused.index) {
            reuseDistinct = ids[i].generation != reused.generation && !reg.alive(ids[i]);
        }
    }
    expectTrue(reuseDistinct, "recycled slot has a new generation; old handle stays dead");
    expectTrue(reg.get<Transform>(ids[0]) == nullptr, "stale handle resolves to no component");
}

void testArchetypeGroupingAndMigration() {
    Registry reg;
    reg.init(1024);

    const EntityID a = reg.create();
    const EntityID b = reg.create();
    Transform t{};
    t.position = {1.f, 2.f, 3.f, 1.f};
    reg.add(a, t);
    reg.add(b, t);
    const std::size_t afterTransform = reg.archetype_count();

    Mesh mesh{};
    mesh.index_count = 36;
    reg.add(a, mesh);
    expectTrue(reg.archetype_count() == afterTransform + 1u, "{Transform,Mesh} is a new archetype");
    expectTrue(reg.has<Transform>(a) && reg.has<Mesh>(a) && !reg.has<Mesh>(b), "component sets differ per entity");

    const Transform* migrated = reg.get<Transform>(a);
    expectTrue(migrated != nullptr && migrated->position.x == 1.f && migrated->position.z == 3.f,
               "Transform data preserved across migration");
    expectTrue(reg.get<Mesh>(a) != nullptr && reg.get<Mesh>(a)->index_count == 36u, "added Mesh value stored");

    reg.add(b, mesh);
    expectTrue(reg.archetype_count() == afterTransform + 1u, "second entity joins the existing archetype");

    reg.remove<Mesh>(a);
    expectTrue(!reg.has<Mesh>(a) && reg.has<Transform>(a), "remove<Mesh> migrates back");
    expectTrue(reg.get<Transform>(a)->position.y == 2.f, "data preserved on remove migration");
    expectTrue(reg.get<Mesh>(b) != nullptr && reg.get<Mesh>(b)->index_count == 36u,
               "swap-remove in the source archetype keeps the other entity intact");
}

void testEachVisitsExactlyTheMatchingEntities() {
    Registry reg;
    reg.init(4096);
    std::vector<EntityID> withBoth;
    for (fuse::u32 i = 0; i < 1000u; ++i) {
        const EntityID id = reg.create();
        reg.add<Transform>(id);
        if (i % 3u == 0u) {
            reg.add<RigidBody>(id);
            withBoth.push_back(id);
        }
    }
    std::vector<fuse::u8> seen(4096, 0u);
    fuse::u32 visits = 0;
    reg.each<Transform, RigidBody>([&](EntityID id, Transform&, RigidBody&) {
        ++visits;
        ++seen[id.index];
    });
    bool exact = visits == withBoth.size();
    for (const EntityID id : withBoth) {
        exact = exact && seen[id.index] == 1u;
    }
    expectTrue(exact, "each<Transform,RigidBody> visits exactly the matching entities once");
}

void testEachParallelMatchesEachRandomised() {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(4);

    std::mt19937 rng(0xB39u);
    fuse::u32 mismatches = 0;
    for (int trial = 0; trial < 100; ++trial) {
        Registry serial;
        Registry parallel;
        serial.init(8192);
        parallel.init(8192);
        const fuse::u32 count = 1u + rng() % 4000u;
        for (fuse::u32 i = 0; i < count; ++i) {
            Transform t{};
            t.position = {static_cast<float>(rng() % 1000), static_cast<float>(rng() % 1000), 0.f, 1.f};
            t.dirty = (rng() & 1u) != 0u;
            const bool withBody = (rng() % 4u) == 0u;
            const EntityID s = serial.create();
            const EntityID p = parallel.create();
            serial.add(s, t);
            parallel.add(p, t);
            if (withBody) {
                serial.add<RigidBody>(s);
                parallel.add<RigidBody>(p);
            }
        }
        auto mutate = [](EntityID, Transform& t) {
            if (t.dirty) {
                t.position.x = t.position.x * 2.f + 1.f;
                t.position.y -= 3.f;
                t.dirty = false;
            }
        };
        serial.each<Transform>(mutate);
        parallel.each_parallel<Transform>(mutate, 1u + rng() % 512u);

        std::vector<Transform> expected(8192);
        serial.each<Transform>([&](EntityID id, Transform& t) { expected[id.index] = t; });
        bool same = true;
        parallel.each<Transform>([&](EntityID id, Transform& t) {
            same = same && t.position.x == expected[id.index].position.x &&
                   t.position.y == expected[id.index].position.y && t.dirty == expected[id.index].dirty;
        });
        mismatches += same ? 0u : 1u;
    }
    scheduler.shutdown();
    std::printf("each_parallel vs each: %u / 100 randomised cases mismatched\n", mismatches);
    expectTrue(mismatches == 0u, "each_parallel matches each across 100 randomised cases");
}

void testSingleThreadIterationThroughput() {
    constexpr fuse::u32 kEntities = 100'000;
    Registry reg;
    reg.init(kEntities + 16u);
    for (fuse::u32 i = 0; i < kEntities; ++i) {
        const EntityID id = reg.create();
        reg.add<Transform>(id);
        reg.add<Mesh>(id);
        reg.add<RigidBody>(id);
    }

    float sink = 0.f;
    auto pass = [&]() {
        reg.each<Transform, Mesh, RigidBody>([&](EntityID, Transform& t, Mesh& m, RigidBody& rb) {
            t.position.x += rb.mass * 0.001f;
            sink += static_cast<float>(m.index_count);
        });
    };
    pass(); // warm caches
    constexpr int kPasses = 20;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kPasses; ++i) {
        pass();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const double componentsPerSecond = (3.0 * kEntities * kPasses) / seconds;
    std::printf("each<Transform,Mesh,RigidBody>: %.1f M components/s (sink %.1f)\n", componentsPerSecond / 1e6,
                static_cast<double>(sink));
#if defined(NDEBUG)
    // 500M/s is the workstation baseline (EXECUTION-PLAN §5). These components total 340 bytes per
    // entity, so shared CI runners are memory-bound well below it; enforce a regression floor that
    // the per-row column lookup (60M/s before hoisting) would fail.
    expectTrue(componentsPerSecond > 100e6, "> 100M components/s single-threaded (CI floor)");
#endif
}

} // namespace

int main() {
    testMillionEntitiesAndStaleHandles();
    testArchetypeGroupingAndMigration();
    testEachVisitsExactlyTheMatchingEntities();
    testEachParallelMatchesEachRandomised();
    testSingleThreadIterationThroughput();

    if (g_failures == 0) {
        std::printf("fuse_b3_ecs_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b3_ecs_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
