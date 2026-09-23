// B2.11 gate 3.5 follow-up: "Zero per-frame heap allocations" for the HybridComposer tick + render
// frame (the RhiContext-only gate lives in fuse_b2_frame_alloc_budget). Global operator new is
// replaced with counters: render-thread allocations inside render() and allocations on every thread
// during tick() (game thread + job workers running the World2D/World3D cull chunks) are enforced to
// be zero in steady state.
#include <fuse/core/init.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

thread_local bool t_countAllocations = false;
thread_local unsigned long t_allocationCount = 0;
std::atomic<bool> g_countAllThreads{false};
std::atomic<unsigned long> g_allThreadAllocations{0};

void noteAllocation() {
    if (t_countAllocations) {
        ++t_allocationCount;
    }
    if (g_countAllThreads.load(std::memory_order_relaxed)) {
        g_allThreadAllocations.fetch_add(1u, std::memory_order_relaxed);
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
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr fuse::u32 kWarmupFrames = 8;
constexpr fuse::u32 kMeasuredFrames = 64;
constexpr int kSprites = 48;
constexpr int kObjects3D = 24;

void testHybridFrameDoesNotAllocate() {
    fuse::hybrid::HybridComposer composer;
    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3D;
    std::vector<std::unique_ptr<fuse::SceneObject2D>> sprites;
    for (int i = 0; i < kSprites; ++i) {
        sprites.push_back(std::make_unique<fuse::SceneObject2D>("sprite" + std::to_string(i)));
        sprites.back()->setPosition(static_cast<float>(i * 4 - 96), static_cast<float>((i % 8) * 10 - 40));
        world2D.addSprite(sprites.back().get());
    }
    // 3D objects keep the World3D snapshot + parallel cull on the measured path (one outside the
    // z window so the cull result is non-trivial).
    std::vector<std::unique_ptr<fuse::SceneObject3D>> objects;
    for (int i = 0; i < kObjects3D; ++i) {
        objects.push_back(std::make_unique<fuse::SceneObject3D>("object" + std::to_string(i)));
        objects.back()->setPosition(static_cast<float>(i), static_cast<float>(-i));
        objects.back()->setZ(i == 0 ? 1000.f : static_cast<float>(i * 10 - 120));
        world3D.addObject(objects.back().get());
    }
    world3D.setClearColor(0.1f, 0.15f, 0.25f);
    composer.attachWorld2D(&world2D);
    composer.attachWorld3D(&world3D);

    const bool gpu = composer.rhiContext() != nullptr && composer.rhiContext()->bootstrap().status().deviceReady;
    std::printf("hybrid alloc budget: RHI device %s\n", gpu ? "ready" : "unavailable (software path only)");

    const fuse::u32 workers = fuse::jobs::JobScheduler::instance().workerCount();
    std::printf("hybrid alloc budget: %u job worker(s)\n", workers);

    fuse::frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    unsigned long tickAllocations = 0;
    unsigned long tickGameThreadAllocations = 0;
    for (fuse::u32 frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
        const bool measuring = frame >= kWarmupFrames;
        ctx.frameIndex = frame + 1u;
        ctx.time = static_cast<float>(frame) * ctx.dt;

        const unsigned long gameThreadBefore = t_allocationCount;
        g_allThreadAllocations.store(0u);
        g_countAllThreads.store(measuring);
        t_countAllocations = measuring;
        composer.tick(ctx);
        t_countAllocations = false;
        g_countAllThreads.store(false);
        tickAllocations += measuring ? g_allThreadAllocations.load() : 0u;
        tickGameThreadAllocations += t_allocationCount - gameThreadBefore;
        t_allocationCount = gameThreadBefore;

        t_countAllocations = measuring;
        composer.render(ctx);
        t_countAllocations = false;
    }

    const double frames = static_cast<double>(kMeasuredFrames);
    std::printf("hybrid alloc budget: render() %lu heap allocations over %u frames (%.2f/frame); "
                "tick() all threads %lu (%.2f/frame: game thread %lu, job workers %lu)\n",
                t_allocationCount, kMeasuredFrames, static_cast<double>(t_allocationCount) / frames,
                tickAllocations, static_cast<double>(tickAllocations) / frames, tickGameThreadAllocations,
                tickAllocations - tickGameThreadAllocations);
    expectTrue(composer.lastCommandList().commandCount() >= static_cast<fuse::u32>(kSprites),
               "frame recorded clear + sprites");
    expectTrue(world2D.readSnapshot().visibleCount() == static_cast<fuse::u32>(kSprites),
               "World2D cull kept every sprite visible");
    expectTrue(world3D.readSnapshot().visibleCount() == static_cast<fuse::u32>(kObjects3D - 1),
               "World3D cull dropped only the object outside the z window");
    // tick() never touches Vulkan, so it is enforced even with a validation layer injected.
    expectTrue(tickAllocations == 0u,
               "steady-state HybridComposer::tick performs zero heap allocations on any thread");
    const char* layers = std::getenv("VK_INSTANCE_LAYERS");
    if (layers != nullptr && std::strstr(layers, "validation") != nullptr) {
        std::printf("NOTE: validation layer injected — allocation count reported, not enforced\n");
        return;
    }
    expectTrue(t_allocationCount == 0u, "steady-state HybridComposer::render performs zero heap allocations");
}

} // namespace

int main() {
    fuse::core::initialize();
    testHybridFrameDoesNotAllocate();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b2_hybrid_alloc_budget: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b2_hybrid_alloc_budget: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
