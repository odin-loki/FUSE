#include <fuse/core/init.hpp>
#include <fuse/handle_map.hpp>
#include <fuse/renderer/cuda/interop.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/debug_utils.hpp>
#include <fuse/types.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

void testHandleMapGeneration() {
    fuse::HandleMap<u32> map;
    fuse::Handle<u32> first = map.insert(42u);
    expectTrue(map.valid(first), "inserted handle valid");
    expectTrue(*map.get(first) == 42u, "inserted value readable");

    map.remove(first);
    expectTrue(!map.valid(first), "removed handle stale");

    fuse::Handle<u32> second = map.insert(7u);
    expectTrue(second.index() == first.index(), "slot reused");
    expectTrue(second.generation() != first.generation(), "generation bumped on reuse");
}

void testBindlessIndexRecycle() {
    fuse::renderer::BindlessDescriptors bindless;
    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated");

    bindless.init(*bootstrap->device());
    const fuse::renderer::Texture texture{};
    const u32 a = bindless.registerTexture(texture);
    const u32 b = bindless.registerTexture(texture);
    expectTrue(a == 0u && b == 1u, "bindless indices allocate sequentially");
    bindless.unregisterTexture(a);
    const u32 c = bindless.registerTexture(texture);
    expectTrue(c == a, "bindless index recycled");
    bindless.destroy(*bootstrap->device());
}

void testResourceManagerBuffersAndTextures() {
    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for resources");

    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    fuse::renderer::ResourceManager::Desc resourceDesc{};
    resourceDesc.stagingRingBytes = 1024u * 1024u;

    const bool ready = resources.init(*bootstrap->device(), bindless, resourceDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        expectTrue(ready, "resource manager initializes with device");
    } else {
        expectTrue(!ready, "resource manager skips without device");
        bindless.destroy(*bootstrap->device());
        return;
    }
#else
    expectTrue(ready, "compile-time stub still exercises resource scaffolding");
#endif

    fuse::renderer::BufferDesc bufferDesc{};
    bufferDesc.size = 256;
    bufferDesc.usage = fuse::renderer::BufferUsage::Storage;
    bufferDesc.memoryUsage = fuse::renderer::MemoryUsage::CpuToGpu;

    const u8 payload[4] = {1, 2, 3, 4};
    const fuse::renderer::BufferHandle buffer =
        resources.createBuffer(bufferDesc, payload);
    fuse::renderer::BufferDesc indirectDesc{};
    indirectDesc.size = 64;
    indirectDesc.usage = fuse::renderer::BufferUsage::Indirect;
    indirectDesc.memoryUsage = fuse::renderer::MemoryUsage::GpuOnly;
    const fuse::renderer::BufferHandle indirectBuffer = resources.createBuffer(indirectDesc);
    expectTrue(indirectBuffer.isValid(), "indirect buffer handle issued");
    expectTrue(buffer.isValid(), "buffer handle issued");
    const fuse::renderer::Buffer* createdBuffer = resources.getBuffer(buffer);
    expectTrue(createdBuffer != nullptr, "buffer resolvable");
#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->device() != nullptr && bootstrap->device()->isValid() &&
        bootstrap->device()->info().bufferDeviceAddress) {
        expectTrue(createdBuffer->deviceAddress != 0,
                   "Storage CpuToGpu buffer has deviceAddress when bufferDeviceAddress is enabled");
    }
