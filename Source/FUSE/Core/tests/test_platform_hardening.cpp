#include <fuse/core/init.hpp>
#include <fuse/gameplay/shield.hpp>
#include <fuse/platform/crash_report.hpp>
#include <fuse/platform/harden_level.hpp>
#include <fuse/platform/lifecycle.hpp>
#include <fuse/platform/path_sep.hpp>
#include <fuse/platform/power.hpp>
#include <fuse/platform/profile.hpp>
#include <fuse/platform/surface_loss.hpp>
#include <fuse/platform/thread.hpp>

#include <cmath>
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

void testPerformanceCoreCountInRange() {
    const fuse::u32 cores = fuse::platform::getCoreCount();
    const fuse::u32 perf = fuse::platform::getPerformanceCoreCount();
    expectTrue(cores >= 1u, "getCoreCount is at least 1");
    if (perf < 1u || perf > cores) {
        std::fprintf(stderr, "FAIL: getPerformanceCoreCount %u is outside 1..%u\n", perf, cores);
        ++g_failures;
    }
}

void testSetThreadPriorityDoesNotCrash() {
    const fuse::platform::ThreadId self = fuse::platform::currentThreadId();
    fuse::platform::setThreadPriority(0, -2);
    fuse::platform::setThreadPriority(self, -1);
    fuse::platform::setThreadPriority(self, 0);
    fuse::platform::setThreadPriority(self, 1);
    fuse::platform::setThreadPriority(self, 2);
    fuse::platform::setThreadPriority(0, 99);
    fuse::platform::setThreadPriority(self, -99);
    fuse::platform::ThreadId other = self + 1u;
    if (other == 0u) {
        other = 1u;
    }
    fuse::platform::setThreadPriority(other, 2);
    fuse::platform::setThreadPriority(self, 0);
}

void testShieldPoolBreaksAndRecharges() {
    fuse::gameplay::ShieldPool pool;
    pool.current = 5.f;
    pool.max = 5.f;
    pool.rechargePerSec = 10.f;
    pool.rechargeDelay = 1.f;
    pool.delayLeft = 0.f;
    pool.broken = false;

    expectTrue(fuse::gameplay::absorb(pool, 0.f) == 0.f && pool.current == 5.f,
               "non-positive absorb does nothing");
    expectTrue(fuse::gameplay::absorb(pool, -2.f) == 0.f && pool.current == 5.f,
               "negative absorb does nothing");
    expectTrue(fuse::gameplay::absorb(pool, std::nanf("")) == 0.f && pool.current == 5.f && !pool.broken,
               "non-finite absorb does nothing");

    const fuse::f32 first = fuse::gameplay::absorb(pool, 3.f);
    expectTrue(first == 3.f && pool.current == 2.f && !pool.broken && pool.delayLeft == 0.f,
               "shield of 5 absorbs 3 and leaves 2");

    const fuse::f32 second = fuse::gameplay::absorb(pool, 10.f);
    expectTrue(second == 2.f && pool.current == 0.f && pool.broken && pool.delayLeft == 1.f,
               "overkill absorbs the remaining 2 and breaks the shield");
    expectTrue(fuse::gameplay::absorb(pool, 4.f) == 0.f && pool.delayLeft == 1.f,
               "a broken shield absorbs nothing");

    fuse::gameplay::tick(pool, 0.f);
    fuse::gameplay::tick(pool, -0.25f);
    expectTrue(pool.broken && pool.delayLeft == 1.f && pool.current == 0.f,
               "non-positive tick does nothing");

    fuse::gameplay::tick(pool, 0.25f);
    expectTrue(pool.broken && pool.current == 0.f && pool.delayLeft == 0.75f,
               "broken shield does not recharge during the delay");

    fuse::gameplay::tick(pool, 1.f);
    expectTrue(!pool.broken && pool.delayLeft == 0.f && pool.current == 2.5f,
               "delay expiry clears broken and recharges with the leftover time");

    fuse::gameplay::tick(pool, 1.f);
    expectTrue(!pool.broken && pool.current == pool.max, "recharge stops at max");
}

void testPathHasSep() {
    expectTrue(fuse::platform::path_has_sep("a/b"), "forward slash is a separator");
    expectTrue(fuse::platform::path_has_sep("a\\b"), "backslash is a separator");
    expectTrue(!fuse::platform::path_has_sep("ab"), "text without a separator");
    expectTrue(!fuse::platform::path_has_sep(""), "empty path has no separator");
}

void testHardenLevelOk() {
    expectTrue(fuse::platform::harden_level_ok(0), "harden level 0 is in range");
    expectTrue(fuse::platform::harden_level_ok(3), "harden level 3 is in range");
    expectTrue(!fuse::platform::harden_level_ok(-1), "harden level -1 is out of range");
    expectTrue(!fuse::platform::harden_level_ok(4), "harden level 4 is out of range");
}

} // namespace

int main() {
    testHardenLevelOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core platform hardening tests: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    testPathHasSep();
    testLifecycleHooksUpdatePowerState();
    testSurfaceLossStub();
    testCrashReportStub();
    testDesktopProfileLimits();
    testMobileProfileLimits();
    testCoreInitInstallsCrashHandlers();
    testPerformanceCoreCountInRange();
    testSetThreadPriorityDoesNotCrash();
    testShieldPoolBreaksAndRecharges();

    if (g_failures == 0) {
        std::printf("fuse_core platform hardening tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_core platform hardening tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
