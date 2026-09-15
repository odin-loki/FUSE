#include <fuse/platform/crash_report.hpp>
#include <fuse/platform/lifecycle.hpp>
#include <fuse/platform/power.hpp>
#include <fuse/platform/profile.hpp>
#include <fuse/platform/surface_loss.hpp>

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace fuse::platform {

namespace {

std::atomic<PowerState> g_powerState{PowerState::Normal};
std::atomic<AppVisibility> g_appVisibility{AppVisibility::Foreground};
std::atomic<bool> g_surfaceValid{true};
std::atomic<bool> g_crashHandlersInstalled{false};

std::vector<LifecycleCallback> g_lifecycleCallbacks;
std::vector<PowerStateCallback> g_powerStateCallbacks;
std::mutex g_callbackMutex;

SurfaceLossCallback g_onSurfaceLost;
SurfaceLossCallback g_onSurfaceRestored;
std::mutex g_surfaceMutex;

CrashReportCallback g_crashCallback;
std::mutex g_crashMutex;

std::atomic<bool> g_hasProfileOverride{false};
std::atomic<int> g_profileOverride{static_cast<int>(PlatformProfile::Desktop)};

PlatformProfile defaultProfile() {
#if defined(FUSE_PLATFORM_MOBILE) && FUSE_PLATFORM_MOBILE
    return PlatformProfile::Mobile;
#else
    return PlatformProfile::Desktop;
#endif
}

void dispatchPowerState(PowerState next) {
    const PowerState previous = g_powerState.exchange(next);
    if (previous == next) {
        return;
    }

    std::vector<PowerStateCallback> callbacks;
    {
        const std::lock_guard<std::mutex> lock(g_callbackMutex);
        callbacks = g_powerStateCallbacks;
    }

    for (const auto& callback : callbacks) {
        if (callback) {
            callback(previous, next);
        }
    }
}

void dispatchLifecycle(AppVisibility visibility) {
    std::vector<LifecycleCallback> callbacks;
    {
        const std::lock_guard<std::mutex> lock(g_callbackMutex);
        callbacks = g_lifecycleCallbacks;
    }

    for (const auto& callback : callbacks) {
        if (callback) {
            callback(visibility);
        }
    }
}

} // namespace

PowerState getPowerState() {
    return g_powerState.load(std::memory_order_acquire);
}

void registerPowerStateCallback(PowerStateCallback callback) {
    const std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_powerStateCallbacks.push_back(std::move(callback));
}

void clearPowerStateCallbacks() {
    const std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_powerStateCallbacks.clear();
}

void setPowerState(PowerState state) {
    dispatchPowerState(state);
}

void registerLifecycleCallback(LifecycleCallback callback) {
    const std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_lifecycleCallbacks.push_back(std::move(callback));
}

void clearLifecycleCallbacks() {
    const std::lock_guard<std::mutex> lock(g_callbackMutex);
    g_lifecycleCallbacks.clear();
}

void notifyAppVisibility(AppVisibility visibility) {
    const AppVisibility previous = g_appVisibility.exchange(visibility);
    if (previous == visibility) {
        return;
    }

    if (visibility == AppVisibility::Background) {
        dispatchPowerState(PowerState::Background);
    } else if (getPowerState() == PowerState::Background) {
        dispatchPowerState(PowerState::Normal);
    }

    dispatchLifecycle(visibility);
}

AppVisibility getAppVisibility() {
    return g_appVisibility.load(std::memory_order_acquire);
}

void registerSurfaceLossHandlers(SurfaceLossCallback onLost, SurfaceLossCallback onRestored) {
    const std::lock_guard<std::mutex> lock(g_surfaceMutex);
    g_onSurfaceLost = std::move(onLost);
    g_onSurfaceRestored = std::move(onRestored);
}

void clearSurfaceLossHandlers() {
    const std::lock_guard<std::mutex> lock(g_surfaceMutex);
    g_onSurfaceLost = nullptr;
    g_onSurfaceRestored = nullptr;
}

void notifySurfaceLost() {
    g_surfaceValid.store(false, std::memory_order_release);

    SurfaceLossCallback callback;
    {
        const std::lock_guard<std::mutex> lock(g_surfaceMutex);
        callback = g_onSurfaceLost;
    }

    if (callback) {
        callback();
    }
}

void notifySurfaceRestored() {
    g_surfaceValid.store(true, std::memory_order_release);

    SurfaceLossCallback callback;
    {
        const std::lock_guard<std::mutex> lock(g_surfaceMutex);
        callback = g_onSurfaceRestored;
    }

    if (callback) {
        callback();
    }
}

bool isSurfaceValid() {
    return g_surfaceValid.load(std::memory_order_acquire);
}

bool installCrashHandlers() {
    g_crashHandlersInstalled.store(true, std::memory_order_release);
    return true;
}

void shutdownCrashHandlers() {
    g_crashHandlersInstalled.store(false, std::memory_order_release);
}

void setCrashReportCallback(CrashReportCallback callback) {
    const std::lock_guard<std::mutex> lock(g_crashMutex);
    g_crashCallback = std::move(callback);
}

void submitCrashReport(const CrashReportContext& context) {
    CrashReportCallback callback;
    {
        const std::lock_guard<std::mutex> lock(g_crashMutex);
        callback = g_crashCallback;
    }

    if (callback) {
        callback(context);
    }
}

bool crashHandlersInstalled() {
    return g_crashHandlersInstalled.load(std::memory_order_acquire);
}

PlatformProfile activeProfile() {
    if (g_hasProfileOverride.load(std::memory_order_acquire)) {
        return static_cast<PlatformProfile>(g_profileOverride.load(std::memory_order_acquire));
    }
    return defaultProfile();
}

bool isMobileProfile() {
    return activeProfile() == PlatformProfile::Mobile;
}

bool isDesktopProfile() {
    return activeProfile() == PlatformProfile::Desktop;
}

JobProfileLimits jobProfileLimitsForProfile(PlatformProfile profile) {
    JobProfileLimits limits;
    if (profile == PlatformProfile::Mobile) {
        limits.reserve = 1;
        limits.minWorkers = 1;
        limits.maxWorkers = 4;
        limits.fiberStackBytes = 32u * 1024u;
        limits.ioBudgetMicrosPerFrame = 2000u;
    } else {
        limits.reserve = 2;
        limits.minWorkers = 1;
        limits.maxWorkers = 16;
        limits.fiberStackBytes = 64u * 1024u;
        limits.ioBudgetMicrosPerFrame = 8000u;
    }
    return limits;
}

JobProfileLimits currentJobProfileLimits() {
    return jobProfileLimitsForProfile(activeProfile());
}

PlatformProfile setActiveProfileOverride(PlatformProfile profile) {
    const PlatformProfile previous = activeProfile();
    g_profileOverride.store(static_cast<int>(profile), std::memory_order_release);
    g_hasProfileOverride.store(true, std::memory_order_release);
    return previous;
}

void clearActiveProfileOverride() {
    g_hasProfileOverride.store(false, std::memory_order_release);
}

} // namespace fuse::platform
