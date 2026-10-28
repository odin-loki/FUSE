#pragma once

#include <fuse/types.hpp>

namespace fuse::platform {

using ThreadId = u64;

/// Logical CPU count (`hardware_concurrency`, at least 1).
u32 getCoreCount();

/// Performance/big cores. Always in `1..getCoreCount()`.
///
/// Windows: `GetLogicalProcessorInformationEx(RelationProcessorCore)`. Each record is one core
/// and carries an `EfficiencyClass`; the result is how many cores share the highest class
/// (performance cores on hybrid CPUs, every core when the OS reports a single class). A failed
/// query or a zero count uses `getCoreCount()`. A count above `getCoreCount()` is clamped to it.
/// Other platforms return `getCoreCount()`.
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

/// Scheduling priority for a thread. `priority` runs from -2 (low) to 2 (high); values outside
/// that range are clamped. The change is applied only when `thread` is 0 or equals
/// `currentThreadId()`. Any other id is ignored — native handles are not stored. If the OS
/// rejects the change, the call returns and does not throw.
///
/// Windows, `SetThreadPriority(GetCurrentThread(), priority)`. Idle and time-critical are not
/// used; these relative priorities are exactly -2..2:
///   -2 `THREAD_PRIORITY_LOWEST`
///   -1 `THREAD_PRIORITY_BELOW_NORMAL`
///    0 `THREAD_PRIORITY_NORMAL`
///    1 `THREAD_PRIORITY_ABOVE_NORMAL`
///    2 `THREAD_PRIORITY_HIGHEST`
///
/// POSIX: when the calling thread's scheduler policy has a priority range the process can set,
/// `pthread_setschedparam` maps -2..2 linearly onto `[sched_get_priority_min, sched_get_priority_max]`
/// as `min + (priority + 2) * (max - min) / 4`. Otherwise, or when that call is rejected,
/// `SCHED_OTHER` is requested (its only legal `sched_priority` is 0) and the level is applied
/// with `setpriority` rather than `sched_yield`:
///   -2 → nice 19, -1 → nice 10, 0 → nice 0, 1 → nice -10, 2 → nice -20.
void setThreadPriority(ThreadId thread, int priority);

} // namespace fuse::platform
