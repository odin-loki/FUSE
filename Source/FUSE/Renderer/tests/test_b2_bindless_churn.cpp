// B2.11 gate: "Bindless descriptor table registers and unregisters textures — no descriptor heap
// corruption". The Vulkan arrays are sized from the device's update-after-bind descriptor limits
// (clamped to a budget) and the CPU heap caps follow them. Checks the sizing policy on synthetic
// limits, exhausts the heap to the device-derived cap (and the shorter UBO array), holds many live
// textures, then churns create/destroy, checking slot recycling, live counts and that every GPU
// descriptor write stays inside the set (run under fuse_vulkan_validation_gate for the Vulkan side).
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <algorithm>
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

constexpr fuse::u32 kLiveTextures = 1500; // above the old fixed 1024-entry arrays
constexpr fuse::u32 kChurnCycles = 3000;

fuse::renderer::TextureDesc tinyTexture() {
    fuse::renderer::TextureDesc desc{};
    desc.width = 4;
    desc.height = 4;
    desc.format = fuse::renderer::GpuFormat::R8G8B8A8Unorm;
    desc.usage = fuse::renderer::ImageUsage::Sampled;
    return desc;
}

fuse::u32 bindlessBindingOf(const fuse::renderer::BindlessDescriptors& bindless, fuse::u32 index) {
    return bindless.bindingIndexForSlot(fuse::renderer::BindlessHeapKind::Texture, index).binding;
}

/// Sizing policy on synthetic device limits (no GPU needed).
void testArraySizingPolicy() {
    using fuse::renderer::BindlessArraySizes;
    using fuse::renderer::VulkanDescriptorLimits;
    const fuse::u32 reserve = fuse::renderer::kBindlessReservedPerStageDescriptors;

    // Unknown limits (no device): fixed fallback.
    const BindlessArraySizes fallback = fuse::renderer::computeBindlessArraySizes(VulkanDescriptorLimits{});
    expectTrue(fallback.sampledImages == fuse::renderer::kBindlessGpuArrayCapacity &&
                   fallback.storageBuffers == fuse::renderer::kBindlessGpuArrayCapacity,
               "zero limits fall back to the fixed array length");

    // Huge limits (Lavapipe reports 1e6): clamped to the budgets.
    VulkanDescriptorLimits huge{};
    huge.sampledImages = huge.storageImages = huge.storageBuffers = huge.uniformBuffers = huge.samplers = 1000000u;
    huge.perStageResources = 1000000u;
    huge.allPools = UINT32_MAX;
    const BindlessArraySizes big = fuse::renderer::computeBindlessArraySizes(huge);
    expectTrue(big.sampledImages == fuse::renderer::kBindlessSampledImageBudget, "sampled images clamp to budget");
    expectTrue(big.storageImages == fuse::renderer::kBindlessStorageImageBudget, "storage images clamp to budget");
    expectTrue(big.storageBuffers == fuse::renderer::kBindlessStorageBufferBudget, "storage buffers clamp to budget");
    expectTrue(big.uniformBuffers == fuse::renderer::kBindlessUniformBufferBudget, "UBOs clamp to budget");
    expectTrue(big.samplers == fuse::renderer::kMaxSamplers, "samplers clamp to the CPU heap");

    // Tight discrete-GPU-like limits: small UBO array, per-stage total forces proportional scaling.
    VulkanDescriptorLimits tight{};
    tight.sampledImages = 500000u;
    tight.storageImages = 8192u;
    tight.storageBuffers = 500000u;
    tight.uniformBuffers = 90u;
    tight.samplers = 2048u;
    tight.perStageResources = 60000u;
    tight.allPools = 1000000u;
    const BindlessArraySizes small = fuse::renderer::computeBindlessArraySizes(tight);
    const fuse::u64 perStageTotal = static_cast<fuse::u64>(small.sampledImages) + small.storageImages +
                                    small.storageBuffers + small.uniformBuffers;
    std::printf("bindless sizing (tight limits): sampled %u storage-img %u ssbo %u ubo %u samplers %u "
                "(per-stage total %llu of %u)\n",
                small.sampledImages, small.storageImages, small.storageBuffers, small.uniformBuffers, small.samplers,
                static_cast<unsigned long long>(perStageTotal), tight.perStageResources);
    expectTrue(small.uniformBuffers <= tight.uniformBuffers - reserve, "UBO array leaves the per-stage reserve");
    expectTrue(small.storageImages <= tight.storageImages - reserve, "storage images leave the per-stage reserve");
    expectTrue(perStageTotal <= tight.perStageResources, "arrays fit maxPerStageUpdateAfterBindResources");
    expectTrue(small.storageImages <= small.sampledImages && small.uniformBuffers <= small.storageBuffers,
               "flagged arrays never longer than the slot index space");
    expectTrue(small.samplers <= fuse::renderer::kMaxSamplers && small.samplers >= 1u, "sampler array in range");
    expectTrue(small.sampledImages > 1024u, "tight limits still beat the old fixed 1024");
}

