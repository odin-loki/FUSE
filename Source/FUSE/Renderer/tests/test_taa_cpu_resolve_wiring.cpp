// TAA resolve wiring: `TaaResolve::resolve` runs the CPU reference resolver (`TaaCpuResolver`) when
// host-memory surfaces are bound, swaps the shared history buffer, and keeps the CPU history in step
// with the history buffer's validity (warm-up, invalidation). The resolved output must match a
// standalone `TaaCpuResolver` fed the same frames bit for bit.
#include <fuse/core/init.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/taa/taa_cpu_resolve.hpp>
#include <fuse/renderer/taa/taa_history.hpp>
#include <fuse/renderer/taa/taa_pass.hpp>
#include <fuse/renderer/taa/taa_resolve.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

using fuse::f32;
using fuse::u32;
using fuse::math::Vec2;
using fuse::math::Vec3;
using namespace fuse::renderer;

constexpr u32 kWidth = 24;
constexpr u32 kHeight = 16;

struct Context {
    std::unique_ptr<VulkanBootstrap> bootstrap;
    BindlessDescriptors bindless{};
    ResourceManager resources;

    bool init() {
        VulkanBootstrapDesc desc{};
        desc.instance.enableValidation = false;
        desc.createSwapchain = false;
        bootstrap = VulkanBootstrap::create(desc);
        if (bootstrap == nullptr) {
            return false;
        }
        bindless.init(*bootstrap->device());
        return resources.init(*bootstrap->device(), bindless);
    }

    ~Context() {
        if (bootstrap != nullptr && bootstrap->device() != nullptr) {
            resources.destroy();
            bindless.destroy(*bootstrap->device());
        }
    }
};

/// Frame `frame` of a moving vertical edge (bright left, dark right) with sub-pixel jitter.
void makeFrame(u32 frame, std::vector<Vec3>& color, std::vector<Vec2>& velocity, std::vector<f32>& depth) {
    color.assign(static_cast<size_t>(kWidth) * kHeight, Vec3{});
    velocity.assign(color.size(), Vec2{});
    depth.assign(color.size(), 10.f);
    const f32 jitter = (frame % 4u) * 0.25f - 0.375f;
    const f32 edge = 8.f + 0.5f * static_cast<f32>(frame) + jitter;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const u32 i = y * kWidth + x;
            const f32 coverage = std::fmin(1.f, std::fmax(0.f, edge - static_cast<f32>(x)));
            color[i] = Vec3{coverage, 0.5f * coverage + 0.1f, 0.2f};
            velocity[i] = Vec2{0.5f, 0.f};
            depth[i] = coverage > 0.5f ? 5.f : 10.f;
        }
    }
}

bool sameImage(const std::vector<Vec3>& a, const std::vector<Vec3>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Vec3)) == 0;
}

void testCpuResolveMatchesReference(Context& ctx) {
    TaaHistoryBuffer history;
    expectTrue(history.init(ctx.resources, TaaHistoryBufferDesc{kWidth, kHeight}), "history targets allocated");

    TaaResolve resolve;
    TaaCpuResolver reference;
    std::vector<Vec3> color;
    std::vector<Vec2> velocity;
    std::vector<f32> depth;
    std::vector<Vec3> output(static_cast<size_t>(kWidth) * kHeight);
    std::vector<Vec3> expected(output.size());

    TAAParams params{};
    params.blend_factor = 0.1f;
    params.velocity_rejection = 0.9f;
    params.depth_rejection = 0.1f;

    u32 matched = 0;
    constexpr u32 kFrames = 12;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        if (frame == 7u) {
            // Camera cut: the shared history is invalidated; both paths must restart accumulation.
            history.invalidateHistory();
            reference.invalidate();
        }
        makeFrame(frame, color, velocity, depth);

        TaaResolveDesc desc{};
        desc.width = kWidth;
        desc.height = kHeight;
        desc.params = params;
        desc.enforce_rejection_surfaces = true;
        desc.cpu.current_frame = color.data();
        desc.cpu.velocity_buffer = velocity.data();
        desc.cpu.depth_buffer = depth.data();
        desc.cpu.output = output.data();

        const u32 activeBefore = history.activeIndex();
        const bool wasWarm = history.hasValidHistory();
        const bool ok = resolve.resolve(desc, history);
        expectTrue(ok, "CPU-surface resolve succeeds");
        const TaaResolveStats& stats = resolve.lastStats();
        expectTrue(stats.resolved && stats.cpu_resolved, "stats report a CPU resolve");
        expectTrue(stats.history_swapped && history.activeIndex() != activeBefore, "history ping-pong swapped");
        expectTrue(stats.first_frame == !wasWarm, "first_frame follows history warm state");
        expectTrue(stats.cpu_history_used == wasWarm, "CPU history used exactly when shared history is warm");
        expectTrue(resolve.lastMessage().find("CPU reference") != std::string::npos,
                   "message names the CPU reference resolve");

        TaaCpuFrameInputs inputs{};
        inputs.current = color.data();
        inputs.velocity = velocity.data();
        inputs.depth = depth.data();
        inputs.width = kWidth;
        inputs.height = kHeight;
        expectTrue(reference.resolve(inputs, params, expected.data()), "reference resolve succeeds");
        matched += sameImage(output, expected) ? 1u : 0u;

        if (!wasWarm) {
            expectTrue(sameImage(output, color), "warm-up frame outputs the current frame");
        }
    }
    expectTrue(matched == kFrames, "TaaResolve output matches the standalone CPU resolver every frame");

    // Accumulation actually happened: a warm frame differs from its raw input somewhere.
    std::vector<Vec3> raw;
    makeFrame(kFrames - 1u, raw, velocity, depth);
    expectTrue(!sameImage(output, raw), "warm resolve blends history into the output");
    std::printf("taa cpu wiring: %u / %u frames match reference, accumulated=%u\n", matched, kFrames,
                history.accumulatedFrames());
    history.destroy();
}

