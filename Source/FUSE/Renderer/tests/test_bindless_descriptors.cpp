#include <fuse/core/init.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/types.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using fuse::u32;
using fuse::u64;

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::unique_ptr<fuse::renderer::VulkanBootstrap> makeBootstrap() {
    fuse::renderer::VulkanBootstrapDesc desc{};
    desc.instance.enableValidation = false;
    return fuse::renderer::VulkanBootstrap::create(desc);
}

void testAllocFreeReuse() {
    auto bootstrap = makeBootstrap();
    expectTrue(bootstrap != nullptr, "bootstrap allocated");

    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    const fuse::renderer::BindlessSlotHandle first = bindless.allocateTextureSlot(false);
    const fuse::renderer::BindlessSlotHandle second = bindless.allocateTextureSlot(false);
    expectTrue(first.index == 0u && second.index == 1u, "sequential texture slot indices");
    expectTrue(bindless.validateSlot(first) && bindless.validateSlot(second), "fresh slots valid");

    bindless.freeTextureSlot(first);
    expectTrue(!bindless.validateSlot(first), "freed slot handle invalidated");
    expectTrue(bindless.isSlotOccupied(fuse::renderer::BindlessHeapKind::Texture, first.index) == false,
               "slot marked unoccupied");

    const fuse::renderer::BindlessSlotHandle reused = bindless.allocateTextureSlot(false);
    expectTrue(reused.index == first.index, "freed slot index reused");
    expectTrue(reused.generation != first.generation, "generation bumped on free before reuse");
    expectTrue(bindless.validateSlot(reused), "reused slot valid with new generation");
    expectTrue(!bindless.validateSlot(first), "stale handle still rejected");

    bindless.destroy(*bootstrap->device());
}

void testGenerationBumpOnFree() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    const fuse::renderer::BindlessSlotHandle handle = bindless.allocateBufferSlot();
    expectTrue(handle.generation == 1u, "first buffer slot generation is 1");
    const u32 genBeforeFree = bindless.slotGeneration(fuse::renderer::BindlessHeapKind::Buffer, handle.index);

    bindless.freeBufferSlot(handle);
    const u32 genAfterFree = bindless.slotGeneration(fuse::renderer::BindlessHeapKind::Buffer, handle.index);
    expectTrue(genAfterFree > genBeforeFree, "generation bumped on free");

    const fuse::renderer::BindlessSlotHandle stale{fuse::renderer::BindlessHeapKind::Buffer, handle.index,
                                                   handle.generation};
    expectTrue(!bindless.validateSlot(stale), "stale generation rejected after free");
    bindless.freeBufferSlot(stale);
    expectTrue(bindless.registeredBufferCount() == 0u, "double-free with stale handle is no-op");

    bindless.destroy(*bootstrap->device());
}

void testOobReject() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    const fuse::renderer::BindlessSlotHandle oob{fuse::renderer::BindlessHeapKind::Texture, 9999u, 1u};
    expectTrue(!bindless.validateSlot(oob), "OOB index rejected");
    expectTrue(!bindless.isSlotOccupied(fuse::renderer::BindlessHeapKind::Texture, 9999u), "OOB not occupied");

    bindless.freeTextureSlot(oob);
    expectTrue(bindless.registeredTextureCount() == 0u, "OOB free does not affect live count");

    const fuse::renderer::BindlessSlotHandle wrongKind{fuse::renderer::BindlessHeapKind::Buffer, 0u, 1u};
    bindless.freeTextureSlot(wrongKind);
    expectTrue(bindless.registeredTextureCount() == 0u, "wrong-kind free ignored");

    bindless.destroy(*bootstrap->device());
}

