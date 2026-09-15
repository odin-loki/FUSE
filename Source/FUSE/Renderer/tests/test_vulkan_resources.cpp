#include <fuse/core/init.hpp>
#include <fuse/handle_map.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/types.hpp>

#include <cstdio>
#include <cstdlib>

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
    expectTrue(buffer.isValid(), "buffer handle issued");
    expectTrue(resources.getBuffer(buffer) != nullptr, "buffer resolvable");

    fuse::renderer::TextureDesc textureDesc{};
    textureDesc.width = 4;
    textureDesc.height = 4;
    textureDesc.usage = fuse::renderer::ImageUsage::Sampled;

    const fuse::renderer::TextureHandle texture = resources.createTexture(textureDesc);
    expectTrue(texture.isValid(), "texture handle issued");
    expectTrue(resources.getTexture(texture) != nullptr, "texture resolvable");
    expectTrue(resources.getTexture(texture)->bindlessIndex != UINT32_MAX,
               "texture bindless index assigned");

    expectTrue(resources.stagingRingCapacity() == resourceDesc.stagingRingBytes,
               "staging ring capacity recorded");

    resources.destroyTexture(texture);
    resources.destroyBuffer(buffer);
    expectTrue(!resources.getTexture(texture), "destroyed texture handle stale");
    expectTrue(!resources.getBuffer(buffer), "destroyed buffer handle stale");

    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    fuse::core::initialize();

    testHandleMapGeneration();
    testBindlessIndexRecycle();
    testResourceManagerBuffersAndTextures();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_vulkan_resources: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_vulkan_resources: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
