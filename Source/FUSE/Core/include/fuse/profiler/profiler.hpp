#pragma once

#include <fuse/types.hpp>

#include <string>
#include <type_traits>

namespace fuse::profiler {

/// Sentinel returned by `lastEventIndex()` when the ring buffer has no recorded events.
constexpr u32 kInvalidEventIndex = static_cast<u32>(-1);

enum class EventPhase : u8 {
    Begin,
    End,
    FlowStart,
    FlowFinish,
    Counter,
};

enum class CounterValueKind : u8 {
    None,
    Int,
    Float,
};

struct ProfileEvent {
    const char* name = nullptr;
    u64 timestampNs = 0;
    EventPhase phase = EventPhase::Begin;
    u32 threadId = 0;
    u32 scopeId = 0;
    u32 nestingDepth = 0;
    u32 flowNestingDepth = 0;
    CounterValueKind counterKind = CounterValueKind::None;
    s64 counterIntValue = 0;
    f64 counterFloatValue = 0.0;
    u32 counterSnapshotFrame = 0;
};

/// RAII CPU scope timer — records begin/end into the frame ring buffer when enabled.
class ProfileScope {
public:
    explicit ProfileScope(const char* name);
    ~ProfileScope();

    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    const char* m_name = nullptr;
    u32 m_scopeId = 0;
    u32 m_nestingDepth = 0;
    bool m_active = false;
};

bool enabled();
void setEnabled(bool enabled);

void beginFrame();
void endFrame();

u32 frameIndex();
u32 eventCount();
u32 maxNestingDepth();
u32 nestingDepth();
u32 maxFlowNestingDepth();
u32 scopeNestingDepth();
u32 flowNestingDepth();
u32 openAsyncFlowCount();

bool hasEvents();
bool isBufferEmpty();
bool isBufferFull();
bool isEventIndexValid(u32 index);
bool isValidProfileEvent(const ProfileEvent& event);
u32 lastEventIndex();
const ProfileEvent& eventAt(u32 index);
const ProfileEvent& lastEvent();
void reset();

/// Ring-buffer capacity in events (B1.6 deepen — introspection stub).
constexpr u32 kRingBufferCapacity = 4096u;
inline constexpr u32 ringBufferCapacity() { return kRingBufferCapacity; }

/// Remaining event slots before the ring overwrites oldest entries (B1.6 deepen).
u32 remainingEventCapacity();

/// True when `name` is nullptr or points to an empty C string (B1.6 deepen — empty-name guard).
bool isEmptyProfilerName(const char* name);

/// Read-only name preflight for scope/flow/counter recording (B1.6 deepen).
struct ProfileNamePreflight {
    bool null_name = false;
    bool empty_name = false;

    bool canRecord() const { return !null_name && !empty_name; }
    bool shouldSkip() const { return !canRecord(); }
};

/// Read-only scope nesting diagnostics (B1.6 deepen).
struct ScopeNestingPreflight {
    u32 current_depth = 0;
    u32 max_depth_seen = 0;
    bool profiler_disabled = false;

    bool canPush() const { return !profiler_disabled; }
};

/// Read-only async-flow begin diagnostics (B1.6 deepen).
struct AsyncFlowBeginPreflight {
    bool profiler_disabled = false;
    bool null_name = false;
    bool empty_name = false;

    bool canBegin() const { return !profiler_disabled && !null_name && !empty_name; }
    bool shouldSkip() const { return !canBegin(); }
};

/// Read-only async-flow end diagnostics (B1.6 deepen).
struct AsyncFlowEndPreflight {
    bool profiler_disabled = false;
    bool null_name = false;
    bool empty_name = false;
    bool orphan_end = false;

    bool canEnd() const { return !profiler_disabled && !null_name && !empty_name && !orphan_end; }
    bool shouldSkip() const { return !canEnd(); }
};

/// Read-only chrome export diagnostics (B1.6 deepen).
struct ChromeTraceExportPreflight {
    u32 event_count = 0;
    u32 open_async_flow_count = 0;
    bool has_events = false;
    bool has_unmatched_flows = false;
    bool would_emit_empty_trace = false;