#endif
    {
        u8 cpuReadback[4] = {};
        expectTrue(resources.readBuffer(buffer, cpuReadback, sizeof(cpuReadback)),
                   "CpuToGpu readBuffer succeeds");
        expectTrue(cpuReadback[0] == 1 && cpuReadback[1] == 2 && cpuReadback[2] == 3 &&
                       cpuReadback[3] == 4,
                   "CpuToGpu readBuffer matches initialData bytes");
    }

    fuse::renderer::BufferDesc gpuOnlyDesc{};
    gpuOnlyDesc.size = 64;
    gpuOnlyDesc.usage = static_cast<fuse::renderer::BufferUsage>(
        static_cast<u32>(fuse::renderer::BufferUsage::Storage) |
        static_cast<u32>(fuse::renderer::BufferUsage::TransferSrc));
    gpuOnlyDesc.memoryUsage = fuse::renderer::MemoryUsage::GpuOnly;
    u8 gpuPayload[64];
    for (u8 i = 0; i < 64; ++i) {
        gpuPayload[i] = i;
    }
    const fuse::usize stagingBefore = resources.stagingRingOffset();
    const fuse::renderer::BufferHandle gpuOnly =
        resources.createBuffer(gpuOnlyDesc, gpuPayload);
    expectTrue(gpuOnly.isValid(), "GpuOnly + initialData issues a handle");
    const fuse::renderer::Buffer* gpuBuf = resources.getBuffer(gpuOnly);
    expectTrue(gpuBuf != nullptr, "GpuOnly buffer resolvable");
    if (gpuBuf != nullptr && gpuBuf->mapped == nullptr) {
        expectTrue(resources.stagingRingOffset() >= stagingBefore + gpuOnlyDesc.size,
                   "unmapped initialData consumes staging ring");
    }
    (void)resources.lastGpuCopyUsedTransferQueue();
    (void)resources.lastGpuCopyUsedFence();
    (void)resources.lastGpuCopyWaitTimedOut();
#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady && bootstrap->device() != nullptr &&
        gpuBuf != nullptr && gpuBuf->mapped == nullptr) {
        const auto& queues = bootstrap->device()->queues();
        if (queues.transfer != nullptr && queues.transfer != queues.graphics) {
            expectTrue(resources.lastGpuCopyUsedTransferQueue(),
                       "GpuOnly initialData copy used dedicated transfer queue");
        }
        if (resources.lastGpuCopyUsedTransferQueue() || resources.lastGpuCopyUsedFence()) {
            expectTrue(resources.lastGpuCopyUsedFence(),
                       "GpuOnly initialData copy waited on a transient fence");
            expectTrue(!resources.lastGpuCopyWaitTimedOut(),
                       "GpuOnly initialData copy fence wait did not time out");
        }
    }
#else
    expectTrue(!resources.lastGpuCopyUsedFence(), "stub GpuOnly copy does not wait on a GPU fence");
    expectTrue(!resources.lastGpuCopyWaitTimedOut(), "stub GpuOnly copy does not time out a GPU fence");
