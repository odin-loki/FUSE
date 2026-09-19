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

/// Why an event name failed validation (B1.6 deepen).
/// Why an event name failed validation — read-only guard for empty/null names.
enum class EventNameRejectReason : u8 {
    None = 0,
    Null,
    Empty,
};

/// Why profiler nesting/async state is unbalanced (B1.6 deepen).
enum class NestingStateRejectReason : u8 {
    UnbalancedScopeNesting,
    UnbalancedFlowNesting,
    OpenAsyncFlows,

/// Why chrome trace export preflight rejected the request (B1.6 deepen).
enum class ChromeTraceExportRejectReason : u8 {
    FlowDepthDetached,

/// Why a guarded chrome trace export preflight rejected the request (B1.6 deepen).
    ProfilerDisabled,
    NoExportableEvents,


/// Why an event lookup preflight rejected the request (B1.6 deepen).
enum class EventLookupRejectReason : u8 {
/// Why an event name was rejected by recording guards (B1.6 deepen).
    None,
    Blank,

/// Why an event index lookup was rejected (B1.6 deepen).
    EmptyBuffer,
    OutOfRange,
    InvalidEvent,
};

/// Why profiler nesting state is unbalanced or detached (B1.6 deepen).

/// Why a strict chrome trace export would be rejected (B1.6 deepen).
    BufferOverflow,
/// Why an event lookup preflight rejected the request.
    None = 0,
    NotExportable,

/// Why profiler nesting/async state is unbalanced.
    ActiveScope,
    ActiveFlowNesting,
    CrossThreadFlowHandoffPending,

/// Why chrome trace safe export preflight rejected the request.
    UnbalancedNesting,

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

/// Read-only empty-name diagnostics — no mutation (B1.6 deepen).
struct EventNamePreflight {
    bool profilerDisabled = false;
    bool nullName = false;
    bool emptyName = false;

    bool canRecord() const { return !profilerDisabled && !nullName && !emptyName; }
};

/// Read-only async-flow diagnostics — no mutation (B1.6 deepen).
struct AsyncFlowPreflight {
    bool profilerDisabled = false;
    bool nullName = false;
    bool emptyName = false;
    bool noOpenFlows = false;

    bool canBegin() const { return !profilerDisabled && !nullName && !emptyName; }
    bool canEnd() const { return canBegin() && !noOpenFlows; }
};

/// Read-only chrome export diagnostics — safe to call before `exportChromeTraceJson()`.
struct ChromeTraceExportPreflight {
    u32 eventCount = 0;
    u32 exportableEventCount = 0;
    u32 totalEventsWritten = 0;
    u32 droppedEventCount = 0;
    u32 nonExportableEventCount = 0;
    u32 frameIndex = 0;
    u32 openAsyncFlowCount = 0;
    u32 activeScopeNestingDepth = 0;
    u32 activeFlowNestingDepth = 0;
    u32 maxScopeNestingDepth = 0;
    u32 maxFlowNestingDepth = 0;
    u32 nonExportableEventCount = 0;
    u32 droppedEventCount = 0;
    u32 firstExportableEventIndex = kInvalidEventIndex;
    u32 lastExportableEventIndex = kInvalidEventIndex;
    bool profilerDisabled = false;
    bool bufferEmpty = false;
    bool ringSaturated = false;
    bool bufferFull = false;
    bool ringBufferWrapped = false;
    u32 remainingEventCapacity = 0;
    u32 rejectedInvalidNameCount = 0;
    u32 orphanAsyncFlowEndCount = 0;
    bool hasRingWrapped = false;
    u32 remainingCapacity = 0;
    bool allEventsExportable = false;
    u32 firstExportableEventIndex = kInvalidEventIndex;
    u32 lastExportableEventIndex = kInvalidEventIndex;
    u32 ringCapacity = 0;
    bool scopeNestingUnbalanced = false;
    bool flowNestingUnbalanced = false;
    bool hasOpenAsyncFlows = false;
    bool flowDepthDetached = false;
    u32 invalidNameEventCount = 0;
    u32 nonExportableEventCount = 0;
    bool ringBufferFull = false;
    bool hasInvalidNameEvents = false;
    bool crossThreadFlowHandoffPending = false;
    bool exportWouldTrimEvents = false;
    bool hasOnlyExportableEvents = false;
    u32 orphanAsyncFlowEndCount = 0;
    bool hasOrphanAsyncFlowEnds = false;
    bool bufferFull = false;
    u32 scopeBeginEventCount = 0;
    u32 scopeEndEventCount = 0;
    u32 asyncFlowStartEventCount = 0;
    u32 asyncFlowFinishEventCount = 0;
    bool hasRejectedInvalidNames = false;
    u32 flowStartEventCount = 0;
    u32 flowFinishEventCount = 0;
    u32 counterEventCount = 0;
    u32 firstEventIndex = kInvalidEventIndex;
    u32 lastEventIndex = kInvalidEventIndex;
    u32 ringCapacity = 0;
    u32 droppedEventCount = 0;
    bool isBufferFull = false;
    u32 invalidEventCount = 0;
    bool hasExportWarnings = false;
    bool needsFlowNestingCleanup = false;
    u32 lastExportableEventIndex = kInvalidEventIndex;
    u32 totalWriteCount = 0;
    u32 remainingEventCapacity = 0;
    bool hasDroppedEvents = false;
    bool hasRingWrapped = false;
    ChromeTraceExportRejectReason rejectReason = ChromeTraceExportRejectReason::None;
    u32 scopeBeginCount = 0;
    u32 scopeEndCount = 0;
    u32 flowStartCount = 0;
    u32 flowFinishCount = 0;
    bool scopePairImbalancedInBuffer = false;
    bool flowPairImbalancedInBuffer = false;
    u32 ignoredAsyncFlowEndCount = 0;
    bool hasIgnoredAsyncFlowEnds = false;
    bool hasUnpairedScopeEvents = false;
    bool hasUnpairedAsyncFlowEvents = false;
    bool scopeBeginEndMismatch = false;
    bool flowStartFinishMismatch = false;
    bool hasActiveProfilingNesting = false;
    u32 danglingFlowBeginCount = 0;
    u32 orphanFlowEndCount = 0;
    bool hasUnpairedFlowEvents = false;
    bool nestingStateConsistent = false;
    u32 firstExportableEventIndex = kInvalidEventIndex;
    bool hasActiveScope = false;
    bool hasActiveAsyncFlowNesting = false;
    bool hasUnbalancedBufferedFlowPairs = false;

