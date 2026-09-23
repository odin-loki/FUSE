// B4 pipeline steady-state allocation gate: a physics-enabled world's frame must make zero heap
// allocations on any thread (game thread + job workers running the parallel broadphase / refine).
// Global operator new is replaced with an all-thread counter; each case warms up (buffers grow to
// their working size) and then counts allocations over the measured frames.
//
// Cases (1000 bodies each):
//   - PhysicsPipeline 3D (spatial hash) with a static ground plane
//   - PhysicsPipeline 2D (spatial hash 2D) mixing circles and boxes
//   - World3D with physics enabled (World3D::tick: sync, PhysicsWorld3D::step, snapshot, cull)
//   - World2D with physics enabled (World2D::tick)
// Also prints the median PhysicsPipeline::step wall time for the 1000-body 3D case.

#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/physics/physics_pipeline.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<bool> g_counting{false};
std::atomic<unsigned long> g_allocations{0};

void noteAllocation() {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1u, std::memory_order_relaxed);
    }
}

void* countedAlloc(std::size_t size) {
    noteAllocation();
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
} // namespace

void* operator new(std::size_t size) { return countedAlloc(size); }
void* operator new[](std::size_t size) { return countedAlloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    noteAllocation();
    return std::malloc(size == 0 ? 1 : size);
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return ::operator new(size, tag); }
void* operator new(std::size_t size, std::align_val_t alignment) {
    noteAllocation();
    const std::size_t align = static_cast<std::size_t>(alignment);
    const std::size_t rounded = ((size == 0 ? 1 : size) + align - 1u) / align * align;
    if (void* p = std::aligned_alloc(align, rounded)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr fuse::u32 kWarmupFrames = 16;
constexpr fuse::u32 kMeasuredFrames = 64;
constexpr int kGrid = 10; // 10^3 = 1000 bodies
constexpr int kBodies = kGrid * kGrid * kGrid;
// Steady-state allocation bound per frame (all threads).
constexpr double kMaxAllocationsPerFrame = 0.0;
constexpr float kDt = 1.f / 60.f;

struct Measurement {
    unsigned long allocations = 0;
    double medianStepUs = 0.0;
};

template <typename Step>
Measurement measureFrames(Step&& step) {
    std::vector<double> stepUs;
    stepUs.reserve(kMeasuredFrames);
    Measurement result{};
    for (fuse::u32 frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
        const bool measuring = frame >= kWarmupFrames;
        g_allocations.store(0u);
        g_counting.store(measuring);
        const auto start = std::chrono::steady_clock::now();
        step(frame);
        const auto end = std::chrono::steady_clock::now();
        g_counting.store(false);
        if (measuring) {
            result.allocations += g_allocations.load();
            stepUs.push_back(std::chrono::duration<double, std::micro>(end - start).count());
        }
    }
    std::sort(stepUs.begin(), stepUs.end());
    result.medianStepUs = stepUs[stepUs.size() / 2u];
    return result;
}

void report(const char* name, const Measurement& m, const char* failMessage) {
    const double perFrame = static_cast<double>(m.allocations) / static_cast<double>(kMeasuredFrames);
    std::printf("physics alloc gate: %-26s %lu heap allocations over %u frames (%.2f/frame), median frame %.1f us\n",
                name, m.allocations, kMeasuredFrames, perFrame, m.medianStepUs);
    expectTrue(perFrame <= kMaxAllocationsPerFrame, failMessage);
}

void testPipeline3D() {
    fuse::physics::PhysicsPipeline pipeline;
    pipeline.init({});
    pipeline.addStaticPlane({0.f, 1.f, 0.f}, 0.f);
    for (int x = 0; x < kGrid; ++x) {
        for (int y = 0; y < kGrid; ++y) {
            for (int z = 0; z < kGrid; ++z) {
                // 0.9 spacing with radius 0.5: face neighbours overlap, so every cell has pairs.
                pipeline.addSphereBody({x * 0.9f, 1.f + y * 0.9f, z * 0.9f}, 0.5f);
            }
        }
    }
    const Measurement m = measureFrames([&](fuse::u32) { pipeline.step(kDt); });
    expectTrue(pipeline.candidatePairs().size() > 1000u, "3D pipeline produced candidate pairs");
    expectTrue(pipeline.contactCount() > 1000u, "3D pipeline produced contacts");
    std::printf("physics alloc gate: pipeline 3D candidate pairs %zu, contacts %u\n", pipeline.candidatePairs().size(),
                pipeline.contactCount());
    report("PhysicsPipeline 3D x1000", m, "steady-state PhysicsPipeline 3D step performs zero heap allocations");
}

void testPipeline2D() {
    fuse::physics::PhysicsPipelineDesc desc{};
    desc.broadphaseMode = fuse::physics::BroadphaseMode::SpatialHash2D;
    fuse::physics::PhysicsPipeline pipeline;
    pipeline.init(desc);
    for (int i = 0; i < kBodies; ++i) {
        const fuse::physics::vec3 p{static_cast<float>(i % 40) * 0.9f, static_cast<float>(i / 40) * 0.9f, 0.f};
        if (i % 3 == 0) {
            pipeline.addBoxBody(p, {0.45f, 0.45f, 0.f});
        } else {
            pipeline.addSphereBody(p, 0.5f);
        }
    }
    const Measurement m = measureFrames([&](fuse::u32) { pipeline.step(kDt); });
    expectTrue(pipeline.candidatePairs().size() > 1000u, "2D pipeline produced candidate pairs");
    report("PhysicsPipeline 2D x1000", m, "steady-state PhysicsPipeline 2D step performs zero heap allocations");
}

void testWorld3D() {
    fuse::world3d::World3D world;
    world.setPhysicsEnabled(true);
    world.physics().addStaticPlane({0.f, 1.f, 0.f}, 0.f);
    std::vector<std::unique_ptr<fuse::SceneObject3D>> objects;
    objects.reserve(kBodies);
    for (int i = 0; i < kBodies; ++i) {
        objects.push_back(std::make_unique<fuse::SceneObject3D>("body" + std::to_string(i)));
        objects.back()->setPosition(static_cast<float>(i % kGrid) * 0.9f,
                                    1.f + static_cast<float>((i / kGrid) % kGrid) * 0.9f);
        objects.back()->setZ(static_cast<float>(i / (kGrid * kGrid)) * 0.9f);
        world.addObject(objects.back().get());
    }
    fuse::frame::FrameCtx ctx;
    ctx.dt = kDt;
    const Measurement m = measureFrames([&](fuse::u32 frame) {
        ctx.frameIndex = frame + 1u;
        ctx.time = static_cast<float>(frame) * kDt;
        world.tick(ctx);
    });
    expectTrue(world.physics().bodyCount() == static_cast<fuse::u32>(kBodies + 1), "World3D physics bodies attached");
    expectTrue(world.physics().contactCount() > 0u, "World3D physics produced contacts");
    report("World3D tick (physics) x1000", m, "steady-state physics-enabled World3D::tick performs zero heap allocations");
}

void testWorld2D() {
    fuse::world2d::World2D world;
    world.setPhysicsEnabled(true);
    std::vector<std::unique_ptr<fuse::SceneObject2D>> sprites;
    sprites.reserve(kBodies);
    for (int i = 0; i < kBodies; ++i) {
        sprites.push_back(std::make_unique<fuse::SceneObject2D>("sprite" + std::to_string(i)));
        fuse::SceneObject2D& sprite = *sprites.back();
        sprite.setPhysicsEnabled(true);
        sprite.setPhysicsShape(fuse::PhysicsShape2D::Circle);
        sprite.setPhysicsRadius(0.5f);
        sprite.setCollisionLayer(1);
        sprite.setPosition(static_cast<float>(i % 40) * 0.9f, static_cast<float>(i / 40) * 0.9f);
        world.addSprite(&sprite);
    }
    fuse::frame::FrameCtx ctx;
    ctx.dt = kDt;
    const Measurement m = measureFrames([&](fuse::u32 frame) {
        ctx.frameIndex = frame + 1u;
        ctx.time = static_cast<float>(frame) * kDt;
        world.tick(ctx);
    });
    expectTrue(world.physics().bodyCount() == static_cast<fuse::u32>(kBodies), "World2D physics bodies attached");
    expectTrue(world.physics().contactCount() > 0u, "World2D physics produced contacts");
    report("World2D tick (physics) x1000", m, "steady-state physics-enabled World2D::tick performs zero heap allocations");
}

} // namespace

int main() {
    fuse::core::initialize();
    std::printf("physics alloc gate: %u job worker(s)\n", fuse::jobs::JobScheduler::instance().workerCount());
    testPipeline3D();
    testPipeline2D();
    testWorld3D();
    testWorld2D();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b4_pipeline_alloc_gate: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b4_pipeline_alloc_gate: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