    bool canExport() const { return true; }
    bool shouldSkip() const { return would_emit_empty_trace; }
};

ProfileNamePreflight preflightProfileName(const char* name);
ScopeNestingPreflight preflightScopeNesting();
AsyncFlowBeginPreflight preflightAsyncFlowBegin(const char* name);
AsyncFlowEndPreflight preflightAsyncFlowEnd(const char* name);
ChromeTraceExportPreflight preflightChromeTraceExport();

/// Convenience guards mirroring preflight skip predicates (B1.6 deepen).
bool shouldSkipProfileScope(const char* name);
bool shouldSkipAsyncFlowBegin(const char* name);
bool shouldSkipAsyncFlowEnd(const char* name);
bool shouldSkipCounterSample(const char* track);

/// Safe indexed lookup — returns true and copies into `out` when `index` is valid (B1.6 deepen).
bool tryEventAt(u32 index, ProfileEvent& out);

/// Safe indexed lookup — returns nullptr when `index` is out of range (B1.6 deepen).
const ProfileEvent* eventAtOrNull(u32 index);

/// Monotonic flow id for async chrome://tracing `ph:"s"` / `ph:"f"` pairs (e.g. job load id).
u32 nextFlowId();

/// Async flow begin/end stubs — typically emitted on different threads for I/O or job handoff.
/// Records current scope nesting depth so chrome export can correlate flows inside profiled slices.
void beginAsyncFlow(const char* name, u32 flowId);
void endAsyncFlow(const char* name, u32 flowId);

/// Counter sample stubs — emit chrome `ph:"C"` events for budget overlays.
/// Inherits the active scope and async flow nesting depth when sampled inside profiled slices.
void sampleCounter(const char* track, s64 value);
void sampleCounterFloat(const char* track, f64 value);

/// Counter sample with `snapshot_at_frame` metadata — records `frameIndex()` for frame-aligned budgets.
void sampleCounterSnapshotAtFrame(const char* track, s64 value);
void sampleCounterFloatSnapshotAtFrame(const char* track, f64 value);

template<typename T>
inline void sampleCounterDispatch(const char* track, T value) {
    if constexpr (std::is_floating_point_v<T>) {
        sampleCounterFloat(track, static_cast<f64>(value));
    } else {
        sampleCounter(track, static_cast<s64>(value));
    }
}

template<typename T>
inline void sampleCounterSnapshotAtFrameDispatch(const char* track, T value) {
    if constexpr (std::is_floating_point_v<T>) {
        sampleCounterFloatSnapshotAtFrame(track, static_cast<f64>(value));
    } else {
        sampleCounterSnapshotAtFrame(track, static_cast<s64>(value));
    }
}

/// Stub export for chrome://tracing offline analysis (not hot path).
std::string exportChromeTraceJson();

} // namespace fuse::profiler

#if defined(FUSE_NO_PROFILER) && FUSE_NO_PROFILER
#define FUSE_PROFILE_SCOPE(name) ((void)0)
#define FUSE_PROFILE_ASYNC_FLOW_BEGIN(name, flowId) ((void)0)
#define FUSE_PROFILE_ASYNC_FLOW_END(name, flowId) ((void)0)
#define FUSE_PROFILE_COUNTER(track, value) ((void)0)
#define FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME(track, value) ((void)0)
#else
#define FUSE_PROFILE_SCOPE_IMPL(line, name) ::fuse::profiler::ProfileScope _fuse_profile_scope_##line(name)
#define FUSE_PROFILE_SCOPE(name) FUSE_PROFILE_SCOPE_IMPL(__LINE__, name)
#define FUSE_PROFILE_ASYNC_FLOW_BEGIN(name, flowId) ::fuse::profiler::beginAsyncFlow(name, flowId)
#define FUSE_PROFILE_ASYNC_FLOW_END(name, flowId) ::fuse::profiler::endAsyncFlow(name, flowId)
#define FUSE_PROFILE_COUNTER(track, value) ::fuse::profiler::sampleCounterDispatch(track, value)
#define FUSE_PROFILE_COUNTER_SNAPSHOT_AT_FRAME(track, value) \
    ::fuse::profiler::sampleCounterSnapshotAtFrameDispatch(track, value)
#endif