#endif
    {
        u8 gpuReadback[64] = {};
        if (resources.readBuffer(gpuOnly, gpuReadback, sizeof(gpuReadback))) {
            bool match = true;
            for (u8 i = 0; i < 64; ++i) {
                if (gpuReadback[i] != gpuPayload[i]) {
                    match = false;
                    break;
                }
            }
            expectTrue(match, "GpuOnly readBuffer matches initialData bytes");
        }
    }

    const fuse::usize stagingMid = resources.stagingRingOffset();
    const u32 wrapCountBefore = resources.stagingRingWrapCount();
    const fuse::usize stagingCapacity = resources.stagingRingCapacity();
    fuse::usize wrapSize = 16u;
    if (stagingMid < stagingCapacity) {
        wrapSize = (stagingCapacity - stagingMid) + 16u;
        if (wrapSize > stagingCapacity) {
            wrapSize = stagingCapacity;
        }
    }
    gpuOnlyDesc.size = wrapSize;
    std::vector<u8> wrapPayload(gpuOnlyDesc.size, 0x5A);
    const fuse::renderer::BufferHandle wrapped =
        resources.createBuffer(gpuOnlyDesc, wrapPayload.data());
    expectTrue(wrapped.isValid(), "GpuOnly initialData that misses remaining staging still returns a handle");
    const fuse::renderer::Buffer* wrappedBuf = resources.getBuffer(wrapped);
    if (wrappedBuf != nullptr && wrappedBuf->mapped == nullptr && stagingMid > 0u &&
        wrapSize <= stagingCapacity) {
        expectTrue(resources.stagingRingWrapCount() == wrapCountBefore + 1u,
                   "staging ring wrap count increases when remaining space is insufficient");
        expectTrue(resources.stagingRingOffset() >= wrapSize,
                   "staging offset advances from 0 after wrap");
    }

    gpuOnlyDesc.size = resources.stagingRingCapacity() + 16u;
    std::vector<u8> tooLarge(gpuOnlyDesc.size, 0x5A);
    const fuse::usize stagingAfterWrap = resources.stagingRingOffset();
    const u32 wrapCountAfterWrap = resources.stagingRingWrapCount();
    const fuse::renderer::BufferHandle skipped =
        resources.createBuffer(gpuOnlyDesc, tooLarge.data());
    expectTrue(skipped.isValid(), "GpuOnly initialData larger than staging still returns a handle");
    const fuse::renderer::Buffer* skippedBuf = resources.getBuffer(skipped);
    if (skippedBuf != nullptr && skippedBuf->mapped == nullptr) {
        expectTrue(resources.stagingRingOffset() == stagingAfterWrap,
                   "staging offset unchanged when initialData copy is skipped");
        expectTrue(resources.stagingRingWrapCount() == wrapCountAfterWrap,
                   "staging wrap count unchanged when size exceeds capacity");
    }

    fuse::renderer::TextureDesc textureDesc{};
    textureDesc.width = 4;
    textureDesc.height = 4;
    textureDesc.usage = fuse::renderer::ImageUsage::Sampled;
    u8 texels[4 * 4 * 4] = {};

    const fuse::usize stagingBeforeTexture = resources.stagingRingOffset();
    const u32 wrapCountBeforeTexture = resources.stagingRingWrapCount();
    const fuse::renderer::TextureHandle texture = resources.createTexture(textureDesc, texels);
    expectTrue(texture.isValid(), "texture handle issued");
    expectTrue(resources.getTexture(texture) != nullptr, "texture resolvable");
    expectTrue(resources.getTexture(texture)->bindlessIndex != UINT32_MAX,
               "texture bindless index assigned");
    const fuse::usize textureBytes = 4u * 4u * 4u;
    if (stagingBeforeTexture + textureBytes > resources.stagingRingCapacity()) {
        expectTrue(resources.stagingRingWrapCount() == wrapCountBeforeTexture + 1u,
                   "texture initialData wraps staging ring when remaining space is insufficient");
        expectTrue(resources.stagingRingOffset() >= textureBytes,
                   "texture initialData advances staging offset from 0 after wrap");
    } else {
        expectTrue(resources.stagingRingOffset() > stagingBeforeTexture,
                   "texture initialData advances staging offset");
    }
#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        expectTrue(resources.lastGpuTextureCopySubmitted(),
                   "GPU vkCmdCopyBufferToImage submitted for texture initialData");
        expectTrue(resources.lastGpuTextureCopyBytes() == 4u * 4u * 4u,
                   "GPU texture copy bytes match R8G8B8A8Unorm 4x4");
        expectTrue(resources.lastGpuCopyUsedFence(),
                   "texture initialData copy waited on a transient fence");
        expectTrue(!resources.lastGpuCopyWaitTimedOut(),
                   "texture initialData copy fence wait did not time out");
    }
#endif

    fuse::renderer::TextureDesc interopDesc{};
    interopDesc.width = 2;
    interopDesc.height = 2;
    interopDesc.usage = fuse::renderer::ImageUsage::Sampled;
    interopDesc.cudaInterop = true;
    const fuse::renderer::TextureHandle interopTexture = resources.createTexture(interopDesc);
    expectTrue(interopTexture.isValid(), "cudaInterop texture handle issued");
    const fuse::renderer::Texture* interop = resources.getTexture(interopTexture);
    expectTrue(interop != nullptr, "cudaInterop texture resolvable");
    const bool stubOrDeviceReady =
#if defined(FUSE_VULKAN_BACKEND)
        bootstrap->status().deviceReady;
