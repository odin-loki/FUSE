#pragma once

#include <functional>

namespace fuse::platform {

/// GPU surface lifecycle — Android/iOS context loss on background (architecture-parallel §4.4).
using SurfaceLossCallback = std::function<void()>;

void registerSurfaceLossHandlers(SurfaceLossCallback onLost, SurfaceLossCallback onRestored);

void clearSurfaceLossHandlers();

/// Platform render thread invokes when the drawable surface is lost or restored.
void notifySurfaceLost();
void notifySurfaceRestored();

/// False after `notifySurfaceLost()` until `notifySurfaceRestored()`.
bool isSurfaceValid();

} // namespace fuse::platform
