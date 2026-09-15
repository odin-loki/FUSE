#pragma once

#include <functional>

namespace fuse::platform {

/// OS app visibility — mirrors T2D `backgrounded` (architecture-parallel §3.6).
enum class AppVisibility {
    Foreground,
    Background
};

using LifecycleCallback = std::function<void(AppVisibility visibility)>;

/// Register a callback invoked on the game thread when visibility changes.
void registerLifecycleCallback(LifecycleCallback callback);

void clearLifecycleCallbacks();

/// Platform backends call when the app moves foreground/background (stub: manual for tests).
void notifyAppVisibility(AppVisibility visibility);

AppVisibility getAppVisibility();

} // namespace fuse::platform
