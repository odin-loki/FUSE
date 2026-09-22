// B2.11 gate: "Bindless descriptor table registers and unregisters textures — no descriptor heap
// corruption". Holds more live textures than a small heap would fit, then churns create/destroy,
// checking slot recycling, live counts and that every GPU descriptor write stays inside the set
// (run under fuse_vulkan_validation_gate for the Vulkan-side check).
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr fuse::u32 kLiveTextures = 1500;
constexpr fuse::u32 kChurnCycles = 3000;

fuse::renderer::TextureDesc tinyTexture() {
    fuse::renderer::TextureDesc desc{};
    desc.width = 4;
    desc.height = 4;
    desc.format = fuse::renderer::GpuFormat::R8G8B8A8Unorm;
    desc.usage = fuse::renderer::ImageUsage::Sampled;
    return desc;
}

void testBindlessChurn() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    if (bootstrap == nullptr || !bootstrap->status().deviceReady) {
        std::printf("SKIP: no Vulkan device — bindless churn needs an ICD (Lavapipe in CI)\n");
        return;
    }

    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());
    fuse::renderer::ResourceManager resources;
    fuse::renderer::ResourceManager::Desc desc{};
    desc.stagingRingBytes = 1024u * 1024u;
    expectTrue(resources.init(*bootstrap->device(), bindless, desc), "resource manager ready");

    const fuse::u32 baselineTextures = bindless.registeredTextureCount();
    const fuse::u32 gpuCapacity = bindless.gpuTextureCapacity();
    std::printf("bindless: GPU sampled-image capacity %u\n", gpuCapacity);

    // Phase 1: many live textures at once. Every accepted slot must fit the GPU descriptor array.
    std::vector<fuse::renderer::TextureHandle> live;
    fuse::u32 rejected = 0;
    fuse::u32 maxIndex = 0;
    for (fuse::u32 i = 0; i < kLiveTextures; ++i) {
        const fuse::renderer::TextureHandle handle = resources.createTexture(tinyTexture());
        if (!handle.isValid()) {
            ++rejected;
            continue;
        }
        live.push_back(handle);
        const fuse::renderer::Texture* texture = resources.getTexture(handle);
        if (texture != nullptr && texture->bindlessIndex > maxIndex) {
            maxIndex = texture->bindlessIndex;
        }
    }
    std::printf("bindless: %zu live, %u rejected, max index %u\n", live.size(), rejected, maxIndex);
    expectTrue(maxIndex < gpuCapacity, "no registered slot index beyond the GPU descriptor array");
    expectTrue(static_cast<fuse::u32>(live.size()) + rejected == kLiveTextures, "every request accounted for");
    expectTrue(bindless.registeredTextureCount() == baselineTextures + live.size(),
               "live count matches registrations");

    for (const fuse::renderer::TextureHandle handle : live) {
        resources.destroyTexture(handle);
    }
    expectTrue(bindless.registeredTextureCount() == baselineTextures, "all slots released");

    // Phase 2: churn — create/destroy with a small live window; indices must be recycled.
    std::vector<fuse::renderer::TextureHandle> window;
    fuse::u32 churnMaxIndex = 0;
    for (fuse::u32 cycle = 0; cycle < kChurnCycles; ++cycle) {
        const fuse::renderer::TextureHandle handle = resources.createTexture(tinyTexture());
        expectTrue(handle.isValid(), "churn create succeeds");
        if (!handle.isValid()) {
            break;
        }
        const fuse::renderer::Texture* texture = resources.getTexture(handle);
        if (texture != nullptr && texture->bindlessIndex > churnMaxIndex) {
            churnMaxIndex = texture->bindlessIndex;
        }
        window.push_back(handle);
        if (window.size() > 8u) {
            resources.destroyTexture(window.front());
            window.erase(window.begin());
        }
    }
    for (const fuse::renderer::TextureHandle handle : window) {
        resources.destroyTexture(handle);
    }
    std::printf("bindless churn: %u cycles, max index %u\n", kChurnCycles, churnMaxIndex);
    // The free list is LIFO, so after phase 1 churn reuses high indices; what matters is that
    // every slot is recycled inside the GPU array and the heap never grows.
    expectTrue(churnMaxIndex < gpuCapacity, "churn recycles slots inside the GPU descriptor array");
    expectTrue(bindless.registeredTextureCount() == baselineTextures, "churn leaves no leaked slots");

    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    testBindlessChurn();

    if (g_failures == 0) {
        std::printf("fuse_b2_bindless_churn: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b2_bindless_churn: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
