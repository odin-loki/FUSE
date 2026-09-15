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

/// Stable id for the calling OS thread (hash stub until platform backends land).
ThreadId currentThreadId();

/// Low 32-bit tid for chrome://tracing JSON export.
u32 chromeTraceThreadId();

/// Register the process main thread (call once from entry).
void registerMainThread();

/// Registered main thread id; 0 when registerMainThread() was not called.
ThreadId mainThreadId();

/// True when called from the registered main thread (true before registration).
bool isMainThread();

/// Register the game/render thread (call once from main loop thread).
void registerRenderThread();

/// Thread that may record/submit GPU work (v1: game thread).
ThreadId renderThread();

/// True when called from the registered render thread.
bool isRenderThread();

/// Platform-specific priority hint (no-op stub in U1).
void setThreadPriority(ThreadId thread, int priority);

} // namespace fuse::platform