void testBindingIndexHelpers() {
    const fuse::renderer::BindlessBindingIndex sampled = fuse::renderer::bindlessTextureBinding(42u, false);
    expectTrue(sampled.binding == fuse::renderer::kBindlessBindingSampledImages, "sampled texture binding");
    expectTrue(sampled.arrayIndex == 42u, "sampled texture array index");

    const fuse::renderer::BindlessBindingIndex storage = fuse::renderer::bindlessTextureBinding(7u, true);
    expectTrue(storage.binding == fuse::renderer::kBindlessBindingStorageImages, "storage texture binding");

    const fuse::renderer::BindlessBindingIndex ssbo = fuse::renderer::bindlessBufferBinding(3u, false);
    expectTrue(ssbo.binding == fuse::renderer::kBindlessBindingStorageBuffers, "SSBO binding");

    const fuse::renderer::BindlessBindingIndex ubo = fuse::renderer::bindlessBufferBinding(3u, true);
    expectTrue(ubo.binding == fuse::renderer::kBindlessBindingUniformBuffers, "UBO binding");

    const fuse::renderer::BindlessBindingIndex sampler = fuse::renderer::bindlessSamplerBinding(11u);
    expectTrue(sampler.binding == fuse::renderer::kBindlessBindingSamplers, "sampler binding");

    const u32 packed = fuse::renderer::packBindlessBindingIndex(sampled.binding, sampled.arrayIndex);
    u32 binding = 0;
    u32 arrayIndex = 0;
    expectTrue(fuse::renderer::unpackBindlessBindingIndex(packed, binding, arrayIndex), "pack/unpack round trip");
    expectTrue(binding == sampled.binding && arrayIndex == sampled.arrayIndex, "packed values match");
}

void testSparseResizeStub() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    expectTrue(bindless.heapCapacity(fuse::renderer::BindlessHeapKind::Texture) == 0u, "initial heap empty");
    expectTrue(bindless.resizeHeap(fuse::renderer::BindlessHeapKind::Texture, 128u), "resize grows table");
    expectTrue(bindless.heapCapacity(fuse::renderer::BindlessHeapKind::Texture) == 128u, "capacity recorded");
    expectTrue(bindless.resizeHeap(fuse::renderer::BindlessHeapKind::Texture, 64u), "shrink is no-op stub");
    expectTrue(bindless.heapCapacity(fuse::renderer::BindlessHeapKind::Texture) == 128u, "capacity unchanged on shrink");

    const fuse::renderer::BindlessSlotHandle handle = bindless.allocateTextureSlot(false);
    expectTrue(handle.index == 0u, "alloc after resize still sequential from zero");

    bindless.destroy(*bootstrap->device());
}

void testUninitializedAndInvalidHandles() {
    fuse::renderer::BindlessDescriptors bindless;

    expectTrue(bindless.allocateTextureSlot(false).isValid() == false, "alloc before init rejected");
    expectTrue(!bindless.validateSlot(fuse::renderer::BindlessSlotHandle::invalid()), "default invalid handle");
    expectTrue(bindless.heapLiveCount(fuse::renderer::BindlessHeapKind::Texture) == 0u,
               "live count zero before init");

    auto bootstrap = makeBootstrap();
    bindless.init(*bootstrap->device());

    const fuse::renderer::BindlessSlotHandle live = bindless.allocateTextureSlot(false);
    const fuse::renderer::BindlessSlotHandle wrongGen{
        fuse::renderer::BindlessHeapKind::Texture, live.index, live.generation + 1u};
    expectTrue(!bindless.validateSlot(wrongGen), "wrong generation rejected on occupied slot");

    bindless.destroy(*bootstrap->device());
}

void testHandlePackUnpack() {
    const fuse::renderer::BindlessSlotHandle handle{fuse::renderer::BindlessHeapKind::Buffer, 12345u, 67890u};
    const u64 packed = fuse::renderer::packBindlessSlotHandle(handle);
    const fuse::renderer::BindlessSlotHandle roundTrip = fuse::renderer::unpackBindlessSlotHandle(packed);
    expectTrue(roundTrip == handle, "slot handle pack/unpack round trip");
}

void testBindingFromHandle() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    const fuse::renderer::BindlessSlotHandle sampled = bindless.allocateTextureSlot(false);
    const fuse::renderer::BindlessSlotHandle storage = bindless.allocateTextureSlot(true);
    const fuse::renderer::BindlessBindingIndex sampledBinding = bindless.bindingIndexForHandle(sampled);
    const fuse::renderer::BindlessBindingIndex storageBinding = bindless.bindingIndexForHandle(storage);

    expectTrue(sampledBinding.binding == fuse::renderer::kBindlessBindingSampledImages,
               "sampled handle maps to sampled binding");
    expectTrue(storageBinding.binding == fuse::renderer::kBindlessBindingStorageImages,
               "storage handle maps to storage binding");
    expectTrue(bindless.slotIsStorageTexture(storage.index), "storage flag recorded");
    expectTrue(!bindless.slotIsStorageTexture(sampled.index), "sampled slot not storage");

    const fuse::renderer::BindlessSlotHandle stale{fuse::renderer::BindlessHeapKind::Texture, sampled.index,
                                                   sampled.generation};
    bindless.freeTextureSlot(sampled);
    expectTrue(bindless.bindingIndexForHandle(stale).binding == 0u &&
                   bindless.bindingIndexForHandle(stale).arrayIndex == 0u,
               "stale handle returns empty binding");

    const fuse::renderer::BindlessSlotHandle sampler = bindless.allocateSamplerSlot();
    const fuse::renderer::BindlessBindingIndex samplerBinding = bindless.bindingIndexForHandle(sampler);
    expectTrue(samplerBinding.binding == fuse::renderer::kBindlessBindingSamplers, "sampler binding from handle");

    bindless.destroy(*bootstrap->device());
}

