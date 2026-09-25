// B2.11 gate: "Zero per-frame heap allocations — all frame memory from per-frame LinearAllocator".
// Replaces global operator new/delete in this executable with counters that are armed only on the
// render thread during steady-state frames (after warm-up), then drives RhiContext frames.
#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/rhi_context.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

thread_local bool t_countAllocations = false;
thread_local unsigned long t_allocationCount = 0;

void* countedAlloc(std::size_t size) {
    if (t_countAllocations) {
        ++t_allocationCount;
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

} // namespace

void* operator new(std::size_t size) { return countedAlloc(size); }
void* operator new[](std::size_t size) { return countedAlloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (t_countAllocations) {
        ++t_allocationCount;
    }
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

constexpr fuse::u32 kWarmupFrames = 8;   // ring slots, lazy pipelines, first-use caches
constexpr fuse::u32 kMeasuredFrames = 64;

void testSteadyStateFramesDoNotAllocate() {
    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;
    desc.bootstrap.createSwapchain = false;

    auto context = fuse::renderer::RhiContext::create(desc);
    expectTrue(context != nullptr, "RHI context allocated");
    if (context == nullptr || !context->bootstrap().status().deviceReady) {
        std::printf("SKIP: no Vulkan device — frame allocation budget needs an ICD (Lavapipe in CI)\n");
        return;
    }
    expectTrue(fuse::platform::mayTouchGpuContext(), "render thread available");

    // Representative hybrid frame: clear + sprites through clear3d/sprites2d/composite/present.
    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.f, 0.f, 0.f);
    for (int i = 0; i < 64; ++i) {
        commands.drawSprite2D(static_cast<float>(i), 0.f, 0.f, 255, 255, 255);
    }

    bool allSubmitted = true;
    for (fuse::u32 frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
        const bool measuring = frame >= kWarmupFrames;
        t_countAllocations = measuring;
        const bool began = context->beginFrame(frame);
        const bool submitted = context->submitFrame(commands, frame);
        t_countAllocations = false;
        allSubmitted = allSubmitted && began && submitted;
    }

    std::printf("frame alloc budget: %lu heap allocations over %u steady-state frames (%.2f/frame)\n",
                t_allocationCount, kMeasuredFrames,
                static_cast<double>(t_allocationCount) / static_cast<double>(kMeasuredFrames));
    expectTrue(allSubmitted, "every frame began and submitted");
    // Injected layers (fuse_vulkan_validation_gate sets VK_INSTANCE_LAYERS) allocate through this
    // process-wide operator new on every vkCmd*; that is layer overhead, not engine frame memory.
    const char* layers = std::getenv("VK_INSTANCE_LAYERS");
    if (layers != nullptr && std::strstr(layers, "validation") != nullptr) {
        std::printf("NOTE: validation layer injected — allocation count reported, not enforced\n");
        return;
    }
    expectTrue(t_allocationCount == 0u, "steady-state frames perform zero heap allocations");
}

} // namespace

int main() {
    fuse::core::initialize();
    testSteadyStateFramesDoNotAllocate();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b2_frame_alloc_budget: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b2_frame_alloc_budget: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
