#include <fuse/core/init.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/types.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>

using fuse::u32;

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

    expectTrue(!bindless.resizeHeap(fuse::renderer::BindlessHeapKind::Texture, fuse::renderer::kMaxTextures + 1u),
               "resize above cap rejected");

    const fuse::renderer::BindlessSlotHandle handle = bindless.allocateTextureSlot(false);
    expectTrue(handle.index == 0u, "alloc after resize still sequential from zero");

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
    testLegacyRegisterUnregister();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_bindless_descriptors: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_bindless_descriptors: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