void testHeapCounts() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    expectTrue(bindless.heapCapacity(fuse::renderer::BindlessHeapKind::Buffer) == 0u, "buffer heap starts empty");
    expectTrue(bindless.heapFreeCount(fuse::renderer::BindlessHeapKind::Buffer) == 0u, "no free slots initially");

    const fuse::renderer::BindlessSlotHandle a = bindless.allocateBufferSlot();
    const fuse::renderer::BindlessSlotHandle b = bindless.allocateBufferSlot();
    expectTrue(bindless.heapLiveCount(fuse::renderer::BindlessHeapKind::Buffer) == 2u, "two live buffer slots");
    expectTrue(bindless.heapFreeCount(fuse::renderer::BindlessHeapKind::Buffer) == 0u, "no free buffer slots");

    bindless.freeBufferSlot(a);
    expectTrue(bindless.heapLiveCount(fuse::renderer::BindlessHeapKind::Buffer) == 1u, "one live after free");
    expectTrue(bindless.heapFreeCount(fuse::renderer::BindlessHeapKind::Buffer) == 1u, "one free after release");

    expectTrue(bindless.resizeHeap(fuse::renderer::BindlessHeapKind::Buffer, 8u), "resize buffer heap");
    expectTrue(bindless.heapCapacity(fuse::renderer::BindlessHeapKind::Buffer) == 8u, "buffer capacity grown");
    expectTrue(bindless.heapFreeCount(fuse::renderer::BindlessHeapKind::Buffer) == 7u,
               "resize pre-seeds free slots minus live");

    bindless.destroy(*bootstrap->device());
}

void testSamplerCapExhaustion() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    std::vector<fuse::renderer::BindlessSlotHandle> handles;
    handles.reserve(fuse::renderer::kMaxSamplers);
    for (u32 i = 0; i < fuse::renderer::kMaxSamplers; ++i) {
        const fuse::renderer::BindlessSlotHandle handle = bindless.allocateSamplerSlot();
        expectTrue(handle.isValid(), "sampler slot allocated within cap");
        handles.push_back(handle);
    }

    expectTrue(bindless.heapLiveCount(fuse::renderer::BindlessHeapKind::Sampler) ==
                   fuse::renderer::kMaxSamplers,
               "sampler heap at capacity");
    expectTrue(!bindless.allocateSamplerSlot().isValid(), "alloc beyond sampler cap rejected");

    bindless.destroy(*bootstrap->device());
}

void testUniformBufferSlots() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    const fuse::renderer::BindlessSlotHandle ssbo = bindless.allocateBufferSlot(false);
    const fuse::renderer::BindlessSlotHandle ubo = bindless.allocateBufferSlot(true);
    expectTrue(ssbo.isValid() && ubo.isValid(), "ssbo and ubo slots allocated");
    expectTrue(!bindless.slotIsUniformBuffer(ssbo.index), "ssbo slot not uniform");
    expectTrue(bindless.slotIsUniformBuffer(ubo.index), "ubo slot marked uniform");

    const fuse::renderer::BindlessBindingIndex ssboBinding = bindless.bindingIndexForHandle(ssbo);
    const fuse::renderer::BindlessBindingIndex uboBinding = bindless.bindingIndexForHandle(ubo);
    expectTrue(ssboBinding.binding == fuse::renderer::kBindlessBindingStorageBuffers, "ssbo binding");
    expectTrue(uboBinding.binding == fuse::renderer::kBindlessBindingUniformBuffers, "ubo binding");

    bindless.destroy(*bootstrap->device());
}

