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

/// Read-only scope nesting diagnostics (B1.6 deepen — nesting guard).
struct NestingPreflight {
    u32 scopeDepth = 0;
    u32 flowDepth = 0;
    u32 openFlowCount = 0;
    bool scopeBalanced = true;
    bool flowBalanced = true;
    bool hasOpenFlows = false;

    bool isBalanced() const { return scopeBalanced && flowBalanced; }
};

/// Read-only async-flow begin/end diagnostics (B1.6 deepen — async-flow guard).
struct AsyncFlowPreflight {
    bool emptyName = false;
    bool disabled = false;
    bool orphanEnd = false;

    bool canBegin() const { return !emptyName && !disabled; }
    bool canEnd() const { return !emptyName && !disabled && !orphanEnd; }
};

/// Read-only profile-scope entry diagnostics (B1.6 deepen — empty-name guard).
struct ProfileScopePreflight {
    bool emptyName = false;
    bool disabled = false;

    bool canEnter() const { return !emptyName && !disabled; }
};

/// Read-only chrome export diagnostics (B1.6 deepen — export guard).
struct ExportPreflight {
    bool disabled = false;
    bool emptyBuffer = false;
    u32 bufferedEventCount = 0;
    u32 exportableEventCount = 0;
    u32 skippedInvalidNames = 0;
    u32 frameIndex = 0;

    bool canExport() const { return !disabled; }
    bool hasExportableEvents() const { return exportableEventCount > 0u; }
};

/// Read-only event lookup diagnostics (B1.6 deepen — event-lookup preflight).
struct EventLookupPreflight {
    bool emptyBuffer = false;
    bool indexOutOfRange = false;
    bool invalidEvent = false;
    u32 index = 0;
    u32 eventCount = 0;

    bool canLookup() const { return !emptyBuffer && !indexOutOfRange; }
    bool canReadValidEvent() const { return canLookup() && !invalidEvent; }
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
bool hasOpenAsyncFlows();
bool isScopeNestingBalanced();
bool isFlowNestingBalanced();

/// True when `name` is non-null and non-empty (B1.6 deepen — empty-name guard).
bool isValidEventName(const char* name);

NestingPreflight preflightNesting();
ProfileScopePreflight preflightProfileScope(const char* name);
AsyncFlowPreflight preflightBeginAsyncFlow(const char* name);
AsyncFlowPreflight preflightEndAsyncFlow(const char* name);
ExportPreflight preflightExport();
EventLookupPreflight preflightEventAt(u32 index);
EventLookupPreflight preflightLastEvent();

/// Convenience guards mirroring preflight predicates (B1.6 deepen).
bool canEnterProfileScope(const char* name);
bool canBeginAsyncFlow(const char* name);
bool canEndAsyncFlow(const char* name);
bool canExportChromeTrace();
bool canLookupEventAt(u32 index);

bool hasEvents();
bool isBufferEmpty();
bool isBufferFull();
bool isEventIndexValid(u32 index);
bool isValidProfileEvent(const ProfileEvent& event);
u32 lastEventIndex();
const ProfileEvent& eventAt(u32 index);
bool tryEventAt(u32 index, ProfileEvent& outEvent);
const ProfileEvent& lastEvent();
void reset();

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
