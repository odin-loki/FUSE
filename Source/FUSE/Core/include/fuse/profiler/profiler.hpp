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

/// Why a profile record would be skipped without mutating profiler state (B1.6 deepen).
enum class ProfileRecordSkipReason : u8 {
    None = 0,
    ProfilerDisabled,
    InvalidName,
    NoOpenAsyncFlow,
};

/// Read-only scope-record preflight — safe to call before constructing `ProfileScope`.
struct ProfileScopePreflight {
    bool profilerDisabled = false;
    bool invalidName = false;

    bool canRecord() const { return !profilerDisabled && !invalidName; }
};

/// Read-only async-flow begin preflight — safe to call before `beginAsyncFlow`.
struct AsyncFlowBeginPreflight {
    bool profilerDisabled = false;
    bool invalidName = false;

    bool canBegin() const { return !profilerDisabled && !invalidName; }
};

/// Read-only async-flow end preflight — safe to call before `endAsyncFlow`.
struct AsyncFlowEndPreflight {
    bool profilerDisabled = false;
    bool invalidName = false;
    bool noOpenAsyncFlows = false;

    bool canEnd() const { return !profilerDisabled && !invalidName && !noOpenAsyncFlows; }
};

/// Read-only nesting/async-flow state preflight — safe before export or frame teardown.
struct NestingStatePreflight {
    u32 activeScopeNestingDepth = 0;
    u32 activeFlowNestingDepth = 0;
    u32 openAsyncFlowCount = 0;
    bool scopeNestingUnbalanced = false;
    bool flowNestingUnbalanced = false;
    bool hasOpenAsyncFlows = false;
    bool flowDepthDetached = false;
    bool crossThreadFlowHandoffPending = false;

    bool isBalanced() const {
        return !scopeNestingUnbalanced && !flowNestingUnbalanced && !flowDepthDetached
            && !crossThreadFlowHandoffPending;
    }
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

/// Read-only chrome export diagnostics — safe to call before `exportChromeTraceJson()`.
struct ChromeTraceExportPreflight {
    u32 eventCount = 0;
    u32 exportableEventCount = 0;
    u32 frameIndex = 0;
    u32 openAsyncFlowCount = 0;
    u32 activeScopeNestingDepth = 0;
    u32 activeFlowNestingDepth = 0;
    u32 maxScopeNestingDepth = 0;
    u32 maxFlowNestingDepth = 0;
    bool profilerDisabled = false;
    bool bufferEmpty = false;
    bool scopeNestingUnbalanced = false;
    bool flowNestingUnbalanced = false;
    bool hasOpenAsyncFlows = false;
    bool flowDepthDetached = false;
    u32 invalidNameEventCount = 0;
    bool ringBufferFull = false;
    bool hasInvalidNameEvents = false;
    bool crossThreadFlowHandoffPending = false;

    bool canExport() const { return !profilerDisabled; }
    bool hasExportableEvents() const { return exportableEventCount > 0; }
    bool hasUnbalancedNesting() const { return scopeNestingUnbalanced || flowNestingUnbalanced; }
    bool canExportSafely() const {
        return canExport() && !hasUnbalancedNesting() && !flowDepthDetached && !crossThreadFlowHandoffPending;
    }
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
u32 ringCapacity();
u32 maxNestingDepth();
u32 nestingDepth();
u32 maxFlowNestingDepth();
u32 scopeNestingDepth();
u32 flowNestingDepth();
u32 openAsyncFlowCount();
bool hasOpenAsyncFlows();
bool isScopeNestingBalanced();
bool isFlowNestingBalanced();
bool hasUnbalancedNesting();
bool isFlowDepthDetached();
bool isCrossThreadFlowHandoffPending();

bool hasEvents();
bool isBufferEmpty();
bool isBufferFull();
bool isEventIndexValid(u32 index);
bool isValidEventName(const char* name);
bool isValidProfileEvent(const ProfileEvent& event);
bool isProfileEventSentinel(const ProfileEvent& event);
u32 invalidNameEventCount();
bool hasInvalidNameEvents();
u32 exportableEventCount();
bool isEventExportable(u32 index);
u32 firstEventIndex();
u32 lastEventIndex();
u32 findFirstEventIndexByPhase(EventPhase phase);
u32 findLastEventIndexByPhase(EventPhase phase);
u32 countEventsByPhase(EventPhase phase);
u32 findFirstEventIndexByName(const char* name);
u32 findLastEventIndexByName(const char* name);
u32 countEventsByName(const char* name);
u32 findFirstEventIndexByFlowId(u32 flowId);
u32 findLastEventIndexByFlowId(u32 flowId);
u32 countEventsByFlowId(u32 flowId);
const ProfileEvent& emptyProfileEvent();
const ProfileEvent& eventAt(u32 index);
bool tryEventAt(u32 index, ProfileEvent& outEvent);
bool tryExportableEventAt(u32 index, ProfileEvent& outEvent);
bool tryFirstEvent(ProfileEvent& outEvent);
bool tryLastEvent(ProfileEvent& outEvent);
bool tryFindFirstEventByName(const char* name, ProfileEvent& outEvent);
bool tryFindLastEventByName(const char* name, ProfileEvent& outEvent);
bool tryFindFirstFlowEventById(u32 flowId, ProfileEvent& outEvent);
bool tryFindLastFlowEventById(u32 flowId, ProfileEvent& outEvent);
const ProfileEvent& lastEvent();
void reset();

ProfileScopePreflight preflightProfileScope(const char* name);
AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name);
AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name);
NestingStatePreflight preflightNestingState();
ChromeTraceExportPreflight preflightChromeTraceExport();

bool wouldSkipProfileScope(const char* name, ProfileRecordSkipReason* reason = nullptr);
bool wouldSkipAsyncFlowBegin(const char* name, ProfileRecordSkipReason* reason = nullptr);
bool wouldSkipAsyncFlowEnd(const char* name, ProfileRecordSkipReason* reason = nullptr);
bool wouldSkipCounterSample(const char* track, ProfileRecordSkipReason* reason = nullptr);

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