void testHandleRegisterUnregister() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    const fuse::renderer::Texture texture{};
    const fuse::renderer::Buffer buffer{};
    const fuse::renderer::BindlessSlotHandle texHandle = bindless.registerTextureSlot(texture, true);
    const fuse::renderer::BindlessSlotHandle bufHandle = bindless.registerBufferSlot(buffer, true);
    const fuse::renderer::BindlessSlotHandle sampHandle = bindless.registerSamplerSlot(nullptr);

    expectTrue(bindless.validateSlot(texHandle), "texture slot handle valid");
    expectTrue(bindless.validateSlot(bufHandle), "buffer slot handle valid");
    expectTrue(bindless.validateSlot(sampHandle), "sampler slot handle valid");
    expectTrue(bindless.slotIsStorageTexture(texHandle.index), "storage texture via handle API");
    expectTrue(bindless.slotIsUniformBuffer(bufHandle.index), "uniform buffer via handle API");

    bindless.unregisterSlot(texHandle);
    bindless.unregisterSlot(bufHandle);
    bindless.unregisterSlot(sampHandle);
    expectTrue(!bindless.validateSlot(texHandle), "texture handle stale after unregister");
    expectTrue(!bindless.validateSlot(bufHandle), "buffer handle stale after unregister");
    expectTrue(!bindless.validateSlot(sampHandle), "sampler handle stale after unregister");
    expectTrue(bindless.heapLiveCount(fuse::renderer::BindlessHeapKind::Texture) == 0u,
               "texture heap empty after handle unregister");

    bindless.destroy(*bootstrap->device());
}

void testInvalidHandleUnregister() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    expectTrue(!bindless.validateSlot(fuse::renderer::BindlessSlotHandle::invalid()),
               "default invalid handle rejected");
    bindless.unregisterSlot(fuse::renderer::BindlessSlotHandle::invalid());
    expectTrue(bindless.heapLiveCount(fuse::renderer::BindlessHeapKind::Texture) == 0u,
               "invalid unregister is no-op");

    const fuse::renderer::BindlessSlotHandle live = bindless.allocateTextureSlot(false);
    const fuse::renderer::BindlessSlotHandle stale{
        fuse::renderer::BindlessHeapKind::Texture, live.index, live.generation + 99u};
    bindless.unregisterSlot(stale);
    expectTrue(bindless.validateSlot(live), "stale unregister leaves live slot");

    bindless.destroy(*bootstrap->device());
}

void testGenerationMonotonicReuse() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    fuse::renderer::BindlessSlotHandle handle = bindless.allocateSamplerSlot();
    expectTrue(handle.generation == 1u, "initial sampler generation is 1");
    const u32 index = handle.index;

    for (u32 cycle = 0; cycle < 64u; ++cycle) {
        bindless.freeSamplerSlot(handle);
        const u32 genAfterFree = bindless.slotGeneration(fuse::renderer::BindlessHeapKind::Sampler, index);
        expectTrue(genAfterFree == handle.generation + 1u, "generation increments on each free");
        handle = bindless.allocateSamplerSlot();
        expectTrue(handle.index == index, "same index reused across cycles");
        expectTrue(handle.generation == genAfterFree, "reused handle carries bumped generation");
    }

    bindless.destroy(*bootstrap->device());
}

void testDoubleFreeValidHandle() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    const fuse::renderer::BindlessSlotHandle handle = bindless.allocateBufferSlot(false);
    bindless.freeBufferSlot(handle);
    expectTrue(!bindless.validateSlot(handle), "handle invalid after first free");
    bindless.freeBufferSlot(handle);
    expectTrue(bindless.heapLiveCount(fuse::renderer::BindlessHeapKind::Buffer) == 0u,
               "double-free with stale handle is no-op");
    expectTrue(bindless.heapFreeCount(fuse::renderer::BindlessHeapKind::Buffer) == 1u,
               "slot not double-enqueued on free list");

    bindless.destroy(*bootstrap->device());
}

