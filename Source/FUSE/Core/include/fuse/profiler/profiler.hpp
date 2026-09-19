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
    bool exportWouldTrimEvents = false;
    bool hasOnlyExportableEvents = false;
    u32 orphanAsyncFlowEndCount = 0;
    bool hasOrphanAsyncFlowEnds = false;

    bool canExport() const { return !profilerDisabled; }
    bool hasExportableEvents() const { return exportableEventCount > 0; }
    bool hasUnbalancedNesting() const { return scopeNestingUnbalanced || flowNestingUnbalanced; }
    bool canExportWithEvents() const { return canExport() && hasExportableEvents(); }
    bool canExportSafely() const {
        return canExport() && !hasUnbalancedNesting() && !flowDepthDetached && !crossThreadFlowHandoffPending
            && !ringBufferFull && !hasInvalidNameEvents;
    }
};

/// Read-only scope-entry diagnostics — safe to call before constructing `ProfileScope`.
struct ProfileScopePreflight {
    bool profilerDisabled = false;
    bool invalidName = false;
    bool canEnter = false;
};

/// Read-only async-flow begin diagnostics — safe to call before `beginAsyncFlow()`.
struct AsyncFlowBeginPreflight {
    bool profilerDisabled = false;
    bool invalidName = false;
    bool canBegin = false;
};

/// Read-only async-flow end diagnostics — safe to call before `endAsyncFlow()`.
struct AsyncFlowEndPreflight {
    bool profilerDisabled = false;
    bool invalidName = false;
    bool wouldUnderflowOpenCount = false;
    bool canEnd = false;
};

/// Read-only nesting and async-flow diagnostics — safe before scope/flow entry.
struct NestingAsyncFlowPreflight {
    u32 activeScopeNestingDepth = 0;
    u32 activeFlowNestingDepth = 0;
    u32 maxScopeNestingDepth = 0;
    u32 maxFlowNestingDepth = 0;
    u32 openAsyncFlowCount = 0;
    bool scopeNestingUnbalanced = false;
    bool flowNestingUnbalanced = false;
    bool hasOpenAsyncFlows = false;
    bool flowDepthDetached = false;
    bool crossThreadFlowHandoffPending = false;

    bool hasUnbalancedNesting() const { return scopeNestingUnbalanced || flowNestingUnbalanced; }
    bool isBalanced() const {
        return !hasUnbalancedNesting() && !flowDepthDetached && !crossThreadFlowHandoffPending;
    }
};

/// Read-only nesting/async-flow diagnostics — safe to call before recording more events.
struct NestingAsyncFlowPreflight {
    u32 activeScopeNestingDepth = 0;
    u32 activeFlowNestingDepth = 0;
    u32 openAsyncFlowCount = 0;
    u32 maxScopeNestingDepth = 0;
    u32 maxFlowNestingDepth = 0;
    bool scopeNestingBalanced = true;
    bool flowNestingBalanced = true;
    bool hasOpenAsyncFlows = false;
    bool flowDepthDetached = false;
    bool crossThreadFlowHandoffPending = false;

