#include <fuse/core/init.hpp>
#include <fuse/platform/crash_report.hpp>
#include <fuse/platform/lifecycle.hpp>
#include <fuse/platform/power.hpp>
#include <fuse/platform/profile.hpp>
#include <fuse/platform/surface_loss.hpp>
#include <fuse/platform/thread.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        ++g_failures;
    }
}

void resetPlatformState() {
    fuse::platform::clearLifecycleCallbacks();
    fuse::platform::clearPowerStateCallbacks();
    fuse::platform::clearSurfaceLossHandlers();
    fuse::platform::clearActiveProfileOverride();
    fuse::platform::setPowerState(fuse::platform::PowerState::Normal);
    fuse::platform::notifyAppVisibility(fuse::platform::AppVisibility::Foreground);
    if (fuse::platform::isSurfaceValid() == false) {
        fuse::platform::notifySurfaceRestored();
    }
}

void testLifecycleHooksUpdatePowerState() {
    resetPlatformState();

    fuse::platform::PowerState lastPrevious = fuse::platform::PowerState::Normal;
    fuse::platform::PowerState lastCurrent = fuse::platform::PowerState::Normal;
    fuse::platform::registerPowerStateCallback(
        [&](fuse::platform::PowerState previous, fuse::platform::PowerState current) {
            lastPrevious = previous;
            lastCurrent = current;
        });

    fuse::platform::AppVisibility lastVisibility = fuse::platform::AppVisibility::Foreground;
    fuse::platform::registerLifecycleCallback([&](fuse::platform::AppVisibility visibility) {
        lastVisibility = visibility;
    });

    fuse::platform::notifyAppVisibility(fuse::platform::AppVisibility::Background);
    expectTrue(fuse::platform::getAppVisibility() == fuse::platform::AppVisibility::Background,
               "background visibility recorded");
    expectTrue(fuse::platform::getPowerState() == fuse::platform::PowerState::Background,
               "background visibility maps to Background power state");
    expectTrue(lastVisibility == fuse::platform::AppVisibility::Background, "lifecycle callback fired");
    expectTrue(lastPrevious == fuse::platform::PowerState::Normal, "power callback previous state");
    expectTrue(lastCurrent == fuse::platform::PowerState::Background, "power callback current state");

    fuse::platform::notifyAppVisibility(fuse::platform::AppVisibility::Foreground);
    expectTrue(fuse::platform::getPowerState() == fuse::platform::PowerState::Normal,
               "foreground restores Normal power state");
}

void testSurfaceLossStub() {
    resetPlatformState();

    int lostCount = 0;
    int restoredCount = 0;
    fuse::platform::registerSurfaceLossHandlers(
        [&]() { ++lostCount; },
        [&]() { ++restoredCount; });

    expectTrue(fuse::platform::isSurfaceValid(), "surface starts valid");
    fuse::platform::notifySurfaceLost();
    expectTrue(!fuse::platform::isSurfaceValid(), "surface invalid after loss");
    expectEq(static_cast<fuse::u32>(lostCount), 1u, "surface loss callback fired once");

    fuse::platform::notifySurfaceRestored();
    expectTrue(fuse::platform::isSurfaceValid(), "surface valid after restore");
    expectEq(static_cast<fuse::u32>(restoredCount), 1u, "surface restore callback fired once");
}

void testCrashReportStub() {
    resetPlatformState();

    expectTrue(fuse::platform::installCrashHandlers(), "install crash handlers succeeds");
    expectTrue(fuse::platform::crashHandlersInstalled(), "crash handlers marked installed");

    std::string capturedMessage;
    fuse::platform::setCrashReportCallback([&](const fuse::platform::CrashReportContext& context) {
        if (context.message != nullptr) {
            capturedMessage = context.message;
        }
    });

    fuse::platform::CrashReportContext context;
    context.message = "stub crash";
    context.file = "test_platform_hardening.cpp";
    context.line = 42;
    fuse::platform::submitCrashReport(context);
    expectTrue(capturedMessage == "stub crash", "crash report callback receives message");

    fuse::platform::shutdownCrashHandlers();
    expectTrue(!fuse::platform::crashHandlersInstalled(), "crash handlers shut down");
}

void testDesktopProfileLimits() {
    resetPlatformState();
    fuse::platform::setActiveProfileOverride(fuse::platform::PlatformProfile::Desktop);

    const fuse::platform::JobProfileLimits limits = fuse::platform::currentJobProfileLimits();
    expectEq(limits.reserve, 2u, "desktop reserve is 2");
    expectEq(limits.maxWorkers, 16u, "desktop max workers is 16");
    expectEq(limits.fiberStackBytes, 64u * 1024u, "desktop fiber stack is 64 KiB");
    expectEq(limits.ioBudgetMicrosPerFrame, 8000u, "desktop I/O budget is 8 ms");
    expectTrue(fuse::platform::recommendedFiberStackBytes() == limits.fiberStackBytes,
               "recommendedFiberStackBytes follows profile");
}

void testMobileProfileLimits() {
    resetPlatformState();
    fuse::platform::setActiveProfileOverride(fuse::platform::PlatformProfile::Mobile);

    const fuse::platform::JobProfileLimits limits = fuse::platform::currentJobProfileLimits();
    expectEq(limits.reserve, 1u, "mobile reserve is 1");
    expectEq(limits.maxWorkers, 4u, "mobile max workers is 4");
    expectEq(limits.fiberStackBytes, 32u * 1024u, "mobile fiber stack is 32 KiB");
    expectEq(limits.ioBudgetMicrosPerFrame, 2000u, "mobile I/O budget is 2 ms");
    expectTrue(fuse::platform::isMobileProfile(), "mobile override active");
}

void testCoreInitInstallsCrashHandlers() {
    resetPlatformState();
    fuse::platform::shutdownCrashHandlers();

    expectTrue(fuse::core::initialize(), "core initialize succeeds");
    expectTrue(fuse::platform::crashHandlersInstalled(), "core init installs crash handlers");
    fuse::core::shutdown();
}

} // namespace

int main() {
    testLifecycleHooksUpdatePowerState();
    testSurfaceLossStub();
    testCrashReportStub();
    testDesktopProfileLimits();
    testMobileProfileLimits();
    testCoreInitInstallsCrashHandlers();

    if (g_failures == 0) {
        std::printf("fuse_core platform hardening tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core platform hardening tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