/// Exhaust the device-derived caps with CPU slots: nothing may be handed out past an array.
void testHeapCapsFollowDeviceLimits(fuse::renderer::VulkanBootstrap& bootstrap) {
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap.device());
    if (!bindless.vulkanDescriptorsReady()) {
        std::printf("SKIP: Vulkan bindless set unavailable\n");
        bindless.destroy(*bootstrap.device());
        return;
    }
    const fuse::renderer::BindlessArraySizes& sizes = bindless.arraySizes();
    const fuse::renderer::BindlessArraySizes expected =
        fuse::renderer::computeBindlessArraySizes(bootstrap.device()->info().descriptorLimits);
    const fuse::renderer::VulkanDescriptorLimits& limits = bootstrap.device()->info().descriptorLimits;
    std::printf("bindless: device limits sampled %u storage-img %u ssbo %u ubo %u samplers %u per-stage %u\n",
                limits.sampledImages, limits.storageImages, limits.storageBuffers, limits.uniformBuffers,
                limits.samplers, limits.perStageResources);
    std::printf("bindless: arrays sampled %u storage-img %u ssbo %u ubo %u samplers %u\n", sizes.sampledImages,
                sizes.storageImages, sizes.storageBuffers, sizes.uniformBuffers, sizes.samplers);
    expectTrue(sizes.sampledImages == expected.sampledImages && sizes.storageBuffers == expected.storageBuffers &&
                   sizes.uniformBuffers == expected.uniformBuffers && sizes.storageImages == expected.storageImages,
               "Vulkan set uses the device-derived array lengths");
    expectTrue(limits.sampledImages == 0u || sizes.sampledImages <= limits.sampledImages, "within device limit");
    expectTrue(bindless.heapMaxCapacity(fuse::renderer::BindlessHeapKind::Texture) == sizes.sampledImages,
               "CPU texture heap cap follows the sampled-image array");
    expectTrue(bindless.heapMaxCapacity(fuse::renderer::BindlessHeapKind::Buffer) == sizes.storageBuffers,
               "CPU buffer heap cap follows the storage-buffer array");

    // Textures: fill to the cap, one more is rejected.
    fuse::u32 textureSlots = 0;
    fuse::u32 maxTextureIndex = 0;
    for (;;) {
        const fuse::renderer::BindlessSlotHandle h = bindless.allocateTextureSlot(false);
        if (!h.isValid()) {
            break;
        }
        ++textureSlots;
        maxTextureIndex = std::max(maxTextureIndex, h.index);
    }
    std::printf("bindless: %u texture slots before rejection (max index %u)\n", textureSlots, maxTextureIndex);
    expectTrue(textureSlots == sizes.sampledImages, "texture heap fills exactly the sampled-image array");
    expectTrue(maxTextureIndex < sizes.sampledImages, "no texture slot beyond the array");
    expectTrue(!bindless.resizeHeap(fuse::renderer::BindlessHeapKind::Texture, fuse::renderer::kMaxTextures) ||
                   bindless.heapCapacity(fuse::renderer::BindlessHeapKind::Texture) <= sizes.sampledImages,
               "resizeHeap cannot grow the CPU heap past the GPU array");

    // Buffers: UBO slots must stay inside the shorter UBO array even when the free list only holds
    // indices past it.
    const fuse::u32 ubo = sizes.uniformBuffers;
    std::vector<fuse::renderer::BindlessSlotHandle> bufferSlots;
    for (fuse::u32 i = 0; i < ubo + 10u && i < sizes.storageBuffers; ++i) {
        bufferSlots.push_back(bindless.allocateBufferSlot(false));
    }
    for (fuse::u32 i = ubo; i < bufferSlots.size(); ++i) {
        bindless.freeBufferSlot(bufferSlots[i]);
    }
    const bool uboPastArray = bindless.allocateBufferSlot(true).isValid();
    expectTrue(!uboPastArray || sizes.storageBuffers <= ubo, "UBO slot never taken from indices past the UBO array");
    bindless.freeBufferSlot(bufferSlots[ubo / 2u]);
    const fuse::renderer::BindlessSlotHandle uboSlot = bindless.allocateBufferSlot(true);
    expectTrue(uboSlot.isValid() && uboSlot.index == ubo / 2u, "UBO slot recycles an index inside the UBO array");
    const fuse::renderer::BindlessSlotHandle ssboSlot = bindless.allocateBufferSlot(false);
    expectTrue(ssboSlot.isValid() && ssboSlot.index >= ubo, "storage-buffer slot may use indices past the UBO array");

    bindless.destroy(*bootstrap.device());
}