    bool canBeginScope() const { return true; }
    bool canBeginAsyncFlow() const { return !crossThreadFlowHandoffPending; }
    bool canEndAsyncFlow() const { return openAsyncFlowCount > 0u; }
    bool isNestingHealthy() const {
        return !flowDepthDetached && !crossThreadFlowHandoffPending;
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
NestingAsyncFlowPreflight preflightNestingAndAsyncFlow();

bool hasEvents();
bool isBufferEmpty();
bool isBufferFull();
bool isEventIndexValid(u32 index);
/// True for null, empty, or whitespace-only names — diagnostic only; does not affect recording guards.
bool isBlankEventName(const char* name);
bool isValidEventName(const char* name);
bool isValidFlowId(u32 flowId);
bool isFlowPhaseEvent(const ProfileEvent& event);
bool eventNameMatches(const ProfileEvent& event, const char* name);
bool eventMatchesFlowId(const ProfileEvent& event, u32 flowId);
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
bool hasEventsWithName(const char* name);
u32 findFirstEventIndexByFlowId(u32 flowId);
u32 findLastEventIndexByFlowId(u32 flowId);
u32 countEventsByFlowId(u32 flowId);
bool hasEventsWithFlowId(u32 flowId);
bool tryFindFirstEventIndexByPhase(EventPhase phase, u32& outIndex);
bool tryFindLastEventIndexByPhase(EventPhase phase, u32& outIndex);
bool tryFindFirstEventIndexByName(const char* name, u32& outIndex);
bool tryFindFirstEventIndexByFlowId(u32 flowId, u32& outIndex);
bool tryFindLastEventIndexByFlowId(u32 flowId, u32& outIndex);
u32 orphanAsyncFlowEndCount();
bool hasOrphanAsyncFlowEnds();
const ProfileEvent& emptyProfileEvent();
const ProfileEvent& eventAt(u32 index);
bool tryEventAt(u32 index, ProfileEvent& outEvent);
bool tryExportableEventAt(u32 index, ProfileEvent& outEvent);
bool tryFirstEvent(ProfileEvent& outEvent);
bool tryLastEvent(ProfileEvent& outEvent);
bool tryFirstEventByName(const char* name, ProfileEvent& outEvent);
bool tryLastEventByName(const char* name, ProfileEvent& outEvent);
bool tryFirstFlowEvent(u32 flowId, ProfileEvent& outEvent);
bool tryLastFlowEvent(u32 flowId, ProfileEvent& outEvent);
bool tryExportableFirstEvent(ProfileEvent& outEvent);
bool tryExportableLastEvent(ProfileEvent& outEvent);
bool tryFindFirstEventByName(const char* name, ProfileEvent& outEvent);
bool tryFindLastEventByName(const char* name, ProfileEvent& outEvent);
bool tryFindFirstFlowEvent(u32 flowId, ProfileEvent& outEvent);
bool tryFindLastFlowEvent(u32 flowId, ProfileEvent& outEvent);
const ProfileEvent& lastEvent();
void reset();

ChromeTraceExportPreflight preflightChromeTraceExport();
ProfileScopePreflight preflightProfileScope(const char* name);
AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name, u32 flowId);
AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name, u32 flowId);
NestingAsyncFlowPreflight preflightNestingAsyncFlow();

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