void testCpuSurfaceSkipReasons(Context& ctx) {
    TaaHistoryBuffer history;
    expectTrue(history.init(ctx.resources, TaaHistoryBufferDesc{kWidth, kHeight}), "history for skip tests");

    std::vector<Vec3> color(static_cast<size_t>(kWidth) * kHeight, Vec3{0.5f, 0.5f, 0.5f});
    std::vector<Vec3> output(color.size());
    std::vector<Vec2> velocity(color.size());

    TaaResolve resolve;
    TaaResolveDesc desc{};
    desc.width = kWidth;
    desc.height = kHeight;
    desc.cpu.current_frame = color.data();
    TaaResolveSkipReason reason = TaaResolveSkipReason::None;
    expectTrue(resolve.wouldSkip(desc, history, &reason) && reason == TaaResolveSkipReason::MissingSurfaces,
               "CPU current without output is MissingSurfaces");

    desc.cpu.output = output.data();
    desc.enforce_rejection_surfaces = true;
    desc.params.velocity_rejection = 0.5f;
    desc.params.depth_rejection = 0.f;
    expectTrue(resolve.wouldSkip(desc, history, &reason) && reason == TaaResolveSkipReason::MissingVelocityBuffer,
               "CPU velocity required when velocity rejection is enforced");
    // An opaque device velocity surface does not satisfy the CPU path.
    desc.surfaces.velocity_buffer = reinterpret_cast<void*>(0x1);
    expectTrue(resolve.wouldSkip(desc, history, &reason) && reason == TaaResolveSkipReason::MissingVelocityBuffer,
               "device velocity handle does not satisfy CPU resolve");
    desc.cpu.velocity_buffer = velocity.data();
    expectTrue(taaResolveRejectionSurfacesSatisfied(desc), "CPU velocity satisfies rejection surfaces");
    expectTrue(resolve.resolve(desc, history), "resolve with CPU velocity succeeds");
    expectTrue(resolve.lastStats().cpu_resolved, "CPU path taken");
    expectTrue(sameImage(output, color), "first CPU resolve copies current frame");

    // Opaque device surfaces only: resolve is recorded, the CPU resolver is not run.
    TaaResolveDesc deviceDesc{};
    deviceDesc.width = kWidth;
    deviceDesc.height = kHeight;
    deviceDesc.surfaces.current_frame = reinterpret_cast<void*>(0x1);
    deviceDesc.surfaces.output = reinterpret_cast<void*>(0x2);
    expectTrue(resolve.resolve(deviceDesc, history), "device-surface resolve still recorded");
    expectTrue(!resolve.lastStats().cpu_resolved, "device-surface resolve does not run the CPU resolver");
    history.destroy();
}

void testTaaPassCpuFrames(Context& ctx) {
    TaaPassDesc passDesc{};
    passDesc.width = kWidth;
    passDesc.height = kHeight;
    auto pass = TaaPass::create(passDesc);
    expectTrue(pass != nullptr && pass->init(ctx.resources), "TaaPass initialises");
    if (pass == nullptr) {
        return;
    }

    std::vector<Vec3> color;
    std::vector<Vec2> velocity;
    std::vector<f32> depth;
    std::vector<Vec3> output(static_cast<size_t>(kWidth) * kHeight);
    for (u32 frame = 0; frame < 4u; ++frame) {
        makeFrame(frame, color, velocity, depth);
        TaaResolveDesc desc{};
        desc.width = kWidth;
        desc.height = kHeight;
        desc.cpu.current_frame = color.data();
        desc.cpu.velocity_buffer = velocity.data();
        desc.cpu.depth_buffer = depth.data();
        desc.cpu.output = output.data();
        expectTrue(pass->resolveFrame(desc), "TaaPass resolves CPU frame");
    }
    expectTrue(pass->lastStats().framesResolved == 4u, "TaaPass counted four resolves");
    expectTrue(pass->resolve().cpuResolver().hasHistory(), "pass CPU resolver holds history");
    pass->invalidateHistory();
    makeFrame(4u, color, velocity, depth);
    TaaResolveDesc desc{};
    desc.width = kWidth;
    desc.height = kHeight;
    desc.cpu.current_frame = color.data();
    desc.cpu.output = output.data();
    expectTrue(pass->resolveFrame(desc), "TaaPass resolves after invalidation");
    expectTrue(sameImage(output, color), "invalidated pass history restarts from the current frame");
    pass->destroy();
}

} // namespace

int main() {
    fuse::core::initialize();
    Context ctx;
    if (!ctx.init()) {
        std::printf("SKIP: resource manager unavailable for TAA history targets\n");
        return 0;
    }
    testCpuResolveMatchesReference(ctx);
    testCpuSurfaceSkipReasons(ctx);
    testTaaPassCpuFrames(ctx);
    if (g_failures == 0) {
        std::printf("fuse_taa_cpu_resolve_wiring: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "fuse_taa_cpu_resolve_wiring: %d failure(s)\n", g_failures);
    return 1;
}