void testClampHeapCapacity() {
    expectTrue(fuse::renderer::clampHeapCapacity(fuse::renderer::BindlessHeapKind::Texture, 100u) == 100u,
               "requested below cap unchanged");
    expectTrue(fuse::renderer::clampHeapCapacity(fuse::renderer::BindlessHeapKind::Texture,
                                                 fuse::renderer::kMaxTextures + 500u) ==
                   fuse::renderer::kMaxTextures,
               "texture request clamped to max");
    expectTrue(fuse::renderer::clampHeapCapacity(fuse::renderer::BindlessHeapKind::Sampler, 99999u) ==
                   fuse::renderer::kMaxSamplers,
               "sampler request clamped to max");
    expectTrue(fuse::renderer::bindlessHeapMaxCapacity(fuse::renderer::BindlessHeapKind::Buffer) ==
                   fuse::renderer::kMaxBuffers,
               "heap max capacity helper matches constant");

    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());
    expectTrue(bindless.heapMaxCapacity(fuse::renderer::BindlessHeapKind::Texture) ==
                   fuse::renderer::kMaxTextures,
               "instance max capacity matches kind ceiling");
    fuse::renderer::BindlessDescriptors bindlessClamp;
    bindlessClamp.init(*bootstrap->device());
    expectTrue(bindlessClamp.resizeHeap(fuse::renderer::BindlessHeapKind::Texture,
                                        fuse::renderer::kMaxTextures + 1u),
               "resize above cap clamps instead of failing");
    expectTrue(bindlessClamp.heapCapacity(fuse::renderer::BindlessHeapKind::Texture) ==
                   fuse::renderer::kMaxTextures,
               "clamped resize grows to max capacity");
    bindlessClamp.destroy(*bootstrap->device());

    bindless.destroy(*bootstrap->device());
}

void testSlotGenerationMismatch() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    const fuse::renderer::BindlessSlotHandle live = bindless.allocateTextureSlot(false);
    expectTrue(!bindless.slotGenerationMismatch(live), "live handle has no generation mismatch");

    const fuse::renderer::BindlessSlotHandle stale{
        fuse::renderer::BindlessHeapKind::Texture, live.index, live.generation + 1u};
    expectTrue(bindless.slotGenerationMismatch(stale), "wrong generation flagged as mismatch");
    expectTrue(!bindless.validateSlot(stale), "mismatch handle fails validateSlot");

    bindless.freeTextureSlot(live);
    expectTrue(bindless.slotGenerationMismatch(live), "freed handle is generation mismatch");
    expectTrue(!bindless.slotGenerationMismatch(
                   fuse::renderer::BindlessSlotHandle{fuse::renderer::BindlessHeapKind::Texture, 9999u, 1u}),
               "OOB handle is not a generation mismatch");

    bindless.destroy(*bootstrap->device());
}

void testSlotHandleAtAndBindingForSlot() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    const fuse::renderer::BindlessSlotHandle allocated = bindless.allocateBufferSlot(true);
    const fuse::renderer::BindlessSlotHandle at = bindless.slotHandleAt(fuse::renderer::BindlessHeapKind::Buffer,
                                                                        allocated.index);
    expectTrue(at == allocated, "slotHandleAt matches allocate handle");
    expectTrue(bindless.validateSlot(at), "slotHandleAt handle validates");

    const fuse::renderer::BindlessBindingIndex fromSlot =
        bindless.bindingIndexForSlot(fuse::renderer::BindlessHeapKind::Buffer, allocated.index);
    const fuse::renderer::BindlessBindingIndex fromHandle = bindless.bindingIndexForHandle(allocated);
    expectTrue(fromSlot.binding == fromHandle.binding && fromSlot.arrayIndex == fromHandle.arrayIndex,
               "bindingIndexForSlot matches handle lookup");
    expectTrue(fromSlot.binding == fuse::renderer::kBindlessBindingUniformBuffers, "ubo binding via slot index");

    bindless.freeBufferSlot(allocated);
    expectTrue(!bindless.slotHandleAt(fuse::renderer::BindlessHeapKind::Buffer, allocated.index).isValid(),
               "slotHandleAt invalid after free");
    expectTrue(bindless.bindingIndexForSlot(fuse::renderer::BindlessHeapKind::Buffer, allocated.index).binding == 0u,
               "bindingIndexForSlot empty after free");

    bindless.destroy(*bootstrap->device());
}

void testFreeSlotUnifiedDispatch() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    const fuse::renderer::BindlessSlotHandle texture = bindless.allocateTextureSlot(true);
    const fuse::renderer::BindlessSlotHandle buffer = bindless.allocateBufferSlot(false);
    const fuse::renderer::BindlessSlotHandle sampler = bindless.allocateSamplerSlot();

    bindless.freeSlot(texture);
    bindless.freeSlot(buffer);
    bindless.freeSlot(sampler);
    expectTrue(bindless.heapLiveCount(fuse::renderer::BindlessHeapKind::Texture) == 0u,
               "freeSlot clears texture heap");
    expectTrue(bindless.heapLiveCount(fuse::renderer::BindlessHeapKind::Buffer) == 0u,
               "freeSlot clears buffer heap");
    expectTrue(bindless.heapLiveCount(fuse::renderer::BindlessHeapKind::Sampler) == 0u,
               "freeSlot clears sampler heap");

    bindless.destroy(*bootstrap->device());
}

