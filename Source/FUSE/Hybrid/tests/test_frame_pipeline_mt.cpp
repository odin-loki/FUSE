// MT regression: workers read immutable snapshots — no SceneObject* in parallel_for bodies.
// This file is compiled into fuse_hybrid_tests (see CMakeLists.txt).

#include <fuse/jobs/parallel_for.hpp>
#include <fuse/world2d/scene_snapshot.hpp>

#include <atomic>
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

void testSnapshotCullUsesHandlesOnly() {
    fuse::world2d::SceneSnapshot2D snapshot;
    snapshot.reserve(64);

    for (fuse::u32 i = 0; i < 64; ++i) {
        fuse::world2d::SpriteDrawCmd cmd;
        cmd.object = fuse::Handle<fuse::Object>(i + 1, 1);
        cmd.x = static_cast<float>(i);
        cmd.y = static_cast<float>(i);
        cmd.visible = true;
        snapshot.addSprite(cmd);
    }

    std::atomic<fuse::u32> visibleCount{0};
    const fuse::u32 count = static_cast<fuse::u32>(snapshot.sprites().size());

    fuse::jobs::parallel_for(0u, count, 8u, [&snapshot, &visibleCount](fuse::u32 i) {
        const fuse::world2d::SpriteDrawCmd& cmd = snapshot.sprites()[i];
        expectTrue(cmd.object.isValid(), "worker reads handle, not raw scene pointer");
        if (cmd.visible && cmd.x < 50.f) {
            visibleCount.fetch_add(1u, std::memory_order_relaxed);
        }
    });

    expectTrue(visibleCount.load() == 50u, "parallel cull counted expected visible sprites");
}

void testEmptySnapshotParallelForIsNoOp() {
    fuse::world2d::SceneSnapshot2D snapshot;
    std::atomic<fuse::u32> visits{0};

    fuse::jobs::parallel_for(0u, 0u, 8u, [&snapshot, &visits](fuse::u32 i) {
        (void)snapshot;
        (void)i;
        visits.fetch_add(1u, std::memory_order_relaxed);
    });

    expectTrue(visits.load() == 0u, "empty cull range invokes zero workers");
}

void testSingleElementBatchLargerThanCount() {
    fuse::world2d::SceneSnapshot2D snapshot;
    fuse::world2d::SpriteDrawCmd cmd;
    cmd.object = fuse::Handle<fuse::Object>(1, 1);
    cmd.x = 3.f;
    cmd.y = 4.f;
    cmd.visible = true;
    snapshot.addSprite(cmd);

    std::atomic<fuse::u32> visits{0};
    fuse::jobs::parallel_for(0u, 1u, 8u, [&snapshot, &visits](fuse::u32 i) {
        expectTrue(i == 0u, "single-element batch visits index zero");
        expectTrue(snapshot.sprites()[i].visible, "single-element batch reads snapshot");
        visits.fetch_add(1u, std::memory_order_relaxed);
    });

    expectTrue(visits.load() == 1u, "grain larger than count still processes one element");
}

void testPartialFinalBatch() {
    fuse::world2d::SceneSnapshot2D snapshot;
    snapshot.reserve(9);

    for (fuse::u32 i = 0; i < 9u; ++i) {
        fuse::world2d::SpriteDrawCmd cmd;
        cmd.object = fuse::Handle<fuse::Object>(i + 1, 1);
        cmd.visible = (i % 3u != 0u);
        snapshot.addSprite(cmd);
    }

    std::atomic<fuse::u32> visibleCount{0};
    const fuse::u32 count = static_cast<fuse::u32>(snapshot.sprites().size());

    fuse::jobs::parallel_for(0u, count, 8u, [&snapshot, &visibleCount](fuse::u32 i) {
        if (snapshot.sprites()[i].visible) {
            visibleCount.fetch_add(1u, std::memory_order_relaxed);
        }
    });

    expectTrue(visibleCount.load() == 6u, "partial final batch counts visible sprites");
}

} // namespace

int runFramePipelineMtTests() {
    testSnapshotCullUsesHandlesOnly();
    testEmptySnapshotParallelForIsNoOp();
    testSingleElementBatchLargerThanCount();
    testPartialFinalBatch();

    if (g_failures == 0) {
        std::printf("fuse frame pipeline MT tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse frame pipeline MT tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
