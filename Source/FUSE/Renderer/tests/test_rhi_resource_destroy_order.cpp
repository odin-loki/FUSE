#include <fuse/core/init.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/gpu_alloc_stats.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using fuse::u32;
using fuse::u8;

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEq(fuse::usize actual, fuse::usize expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (got %zu, expected %zu)\n", message, actual, expected);
        ++g_failures;
    }
}

struct TestContext {
    std::unique_ptr<fuse::renderer::VulkanBootstrap> bootstrap;
    fuse::renderer::BindlessDescriptors bindless;
    fuse::renderer::ResourceManager resources;
    bool active = false;
};

bool initTestContext(TestContext& ctx) {
    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;

    ctx.bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    if (ctx.bootstrap == nullptr) {
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (!ctx.bootstrap->status().deviceReady) {
        return false;
    }
#endif

    ctx.bindless.init(*ctx.bootstrap->device());

    fuse::renderer::ResourceManager::Desc resourceDesc{};
    resourceDesc.stagingRingBytes = 4096u;
    if (!ctx.resources.init(*ctx.bootstrap->device(), ctx.bindless, resourceDesc)) {
        return false;
    }

    ctx.active = true;
    return true;
}

void shutdownTestContext(TestContext& ctx) {
    if (!ctx.active) {
        return;
    }
    ctx.resources.destroy();
    ctx.bindless.destroy(*ctx.bootstrap->device());
    ctx.bootstrap.reset();
    ctx.active = false;
}

void testExplicitDestroyOrderReleasesBindlessSlots() {
    TestContext ctx;
    if (!initTestContext(ctx)) {
        std::printf("SKIP: resource destroy-order test — bootstrap/device unavailable\n");
        return;
    }

    const fuse::renderer::SamplerHandle sampler = ctx.resources.createSampler({});
    const fuse::renderer::TextureHandle texture = ctx.resources.createTexture({});
    fuse::renderer::BufferDesc bufferDesc{};
    bufferDesc.size = 128;
    bufferDesc.usage = fuse::renderer::BufferUsage::Storage;
    const fuse::renderer::BufferHandle buffer = ctx.resources.createBuffer(bufferDesc);

    expectTrue(sampler.isValid() && texture.isValid() && buffer.isValid(),
               "resources created for destroy-order test");
    expectEq(ctx.bindless.registeredSamplerCount(), 1u, "sampler bindless slot live");
    expectEq(ctx.bindless.registeredTextureCount(), 1u, "texture bindless slot live");
    expectEq(ctx.bindless.registeredBufferCount(), 2u, "buffer bindless slots include staging ring");

    ctx.resources.destroySampler(sampler);
    expectEq(ctx.bindless.registeredSamplerCount(), 0u, "sampler slot released first");

    ctx.resources.destroyTexture(texture);
    expectEq(ctx.bindless.registeredTextureCount(), 0u, "texture slot released before buffers");

    ctx.resources.destroyBuffer(buffer);
    expectEq(ctx.bindless.registeredBufferCount(), 1u, "user buffer released; staging ring remains");

    shutdownTestContext(ctx);
}

void testResourceManagerDestroyAllClearsLiveCounts() {
    TestContext ctx;
    if (!initTestContext(ctx)) {
        std::printf("SKIP: destroy-all test — bootstrap/device unavailable\n");
        return;
    }

    (void)ctx.resources.createSampler({});
    (void)ctx.resources.createTexture({});
    fuse::renderer::BufferDesc bufferDesc{};
    bufferDesc.size = 64;
    bufferDesc.usage = fuse::renderer::BufferUsage::Uniform;
    (void)ctx.resources.createBuffer(bufferDesc);

    const auto live = ctx.resources.liveCounts();
    expectTrue(live.samplers == 1u && live.textures == 1u && live.buffers == 1u,
               "live counts reflect user resources");

    ctx.resources.destroy();

    expectEq(ctx.bindless.registeredTextureCount(), 0u, "destroy-all clears texture bindless slots");
    expectEq(ctx.bindless.registeredBufferCount(), 0u, "destroy-all clears buffer bindless slots");
    expectEq(ctx.bindless.registeredSamplerCount(), 0u, "destroy-all clears sampler bindless slots");

    ctx.bindless.destroy(*ctx.bootstrap->device());
    ctx.bootstrap.reset();
    ctx.active = false;
}

void testStagingRingProtectedFromExplicitDestroy() {
    TestContext ctx;
    if (!initTestContext(ctx)) {
        std::printf("SKIP: staging ring test — bootstrap/device unavailable\n");
        return;
    }

    const fuse::usize ringCapacity = ctx.resources.stagingRingCapacity();
    expectTrue(ringCapacity > 0, "staging ring allocated on init");

    fuse::renderer::BufferDesc bufferDesc{};
    bufferDesc.size = 32;
    bufferDesc.usage = fuse::renderer::BufferUsage::Storage;
    const fuse::renderer::BufferHandle userBuffer = ctx.resources.createBuffer(bufferDesc);
    expectTrue(userBuffer.isValid(), "user buffer created");

    const auto before = ctx.resources.liveCounts();
    ctx.resources.destroyBuffer(userBuffer);
    const auto afterDestroyUser = ctx.resources.liveCounts();
    expectEq(afterDestroyUser.buffers, before.buffers - 1u, "user buffer destroy decrements count");

    shutdownTestContext(ctx);
}

void testGpuAllocatorStatsTracking() {
    TestContext ctx;
    if (!initTestContext(ctx)) {
        std::printf("SKIP: GPU stats test — bootstrap/device unavailable\n");
        return;
    }

    const fuse::renderer::GpuAllocStats* statsBefore = ctx.resources.allocatorStats();
    expectTrue(statsBefore != nullptr, "allocator stats available");
    const fuse::usize usedBefore = statsBefore->usedBytes;

    fuse::renderer::BufferDesc bufferDesc{};
    bufferDesc.size = 512;
    bufferDesc.usage = fuse::renderer::BufferUsage::Storage;
    const fuse::renderer::BufferHandle buffer = ctx.resources.createBuffer(bufferDesc);
    expectTrue(buffer.isValid(), "buffer created for stats tracking");

    fuse::renderer::TextureDesc textureDesc{};
    textureDesc.width = 8;
    textureDesc.height = 8;
    const fuse::renderer::TextureHandle texture = ctx.resources.createTexture(textureDesc);
    expectTrue(texture.isValid(), "texture created for stats tracking");

    const fuse::renderer::GpuAllocStats* statsLive = ctx.resources.allocatorStats();
    expectTrue(statsLive->usedBytes > usedBefore, "used bytes increase after allocations");
    expectTrue(statsLive->bufferCount >= 2u, "buffer count includes staging ring + user buffer");
    expectTrue(statsLive->imageCount >= 1u, "image count tracks texture allocation");
    expectTrue(statsLive->peakUsedBytes >= statsLive->usedBytes, "peak tracks high water mark");

    ctx.resources.destroyTexture(texture);
    ctx.resources.destroyBuffer(buffer);

    const fuse::renderer::GpuAllocStats* statsAfter = ctx.resources.allocatorStats();
    expectEq(statsAfter->imageCount, 0u, "image count zero after texture destroy");
    expectTrue(statsAfter->freeCount >= 2u, "free count increments on destroy");

    shutdownTestContext(ctx);
}

void testGlobalGpuStatsHook() {
    fuse::renderer::clearGlobalGpuStatsHook();

    struct HookState {
        std::string lastName;
        fuse::renderer::GpuAllocStats lastStats{};
        u32 callCount = 0;
    } state;

    fuse::renderer::setGlobalGpuStatsHook(
        [](const char* allocatorName, const fuse::renderer::GpuAllocStats& stats, void* userData) {
            auto* hookState = static_cast<HookState*>(userData);
            hookState->lastName = allocatorName != nullptr ? allocatorName : "";
            hookState->lastStats = stats;
            hookState->callCount += 1;
        },
        &state);

    TestContext ctx;
    if (!initTestContext(ctx)) {
        fuse::renderer::clearGlobalGpuStatsHook();
        std::printf("SKIP: GPU stats hook test — bootstrap/device unavailable\n");
        return;
    }

    fuse::renderer::BufferDesc bufferDesc{};
    bufferDesc.size = 16;
    bufferDesc.usage = fuse::renderer::BufferUsage::Uniform;
    const fuse::renderer::BufferHandle buffer = ctx.resources.createBuffer(bufferDesc);
    expectTrue(buffer.isValid(), "buffer created for stats hook");
    expectTrue(state.callCount >= 1u, "global GPU stats hook fires on allocation");
    expectTrue(state.lastName == "fuse_rhi_gpu", "hook receives allocator name");
    expectTrue(state.lastStats.allocCount >= 1u, "hook receives alloc count");

    ctx.resources.destroyBuffer(buffer);
    shutdownTestContext(ctx);
    fuse::renderer::clearGlobalGpuStatsHook();
}

} // namespace

int main() {
    fuse::core::initialize();

    testExplicitDestroyOrderReleasesBindlessSlots();
    testResourceManagerDestroyAllClearsLiveCounts();
    testStagingRingProtectedFromExplicitDestroy();
    testGpuAllocatorStatsTracking();
    testGlobalGpuStatsHook();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_rhi_resource_destroy_order: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_rhi_resource_destroy_order: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