void testStorageFlagsClearedOnFree() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    const fuse::renderer::BindlessSlotHandle storageTex = bindless.allocateTextureSlot(true);
    const fuse::renderer::BindlessSlotHandle ubo = bindless.allocateBufferSlot(true);
    expectTrue(bindless.slotIsStorageTexture(storageTex.index), "storage texture flagged");
    expectTrue(bindless.slotIsUniformBuffer(ubo.index), "uniform buffer flagged");

    bindless.freeTextureSlot(storageTex);
    bindless.freeBufferSlot(ubo);
    expectTrue(!bindless.slotIsStorageTexture(storageTex.index), "storage flag cleared on free");
    expectTrue(!bindless.slotIsUniformBuffer(ubo.index), "uniform flag cleared on free");

    const fuse::renderer::BindlessSlotHandle reusedTex = bindless.allocateTextureSlot(false);
    const fuse::renderer::BindlessSlotHandle reusedBuf = bindless.allocateBufferSlot(false);
    expectTrue(reusedTex.index == storageTex.index, "texture slot reused");
    expectTrue(reusedBuf.index == ubo.index, "buffer slot reused");
    expectTrue(!bindless.slotIsStorageTexture(reusedTex.index), "reused texture defaults to sampled");
    expectTrue(!bindless.slotIsUniformBuffer(reusedBuf.index), "reused buffer defaults to ssbo");

    bindless.destroy(*bootstrap->device());
}

void testBufferCapExhaustion() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    std::vector<fuse::renderer::BindlessSlotHandle> handles;
    handles.reserve(256u);
    for (u32 i = 0; i < 256u; ++i) {
        const fuse::renderer::BindlessSlotHandle handle = bindless.allocateBufferSlot(i % 2u == 0u);
        expectTrue(handle.isValid(), "buffer slot allocated within test cap");
        handles.push_back(handle);
    }

    bindless.resizeHeap(fuse::renderer::BindlessHeapKind::Buffer, fuse::renderer::kMaxBuffers);
    for (u32 i = 256u; i < fuse::renderer::kMaxBuffers; ++i) {
        const fuse::renderer::BindlessSlotHandle handle = bindless.allocateBufferSlot(false);
        expectTrue(handle.isValid(), "buffer slot allocated up to max");
        handles.push_back(handle);
    }

    expectTrue(bindless.heapLiveCount(fuse::renderer::BindlessHeapKind::Buffer) ==
                   fuse::renderer::kMaxBuffers,
               "buffer heap at capacity");
    expectTrue(!bindless.allocateBufferSlot(false).isValid(), "alloc beyond buffer cap rejected");

    bindless.destroy(*bootstrap->device());
}

void testLegacyRegisterUnregister() {
    auto bootstrap = makeBootstrap();
    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());

    const fuse::renderer::Texture texture{};
    const u32 index = bindless.registerTexture(texture);
    expectTrue(index == 0u, "legacy register returns index");
    expectTrue(bindless.registeredTextureCount() == 1u, "legacy register increments live count");

    bindless.unregisterTexture(index);
    expectTrue(bindless.registeredTextureCount() == 0u, "legacy unregister clears slot");

    const u32 recycled = bindless.registerTexture(texture);
    expectTrue(recycled == index, "legacy index recycled");
    expectTrue(bindless.slotGeneration(fuse::renderer::BindlessHeapKind::Texture, index) > 1u,
               "legacy unregister bumped generation");

    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    fuse::core::initialize();

    testAllocFreeReuse();
    testGenerationBumpOnFree();
    testOobReject();
    testBindingIndexHelpers();
    testSparseResizeStub();
    testUninitializedAndInvalidHandles();
    testHandlePackUnpack();
    testBindingFromHandle();
    testHeapCounts();
    testSamplerCapExhaustion();
    testUniformBufferSlots();
    testHandleRegisterUnregister();
    testInvalidHandleUnregister();
    testGenerationMonotonicReuse();
    testDoubleFreeValidHandle();
    testClampHeapCapacity();
    testSlotGenerationMismatch();
    testSlotHandleAtAndBindingForSlot();
    testFreeSlotUnifiedDispatch();
    testStorageFlagsClearedOnFree();
    testBufferCapExhaustion();
    testLegacyRegisterUnregister();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_bindless_descriptors: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_bindless_descriptors: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
