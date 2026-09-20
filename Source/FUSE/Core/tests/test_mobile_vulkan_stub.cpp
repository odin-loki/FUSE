#include <fuse/platform/mobile_vulkan_stub.hpp>

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

void testAndroidVulkanStubStatus() {
    const fuse::platform::MobileVulkanStubStatus off =
        fuse::platform::androidVulkanWsiStubStatus(false);
    expectTrue(off.kind == fuse::platform::MobileVulkanStubKind::AndroidWsi,
               "android stub kind");
    expectTrue(off.documented, "android stub documented");
    expectTrue(!off.buildVulkanEnabled, "android stub reports Vulkan OFF");
    expectTrue(off.message != nullptr, "android stub exposes message");

    const fuse::platform::MobileVulkanStubStatus on =
        fuse::platform::androidVulkanWsiStubStatus(true);
    expectTrue(on.buildVulkanEnabled, "android stub reports Vulkan ON warning path");
}

void testMoltenVkStubStatus() {
    const fuse::platform::MobileVulkanStubStatus status = fuse::platform::moltenVkMacosStubStatus();
    expectTrue(status.kind == fuse::platform::MobileVulkanStubKind::MoltenVkMacos,
               "moltenvk stub kind");
    expectTrue(status.documented, "moltenvk stub documented");
    expectTrue(status.message != nullptr, "moltenvk stub exposes message");
}

} // namespace

int main() {
    testAndroidVulkanStubStatus();
    testMoltenVkStubStatus();

    if (g_failures == 0) {
        std::printf("fuse_mobile_vulkan_stub: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mobile_vulkan_stub: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