#else
        true;
#endif
    if (interop != nullptr && stubOrDeviceReady) {
        expectTrue(interop->allocationSize > 0, "cudaInterop allocationSize recorded");
    }
    if (interop != nullptr && interop->exportedHandle != nullptr) {
        expectTrue(interop->allocationSize > 0, "exportedHandle requires allocationSize");
    }

    void* vkDevice = nullptr;
    if (bootstrap->device() != nullptr) {
        vkDevice = bootstrap->device()->nativeHandle();
    }
    if (interop != nullptr) {
        const auto imageImportDesc =
            fuse::renderer::cuda::makeImageImportDesc(vkDevice, *interop);
        expectTrue(imageImportDesc.exportedHandle == interop->exportedHandle,
                   "makeImageImportDesc copies exportedHandle");
        expectTrue(imageImportDesc.width == interop->desc.width,
                   "makeImageImportDesc width matches texture");
        expectTrue(imageImportDesc.height == interop->desc.height,
                   "makeImageImportDesc height matches texture");

        const fuse::renderer::cuda::CudaSurfaceImport imported =
            fuse::renderer::cuda::import_vulkan_image(vkDevice, *interop);
        if (imported.ok) {
            fuse::renderer::cuda::free_cuda_surface(imported.surfaceObject);
        }
    }

    fuse::renderer::BufferDesc interopBufferDesc{};
    interopBufferDesc.size = 64;
    interopBufferDesc.usage = fuse::renderer::BufferUsage::Storage;
    interopBufferDesc.memoryUsage = fuse::renderer::MemoryUsage::GpuOnly;
    interopBufferDesc.cudaInterop = true;
    const fuse::renderer::BufferHandle interopBufferHandle =
        resources.createBuffer(interopBufferDesc);
    expectTrue(interopBufferHandle.isValid(), "cudaInterop buffer handle issued");
    const fuse::renderer::Buffer* interopBuffer = resources.getBuffer(interopBufferHandle);
    expectTrue(interopBuffer != nullptr, "cudaInterop buffer resolvable");
    if (interopBuffer != nullptr) {
        const auto bufferImportDesc =
            fuse::renderer::cuda::makeBufferImportDesc(vkDevice, *interopBuffer);
        if (stubOrDeviceReady) {
            expectTrue(bufferImportDesc.allocationSize > 0,
                       "cudaInterop buffer allocationSize recorded");
        }
    }
    resources.destroyBuffer(interopBufferHandle);

    resources.destroyTexture(interopTexture);

    expectTrue(resources.stagingRingCapacity() == resourceDesc.stagingRingBytes,
               "staging ring capacity recorded");

    resources.destroyTexture(texture);
    resources.destroyBuffer(skipped);
    resources.destroyBuffer(wrapped);
    resources.destroyBuffer(gpuOnly);
    resources.destroyBuffer(buffer);
    expectTrue(!resources.getTexture(texture), "destroyed texture handle stale");
    expectTrue(!resources.getBuffer(buffer), "destroyed buffer handle stale");

    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testDebugUtilsObjectNaming() {
    expectTrue(!fuse::renderer::setDebugObjectName(nullptr, 9u, 1u, "named"),
               "setDebugObjectName rejects null device");
    expectTrue(!fuse::renderer::setDebugObjectName(reinterpret_cast<void*>(1), 9u, 0u, "named"),
               "setDebugObjectName rejects null handle");
    expectTrue(!fuse::renderer::setDebugObjectName(reinterpret_cast<void*>(1), 9u, 1u, nullptr),
               "setDebugObjectName rejects null name");

    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for debug naming");
    if (bootstrap == nullptr) {
        return;
    }

    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    fuse::renderer::ResourceManager::Desc resourceDesc{};
    resourceDesc.stagingRingBytes = 4096u;
    const bool ready = resources.init(*bootstrap->device(), bindless, resourceDesc);
#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        expectTrue(!ready, "resource manager skips without device");
        bindless.destroy(*bootstrap->device());
        return;
    }
    expectTrue(ready, "resource manager initializes for debug naming");