    bool canExport() const { return !profilerDisabled; }
    bool hasExportableEvents() const { return exportableEventCount > 0; }
    bool hasNonExportableEvents() const { return nonExportableEventCount > 0; }
    bool hasUnbalancedNesting() const { return scopeNestingUnbalanced || flowNestingUnbalanced; }
    bool canExportWithEvents() const { return canExport() && hasExportableEvents(); }
    bool canExportNonEmptyTrace() const { return canExport() && hasExportableEvents(); }
    bool hasPairedScopeEventsInBuffer() const { return scopeBeginEventCount == scopeEndEventCount; }
    bool hasPairedFlowEventsInBuffer() const { return flowStartEventCount == flowFinishEventCount; }
    bool hasConsistentEventPairsInBuffer() const {
        return hasPairedScopeEventsInBuffer() && hasPairedFlowEventsInBuffer();
    bool canExportSafely() const {
        return canExport() && !hasUnbalancedNesting() && !flowDepthDetached && !crossThreadFlowHandoffPending;
    }
    bool canExportSafely() const {
        return canExport() && !hasUnbalancedNesting() && !flowDepthDetached && !crossThreadFlowHandoffPending
            && !ringBufferFull && !hasInvalidNameEvents;
    bool hasBufferPairImbalance() const { return scopePairImbalancedInBuffer || flowPairImbalancedInBuffer; }
            && !hasBufferPairImbalance() && !hasDroppedEvents && !hasInvalidNameEvents;
            && !ringBufferFull;
            && !hasInvalidNameEvents && !ringBufferFull;
    bool hasEventPairingMismatch() const { return scopeBeginEndMismatch || flowStartFinishMismatch; }
            && !hasEventPairingMismatch() && !ringBufferFull;
    u32 ringCapacity = 0;
    u32 firstExportableEventIndex = 0;
    u32 lastExportableEventIndex = 0;
    bool hasActiveScopeNesting = false;
    bool hasActiveFlowNesting = false;
    bool hasNestedAsyncFlowContext = false;

    u32 scopeBeginEventCount = 0;
    u32 counterEventCount = 0;
    u32 flowStartEventCount = 0;
    bool hasActiveScopes = false;

            && !hasInvalidNameEvents;
    bool hasUnpairedFlowEventsInBuffer() const { return hasUnpairedFlowEvents; }
    bool isNestingStateConsistent() const { return nestingStateConsistent; }
            && !hasUnpairedFlowEvents;
            && !hasUnbalancedFlowPairsInBuffer;
    }
    u32 unbalancedFlowPairCount = 0;
    bool hasUnbalancedFlowPairsInBuffer = false;
};

/// Read-only name lookup diagnostics — safe to call before indexing the ring buffer by name.
struct EventNameLookupPreflight {
    bool nameValid = false;
    bool bufferEmpty = false;
    u32 matchCount = 0;
    u32 firstMatchIndex = kInvalidEventIndex;
    u32 lastMatchIndex = kInvalidEventIndex;

    bool canLookup() const { return nameValid && !bufferEmpty; }
    bool hasMatches() const { return matchCount > 0; }

/// Read-only async-flow id lookup diagnostics — safe before correlating chrome `ph:"s"` / `ph:"f"` pairs.
struct FlowIdLookupPreflight {
    bool flowIdValid = false;
    u32 flowStartCount = 0;
    u32 flowFinishCount = 0;
    u32 firstFlowEventIndex = kInvalidEventIndex;
    u32 lastFlowEventIndex = kInvalidEventIndex;
    bool pairBalanced = true;

    bool canLookup() const { return flowIdValid && !bufferEmpty; }
    bool hasFlowEvents() const { return flowStartCount + flowFinishCount > 0; }
    bool isPairBalanced() const { return pairBalanced; }

/// Read-only nesting/async-flow consistency snapshot for export and diagnostics.
struct NestingConsistencyPreflight {
    bool scopeNestingBalanced = true;
    bool flowNestingBalanced = true;
    bool flowDepthAttached = true;
    bool crossThreadFlowHandoffPending = false;
    u32 openAsyncFlowCount = 0;
    u32 activeScopeNestingDepth = 0;
    u32 activeFlowNestingDepth = 0;

