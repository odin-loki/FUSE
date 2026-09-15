#pragma once

#include <fuse/types.hpp>

namespace fuse::platform {

using ThreadId = u64;

/// Logical CPU count (hardware_concurrency on native stubs).
u32 getCoreCount();

/// Performance/big cores when available; falls back to getCoreCount().
u32 getPerformanceCoreCount();

/// Recommended fiber stack size for the active platform profile.
u32 recommendedFiberStackBytes();

/// Register the game/render thread (call once from main loop thread).
void registerRenderThread();

/// Thread that may record/submit GPU work (v1: game thread).
ThreadId renderThread();

/// True when called from the registered render thread.
bool isRenderThread();

/// Platform-specific priority hint (no-op stub in U1).
void setThreadPriority(ThreadId thread, int priority);

} // namespace fuse::platform