#else
    expectTrue(ready, "stub resource manager initializes for debug naming");
#endif

    fuse::renderer::BufferDesc bufferDesc{};
    bufferDesc.size = 64;
    bufferDesc.usage = fuse::renderer::BufferUsage::Storage;
    bufferDesc.memoryUsage = fuse::renderer::MemoryUsage::GpuOnly;
    bufferDesc.name = "fuse_debug_named_buffer";
    const fuse::renderer::BufferHandle buffer = resources.createBuffer(bufferDesc);
    expectTrue(buffer.isValid(), "named buffer handle issued");
    const fuse::renderer::Buffer* createdBuffer = resources.getBuffer(buffer);
    expectTrue(createdBuffer != nullptr, "named buffer resolvable");

    fuse::renderer::TextureDesc textureDesc{};
    textureDesc.width = 2;
    textureDesc.height = 2;
    textureDesc.usage = fuse::renderer::ImageUsage::Sampled;
    textureDesc.name = "fuse_debug_named_texture";
    const fuse::renderer::TextureHandle texture = resources.createTexture(textureDesc);
    expectTrue(texture.isValid(), "named texture handle issued");
    const fuse::renderer::Texture* createdTexture = resources.getTexture(texture);
    expectTrue(createdTexture != nullptr, "named texture resolvable");

    const fuse::renderer::GpuAllocStats* stats = resources.allocatorStats();
    expectTrue(stats != nullptr, "allocator stats available");

#if defined(FUSE_VULKAN_BACKEND)
    const bool debugUtilsEnabled =
        bootstrap->instance() != nullptr &&
        bootstrap->instance()->info().instanceHasExtension("VK_EXT_debug_utils");
    if (debugUtilsEnabled && createdBuffer != nullptr && createdBuffer->handle != nullptr) {
        expectTrue(stats != nullptr && stats->debugNamesSet >= 2u,
                   "allocator recorded debug names for named buffer and texture");
        const fuse::u64 bufferHandle =
            static_cast<fuse::u64>(reinterpret_cast<std::uintptr_t>(createdBuffer->handle));
        expectTrue(fuse::renderer::setDebugObjectName(bootstrap->device()->nativeHandle(), 9u,
                                                     bufferHandle, "fuse_debug_named_buffer"),
                   "setDebugObjectName succeeds on live VkBuffer");
    }
#else
    expectTrue(!fuse::renderer::setDebugObjectName(reinterpret_cast<void*>(1), 9u, 1u, "named"),
               "setDebugObjectName is a no-op on stub");
    if (stats != nullptr) {
        expectTrue(stats->debugNamesSet == 0u, "stub allocator does not set debug names");
    }
#endif
    (void)createdTexture;

    resources.destroyTexture(texture);
    resources.destroyBuffer(buffer);
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testSamplerAnisotropy() {
    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for sampler anisotropy");
    if (bootstrap == nullptr) {
        return;
    }

    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    fuse::renderer::ResourceManager::Desc resourceDesc{};
    resourceDesc.stagingRingBytes = 4096u;
    const bool ready = resources.init(*bootstrap->device(), bindless, resourceDesc);

    fuse::renderer::SamplerDesc samplerDesc{};
    samplerDesc.anisotropy = true;
    samplerDesc.maxAnisotropy = 16.f;

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        expectTrue(!ready, "resource manager skips without device");
        bindless.destroy(*bootstrap->device());
        return;
    }
    expectTrue(ready, "resource manager initializes for sampler anisotropy");
    const fuse::renderer::VulkanDeviceInfo& info = bootstrap->device()->info();
    expectTrue(info.maxSamplerAnisotropy >= 1.f,
               "maxSamplerAnisotropy is at least 1 when device is ready");
    (void)info.samplerAnisotropy;
#else
    expectTrue(ready, "stub resource manager initializes for sampler anisotropy");
