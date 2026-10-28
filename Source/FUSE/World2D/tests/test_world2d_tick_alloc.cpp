// Steady-state World2D::tick heap gate. After warmup (no spawn/despawn) a physics-enabled world with
// a parallel cull and an animated image-map sprite must perform zero operator-new calls. World3D::tick
// is already gated by testDeterminismAndAllocations in fuse_runtime_schedule_gates.
//
// Warmup is shorter than one atlas cycle on purpose: every sheet column has to be cached on first
// sight, not the first time that column is displayed.

#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/sprite_image_map.hpp>
#include <fuse/world2d/tick_us.hpp>
#include <fuse/world2d/world_2d.hpp>

#include "../../Core/tests/fuse_test_msvc_debug_stl.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<unsigned long long> g_allocations{0};
std::atomic<bool> g_counting{false};

void* countedAlloc(std::size_t size) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

} // namespace

void* operator new(std::size_t size) { return countedAlloc(size); }
void* operator new[](std::size_t size) { return countedAlloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return countedAlloc(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return countedAlloc(size);
    } catch (...) {
        return nullptr;
    }
}
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

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f expected %f)\n", message, static_cast<double>(value),
                     static_cast<double>(expected));
        ++g_failures;
    }
}

constexpr int kSprites = 24;
constexpr int kWarmup = 32;
constexpr int kMeasured = 64;
constexpr float kDt = 1.f / 60.f;

const fuse::world2d::SpriteDrawCmd* findCmd(const fuse::world2d::World2D& world, fuse::Handle<fuse::Object> handle) {
    for (const fuse::world2d::SpriteDrawCmd& cmd : world.readSnapshot().sprites()) {
        if (cmd.object == handle) {
            return &cmd;
        }
    }
    return nullptr;
}

void testSteadyStateWorld2DTickDoesNotAllocate() {
    fuse::world2d::World2D world;
    world.setPhysicsEnabled(true);

    fuse::world2d::SpriteImageMapSearchRoots roots;
    roots.projectRoot = "project-root";
    roots.repositoryRoot = "repo-root";
    world.setSpriteImageMapSearchRoots(roots);

    std::vector<std::unique_ptr<fuse::SceneObject2D>> sprites;
    sprites.reserve(static_cast<std::size_t>(kSprites + 1));
    fuse::SceneObject2D* hero = nullptr;
    for (int i = 0; i < kSprites; ++i) {
        sprites.push_back(std::make_unique<fuse::SceneObject2D>("sprite" + std::to_string(i)));
        fuse::SceneObject2D& sprite = *sprites.back();
        sprite.setPhysicsEnabled(true);
        sprite.setPhysicsShape(fuse::PhysicsShape2D::Circle);
        sprite.setPhysicsRadius(0.5f);
        sprite.setCollisionLayer(1);
        sprite.setPosition(static_cast<float>(i % 6) * 0.8f, static_cast<float>(i / 6) * 0.8f);
        if (i == 0) {
            hero = &sprite;
            sprite.setSpriteAnimation(8u, 12.f);
            sprite.setSpriteImageMap("HeroSheet.png");
        }
        world.addSprite(&sprite);
    }
    sprites.push_back(std::make_unique<fuse::SceneObject2D>("child"));
    hero->addChild(sprites.back().get());
    sprites.back()->setPosition(1.f, 0.f);

    const fuse::Handle<fuse::Object> heroHandle = hero->handle();
    const float heroStartY = hero->y();

    fuse::frame::FrameCtx ctx{};
    ctx.dt = kDt;
    g_allocations.store(0);
    for (int frame = 0; frame < kWarmup + kMeasured; ++frame) {
        g_counting.store(frame >= kWarmup);
        ctx.frameIndex = static_cast<fuse::u32>(frame + 1);
        ctx.time = static_cast<float>(frame) * kDt;
        world.tick(ctx);
        g_counting.store(false);
    }

    const unsigned long long steadyAllocations = g_allocations.load();
    std::printf("[gate] World2D::tick steady-state allocations over %d frames: %llu (workers %u)\n", kMeasured,
                steadyAllocations, fuse::jobs::JobScheduler::instance().workerCount());

    const fuse::world2d::SpriteDrawCmd* heroCmd = findCmd(world, heroHandle);
    expectTrue(heroCmd != nullptr, "animated sprite stayed in the snapshot");
    expectTrue(world.spriteCount() == static_cast<fuse::u32>(kSprites), "no sprite spawn or despawn during the run");
    expectTrue(world.readSnapshot().sprites().size() == static_cast<std::size_t>(kSprites + 1),
               "snapshot kept the nested child");
    expectTrue(world.readSnapshot().visibleCount() == static_cast<fuse::u32>(kSprites + 1),
               "parallel cull kept every snapshot entry");
    expectTrue(world.physics().bodyCount() == static_cast<fuse::u32>(kSprites), "physics bodies stayed attached");
    expectTrue(heroCmd != nullptr && heroCmd->y < heroStartY - 1.f, "physics integration moved the hero down");
    expectTrue(heroCmd != nullptr && heroCmd->atlasFrameCount == 8u, "animated sprite exported its frame count");
    expectTrue(heroCmd != nullptr && heroCmd->atlasColorFromImageMap, "image-map tint came from the sheet sample");
    if (heroCmd != nullptr) {
        expectNear(heroCmd->rotation, ctx.time * 1.5f, 1e-4f, "per-frame sprite rotation still applied");
        const fuse::u32 frame = hero->spriteAnimationFrameIndex(ctx.time);
        expectTrue(heroCmd->atlasFrame == frame, "atlas frame followed the animation clock");
        fuse::u8 r = 0;
        fuse::u8 g = 0;
        fuse::u8 b = 0;
        expectTrue(fuse::world2d::sampleSpriteImageMapFrame(roots, "HeroSheet.png", frame, 8u, r, g, b),
                   "direct sheet sample succeeded");
        expectTrue(heroCmd->atlasR == r && heroCmd->atlasG == g && heroCmd->atlasB == b,
                   "cached image-map tint matches a fresh sample");
    }

    if (fuse_test_msvc_debug_stl()) {
        std::printf("[gate] MSVC debug STL — steady-state allocation check not enforced\n");
    } else {
        expectTrue(steadyAllocations == 0u, "zero heap allocations per steady-state World2D tick");
    }
}

/// Predicate only. No heap traffic; must run before measured World2D frames.
void testTickUsOk() {
    expectTrue(fuse::world2d::tick_us_ok(0), "tick_us_ok(0) is true");
    expectTrue(fuse::world2d::tick_us_ok(16667), "tick_us_ok(16667) is true");
    expectTrue(fuse::world2d::tick_us_ok(50000), "tick_us_ok(50000) is true");
    expectTrue(!fuse::world2d::tick_us_ok(-1), "tick_us_ok(-1) is false");
    expectTrue(!fuse::world2d::tick_us_ok(50001), "tick_us_ok(50001) is false");
}

} // namespace

int main() {
    testTickUsOk();
    fuse::core::initialize();
    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::jobs::JobScheduler::instance().initialize(3);
    testSteadyStateWorld2DTickDoesNotAllocate();
    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_world2d_tick_alloc_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_world2d_tick_alloc_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
