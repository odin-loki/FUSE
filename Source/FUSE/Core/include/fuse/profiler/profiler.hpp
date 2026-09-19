#pragma once

#include <fuse/types.hpp>

#include <string>
#include <type_traits>

namespace fuse::profiler {

/// Sentinel returned by `lastEventIndex()` when the ring buffer has no recorded events.
constexpr u32 kInvalidEventIndex = static_cast<u32>(-1);

/// Ring buffer capacity for CPU profile events (diagnostic only).
constexpr u32 kRingCapacity = 4096u;
/// Ring buffer capacity exposed for editor consumers and export preflight.
constexpr u32 kRingEventCapacity = 4096u;

/// Non-hot-path snapshot of chrome export readiness (does not mutate profiler state).
struct ChromeTraceExportPreflight {
    bool can_export = true;
    bool has_events = false;
    bool buffer_empty = true;
    bool scope_nesting_balanced = true;
    bool flow_nesting_balanced = true;
    bool has_open_async_flows = false;
    u32 event_count = 0;
    u32 frame_index = 0;
};

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
    bool invalidName = false;
    bool canEnter = false;

/// Read-only async-flow begin diagnostics — safe to call before `beginAsyncFlow()`.
struct AsyncFlowBeginPreflight {
    bool canBegin = false;

/// Read-only async-flow end diagnostics — safe to call before `endAsyncFlow()`.
struct AsyncFlowEndPreflight {
    bool wouldUnderflowOpenCount = false;
    bool canEnd = false;

/// Read-only nesting and async-flow diagnostics — safe before scope/flow entry.
struct NestingAsyncFlowPreflight {

    bool isBalanced() const {
        return !hasUnbalancedNesting() && !flowDepthDetached && !crossThreadFlowHandoffPending;

/// Read-only nesting/async-flow diagnostics — safe to call before recording more events.
    bool scopeNestingBalanced = true;
    bool flowNestingBalanced = true;

    bool canBeginScope() const { return true; }
    bool canBeginAsyncFlow() const { return !crossThreadFlowHandoffPending; }
    bool canEndAsyncFlow() const { return openAsyncFlowCount > 0u; }
    bool isNestingHealthy() const {
        return !flowDepthDetached && !crossThreadFlowHandoffPending;
/// Record-path preflight for scope/flow/counter stubs (B1.6 deepen follow-up).
struct ProfilerRecordPreflight {
    bool profiler_enabled = false;
    bool name_valid = false;

    [[nodiscard]] bool can_record_scope() const {
        return profiler_enabled && name_valid;
    [[nodiscard]] bool can_record_async_flow() const {
    [[nodiscard]] bool can_record_counter() const {

/// Chrome export preflight for offline trace dumps (B1.6 deepen follow-up).
struct ProfilerExportPreflight {
    bool has_events = false;
    u32 event_count = 0;
    u32 dropped_event_count = 0;
    bool buffer_full = false;

    [[nodiscard]] bool can_export() const {
        return has_events;

/// True when `name` is non-null and not the empty string.
inline bool isNonEmptyProfileName(const char* name) {
    return name != nullptr && name[0] != '\0';
/// Read-only snapshot of profiler state before chrome JSON export (introspection only).
struct ChromeExportPreflight {
    bool bufferEmpty = true;
    bool hasEvents = false;
    bool canExport = true;

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
u32 droppedEventCount();
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
u32 nestingDepth();

u32 ringBufferCapacity();

bool hasEvents();
bool hasExportableEvents();
bool hasOpenAsyncFlows();
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
bool isValidProfileName(const char* name);
bool isScopeNestingBalanced();
bool isFlowNestingBalanced();
bool hasOpenAsyncFlows();
u32 ringCapacity();
ChromeTraceExportPreflight preflightChromeTraceExport();
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
bool tryEventAt(u32 index, ProfileEvent& out);
const ProfileEvent& lastEvent();
bool tryEventAt(u32 index, ProfileEvent& outEvent);
bool tryLastEvent(ProfileEvent& outEvent);
ProfilerRecordPreflight preflightRecord(const char* name);
ProfilerExportPreflight preflightChromeTraceExport();
void reset();

ChromeTraceExportPreflight preflightChromeTraceExport();
ProfileScopePreflight preflightProfileScope(const char* name);
AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name, u32 flowId);
AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name, u32 flowId);
NestingAsyncFlowPreflight preflightNestingAsyncFlow();
/// Non-mutating export preflight — always reports `canExport=true` for the stub exporter.
ChromeExportPreflight preflightChromeExport();
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

/// Read-only async-flow begin diagnostics (B1.6 deepen).
struct AsyncFlowBeginPreflight {

    bool canBegin() const { return !profiler_disabled && !null_name && !empty_name; }
    bool shouldSkip() const { return !canBegin(); }

/// Read-only async-flow end diagnostics (B1.6 deepen).
struct AsyncFlowEndPreflight {
    bool orphan_end = false;

    bool canEnd() const { return !profiler_disabled && !null_name && !empty_name && !orphan_end; }
    bool shouldSkip() const { return !canEnd(); }

/// Read-only chrome export diagnostics (B1.6 deepen).
struct ChromeTraceExportPreflight {
    u32 event_count = 0;
    u32 open_async_flow_count = 0;
    bool has_events = false;
    bool has_unmatched_flows = false;
    bool would_emit_empty_trace = false;

    bool canExport() const { return true; }
    bool shouldSkip() const { return would_emit_empty_trace; }

ProfileNamePreflight preflightProfileName(const char* name);
ScopeNestingPreflight preflightScopeNesting();
AsyncFlowBeginPreflight preflightAsyncFlowBegin(const char* name);
AsyncFlowEndPreflight preflightAsyncFlowEnd(const char* name);

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