#endif

    const fuse::renderer::SamplerHandle sampler = resources.createSampler(samplerDesc);
    expectTrue(sampler.isValid(),
               "SamplerDesc anisotropy true still creates a sampler (disabled if feature off)");
    expectTrue(resources.liveCounts().samplers == 1u, "anisotropic sampler counted as live");

    resources.destroySampler(sampler);
    expectTrue(resources.liveCounts().samplers == 0u, "anisotropic sampler destroyed");
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testSamplerCompare() {
    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for sampler compare");
    if (bootstrap == nullptr) {
        return;
    }

    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    fuse::renderer::ResourceManager::Desc resourceDesc{};
    resourceDesc.stagingRingBytes = 4096u;
    const bool ready = resources.init(*bootstrap->device(), bindless, resourceDesc);

    fuse::renderer::SamplerDesc samplerDesc{};
    samplerDesc.compareEnable = true;
    samplerDesc.compareOp = 1u; // VK_COMPARE_OP_LESS

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        expectTrue(!ready, "resource manager skips without device");
        bindless.destroy(*bootstrap->device());
        return;
    }
    expectTrue(ready, "resource manager initializes for sampler compare");
#else
    expectTrue(ready, "stub resource manager initializes for sampler compare");
#endif

    const fuse::renderer::SamplerHandle sampler = resources.createSampler(samplerDesc);
    expectTrue(sampler.isValid(),
               "SamplerDesc compareEnable true creates a sampler (native or stub handle)");
    expectTrue(resources.liveCounts().samplers == 1u, "compare sampler counted as live");

    resources.destroySampler(sampler);
    expectTrue(resources.liveCounts().samplers == 0u, "compare sampler destroyed");
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testSamplerLodAndGenerateMips() {
    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for sampler LOD and mip generate");
    if (bootstrap == nullptr) {
        return;
    }

    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    fuse::renderer::ResourceManager::Desc resourceDesc{};
    resourceDesc.stagingRingBytes = 4096u;
    const bool ready = resources.init(*bootstrap->device(), bindless, resourceDesc);

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        expectTrue(!ready, "resource manager skips without device");
        expectTrue(!resources.generateMips(fuse::renderer::TextureHandle{}),
                   "generateMips is false when resource manager is not ready");
        bindless.destroy(*bootstrap->device());
        return;
    }
    expectTrue(ready, "resource manager initializes for sampler LOD and mip generate");
#else
    expectTrue(ready, "stub resource manager initializes for sampler LOD and mip generate");
#endif

    fuse::renderer::SamplerDesc lodDesc{};
    lodDesc.minLod = 0.f;
    lodDesc.maxLod = 4.f;
    const fuse::renderer::SamplerHandle lodSampler = resources.createSampler(lodDesc);
    expectTrue(lodSampler.isValid(), "SamplerDesc minLod=0 maxLod=4 createSampler succeeds");

    fuse::renderer::TextureDesc mipDesc{};
    mipDesc.width = 16;
    mipDesc.height = 16;
    mipDesc.mipLevels = 4;
    mipDesc.usage = static_cast<fuse::renderer::ImageUsage>(
        static_cast<u32>(fuse::renderer::ImageUsage::Sampled) |
        static_cast<u32>(fuse::renderer::ImageUsage::TransferSrc) |
        static_cast<u32>(fuse::renderer::ImageUsage::TransferDst));
    const fuse::renderer::TextureHandle mipTexture = resources.createTexture(mipDesc);
    expectTrue(mipTexture.isValid(), "mipLevels=4 Sampled|TransferSrc|TransferDst texture issued");

#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        const bool generated = resources.generateMips(mipTexture);
        expectTrue(generated == resources.lastMipGenerateOk(),
                   "lastMipGenerateOk matches generateMips result");
        if (generated) {
            expectTrue(resources.lastMipGenerateCount() == 3u,
                       "generateMips blits three mip levels from a 4-level chain");
        } else {
            expectTrue(!resources.lastMipGenerateOk(),
                       "generateMips honest false does not crash");
        }
    }