/// createTexture routes by usage: sampled -> sampled-image array, storage-only -> storage-image
/// array, neither -> CPU slot without a GPU descriptor write (was a Lavapipe crash / VUID).
void testTextureUsageRouting(fuse::renderer::ResourceManager& resources, fuse::renderer::BindlessDescriptors& bindless) {
    using fuse::renderer::ImageUsage;
    auto make = [&](ImageUsage usage) {
        fuse::renderer::TextureDesc desc = tinyTexture();
        desc.usage = usage;
        return resources.createTexture(desc);
    };
    const fuse::u32 writesBefore = bindless.descriptorUpdateCount();
    const fuse::renderer::TextureHandle sampled = make(ImageUsage::Sampled);
    const fuse::renderer::TextureHandle storage = make(ImageUsage::Storage);
    const fuse::renderer::TextureHandle both =
        make(static_cast<ImageUsage>(static_cast<fuse::u32>(ImageUsage::Sampled) |
                                     static_cast<fuse::u32>(ImageUsage::Storage)));
    const fuse::u32 writesBeforeAttachment = bindless.descriptorUpdateCount();
    const fuse::renderer::TextureHandle attachment = make(ImageUsage::ColorAttachment);
    expectTrue(sampled.isValid() && storage.isValid() && both.isValid() && attachment.isValid(),
               "sampled / storage-only / both / attachment-only textures created");

    const fuse::renderer::Texture* s = resources.getTexture(sampled);
    const fuse::renderer::Texture* st = resources.getTexture(storage);
    const fuse::renderer::Texture* b = resources.getTexture(both);
    if (s != nullptr && st != nullptr && b != nullptr) {
        expectTrue(!bindless.slotIsStorageTexture(s->bindlessIndex), "sampled texture uses the sampled-image array");
        expectTrue(bindless.slotIsStorageTexture(st->bindlessIndex), "storage-only texture uses the storage-image array");
        expectTrue(st->bindlessIndex < bindless.gpuStorageTextureCapacity(), "storage slot inside the storage array");
        expectTrue(!bindless.slotIsStorageTexture(b->bindlessIndex), "sampled+storage texture binds as sampled");
        expectTrue(bindlessBindingOf(bindless, st->bindlessIndex) == fuse::renderer::kBindlessBindingStorageImages,
                   "storage-only slot maps to the storage-image binding");
    }
    expectTrue(bindless.descriptorUpdateCount() == writesBeforeAttachment,
               "attachment-only texture gets a CPU slot but no GPU descriptor write");
    expectTrue(bindless.descriptorUpdateCount() >= writesBefore + 3u, "sampled and storage descriptors written");
    for (const fuse::renderer::TextureHandle h : {sampled, storage, both, attachment}) {
        resources.destroyTexture(h);
    }
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
    std::printf("bindless: GPU sampled-image capacity %u (device-derived)\n", gpuCapacity);
    expectTrue(gpuCapacity > 1024u, "Lavapipe limits size the array past the old fixed 1024");
    testHeapCapsFollowDeviceLimits(*bootstrap);
    testTextureUsageRouting(resources, bindless);

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
    expectTrue(rejected == (kLiveTextures > gpuCapacity ? kLiveTextures - gpuCapacity : 0u),
               "requests rejected only past the device-derived capacity");
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
    testArraySizingPolicy();
    testBindlessChurn();

    if (g_failures == 0) {
        std::printf("fuse_b2_bindless_churn: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b2_bindless_churn: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