// --- deepen additive from deepen-b16-profiler-c977 ---
struct ProfilerRecordPreflight {
struct ProfilerExportPreflight {
ProfilerRecordPreflight preflightRecord(const char* name);
ProfilerExportPreflight preflightChromeTraceExport();

// --- deepen additive from deepen-b16-profiler-preflights-ea42 ---
bool tryEventAt(u32 index, ProfileEvent& out);

// --- deepen additive from deepen-b16-profiler-guards-0c1a ---
struct ChromeExportPreflight {
ChromeExportPreflight preflightChromeExport();

// --- deepen additive from profiler-b16-preflight-deepen-6ec7 ---
struct ProfileNamePreflight {
struct ScopeNestingPreflight {
    bool would_emit_empty_trace = false;
ProfileNamePreflight preflightProfileName(const char* name);
ScopeNestingPreflight preflightScopeNesting();
AsyncFlowBeginPreflight preflightAsyncFlowBegin(const char* name);
AsyncFlowEndPreflight preflightAsyncFlowEnd(const char* name);

// --- deepen additive from deepen-b16-profiler-preflights-4b82 ---
    [[nodiscard]] bool should_skip() const { return !can_record(); }
struct ScopePreflight {
    [[nodiscard]] bool should_skip() const { return !can_enter(); }
    [[nodiscard]] bool should_skip() const { return !can_begin(); }
    [[nodiscard]] bool would_orphan() const { return no_open_flows; }
    [[nodiscard]] bool should_skip() const { return !can_end(); }
struct CounterSamplePreflight {
    [[nodiscard]] bool should_skip() const { return !can_sample(); }
struct EventLookupPreflight {
[[nodiscard]] ProfileNamePreflight preflightProfileName(const char* name);
[[nodiscard]] ScopePreflight preflightScope(const char* name);
[[nodiscard]] ScopeNestingPreflight preflightScopeNesting();
[[nodiscard]] AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name);
[[nodiscard]] AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name);
[[nodiscard]] CounterSamplePreflight preflightCounterSample(const char* track);
[[nodiscard]] ChromeTraceExportPreflight preflightChromeTraceExport();
[[nodiscard]] EventLookupPreflight preflightEventLookup(u32 index);
[[nodiscard]] bool tryEventAt(u32 index, const ProfileEvent*& event_out);

// --- deepen additive from deepen-b16-profiler-preflights-8f4e ---
bool tryEventAt(u32 index, const ProfileEvent*& outEvent);
struct ProfilerGuardPreflight {
[[nodiscard]] ProfilerGuardPreflight preflightGuardState();

// --- deepen additive from deepen-b16-profiler-preflights-479f ---
struct NestingStatePreflight {
NestingStatePreflight preflightNestingState();

// --- deepen additive from deepen-b16-profiler-export-preflight-9968 ---
ChromeExportPreflight preflightChromeTraceExport();

// --- deepen additive from deepen-b16-profiler-export-preflight-bcfd ---
enum class ChromeTraceExportRejectReason : u8 {
ChromeTraceExportRejectReason chromeTraceExportRejectReason();

// --- deepen additive from deepen-fuse-b16-profiler-11c2 ---
enum class EventNameRejectReason : u8 {
enum class NestingStateRejectReason : u8 {
enum class EventLookupRejectReason : u8 {
bool tryValidateEventName(const char* name, EventNameRejectReason& outReason);
const char* eventNameRejectReasonLabel(EventNameRejectReason reason);
bool preflightProfilerState(NestingStateRejectReason* reason = nullptr);
const char* nestingStateRejectReasonLabel(NestingStateRejectReason reason);
bool preflightChromeTraceExport(ChromeTraceExportRejectReason* reason = nullptr);
const char* chromeTraceExportRejectReasonLabel(ChromeTraceExportRejectReason reason);
bool tryCanLookupEventAt(u32 index, EventLookupRejectReason& outReason);
const char* eventLookupRejectReasonLabel(EventLookupRejectReason reason);
bool tryExportChromeTraceJson(std::string& outJson, ChromeTraceExportRejectReason* reason = nullptr);

// --- deepen additive from deepen-b16-profiler-preflight-guards-b702 ---
bool tryExportChromeTraceJson(std::string& outJson);

// --- deepen additive from deepen-b16-profiler-preflights-acf4 ---
bool isProfilerNestingPreflightOk();
bool isEventLookupPreflightOk(u32 index);
bool isChromeExportPreflightOk();

// --- deepen additive from deepen-b16-profiler-preflights-3f2f ---
struct NestingPreflight {
struct AsyncFlowPreflight {
struct ExportPreflight {
NestingPreflight preflightNesting();
AsyncFlowPreflight preflightBeginAsyncFlow(const char* name);
AsyncFlowPreflight preflightEndAsyncFlow(const char* name);
ExportPreflight preflightExport();
EventLookupPreflight preflightEventAt(u32 index);
EventLookupPreflight preflightLastEvent();

// --- deepen additive from deepen-profiler-b16-guards-10ba ---
bool tryEventPhaseAt(u32 index, EventPhase& outPhase);

// --- deepen additive from deepen-b16-profiler-guards-fb79 ---
bool tryFirstExportableEvent(ProfileEvent& outEvent);
bool tryLastExportableEvent(ProfileEvent& outEvent);

// --- deepen additive from deepen-b16-profiler-guards-5e82 ---
bool tryFindEventByName(const char* name, u32 startIndex, u32& outIndex, ProfileEvent& outEvent);

// --- deepen additive from deepen-b16-profiler-guards-1296 ---
bool preflightChromeTraceNesting(ChromeTraceExportRejectReason* reason = nullptr);

// --- deepen additive from deepen-b16-profiler-guards-3935 ---
bool tryEventAtReverse(u32 reverseIndex, ProfileEvent& outEvent);
bool tryFindEventByScopeId(u32 scopeId, ProfileEvent& outEvent);

// --- deepen additive from deepen-b16-profiler-guards-2ba8 ---
    bool wouldExportEmptyTrace() const { return canExport() && exportableEventCount == 0; }
bool tryFindFirstEventWithPhase(EventPhase phase, ProfileEvent& outEvent);

// --- deepen additive from deepen-b16-profiler-guards-c0f6 ---
bool tryFindFirstEventByPhase(EventPhase phase, ProfileEvent& outEvent);

// --- deepen additive from deepen-b16-profiler-guards-7793 ---
bool wouldIgnoreOrphanAsyncFlowEnd();
bool wouldRecordEventName(const char* name);

// --- deepen additive from deepen-b16-profiler-guards-93c0 ---
struct EventNamePreflight {
EventNamePreflight preflightEventName(const char* name);
AsyncFlowPreflight preflightAsyncFlowBegin(const char* name);
AsyncFlowPreflight preflightAsyncFlowEnd(const char* name);

// --- deepen additive from deepen-b16-profiler-guards-cad0 ---
bool tryEventAtPhase(u32 index, EventPhase expectedPhase, ProfileEvent& outEvent);

// --- deepen additive from deepen-profiler-b16-guards-33c5 ---
bool wouldRecordEvent(const char* name);

// --- deepen additive from deepen-b16-profiler-guards-9145 ---
bool tryFindLastEventByPhase(EventPhase phase, ProfileEvent& outEvent);

// --- deepen additive from deepen-b16-profiler-guards-7722 ---
bool wouldRecordWithName(const char* name);
bool tryEventAtPhase(u32 index, EventPhase phase, ProfileEvent& outEvent);

// --- deepen additive from deepen-b16-profiler-guards-6796 ---
bool tryRecordedEventAt(u32 index, ProfileEvent& outEvent);

// --- deepen additive from deepen-b16-profiler-guards-61a8 ---
bool tryFirstEventOfPhase(EventPhase phase, ProfileEvent& outEvent);
bool tryLastEventOfPhase(EventPhase phase, ProfileEvent& outEvent);
bool tryFindEventByName(const char* name, u32& outIndex);

// --- deepen additive from deepen-b16-profiler-guards-ce9d ---
bool tryFindEventByName(const char* name, ProfileEvent& outEvent);

// --- deepen additive from deepen-b16-profiler-guards-8f82 ---
    ChromeTraceExportRejectReason rejectReason = ChromeTraceExportRejectReason::None;
        return !profilerDisabled && exportableEventCount > 0 && rejectReason == ChromeTraceExportRejectReason::None;
EventNameRejectReason eventNameRejectReason(const char* name);
EventLookupRejectReason eventLookupRejectReason(u32 index);
NestingStateRejectReason nestingStateRejectReason();

// --- deepen additive from deepen-fuse-b16-profiler-e55b ---
bool tryFindLastEventIndexByName(const char* name, u32& outIndex);

// --- deepen additive from deepen-b16-profiler-guards-5b61 ---
EventLookupRejectReason exportableEventLookupRejectReason(u32 index);

// --- deepen additive from deepen-profiler-b16-guards-183a ---
bool tryFindEventIndexByName(const char* name, u32& outIndex);

// --- deepen additive from deepen-profiler-b16-guards-eb78 ---
bool tryFirstFlowStartById(u32 flowId, ProfileEvent& outEvent);
bool tryLastFlowFinishById(u32 flowId, ProfileEvent& outEvent);