#else
    expectTrue(!resources.generateMips(mipTexture), "stub generateMips returns false");
    expectTrue(!resources.lastMipGenerateOk(), "stub lastMipGenerateOk is false");
    expectTrue(resources.lastMipGenerateCount() == 0u, "stub lastMipGenerateCount stays zero");
#endif

    resources.destroySampler(lodSampler);
    resources.destroyTexture(mipTexture);
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testCubeMapImageViews() {
    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for cube map views");
    if (bootstrap == nullptr) {
        return;
    }

    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    fuse::renderer::ResourceManager::Desc resourceDesc{};
    resourceDesc.stagingRingBytes = 4096u;
    const bool ready = resources.init(*bootstrap->device(), bindless, resourceDesc);

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        expectTrue(!ready, "resource manager skips without device");
        bindless.destroy(*bootstrap->device());
        return;
    }
    expectTrue(ready, "resource manager initializes for cube map views");
#else
    expectTrue(ready, "stub resource manager initializes for cube map views");
#endif

    fuse::renderer::TextureDesc default2d{};
    default2d.width = 8;
    default2d.height = 8;
    default2d.usage = fuse::renderer::ImageUsage::Sampled;
    expectTrue(!default2d.cubeMap, "TextureDesc cubeMap defaults to false");
    const fuse::renderer::TextureHandle tex2d = resources.createTexture(default2d);
    expectTrue(tex2d.isValid(), "2D default texture handle issued");
    const fuse::renderer::Texture* created2d = resources.getTexture(tex2d);
    expectTrue(created2d != nullptr, "2D default texture resolvable");
    if (created2d != nullptr) {
        expectTrue(!created2d->desc.cubeMap, "2D default path leaves cubeMap false");
        expectTrue(created2d->desc.arrayLayers == 1u, "2D default path keeps arrayLayers at 1");
        expectTrue(created2d->view != nullptr, "2D default texture view is non-null or honest stub");
    }

    fuse::renderer::TextureDesc cubeDesc{};
    cubeDesc.width = 8;
    cubeDesc.height = 8;
    cubeDesc.arrayLayers = 6;
    cubeDesc.cubeMap = true;
    cubeDesc.usage = fuse::renderer::ImageUsage::Sampled;
    const fuse::renderer::TextureHandle cube = resources.createTexture(cubeDesc);
    expectTrue(cube.isValid(), "cubeMap texture handle issued");
    const fuse::renderer::Texture* createdCube = resources.getTexture(cube);
    expectTrue(createdCube != nullptr, "cubeMap texture resolvable");
    if (createdCube != nullptr) {
        expectTrue(createdCube->desc.cubeMap, "cubeMap flag stored on texture desc");
        expectTrue(createdCube->desc.arrayLayers == 6u, "cubeMap texture has 6 array layers");
        expectTrue(createdCube->view != nullptr,
                   "cubeMap texture view is non-null when device is ready or honest stub");
    }

    resources.destroyTexture(cube);
    resources.destroyTexture(tex2d);
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testDeviceLocalMemoryBudget() {
    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for memory budget");
    if (bootstrap == nullptr) {
        return;
    }

    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    fuse::renderer::ResourceManager::Desc resourceDesc{};
    resourceDesc.stagingRingBytes = 4096u;
    const bool ready = resources.init(*bootstrap->device(), bindless, resourceDesc);

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        expectTrue(!ready, "resource manager skips without device");
        bindless.destroy(*bootstrap->device());
        return;
    }
    expectTrue(ready, "resource manager initializes for memory budget");
    const fuse::renderer::GpuAllocStats* stats = resources.allocatorStats();
    expectTrue(stats != nullptr, "allocator stats available for memory budget");
    if (stats != nullptr) {
        expectTrue(stats->deviceLocalHeapBytes > 0,
                   "device-local heap size queried when GpuAllocator/device ready");
        expectTrue(stats->deviceLocalBudgetBytes > 0,
                   "device-local budget falls back to heap size when EXT budget is absent");
    }