    bool isConsistent() const {
        return scopeNestingBalanced && flowNestingBalanced && flowDepthAttached && !crossThreadFlowHandoffPending;
    }
    bool canExportNonEmpty() const { return canExport() && hasExportableEvents(); }
    bool hasExportWarnings() const {
        return hasInvalidNameEvents || hasUnpairedScopeEvents || hasUnpairedAsyncFlowEvents;
    bool canExportCleanly() const { return canExportSafely() && !hasExportWarnings(); }

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
/// Preflight for profile scope/flow/counter name guards (B1.6 deepen).
struct ProfileNamePreflight {
    bool null_name = true;
    bool empty_name = false;

    [[nodiscard]] bool can_record() const { return !null_name && !empty_name; }
    [[nodiscard]] bool should_skip() const { return !can_record(); }

/// Preflight for entering a CPU profile scope (B1.6 deepen).
struct ScopePreflight {
    bool profiler_disabled = false;

    [[nodiscard]] bool can_enter() const { return !profiler_disabled && !null_name && !empty_name; }
    [[nodiscard]] bool should_skip() const { return !can_enter(); }


/// Preflight for active scope nesting depth introspection (B1.6 deepen).
struct ScopeNestingPreflight {
    u32 current_depth = 0;
    u32 max_observed_depth = 0;

    [[nodiscard]] bool is_at_root() const { return current_depth == 0u; }

/// Preflight for async flow begin guards (B1.6 deepen).

    [[nodiscard]] bool can_begin() const { return !profiler_disabled && !null_name && !empty_name; }
    [[nodiscard]] bool should_skip() const { return !can_begin(); }

/// Preflight for async flow end guards — surfaces orphan finish risk (B1.6 deepen).



    bool no_open_flows = true;
    u32 open_flow_count = 0;

    [[nodiscard]] bool would_orphan() const { return no_open_flows; }
    [[nodiscard]] bool can_end() const { return !profiler_disabled && !null_name && !empty_name && !no_open_flows; }
    [[nodiscard]] bool should_skip() const { return !can_end(); }

/// Preflight for counter sample guards (B1.6 deepen).
struct CounterSamplePreflight {

    [[nodiscard]] bool can_sample() const { return !profiler_disabled && !null_name && !empty_name; }
    [[nodiscard]] bool should_skip() const { return !can_sample(); }

/// Preflight for chrome://tracing export — buffer state and skipped null-name events (B1.6 deepen).
    bool buffer_empty = true;



    u32 null_name_skip_count = 0;
    u32 frame_index = 0;

    [[nodiscard]] bool will_emit_events() const {
        return event_count > 0u && null_name_skip_count < event_count;

    /// Export is always callable — even when the buffer is empty.
    [[nodiscard]] bool exportable() const { return true; }

/// Preflight for ring-buffer event lookup — safe sentinel fallback (B1.6 deepen).
struct EventLookupPreflight {
    bool out_of_range = true;
    u32 requested_index = 0;

    [[nodiscard]] bool can_lookup() const { return !buffer_empty && !out_of_range; }
    [[nodiscard]] bool should_use_sentinel() const { return !can_lookup(); }




/// Read-only chrome export diagnostics — no mutation (B1.6 deepen).
    bool unbalancedScopeNesting = false;
    bool openAsyncFlows = false;

    bool canExport() const { return !bufferEmpty; }

/// Why chrome export preflight would flag a non-ideal trace (B1.6 deepen — export guard).
enum class ChromeTraceExportRejectReason : u8 {
    None,
    EmptyBuffer,
    UnbalancedScopeNesting,
    UnbalancedFlowNesting,
    OpenAsyncFlows,
    BufferFull,

/// Read-only chrome export diagnostics — no mutation (B1.6 deepen — export preflight).
    bool emptyBuffer = false;
    bool bufferFull = false;
    bool unbalancedFlowNesting = false;
    u32 scopeNestingDepth = 0;
    u32 flowNestingDepth = 0;
    u32 openFlowCount = 0;

    bool canExport() const { return !emptyBuffer; }
    bool hasWarnings() const {
        return bufferFull || unbalancedScopeNesting || unbalancedFlowNesting || openAsyncFlows;
    bool isNestingClean() const {
        return !unbalancedScopeNesting && !unbalancedFlowNesting && !openAsyncFlows;

    bool bufferTruncated = false;

    /// Export always emits valid JSON; preflight flags incomplete traces for tooling.
    bool canExport() const { return true; }

    /// True when scope/flow nesting is balanced and no async flows remain open.
    bool isTraceComplete() const {
        return !unbalancedScopeNesting && !unbalancedFlowNesting && !hasOpenAsyncFlows;


/// Read-only scope nesting diagnostics (B1.6 deepen — nesting guard).
struct NestingPreflight {
    u32 scopeDepth = 0;
    u32 flowDepth = 0;
    bool scopeBalanced = true;
    bool flowBalanced = true;
    bool hasOpenFlows = false;

    bool isBalanced() const { return scopeBalanced && flowBalanced; }

/// Read-only async-flow begin/end diagnostics (B1.6 deepen — async-flow guard).
struct AsyncFlowPreflight {
    bool emptyName = false;
    bool disabled = false;
    bool orphanEnd = false;

    bool canBegin() const { return !emptyName && !disabled; }
    bool canEnd() const { return !emptyName && !disabled && !orphanEnd; }

/// Read-only profile-scope entry diagnostics (B1.6 deepen — empty-name guard).

    bool canEnter() const { return !emptyName && !disabled; }

/// Read-only chrome export diagnostics (B1.6 deepen — export guard).
struct ExportPreflight {
    u32 bufferedEventCount = 0;
    u32 skippedInvalidNames = 0;

    bool canExport() const { return !disabled; }
    bool hasExportableEvents() const { return exportableEventCount > 0u; }

/// Read-only event lookup diagnostics (B1.6 deepen — event-lookup preflight).
    bool indexOutOfRange = false;
    bool invalidEvent = false;
    u32 index = 0;

    bool canLookup() const { return !emptyBuffer && !indexOutOfRange; }
    bool canReadValidEvent() const { return canLookup() && !invalidEvent; }


    bool canExportNonEmptyTrace() const { return canExport() && hasExportableEvents(); }
    bool hasBalancedScopeEventsInBuffer() const { return scopeBeginEventCount == scopeEndEventCount; }
    bool hasBalancedAsyncFlowEventsInBuffer() const {
        return asyncFlowStartEventCount == asyncFlowFinishEventCount;
    bool isBufferStructurallyBalanced() const {
        return hasBalancedScopeEventsInBuffer() && hasBalancedAsyncFlowEventsInBuffer();
    bool hasNestingWarnings() const { return hasUnbalancedNesting() || flowDepthDetached; }
    bool hasDroppedEvents() const { return droppedEventCount > 0u; }
        return hasUnbalancedNesting() || hasOpenAsyncFlows || flowDepthDetached || hasDroppedEvents()
               || nonExportableEventCount > 0u;
    bool hasNonExportableEvents() const { return nonExportableEventCount > 0; }
    bool isExportReady() const { return canExport() && hasExportableEvents(); }
    bool canExportCleanTrace() const { return canExport() && !hasUnbalancedNesting() && !flowDepthDetached; }
    bool wouldExportEmptyTrace() const { return canExport() && exportableEventCount == 0; }
    bool hasBufferedScopeImbalance() const { return scopeBeginEventCount != scopeEndEventCount; }
    bool hasBufferedFlowImbalance() const { return flowStartEventCount != flowFinishEventCount; }
        return canExport() && !flowDepthDetached && !hasBufferedFlowImbalance();
    bool canExportTrace() const { return canExport() && hasExportableEvents(); }
    bool hasExportWarnings() const { return hasUnbalancedNesting() || flowDepthDetached; }

/// Read-only scope/async nesting diagnostics — safe before mutating profile slices.
struct NestingStatePreflight {
    u32 openAsyncFlows = 0;
    u32 maxScopeDepth = 0;
    u32 maxFlowDepth = 0;
    bool flowDepthDetached = false;
    bool hasOpenAsyncFlows = false;

    bool isClean() const {
        return scopeBalanced && flowBalanced && !flowDepthDetached && !hasOpenAsyncFlows;

/// Read-only async-flow begin diagnostics — mirrors `beginAsyncFlow()` guards.
    bool profilerDisabled = false;

    bool canBegin() const { return !profilerDisabled && !emptyName; }

/// Read-only async-flow end diagnostics — mirrors `endAsyncFlow()` guards.

    bool canEnd() const { return !profilerDisabled && !emptyName && !orphanEnd; }
    bool hasInvalidEventsInBuffer() const { return invalidEventCount > 0; }
        return hasUnbalancedNesting() || hasOpenAsyncFlows || flowDepthDetached || bufferFull;
    bool canSafelyExport() const {
        return canExport() && !hasUnbalancedNesting() && !flowDepthDetached && !hasOpenAsyncFlows;
    bool canExportCleanly() const {
        return canExport() && !hasUnbalancedNesting() && !flowDepthDetached;
        return hasUnbalancedNesting() || hasOpenAsyncFlows || flowDepthDetached || invalidNameEventCount > 0u;
    bool canExportWithContent() const { return canExport() && hasExportableEvents(); }
    bool readyForExport() const { return canExport() && (bufferEmpty || hasExportableEvents()); }
    bool canExportClean() const {
        return canExport() && !hasUnbalancedNesting() && !flowDepthDetached && !bufferFull;
    bool isExportRecommended() const {
        return canExport() && hasExportableEvents() && !hasUnbalancedNesting() && !flowDepthDetached;
    bool isNestingClean() const { return !hasUnbalancedNesting() && !flowDepthDetached; }
    bool hasValidEventIndices() const {
        return firstEventIndex != kInvalidEventIndex && lastEventIndex != kInvalidEventIndex;
    bool canExportStrict() const {
        return !profilerDisabled && exportableEventCount > 0 && rejectReason == ChromeTraceExportRejectReason::None;



        return canExportSafely() && !hasInvalidNameEvents && !ringBufferFull && droppedEventCount == 0u;
        return canExportSafely() && !hasInvalidNameEvents && !ringBufferFull && !hasUnbalancedBufferedFlowPairs;
    bool canExportSafelyWithConsistentBuffer() const {
        return canExportSafely() && hasConsistentEventPairsInBuffer();
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

/// Returns true for non-null, non-empty scope/flow/counter names.
bool isValidEventName(const char* name);

bool enabled();
void setEnabled(bool enabled);

/// True when `name` is non-null and non-empty — shared guard for scopes, flows, and counters.
bool isValidEventName(const char* name);

/// True when a profiler label/track name is non-null and non-empty.

/// Preflight guards for record entry points — false when disabled or name is invalid.
bool canRecordScope(const char* name);
bool canBeginAsyncFlow(const char* name);
bool canEndAsyncFlow(const char* name);
bool canSampleCounter(const char* track);

void beginFrame();
void endFrame();

u32 frameIndex();
u32 eventCount();
u32 ringCapacity();
u32 droppedEventCount();
u32 ringBufferCapacity();
u32 exportableEventCount();
u32 totalEventsWritten();
bool isRingSaturated();
u32 maxNestingDepth();
u32 nestingDepth();
u32 maxFlowNestingDepth();
u32 scopeNestingDepth();
u32 flowNestingDepth();
u32 openAsyncFlowCount();
u32 orphanAsyncFlowEndCount();
bool hasOpenAsyncFlows();
u32 orphanAsyncFlowEndCount();
bool hasOrphanAsyncFlowEnds();
u32 rejectedInvalidNameCount();
bool hasRejectedInvalidNames();
u32 remainingEventCapacity();
bool hasActiveScope();
bool hasActiveFlowDepth();
bool isScopeNestingBalanced();
bool isFlowNestingBalanced();
bool hasUnbalancedNesting();
bool isFlowDepthDetached();
bool isCrossThreadFlowHandoffPending();
NestingAsyncFlowPreflight preflightNestingAndAsyncFlow();
u32 nestingDepth();
bool needsFlowNestingCleanup();
bool wouldIgnoreOrphanAsyncFlowEnd();
bool wouldRecordEvent(const char* name);
void reconcileDetachedFlowNesting();
bool hasActiveScope();
bool hasActiveAsyncFlowNesting();
u32 flowDepthMismatch();
bool reconcileDetachedFlowDepth();
bool canEndAsyncFlow();
void reconcileDetachedFlowNestingDepth();
bool isScopePairBalancedInBuffer();
bool isFlowPairBalancedInBuffer();
/// True when scope/flow nesting is balanced and no async flows remain open.
bool isProfilerGuardStateBalanced();
u32 ignoredAsyncFlowEndCount();
bool hasIgnoredAsyncFlowEnds();
u32 orphanAsyncFlowEndCount();
bool hasUnpairedScopeEventsInBuffer();
bool hasUnpairedAsyncFlowEventsInBuffer();
u32 scopeBeginEndEventDelta();
u32 asyncFlowStartFinishEventDelta();
void reconcilePendingFlowHandoff();
bool hasActiveScopeNesting();
bool hasActiveFlowNesting();
bool hasActiveProfilingNesting();
bool hasNestedAsyncFlowContext();
bool hasActiveScopes();
bool isFlowNestingConsistent();

/// True when `name` is non-null and contains at least one character (B1.6 deepen).
bool hasEvents();
bool isBufferEmpty();
bool isBufferFull();
u32 remainingEventCapacity();
bool isEventIndexValid(u32 index);
bool isBlankEventName(const char* name);
bool isFirstEventIndex(u32 index);
bool isLastEventIndex(u32 index);
u32 countEventsWithPhase(EventPhase phase);
bool isNullEventName(const char* name);
bool isEmptyEventName(const char* name);
bool isInvalidEventIndex(u32 index);
bool isValidEventName(const char* name);

/// Read-only chrome export diagnostics — no mutation (B1.6 deepen).
struct ChromeExportPreflight {
    bool bufferEmpty = false;
    bool unbalancedScopeNesting = false;
    bool unbalancedFlowNesting = false;
    bool hasOpenAsyncFlows = false;

    /// Export always produces valid JSON; preflight surfaces state warnings only.
    bool canExport() const { return true; }

    bool hasStateWarnings() const {
/// True when `name` is non-null and non-empty — shared guard for scopes, flows, and counters.
bool isValidProfileName(const char* name);

/// Read-only chrome trace export diagnostics — no mutation (B1.6 deepen).
struct ChromeTraceExportPreflight {
    bool profilerDisabled = false;
    bool emptyBuffer = false;
    bool bufferFull = false;


    bool hasCorrelationWarnings() const {
        return unbalancedScopeNesting || unbalancedFlowNesting || hasOpenAsyncFlows;
    }
};

ChromeExportPreflight preflightChromeExport();

u32 ringBufferCapacity();
bool wouldRecordEventName(const char* name);
bool isNullOrEmptyEventName(const char* name);
bool wouldRecordWithName(const char* name);
bool tryValidateEventName(const char* name, EventNameRejectReason& outReason);
EventNameRejectReason eventNameRejectReason(const char* name);
u32 rejectedInvalidNameCount();
bool hasRejectedInvalidNames();
bool isValidProfileEvent(const ProfileEvent& event);
bool canRecordEvent(const char* name);
bool canBeginAsyncFlow(const char* name);
bool canEndAsyncFlow(const char* name);
bool isEmptyProfileEvent(const ProfileEvent& event);
u32 exportableEventCount();
u32 invalidEventCount();
bool isExportableProfileEvent(const ProfileEvent& event);
u32 invalidNameEventCount();
u32 nonExportableEventCount();
bool isEventExportable(u32 index);
bool tryEventPhaseAt(u32 index, EventPhase& outPhase);
u32 totalEventsWritten();
bool hasRingWrapped();
u32 exportableFirstEventIndex();
u32 exportableLastEventIndex();
u32 countEventsOfPhase(EventPhase phase);
u32 firstEventIndexOfPhase(EventPhase phase);
u32 lastEventIndexOfPhase(EventPhase phase);
bool isEventAtPhase(u32 index, EventPhase phase);
EventLookupRejectReason eventLookupRejectReason(u32 index);
bool tryCanLookupEventAt(u32 index, EventLookupRejectReason& outReason);
bool tryExportableEventAt(u32 index, ProfileEvent& outEvent);
bool tryFirstExportableEvent(ProfileEvent& outEvent);
bool tryLastExportableEvent(ProfileEvent& outEvent);
u32 firstEventIndex();
u32 lastEventIndex();
u32 countEventsByPhase(EventPhase phase);
u32 findFirstEventIndexOfPhase(EventPhase phase);
u32 firstExportableEventIndex();
u32 lastExportableEventIndex();
u32 lastEventIndexByPhase(EventPhase phase);
u32 findFirstEventIndexByPhase(EventPhase phase);
u32 findLastEventIndexByPhase(EventPhase phase);
u32 findFirstEventIndexByName(const char* name);
bool tryFindEventByName(const char* name, ProfileEvent& outEvent);
bool tryFirstEventOfPhase(EventPhase phase, ProfileEvent& outEvent);
const ProfileEvent& emptyProfileEvent();
const ProfileEvent& eventAt(u32 index);
const char* eventNameAt(u32 index);
bool tryEventAt(u32 index, ProfileEvent& outEvent);
bool tryEventAtPhase(u32 index, EventPhase expectedPhase, ProfileEvent& outEvent);
bool tryRecordedEventAt(u32 index, ProfileEvent& outEvent);
bool tryFirstEvent(ProfileEvent& outEvent);
bool tryLastEvent(ProfileEvent& outEvent);
bool tryFindLastEventByPhase(EventPhase phase, ProfileEvent& outEvent);
bool tryEventAtPhase(u32 index, EventPhase phase, ProfileEvent& outEvent);
bool tryFindFirstEventByPhase(EventPhase phase, ProfileEvent& outEvent);
bool tryFindFirstEventByName(const char* name, ProfileEvent& outEvent);
bool tryLastEventOfPhase(EventPhase phase, ProfileEvent& outEvent);
bool tryFindEventByName(const char* name, u32& outIndex);
const ProfileEvent& lastEvent();
bool canEndAsyncFlow();
bool wouldIgnoreOrphanAsyncFlowEnd();
void reset();
void reconcileDetachedFlowDepth();

EventNamePreflight preflightEventName(const char* name);
AsyncFlowPreflight preflightAsyncFlowBegin(const char* name);
AsyncFlowPreflight preflightAsyncFlowEnd(const char* name);
ChromeTraceExportPreflight preflightChromeTraceExport();
bool canExportChromeTrace();
bool isNestingBalanced();

/// Snapshot of scope/async nesting and open-flow guard state (read-only introspection).
struct NestingIntrospection {
    u32 scopeDepth = 0;
    u32 flowDepth = 0;
    u32 maxScopeDepth = 0;
    u32 maxFlowDepth = 0;
    u32 openAsyncFlowCount = 0;
    bool scopeBalanced = true;
    bool flowBalanced = true;
NestingIntrospection nestingIntrospection();

/// Returns true when scope, flow, and open-async-flow guards are all clean.
bool isNestingStateClean();

/// Read-only chrome export diagnostics — no mutation.
    u32 eventCount = 0;
    u32 frameIndex = 0;

    bool isClean() const {
        return !unbalancedScopeNesting && !unbalancedFlowNesting && !hasOpenAsyncFlows;








/// True when scope/flow nesting is balanced and no async flows remain open.
bool isProfilerGuardStateBalanced();

/// Preflight for scope/async-flow/counter name strings — rejects null and empty names.
bool isGuardStateBalanced();

/// Preflight for scope/flow/counter names before recording (rejects null and empty strings).
bool isEventNameValid(const char* name);
bool isProfilerNestingPreflightOk();

/// True when `name` is non-null and non-empty (B1.6 deepen — empty-name guard).

NestingPreflight preflightNesting();
ProfileScopePreflight preflightProfileScope(const char* name);
AsyncFlowPreflight preflightBeginAsyncFlow(const char* name);
AsyncFlowPreflight preflightEndAsyncFlow(const char* name);
ExportPreflight preflightExport();
EventLookupPreflight preflightEventAt(u32 index);
EventLookupPreflight preflightLastEvent();

/// Convenience guards mirroring preflight predicates (B1.6 deepen).
bool canEnterProfileScope(const char* name);
bool canLookupEventAt(u32 index);
bool isFlowOpenCountAttached();
bool hasActiveScopes();
bool hasActiveFlows();
u32 orphanAsyncFlowEndCount();
bool hasResidualFlowNestingDepth();

bool hasEvents();
bool hasExportableEvents();
bool hasOpenAsyncFlows();
bool isScopeNestingBalanced();
bool isFlowNestingBalanced();
bool isBufferEmpty();

bool isBufferFull();
bool hasRingBufferWrapped();
u32 totalWriteCount();
u32 droppedEventCount();
bool isEventIndexValid(u32 index);
/// True for null, empty, or whitespace-only names — diagnostic only; does not affect recording guards.
bool isValidFlowId(u32 flowId);
bool isFlowPhaseEvent(const ProfileEvent& event);
bool eventNameMatches(const ProfileEvent& event, const char* name);
bool eventMatchesFlowId(const ProfileEvent& event, u32 flowId);
const char* eventNameRejectReasonLabel(EventNameRejectReason reason);
bool wouldRecordEvent(const char* name);
bool canSampleCounter(const char* track);
bool isWhitespaceOnlyEventName(const char* name);
bool isBlankEventName(const char* name);
bool isEmptyEventName(const char* name);
/// True for null, empty, or whitespace-only names — does not affect recording guards.
bool isFlowEventPhase(EventPhase phase);
bool eventMatchesName(const ProfileEvent& event, const char* name);
bool isValidProfileEvent(const ProfileEvent& event);
bool isProfileEventSentinel(const ProfileEvent& event);
u32 invalidNameEventCount();
bool hasInvalidNameEvents();
u32 exportableEventCount();
bool hasNonExportableEvents();
bool isEventExportable(u32 index);
u32 countEventsWithPhase(EventPhase phase);
u32 findFirstEventIndexWithPhase(EventPhase phase);
u32 exportableFirstEventIndex();
u32 exportableLastEventIndex();
u32 findFirstEventIndex(EventPhase phase);
u32 findLastEventIndex(EventPhase phase);
bool tryFindEventByName(const char* name, u32 startIndex, u32& outIndex, ProfileEvent& outEvent);
bool tryValidateEventName(const char* name, EventNameRejectReason& outReason);
const char* eventNameRejectReasonLabel(EventNameRejectReason reason);
bool isProfilerStateBalanced();
bool preflightProfilerState(NestingStateRejectReason* reason = nullptr);
const char* nestingStateRejectReasonLabel(NestingStateRejectReason reason);
bool tryCanLookupEventAt(u32 index, EventLookupRejectReason& outReason);
const char* eventLookupRejectReasonLabel(EventLookupRejectReason reason);
u32 countEventsByPhase(EventPhase phase);
u32 findFirstEventIndexByPhase(EventPhase phase);
bool tryFindFirstEventByPhase(EventPhase phase, ProfileEvent& outEvent);
u32 firstEventIndex();
u32 ringCapacity();
/// True when `name` is non-null and not an empty C string — shared guard for scopes, flows, and counters.
[[nodiscard]] inline bool isValidProfileName(const char* name) {
    return name != nullptr && name[0] != '\0';
}
bool preflightChromeTraceExport(ChromeTraceExportRejectReason* reason = nullptr);
const char* chromeTraceExportRejectReasonLabel(ChromeTraceExportRejectReason reason);
bool isValidProfilerName(const char* name);
bool isEventLookupPreflightOk(u32 index);
u32 lastEventIndex();
u32 findLastEventIndexByPhase(EventPhase phase);
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
bool hasOrphanAsyncFlowEnds();
bool isValidChromeTraceExport(const std::string& json);
bool tryFindFirstEventWithPhase(EventPhase phase, ProfileEvent& outEvent);
u32 droppedEventCount();
u32 firstExportableEventIndex();
u32 lastExportableEventIndex();
EventLookupRejectReason eventLookupRejectReason(u32 index);
EventLookupRejectReason exportableEventLookupRejectReason(u32 index);
bool tryFindEventIndexByName(const char* name, u32& outIndex);
u32 findFirstEventIndexByScopeId(u32 scopeId);
u32 countEventsByScopeId(u32 scopeId);
bool hasScopeBeginEndMismatch();
bool hasFlowStartFinishMismatch();
bool hasEventsByPhase(EventPhase phase);
bool hasDroppedEvents();
u32 orphanAsyncFlowEndCount();
bool hasFlowStartEvent(u32 flowId);
bool hasFlowFinishEvent(u32 flowId);
bool isFlowPairRecorded(u32 flowId);
u32 countDanglingFlowBegins();
u32 countOrphanFlowEnds();
bool isFlowPairingConsistent();
bool isNestingStateConsistent();
u32 countFlowStartsById(u32 flowId);
u32 countFlowFinishesById(u32 flowId);
bool isAsyncFlowPairBalanced(u32 flowId);
bool tryFirstEventByName(const char* name, ProfileEvent& outEvent);
bool tryLastEventByName(const char* name, ProfileEvent& outEvent);
bool tryFirstFlowEventById(u32 flowId, ProfileEvent& outEvent);
bool tryLastFlowEventById(u32 flowId, ProfileEvent& outEvent);
u32 countBufferedFlowStartsByFlowId(u32 flowId);
u32 countBufferedFlowFinishesByFlowId(u32 flowId);
bool isBufferedFlowIdBalanced(u32 flowId);
bool hasUnbalancedBufferedFlowPairs();
const ProfileEvent& emptyProfileEvent();
const ProfileEvent& eventAt(u32 index);
const char* eventNameAt(u32 index);
bool peekEventAt(u32 index, ProfileEvent& outEvent);
bool tryEventAt(u32 index, ProfileEvent& outEvent);
bool tryEventAtPhase(u32 index, EventPhase phase, ProfileEvent& outEvent);
bool tryExportableEventAt(u32 index, ProfileEvent& outEvent);
bool tryEventPhaseAt(u32 index, EventPhase& outPhase);
bool tryFindFirstEventByPhase(EventPhase phase, ProfileEvent& outEvent);
bool tryFindLastEventByPhase(EventPhase phase, ProfileEvent& outEvent);
bool tryFindFirstEventByName(const char* name, ProfileEvent& outEvent);
bool tryFindLastEventByName(const char* name, ProfileEvent& outEvent);
bool tryFirstExportableEvent(ProfileEvent& outEvent);
bool tryLastExportableEvent(ProfileEvent& outEvent);
bool tryFindFirstEventByFlowId(u32 flowId, ProfileEvent& outEvent);
bool tryFindLastEventByFlowId(u32 flowId, ProfileEvent& outEvent);
bool tryFirstEvent(ProfileEvent& outEvent);
bool tryLastEvent(ProfileEvent& outEvent);
bool tryFirstEventByName(const char* name, ProfileEvent& outEvent);
bool tryLastEventByName(const char* name, ProfileEvent& outEvent);
bool tryFirstFlowEvent(u32 flowId, ProfileEvent& outEvent);
bool tryLastFlowEvent(u32 flowId, ProfileEvent& outEvent);
bool tryExportableFirstEvent(ProfileEvent& outEvent);
bool tryExportableLastEvent(ProfileEvent& outEvent);
bool tryFindFirstFlowEvent(u32 flowId, ProfileEvent& outEvent);
bool tryFindLastFlowEvent(u32 flowId, ProfileEvent& outEvent);
bool tryEventAt(u32 index, ProfileEvent& out);
/// Safe ring-buffer lookup — returns false and clears `outEvent` when the index is invalid.
bool isLastEventIndexValid();
bool tryFirstExportableEvent(ProfileEvent& outEvent);
bool tryLastExportableEvent(ProfileEvent& outEvent);
bool tryEventAtReverse(u32 reverseIndex, ProfileEvent& outEvent);
u32 findEventIndex(EventPhase phase, u32 startIndex = 0u);
bool tryFindEventByScopeId(u32 scopeId, ProfileEvent& outEvent);
bool tryFindFirstEventIndexByPhase(EventPhase phase, u32& outIndex);
bool tryFindLastEventIndexByPhase(EventPhase phase, u32& outIndex);
bool tryFindFirstEventIndexByName(const char* name, u32& outIndex);
bool tryFindLastEventIndexByName(const char* name, u32& outIndex);
bool tryFindFirstEventByName(const char* name, ProfileEvent& outEvent);
bool tryFindLastEventByName(const char* name, ProfileEvent& outEvent);
bool tryFirstFlowStartById(u32 flowId, ProfileEvent& outEvent);
bool tryLastFlowFinishById(u32 flowId, ProfileEvent& outEvent);
bool tryFirstEventByFlowId(u32 flowId, ProfileEvent& outEvent);
bool tryLastEventByFlowId(u32 flowId, ProfileEvent& outEvent);
bool tryFindFirstEventIndexByFlowId(u32 flowId, u32& outIndex);
bool tryFindFirstEventByFlowId(u32 flowId, ProfileEvent& outEvent);
bool tryFindLastEventByFlowId(u32 flowId, ProfileEvent& outEvent);
const ProfileEvent& lastEvent();
ProfilerRecordPreflight preflightRecord(const char* name);
ProfilerExportPreflight preflightChromeTraceExport();
u32 remainingEventCapacity();
bool hasDroppedEvents();
bool hasRingWrapped();
NestingStateRejectReason nestingStateRejectReason();
void reset();

NestingStatePreflight preflightNestingState();
AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name);
AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name);
NestingStateRejectReason nestingStateRejectReason();
const char* nestingStateRejectReasonLabel(NestingStateRejectReason reason);
ChromeTraceExportRejectReason chromeTraceExportRejectReason();
const char* chromeTraceExportRejectReasonLabel(ChromeTraceExportRejectReason reason);
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
/// Read-only chrome export diagnostics — inspect buffer and nesting guard state before export.
    bool profilerEnabled = false;
    bool hasExportableEvents = false;
    bool bufferEmpty = true;
    bool hasOpenAsyncFlows = false;
    bool scopeNestingBalanced = true;
    bool flowNestingBalanced = true;
    u32 eventCount = 0;
    u32 exportableEventCount = 0;
    u32 frameIndex = 0;

    [[nodiscard]] bool canExport() const { return profilerEnabled; }

/// True when `name` is non-null and non-empty (B1.6 deepen).
[[nodiscard]] bool isUsableProfileName(const char* name);
[[nodiscard]] ProfileNamePreflight preflightProfileName(const char* name);
[[nodiscard]] ScopePreflight preflightScope(const char* name);
[[nodiscard]] ScopeNestingPreflight preflightScopeNesting();
[[nodiscard]] AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name);
[[nodiscard]] AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name);
[[nodiscard]] CounterSamplePreflight preflightCounterSample(const char* track);
[[nodiscard]] ChromeTraceExportPreflight preflightChromeTraceExport();
[[nodiscard]] EventLookupPreflight preflightEventLookup(u32 index);
[[nodiscard]] bool canLookupEventAt(u32 index);
[[nodiscard]] bool tryEventAt(u32 index, const ProfileEvent*& event_out);
/// Safe lookup stub — returns false when `index` is out of range; `outEvent` points at the empty sentinel on failure.
bool tryEventAt(u32 index, const ProfileEvent*& outEvent);


/// Nesting and async-flow guard snapshot for editor panels and export preflight.
struct ProfilerGuardPreflight {
bool hasLastEvent();
u32 droppedEventCount();

/// Read-only scope/async nesting diagnostics — no mutation (B1.6 deepen).
struct NestingStatePreflight {
    u32 scopeDepth = 0;
    u32 flowDepth = 0;
    u32 openAsyncFlows = 0;
    u32 maxScopeDepth = 0;
    u32 maxFlowDepth = 0;
    bool hasOpenScopes = false;
    /// True when at least one async flow begin is unmatched by a finish on this thread.
    bool canEndAsyncFlow = false;

    [[nodiscard]] bool hasUnmatchedAsyncFlows() const { return openAsyncFlows > 0u; }

[[nodiscard]] ProfilerGuardPreflight preflightGuardState();
[[nodiscard]] bool hasOpenScopes();
[[nodiscard]] bool hasOpenAsyncFlows();

/// Chrome export preflight — introspection only; export remains valid even when the buffer is empty.
    bool canExport = true;
    /// True when async flow begins were not paired before export (diagnostic only).
    bool hasUnmatchedAsyncFlows = false;

    [[nodiscard]] bool hasEventsToExport() const { return eventCount > 0u; }


    bool hasUnbalancedAsyncFlows = false;

    bool isBalanced() const { return !hasUnbalancedAsyncFlows && scopeDepth == 0u && flowDepth == 0u; }

NestingStatePreflight preflightNestingState();

/// Read-only async-flow begin diagnostics — no mutation (B1.6 deepen).
    bool profilerDisabled = false;
    bool emptyName = false;

    bool canBegin() const { return !profilerDisabled && !emptyName; }

AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name);

/// Read-only async-flow end diagnostics — no mutation (B1.6 deepen).
    bool orphanEnd = false;

    bool canEnd() const { return !profilerDisabled && !emptyName && !orphanEnd; }

AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name);

/// Read-only chrome export diagnostics — no mutation (B1.6 deepen).
struct ChromeExportPreflight {
    bool bufferEmpty = false;
    u32 skippedInvalidNameCount = 0;
    u32 droppedEventCount = 0;

    bool canExport() const { return !profilerDisabled; }

/// Introspection stubs for guard state — non-zero when scopes or async flows are open on this thread.
bool hasOpenScopes();
bool hasOpenAsyncFlows();

/// Export preflight stubs — inspect buffer/export readiness without emitting chrome JSON.
u32 exportableEventCount();
bool isExportEmpty();
bool canExportChromeTrace();

/// Non-mutating chrome export predicate — true when export will emit trace events.

/// Read-only chrome export preflight — diagnoses empty buffer and guard-state warnings.
ChromeExportPreflight preflightChromeTraceExport();
ChromeTraceExportRejectReason chromeTraceExportRejectReason();
/// Non-mutating chrome export predicate — same guards as `preflightChromeTraceExport`.

/// Export preflights — export is always safe to invoke; these diagnose content/readiness.
bool hasExportableEvents();
/// Export preflights — true when chrome JSON would include at least one trace event.
/// True when at least one buffered event has a valid name for chrome export.
/// True when chrome export would emit zero trace events (empty buffer or no valid names).
bool isChromeTraceExportEmpty();
bool preflightChromeTraceNesting(ChromeTraceExportRejectReason* reason = nullptr);
const char* chromeTraceExportRejectReasonLabel(ChromeTraceExportRejectReason reason);
bool tryExportChromeTraceJson(std::string& outJson, ChromeTraceExportRejectReason* reason = nullptr);
EventNameLookupPreflight preflightEventLookupByName(const char* name);
FlowIdLookupPreflight preflightFlowLookupById(u32 flowId);
NestingConsistencyPreflight preflightNestingConsistency();

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

/// Preflight: true when chrome export can run without dangling scope/flow nesting state.
bool isChromeExportPreflightOk();

/// Stub export for chrome://tracing offline analysis (not hot path).
std::string exportChromeTraceJson();
/// Guarded export — returns false when `preflightChromeTraceExport` would reject.
bool tryExportChromeTraceJson(std::string& outJson, ChromeTraceExportRejectReason* reason = nullptr);
/// Export preflight — writes chrome JSON and reports whether any trace events were emitted.
bool tryExportChromeTraceJson(std::string& outJson);
/// Guarded export — returns false when nesting/export preflight rejects (profiler disabled, empty export, or unbalanced nesting).

const char* eventNameRejectReasonLabel(EventNameRejectReason reason);
const char* eventLookupRejectReasonLabel(EventLookupRejectReason reason);
const char* nestingStateRejectReasonLabel(NestingStateRejectReason reason);
const char* chromeTraceExportRejectReasonLabel(ChromeTraceExportRejectReason reason);

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
