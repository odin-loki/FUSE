#include <fuse/math/aim_assist.hpp>
#include <fuse/platform/mobile_api.hpp>
#include <fuse/platform/mobile_tier.hpp>
#include <fuse/platform/mobile_vulkan_stub.hpp>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numbers>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testMobileTierOk() {
    expectTrue(fuse::platform::mobile_tier_ok(0), "mobile_tier_ok(0)");
    expectTrue(fuse::platform::mobile_tier_ok(1), "mobile_tier_ok(1)");
    expectTrue(fuse::platform::mobile_tier_ok(2), "mobile_tier_ok(2)");
    expectTrue(!fuse::platform::mobile_tier_ok(-1), "mobile_tier_ok(-1)");
    expectTrue(!fuse::platform::mobile_tier_ok(3), "mobile_tier_ok(3)");
}

void testMobileApiOk() {
    expectTrue(fuse::platform::mobile_api_ok(1, 0), "mobile_api_ok(1, 0)");
    expectTrue(fuse::platform::mobile_api_ok(1, 3), "mobile_api_ok(1, 3)");
    expectTrue(!fuse::platform::mobile_api_ok(1, 4), "mobile_api_ok(1, 4)");
    expectTrue(!fuse::platform::mobile_api_ok(2, 0), "mobile_api_ok(2, 0)");
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

void testAimAssistYaw() {
    const float full = fuse::math::aim_assist_yaw(0.f, 0.2f, 1.f, 1.f);
    expectTrue(full == 0.2f, "target 0.2 rad away with strength 1 and maxStep 1 moves by 0.2");

    const float limited = fuse::math::aim_assist_yaw(0.f, 0.2f, 0.05f, 1.f);
    expectTrue(limited == 0.05f, "maxStep 0.05 limits the move to 0.05");

    constexpr float pi = std::numbers::pi_v<float>;
    expectTrue(fuse::math::aim_assist_yaw(0.f, pi, 4.f, 1.f) == pi, "delta of +pi stays +pi");
    expectTrue(fuse::math::aim_assist_yaw(0.f, -pi, 4.f, 1.f) == pi, "delta of -pi wraps to +pi");

    const float above = pi + 0.5f;
    expectTrue(fuse::math::aim_assist_yaw(0.f, above, 4.f, 1.f) == above - 2.f * pi,
               "delta above pi wraps into (-pi, pi]");
    const float below = -pi - 0.5f;
    expectTrue(fuse::math::aim_assist_yaw(0.f, below, 4.f, 1.f) == below + 2.f * pi,
               "delta below -pi wraps into (-pi, pi]");
    const float turned = fuse::math::aim_assist_yaw(0.f, 0.2f + 2.f * pi, 1.f, 1.f);
    expectTrue(turned > 0.2f - 1e-5f && turned < 0.2f + 1e-5f, "a full turn wraps before the step");

    const float current = 3.f;
    const float target = current + 0.2f;
    const float unwrapped = fuse::math::aim_assist_yaw(current, target, 1.f, 1.f);
    expectTrue(unwrapped == current + (target - current), "result yaw is not rewrapped");
    expectTrue(unwrapped > pi, "result may leave (-pi, pi]");

    expectTrue(fuse::math::aim_assist_yaw(0.f, -0.2f, 0.05f, 1.f) == -0.05f,
               "maxStep 0.05 limits a negative move to -0.05");
    expectTrue(fuse::math::aim_assist_yaw(0.f, 0.2f, 1.f, 2.f) == 0.2f, "strength above 1 clamps to 1");
    expectTrue(fuse::math::aim_assist_yaw(0.f, 0.2f, 1.f, -1.f) == 0.f, "negative strength clamps to 0");
    expectTrue(fuse::math::aim_assist_yaw(1.25f, 1.5f, -0.5f, 1.f) == 1.25f, "negative maxStep is 0");

    expectTrue(fuse::math::aim_assist_yaw(1.5f, std::numeric_limits<float>::quiet_NaN(), 1.f, 1.f) == 1.5f,
               "non-finite target keeps finite current yaw");
    expectTrue(fuse::math::aim_assist_yaw(1.5f, 0.2f, std::numeric_limits<float>::infinity(), 1.f) == 1.5f,
               "non-finite maxStep keeps finite current yaw");
    expectTrue(fuse::math::aim_assist_yaw(std::numeric_limits<float>::infinity(), 0.2f, 1.f, 1.f) == 0.f,
               "non-finite current yaw returns 0");
}

} // namespace

int main() {
    testMobileTierOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_mobile_vulkan_stub: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testMobileApiOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_mobile_vulkan_stub: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }

    testAndroidVulkanStubStatus();
    testMoltenVkStubStatus();
    testAimAssistYaw();

    if (g_failures == 0) {
        std::printf("fuse_mobile_vulkan_stub: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mobile_vulkan_stub: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