#else
    expectTrue(ready, "stub resource manager initializes for memory budget");
    const fuse::renderer::GpuAllocStats* stats = resources.allocatorStats();
    expectTrue(stats != nullptr, "stub allocator stats available for memory budget");
    if (stats != nullptr) {
        expectTrue(stats->deviceLocalHeapBytes == 0, "stub allocator leaves device-local heap at zero");
        expectTrue(stats->deviceLocalBudgetBytes == 0,
                   "stub allocator leaves device-local budget at zero");
        expectTrue(stats->deviceLocalHeapIndex == 0, "stub allocator leaves device-local heap index at zero");
    }
#endif

    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testTextureReadback() {
    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(desc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for texture readback");
    if (bootstrap == nullptr) {
        return;
    }

    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    fuse::renderer::ResourceManager::Desc resourceDesc{};
    resourceDesc.stagingRingBytes = 4096u;
    const bool ready = resources.init(*bootstrap->device(), bindless, resourceDesc);

    u8 invalidDst[4] = {};
    expectTrue(!resources.readTexture(fuse::renderer::TextureHandle{}, invalidDst, sizeof(invalidDst)),
               "readTexture rejects an invalid handle");

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        expectTrue(!ready, "resource manager skips without device");
        bindless.destroy(*bootstrap->device());
        return;
    }
    expectTrue(ready, "resource manager initializes for texture readback");
#else
    expectTrue(ready, "stub resource manager initializes for texture readback");
#endif

    fuse::renderer::TextureDesc textureDesc{};
    textureDesc.width = 1;
    textureDesc.height = 1;
    textureDesc.format = fuse::renderer::GpuFormat::R8G8B8A8Unorm;
    textureDesc.usage = static_cast<fuse::renderer::ImageUsage>(
        static_cast<u32>(fuse::renderer::ImageUsage::Sampled) |
        static_cast<u32>(fuse::renderer::ImageUsage::TransferSrc) |
        static_cast<u32>(fuse::renderer::ImageUsage::TransferDst));
    const u8 payload[4] = {9, 8, 7, 6};
    const fuse::renderer::TextureHandle texture = resources.createTexture(textureDesc, payload);
    expectTrue(texture.isValid(), "1x1 R8G8B8A8 texture handle issued for readback");
    expectTrue(resources.getTexture(texture) != nullptr, "1x1 readback texture resolvable");

    expectTrue(!resources.readTexture(texture, nullptr, sizeof(payload)),
               "readTexture rejects a null destination");
    expectTrue(!resources.readTexture(texture, invalidDst, 0), "readTexture rejects a zero size");

    u8 readback[4] = {};
#if defined(FUSE_VULKAN_BACKEND)
    if (resources.readTexture(texture, readback, sizeof(readback))) {
        expectTrue(readback[0] == payload[0] && readback[1] == payload[1] &&
                       readback[2] == payload[2] && readback[3] == payload[3],
                   "readTexture matches 1x1 R8G8B8A8 initialData bytes");
        expectTrue(resources.lastTextureReadbackBytes() == 4u,
                   "lastTextureReadbackBytes records a 1x1 RGBA copy");
    }
#else
    expectTrue(!resources.readTexture(texture, readback, sizeof(readback)),
               "stub readTexture returns false");
    expectTrue(resources.lastTextureReadbackBytes() == 0u,
               "stub lastTextureReadbackBytes stays zero");
#endif

    resources.destroyTexture(texture);
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    fuse::core::initialize();

    testHandleMapGeneration();
    testBindlessIndexRecycle();
    testResourceManagerBuffersAndTextures();
    testDebugUtilsObjectNaming();
    testSamplerAnisotropy();
    testSamplerCompare();
    testSamplerLodAndGenerateMips();
    testCubeMapImageViews();
    testDeviceLocalMemoryBudget();
    testTextureReadback();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_vulkan_resources: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_vulkan_resources: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
