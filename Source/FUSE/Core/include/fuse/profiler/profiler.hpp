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

/// Preflight for profile scope/flow/counter name guards (B1.6 deepen).
struct ProfileNamePreflight {
    bool null_name = true;
    bool empty_name = false;

    [[nodiscard]] bool can_record() const { return !null_name && !empty_name; }
    [[nodiscard]] bool should_skip() const { return !can_record(); }
};

/// Preflight for entering a CPU profile scope (B1.6 deepen).
struct ScopePreflight {
    bool profiler_disabled = false;
    bool null_name = true;
    bool empty_name = false;

    [[nodiscard]] bool can_enter() const { return !profiler_disabled && !null_name && !empty_name; }
    [[nodiscard]] bool should_skip() const { return !can_enter(); }
};

/// Preflight for active scope nesting depth introspection (B1.6 deepen).
struct ScopeNestingPreflight {
    u32 current_depth = 0;
    u32 max_observed_depth = 0;

    [[nodiscard]] bool is_at_root() const { return current_depth == 0u; }
};

/// Preflight for async flow begin guards (B1.6 deepen).
struct AsyncFlowBeginPreflight {
    bool profiler_disabled = false;
    bool null_name = true;
    bool empty_name = false;

    [[nodiscard]] bool can_begin() const { return !profiler_disabled && !null_name && !empty_name; }
    [[nodiscard]] bool should_skip() const { return !can_begin(); }
};

/// Preflight for async flow end guards — surfaces orphan finish risk (B1.6 deepen).
struct AsyncFlowEndPreflight {
    bool profiler_disabled = false;
    bool null_name = true;
    bool empty_name = false;
    bool no_open_flows = true;
    u32 open_flow_count = 0;

    [[nodiscard]] bool would_orphan() const { return no_open_flows; }
    [[nodiscard]] bool can_end() const { return !profiler_disabled && !null_name && !empty_name && !no_open_flows; }
    [[nodiscard]] bool should_skip() const { return !can_end(); }
};

/// Preflight for counter sample guards (B1.6 deepen).
struct CounterSamplePreflight {
    bool profiler_disabled = false;
    bool null_name = true;
    bool empty_name = false;

    [[nodiscard]] bool can_sample() const { return !profiler_disabled && !null_name && !empty_name; }
    [[nodiscard]] bool should_skip() const { return !can_sample(); }
};

/// Preflight for chrome://tracing export — buffer state and skipped null-name events (B1.6 deepen).
struct ChromeTraceExportPreflight {
    bool profiler_disabled = false;
    bool buffer_empty = true;
    u32 event_count = 0;
    u32 null_name_skip_count = 0;
    u32 frame_index = 0;

    [[nodiscard]] bool will_emit_events() const {
        return event_count > 0u && null_name_skip_count < event_count;
    }

    /// Export is always callable — even when the buffer is empty.
    [[nodiscard]] bool exportable() const { return true; }
};

/// Preflight for ring-buffer event lookup — safe sentinel fallback (B1.6 deepen).
struct EventLookupPreflight {
    bool buffer_empty = true;
    bool out_of_range = true;
    u32 requested_index = 0;
    u32 event_count = 0;

    [[nodiscard]] bool can_lookup() const { return !buffer_empty && !out_of_range; }
    [[nodiscard]] bool should_use_sentinel() const { return !can_lookup(); }
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
