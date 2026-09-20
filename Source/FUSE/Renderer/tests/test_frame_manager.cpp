#include <fuse/core/init.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/frame.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testScratchResetAndDescriptorPool() {
    fuse::renderer::VulkanInstanceDesc instanceDesc{};
    instanceDesc.enableValidation = false;
    auto instance = fuse::renderer::VulkanInstance::create(instanceDesc);
    expectTrue(instance != nullptr, "VulkanInstance allocated");
    if (instance == nullptr) {
        return;
    }

    auto device = fuse::renderer::VulkanDevice::create(*instance);
    expectTrue(device != nullptr, "VulkanDevice allocated (GPU optional)");
    if (device == nullptr) {
        return;
    }

    auto frames = fuse::renderer::FrameManager::create(*device);
    expectTrue(frames != nullptr, "FrameManager allocated with CPU scratch");
    if (frames == nullptr) {
        return;
    }

    expectTrue(frames->scratchUsedBytes() == 0u, "scratch starts empty");
    expectTrue(frames->scratch().capacityBytes() == 8u * 1024u * 1024u, "scratch is 8MB per slot");

    void* ptr = frames->allocateScratch(64u);
    expectTrue(ptr != nullptr, "64-byte scratch allocation succeeds");
    expectTrue(frames->scratchUsedBytes() >= 64u, "usedBytes accounts for 64-byte allocation");

    frames->beginFrame(0u);
    expectTrue(frames->scratchUsedBytes() == 0u, "beginFrame resets current slot scratch");

#if defined(FUSE_VULKAN_BACKEND)
    if (device->isValid() && frames->isReady()) {
        expectTrue(frames->current().commands.descriptorPool != nullptr,
                   "native descriptor pool exists when Vulkan device is valid");
        for (fuse::u32 i = 0; i < fuse::renderer::kFramesInFlight; ++i) {
            expectTrue(frames->slot(i).commands.descriptorPool != nullptr,
                       "each in-flight slot has a descriptor pool");
        }
    }
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testScratchResetAndDescriptorPool();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_frame_manager: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_frame_manager: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
