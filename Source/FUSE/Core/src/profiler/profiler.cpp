#include <fuse/profiler/profiler.hpp>

#include <fuse/platform/thread.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace fuse::profiler {

namespace {

constexpr u32 kRingCapacity = kRingEventCapacity;

std::atomic<bool> g_enabled{true};
std::atomic<u32> g_frameIndex{0};
std::atomic<u32> g_nextScopeId{1};
std::atomic<u32> g_nextFlowId{1};

std::array<ProfileEvent, kRingCapacity> g_events{};
std::atomic<u32> g_writeHead{0};
std::atomic<u32> g_eventCount{0};
std::atomic<u32> g_droppedEventCount{0};
std::atomic<u32> g_maxNestingDepth{0};
std::atomic<u32> g_maxFlowNestingDepth{0};
std::atomic<u32> g_openAsyncFlowCount{0};
std::atomic<u32> g_orphanAsyncFlowEndCount{0};

std::mutex g_exportMutex;

bool isRecordableName(const char* name) {
    return name != nullptr && name[0] != '\0';
std::atomic<u32> g_droppedEventCount{0};
std::atomic<u32> g_totalRecordedEvents{0};


bool isNonEmptyProfileName(const char* name) {
    return name != nullptr && *name != '\0';
}

u64 nowNanoseconds() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

u32& threadLocalNestingDepth() {
    static thread_local u32 depth = 0;
    return depth;
}

u32 pushNestingDepth() {
    u32& depth = threadLocalNestingDepth();
    const u32 next = depth + 1u;
    depth = next;

    const u32 observed = g_maxNestingDepth.load(std::memory_order_acquire);
    if (next > observed) {
        g_maxNestingDepth.store(next, std::memory_order_release);
    }
    return next;
}

void popNestingDepth() {
    u32& depth = threadLocalNestingDepth();
    if (depth > 0u) {
        --depth;
    }
}

u32 currentNestingDepth() {
    return threadLocalNestingDepth();
}

u32& threadLocalFlowNestingDepth() {
    static thread_local u32 depth = 0;
    return depth;
}

u32 pushFlowNestingDepth() {
    u32& depth = threadLocalFlowNestingDepth();
    const u32 next = depth + 1u;
    depth = next;

    const u32 observed = g_maxFlowNestingDepth.load(std::memory_order_acquire);
    if (next > observed) {
        g_maxFlowNestingDepth.store(next, std::memory_order_release);
    }
    return next;
}

void popFlowNestingDepth() {
    u32& depth = threadLocalFlowNestingDepth();
    if (depth > 0u) {
        --depth;
    }
}

u32 currentFlowNestingDepth() {
    return threadLocalFlowNestingDepth();
}

bool isValidEventName(const char* name) {
bool eventNameIsRecordable(const char* name) {
    return name != nullptr && name[0] != '\0';
const ProfileEvent& emptyProfileEventStub() {
    static const ProfileEvent kEmpty{};
    return kEmpty;
}

std::string formatCounterArgsJson(const ProfileEvent& event) {
    std::string args = "\"args\":{\"value\":";
    if (event.counterKind == CounterValueKind::Float) {
        char valueBuffer[64];
        std::snprintf(valueBuffer, sizeof(valueBuffer), "%.17g", event.counterFloatValue);
        args += valueBuffer;
    } else {
        args += std::to_string(static_cast<long long>(event.counterIntValue));
    }

    if (event.nestingDepth > 0u) {
        args += ",\"depth\":" + std::to_string(event.nestingDepth);
    }
    if (event.flowNestingDepth > 0u) {
        args += ",\"flow_depth\":" + std::to_string(event.flowNestingDepth);
    }
    if (event.counterSnapshotFrame > 0u) {
        args += ",\"snapshot_at_frame\":" + std::to_string(event.counterSnapshotFrame);
    }
    args += '}';
    return args;
}

std::string escapeJsonString(const char* value) {
    std::string escaped;
    if (value == nullptr) {
        return escaped;
    }

    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(*cursor) < 0x20u) {
                char unicode[8];
                std::snprintf(unicode, sizeof(unicode), "\\u%04x", static_cast<unsigned char>(*cursor));
                escaped += unicode;
            } else {
                escaped.push_back(*cursor);
            }
            break;
        }
    }
    return escaped;
}

void recordEvent(const char* name,
                 EventPhase phase,
                 u32 scopeId,
                 u32 nestingDepth,
                 u32 flowNestingDepth = 0u,
                 CounterValueKind counterKind = CounterValueKind::None,
                 s64 counterIntValue = 0,
                 f64 counterFloatValue = 0.0,
                 u32 counterSnapshotFrame = 0u) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }

    const u32 index = g_writeHead.fetch_add(1u, std::memory_order_acq_rel) % kRingCapacity;
    g_events[index] = ProfileEvent{
        name,
        nowNanoseconds(),
        phase,
        fuse::platform::chromeTraceThreadId(),
        scopeId,
        nestingDepth,
        flowNestingDepth,
        counterKind,
        counterIntValue,
        counterFloatValue,
        counterSnapshotFrame,
    };

    g_totalRecordedEvents.fetch_add(1u, std::memory_order_acq_rel);

    const u32 count = g_eventCount.load(std::memory_order_acquire);
    if (count < kRingCapacity) {
        g_eventCount.fetch_add(1u, std::memory_order_acq_rel);
    } else {
        g_droppedEventCount.fetch_add(1u, std::memory_order_acq_rel);
    }
}

const char* chromePhaseToken(EventPhase phase) {
    switch (phase) {
    case EventPhase::Begin:
        return "B";
    case EventPhase::End:
        return "E";
    case EventPhase::FlowStart:
        return "s";
    case EventPhase::FlowFinish:
        return "f";
    case EventPhase::Counter:
        return "C";
    }
    return "X";
}

const char* chromeCategory(EventPhase phase) {
    switch (phase) {
    case EventPhase::FlowStart:
    case EventPhase::FlowFinish:
        return "async";
    case EventPhase::Counter:
        return "counter";
    default:
        return "cpu";
    }
}

} // namespace

bool isValidEventName(const char* name);
bool isValidEventName(const char* name) {
    return name != nullptr && name[0] != '\0';
}

EventNameRejectReason diagnoseEventNameRejectReason(const char* name) {
    if (name == nullptr) {
        return EventNameRejectReason::Null;
    }
    if (name[0] == '\0') {
        return EventNameRejectReason::Empty;
    return EventNameRejectReason::None;

ProfileScope::ProfileScope(const char* name)
    : m_name(name),
      m_active(g_enabled.load(std::memory_order_acquire) && isValidEventName(name)) {
      m_active(g_enabled.load(std::memory_order_acquire) && name != nullptr) {
      m_active(name != nullptr && g_enabled.load(std::memory_order_acquire)) {
      m_active(g_enabled.load(std::memory_order_acquire) && isNonEmptyProfileName(name)) {
      m_active(g_enabled.load(std::memory_order_acquire) && isRecordableName(name)) {
      m_active(g_enabled.load(std::memory_order_acquire) && isValidProfileName(name)) {
      m_active(g_enabled.load(std::memory_order_acquire) && eventNameIsRecordable(name)) {
    if (m_active) {
        m_scopeId = g_nextScopeId.fetch_add(1u, std::memory_order_acq_rel);
        m_nestingDepth = pushNestingDepth();
        recordEvent(m_name, EventPhase::Begin, m_scopeId, m_nestingDepth);
    }
}

ProfileScope::~ProfileScope() {
    if (m_active) {
        recordEvent(m_name, EventPhase::End, m_scopeId, m_nestingDepth);
        popNestingDepth();
    }
}

bool enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void setEnabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_release);
}

void beginFrame() {
    g_frameIndex.fetch_add(1u, std::memory_order_acq_rel);
}

void endFrame() {
    // Frame boundary marker for future GPU correlation — no-op in CPU stub.
}

u32 frameIndex() {
    return g_frameIndex.load(std::memory_order_acquire);
}

u32 eventCount() {
    return g_eventCount.load(std::memory_order_acquire);
}

u32 ringCapacity() {
    return kRingCapacity;
}

u32 droppedEventCount() {
    return g_droppedEventCount.load(std::memory_order_acquire);
}

u32 maxNestingDepth() {
    return g_maxNestingDepth.load(std::memory_order_acquire);
}

u32 nestingDepth() {
    return currentNestingDepth();
}

u32 maxFlowNestingDepth() {
    return g_maxFlowNestingDepth.load(std::memory_order_acquire);
}

u32 scopeNestingDepth() {
u32 nestingDepth() {
    return currentNestingDepth();
}

u32 flowNestingDepth() {
    return currentFlowNestingDepth();
}

u32 openAsyncFlowCount() {
    return g_openAsyncFlowCount.load(std::memory_order_acquire);
}

bool isValidProfileName(const char* name) {
    return isValidEventName(name);
}

bool hasOpenAsyncFlows() {
    return openAsyncFlowCount() > 0u;
}

bool isScopeNestingBalanced() {
    return nestingDepth() == 0u;

bool isFlowNestingBalanced() {
    return flowNestingDepth() == 0u;

bool hasUnbalancedNesting() {
    return !isScopeNestingBalanced() || !isFlowNestingBalanced();

bool isFlowDepthDetached() {
    return flowNestingDepth() != openAsyncFlowCount();

bool isCrossThreadFlowHandoffPending() {
    return isFlowDepthDetached() && flowNestingDepth() > 0u;

NestingAsyncFlowPreflight preflightNestingAndAsyncFlow() {
    NestingAsyncFlowPreflight preflight{};
    preflight.activeScopeNestingDepth = scopeNestingDepth();
    preflight.activeFlowNestingDepth = flowNestingDepth();
    preflight.openAsyncFlowCount = openAsyncFlowCount();
    preflight.maxScopeNestingDepth = maxNestingDepth();
    preflight.maxFlowNestingDepth = maxFlowNestingDepth();
    preflight.scopeNestingBalanced = isScopeNestingBalanced();
    preflight.flowNestingBalanced = isFlowNestingBalanced();
    preflight.hasOpenAsyncFlows = hasOpenAsyncFlows();
    preflight.flowDepthDetached = isFlowDepthDetached();
    preflight.crossThreadFlowHandoffPending = isCrossThreadFlowHandoffPending();
    return preflight;
u32 ringBufferCapacity() {
    return kRingCapacity;
}




bool isValidEventName(const char* name) {
    return eventNameIsRecordable(name);

ChromeExportPreflight preflightChromeExport() {
    ChromeExportPreflight preflight;
    preflight.bufferEmpty = isBufferEmpty();
    preflight.unbalancedScopeNesting = !isScopeNestingBalanced();
    preflight.unbalancedFlowNesting = !isFlowNestingBalanced();
}

ChromeTraceExportPreflight preflightChromeTraceExport() {
    ChromeTraceExportPreflight preflight{};
    preflight.profilerDisabled = !enabled();
    preflight.emptyBuffer = isBufferEmpty();
    preflight.unbalancedScopeNesting = !isScopeNestingBalanced();
    preflight.unbalancedFlowNesting = !isFlowNestingBalanced();
    preflight.hasOpenAsyncFlows = hasOpenAsyncFlows();
    preflight.bufferFull = isBufferFull();
    return preflight;
}

bool canExportChromeTrace() {
    return preflightChromeTraceExport().canExport();
}

bool isNestingBalanced() {
    return isScopeNestingBalanced() && isFlowNestingBalanced();
}

bool isValidEventName(const char* name) {
    return name != nullptr && name[0] != '\0';
}

NestingIntrospection nestingIntrospection() {
    NestingIntrospection result{};
    result.scopeDepth = nestingDepth();
    result.flowDepth = flowNestingDepth();
    result.maxScopeDepth = maxNestingDepth();
    result.maxFlowDepth = maxFlowNestingDepth();
    result.openAsyncFlowCount = openAsyncFlowCount();
    result.scopeBalanced = isScopeNestingBalanced();
    result.flowBalanced = isFlowNestingBalanced();
    result.hasOpenAsyncFlows = hasOpenAsyncFlows();
    return result;
}

bool isNestingStateClean() {
    return isScopeNestingBalanced() && isFlowNestingBalanced() && !hasOpenAsyncFlows();
}

ChromeTraceExportPreflight preflightChromeTraceExport() {
    ChromeTraceExportPreflight result{};
    result.profilerDisabled = !enabled();
    result.emptyBuffer = isBufferEmpty();
    result.unbalancedScopeNesting = !isScopeNestingBalanced();
    result.unbalancedFlowNesting = !isFlowNestingBalanced();
    result.hasOpenAsyncFlows = hasOpenAsyncFlows();
    result.eventCount = eventCount();
    result.frameIndex = frameIndex();
    return result;
}

NestingIntrospection nestingIntrospection() {
    NestingIntrospection result{};
    result.scopeDepth = nestingDepth();
    result.flowDepth = flowNestingDepth();
    result.maxScopeDepth = maxNestingDepth();
    result.maxFlowDepth = maxFlowNestingDepth();
    result.openAsyncFlowCount = openAsyncFlowCount();
    result.scopeBalanced = isScopeNestingBalanced();
    result.flowBalanced = isFlowNestingBalanced();
    result.hasOpenAsyncFlows = hasOpenAsyncFlows();
    return result;
}

bool isNestingStateClean() {
    return isScopeNestingBalanced() && isFlowNestingBalanced() && !hasOpenAsyncFlows();
}

ChromeTraceExportPreflight preflightChromeTraceExport() {
    ChromeTraceExportPreflight result{};
    result.profilerDisabled = !enabled();
    result.emptyBuffer = isBufferEmpty();
    result.unbalancedScopeNesting = !isScopeNestingBalanced();
    result.unbalancedFlowNesting = !isFlowNestingBalanced();
    result.hasOpenAsyncFlows = hasOpenAsyncFlows();
    result.eventCount = eventCount();
    result.frameIndex = frameIndex();
    return result;
}

NestingStateRejectReason diagnoseNestingStateRejectReason() {
    if (!isScopeNestingBalanced()) {
        return NestingStateRejectReason::UnbalancedScopeNesting;
    }
    if (!isFlowNestingBalanced()) {
        return NestingStateRejectReason::UnbalancedFlowNesting;
    }
    if (hasOpenAsyncFlows()) {
        return NestingStateRejectReason::OpenAsyncFlows;
    }
    return NestingStateRejectReason::None;
}

ChromeTraceExportRejectReason diagnoseChromeTraceExportRejectReason() {
    if (!isScopeNestingBalanced()) {
        return ChromeTraceExportRejectReason::UnbalancedScopeNesting;
    }
    if (!isFlowNestingBalanced()) {
        return ChromeTraceExportRejectReason::UnbalancedFlowNesting;
    }
    if (hasOpenAsyncFlows()) {
        return ChromeTraceExportRejectReason::OpenAsyncFlows;
    }
    return ChromeTraceExportRejectReason::None;
}

NestingIntrospection nestingIntrospection() {
    NestingIntrospection result{};
    result.scopeDepth = nestingDepth();
    result.flowDepth = flowNestingDepth();
    result.maxScopeDepth = maxNestingDepth();
    result.maxFlowDepth = maxFlowNestingDepth();
    result.openAsyncFlowCount = openAsyncFlowCount();
    result.scopeBalanced = isScopeNestingBalanced();
    result.flowBalanced = isFlowNestingBalanced();
    result.hasOpenAsyncFlows = hasOpenAsyncFlows();
    return result;
}

bool isNestingStateClean() {
    return isScopeNestingBalanced() && isFlowNestingBalanced() && !hasOpenAsyncFlows();
}

ChromeTraceExportPreflight preflightChromeTraceExport() {
    ChromeTraceExportPreflight result{};
    result.profilerDisabled = !enabled();
    result.emptyBuffer = isBufferEmpty();
    result.unbalancedScopeNesting = !isScopeNestingBalanced();
    result.unbalancedFlowNesting = !isFlowNestingBalanced();
    result.hasOpenAsyncFlows = hasOpenAsyncFlows();
    result.eventCount = eventCount();
    result.frameIndex = frameIndex();
    return result;
}

bool hasEvents() {
    return eventCount() > 0u;
}

bool hasExportableEvents() {
    return hasEvents();
}

bool hasOpenAsyncFlows() {
    return openAsyncFlowCount() > 0u;
}

bool isBufferEmpty() {
    return eventCount() == 0u;
}

bool isBufferFull() {
    return eventCount() >= kRingCapacity;
bool hasOpenAsyncFlows() {
    return openAsyncFlowCount() > 0u;

    return eventCount() >= ringCapacity();

bool isEventIndexValid(u32 index) {
    return index < eventCount();

bool isBlankEventName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return true;

    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        const char ch = *cursor;
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            return false;

bool isValidEventName(const char* name) {
    return name != nullptr && name[0] != '\0';

bool isValidFlowId(u32 flowId) {
    return flowId != 0u;

bool isFlowPhaseEvent(const ProfileEvent& event) {
    return event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish;

bool eventNameMatches(const ProfileEvent& event, const char* name) {
    return isValidEventName(name) && isValidEventName(event.name)
        && std::strcmp(event.name, name) == 0;

bool eventMatchesFlowId(const ProfileEvent& event, u32 flowId) {
    return isValidFlowId(flowId) && isFlowPhaseEvent(event) && event.scopeId == flowId
        && isValidEventName(event.name);
bool eventNameMatches(const char* eventName, const char* queryName) {
    return isValidEventName(eventName) && isValidEventName(queryName)
        && std::strcmp(eventName, queryName) == 0;

bool isAsyncFlowPhase(EventPhase phase) {
    return phase == EventPhase::FlowStart || phase == EventPhase::FlowFinish;

bool isValidProfileName(const char* name) {
    return isNonEmptyProfileName(name);
    const u32 count = eventCount();
    return count > 0u && index < count;
}

}

bool isValidProfileName(const char* name) {
    return isValidEventName(name);
}

bool isValidProfileEvent(const ProfileEvent& event) {
    return isValidEventName(event.name);

bool isProfileEventSentinel(const ProfileEvent& event) {
    return event.name == nullptr && event.timestampNs == 0u && event.scopeId == 0u
        && event.phase == EventPhase::Begin;

u32 invalidNameEventCount() {
    const u32 total = eventCount();
    const u32 exportable = exportableEventCount();
    return total >= exportable ? total - exportable : 0u;

bool hasInvalidNameEvents() {
    return invalidNameEventCount() > 0u;

u32 exportableEventCount() {
    u32 count = 0u;
    for (u32 i = 0u; i < total; ++i) {
        if (isValidEventName(eventAt(i).name)) {
            ++count;
    return count;

bool isEventExportable(u32 index) {
    return isEventIndexValid(index) && isValidEventName(eventAt(index).name);
    return isRecordableName(event.name);
}

bool tryValidateEventName(const char* name, EventNameRejectReason& outReason) {
    outReason = diagnoseEventNameRejectReason(name);
    return outReason == EventNameRejectReason::None;

const char* eventNameRejectReasonLabel(EventNameRejectReason reason) {
    switch (reason) {
    case EventNameRejectReason::None:
        return "none";
    case EventNameRejectReason::Null:
        return "null";
    case EventNameRejectReason::Empty:
        return "empty";
    return "unknown";

bool isProfilerStateBalanced() {
    return diagnoseNestingStateRejectReason() == NestingStateRejectReason::None;

bool preflightProfilerState(NestingStateRejectReason* reason) {
    const NestingStateRejectReason rejectReason = diagnoseNestingStateRejectReason();
    if (reason != nullptr) {
        *reason = rejectReason;
    return rejectReason == NestingStateRejectReason::None;

const char* nestingStateRejectReasonLabel(NestingStateRejectReason reason) {
    case NestingStateRejectReason::None:
    case NestingStateRejectReason::UnbalancedScopeNesting:
        return "unbalanced_scope_nesting";
    case NestingStateRejectReason::UnbalancedFlowNesting:
        return "unbalanced_flow_nesting";
    case NestingStateRejectReason::OpenAsyncFlows:
        return "open_async_flows";

bool canExportChromeTrace() {
    return diagnoseChromeTraceExportRejectReason() == ChromeTraceExportRejectReason::None;

bool preflightChromeTraceExport(ChromeTraceExportRejectReason* reason) {
    const ChromeTraceExportRejectReason rejectReason = diagnoseChromeTraceExportRejectReason();
    return rejectReason == ChromeTraceExportRejectReason::None;

const char* chromeTraceExportRejectReasonLabel(ChromeTraceExportRejectReason reason) {
    case ChromeTraceExportRejectReason::None:
    case ChromeTraceExportRejectReason::UnbalancedScopeNesting:
    case ChromeTraceExportRejectReason::UnbalancedFlowNesting:
    case ChromeTraceExportRejectReason::OpenAsyncFlows:

bool canLookupEventAt(u32 index) {
    EventLookupRejectReason reason = EventLookupRejectReason::None;
    return tryCanLookupEventAt(index, reason);
}

bool isValidProfileName(const char* name) {
    return isRecordableName(name);

bool isScopeNestingBalanced() {
    return currentNestingDepth() == 0u;

bool isFlowNestingBalanced() {
    return currentFlowNestingDepth() == 0u;

bool hasOpenAsyncFlows() {
    return openAsyncFlowCount() > 0u;

u32 ringCapacity() {
    return kRingCapacity;

ChromeTraceExportPreflight preflightChromeTraceExport() {
    ChromeTraceExportPreflight preflight{};
    preflight.event_count = eventCount();
    preflight.frame_index = frameIndex();
    preflight.has_events = preflight.event_count > 0u;
    preflight.buffer_empty = preflight.event_count == 0u;
    preflight.scope_nesting_balanced = isScopeNestingBalanced();
    preflight.flow_nesting_balanced = isFlowNestingBalanced();
    preflight.has_open_async_flows = hasOpenAsyncFlows();
    preflight.can_export = preflight.scope_nesting_balanced && preflight.flow_nesting_balanced;
    return preflight;


const ProfileEvent& emptyProfileEvent() {
    static const ProfileEvent kEmpty{};
    return kEmpty;
    return isNonEmptyProfileName(event.name);


const ProfileEvent& eventAt(u32 index) {
    if (!isEventIndexValid(index)) {
        return emptyProfileEvent();

    const u32 count = eventCount();
    if (count == 0u || index >= count) {

    return emptyProfileEventStub();


    return count > 0u && index < count;



const ProfileEvent& emptyProfileEvent() {
    static const ProfileEvent kEmpty{};
    return kEmpty;




    const u32 count = eventCount();
    const u32 head = g_writeHead.load(std::memory_order_acquire);
    const u32 start = head >= count ? head - count : 0u;
    const u32 ringIndex = (start + index) % kRingCapacity;
    return g_events[ringIndex];
}

bool tryCanLookupEventAt(u32 index, EventLookupRejectReason& outReason) {
    if (eventCount() == 0u) {
        outReason = EventLookupRejectReason::EmptyBuffer;
        return false;
    }
    if (index >= eventCount()) {
        outReason = EventLookupRejectReason::OutOfRange;
        return false;
    }

    const ProfileEvent& candidate = eventAt(index);
    if (!isValidProfileEvent(candidate)) {
        outReason = EventLookupRejectReason::InvalidEvent;
        return false;
    }

    outReason = EventLookupRejectReason::None;
    return true;
}

const char* eventLookupRejectReasonLabel(EventLookupRejectReason reason) {
    switch (reason) {
    case EventLookupRejectReason::None:
        return "none";
    case EventLookupRejectReason::EmptyBuffer:
        return "empty_buffer";
    case EventLookupRejectReason::OutOfRange:
        return "out_of_range";
    case EventLookupRejectReason::InvalidEvent:
        return "invalid_event";
    }
    return "unknown";
}

bool tryEventAt(u32 index, ProfileEvent& outEvent) {
    EventLookupRejectReason reason = EventLookupRejectReason::None;
    if (!tryCanLookupEventAt(index, reason)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return isValidProfileEvent(outEvent);

bool tryExportableEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventExportable(index)) {

    return true;

bool tryFirstEvent(ProfileEvent& outEvent) {
    const u32 index = firstEventIndex();
    if (index == kInvalidEventIndex) {

    return tryEventAt(index, outEvent);

bool tryLastEvent(ProfileEvent& outEvent) {
    const u32 index = lastEventIndex();


bool tryFirstEventByName(const char* name, ProfileEvent& outEvent) {
bool tryExportableFirstEvent(ProfileEvent& outEvent) {

    return tryExportableEventAt(index, outEvent);

bool tryExportableLastEvent(ProfileEvent& outEvent) {


bool tryFindFirstEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByName(name);


bool tryLastEventByName(const char* name, ProfileEvent& outEvent) {

bool tryFindLastEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByName(name);


bool tryFirstFlowEvent(u32 flowId, ProfileEvent& outEvent) {

bool tryFindFirstFlowEvent(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByFlowId(flowId);


bool tryLastFlowEvent(u32 flowId, ProfileEvent& outEvent) {

bool tryFindLastFlowEvent(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByFlowId(flowId);


u32 firstEventIndex() {
    return hasEvents() ? 0u : kInvalidEventIndex;

u32 findFirstEventIndexByPhase(EventPhase phase) {
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.phase == phase && isValidEventName(event.name)) {
            return i;
    return kInvalidEventIndex;

u32 findLastEventIndexByPhase(EventPhase phase) {
    for (u32 i = total; i > 0u; --i) {
        const ProfileEvent& event = eventAt(i - 1u);
            return i - 1u;

u32 countEventsByPhase(EventPhase phase) {
    u32 count = 0u;
            ++count;
    return count;

u32 findFirstEventIndexByName(const char* name) {
    if (!isValidEventName(name)) {

        if (eventNameMatches(event, name)) {
        if (eventNameMatches(event.name, name)) {

u32 findLastEventIndexByName(const char* name) {


u32 countEventsByName(const char* name) {
        return 0u;

        if (eventNameMatches(eventAt(i), name)) {

bool hasEventsWithName(const char* name) {
    return findFirstEventIndexByName(name) != kInvalidEventIndex;

u32 findFirstEventIndexByFlowId(u32 flowId) {
    if (!isValidFlowId(flowId)) {
    if (flowId == 0u) {

        if (eventMatchesFlowId(event, flowId)) {
        if (isAsyncFlowPhase(event.phase) && event.scopeId == flowId && isValidEventName(event.name)) {

u32 findLastEventIndexByFlowId(u32 flowId) {


u32 countEventsByFlowId(u32 flowId) {

        if (eventMatchesFlowId(eventAt(i), flowId)) {

bool hasEventsWithFlowId(u32 flowId) {
    return findFirstEventIndexByFlowId(flowId) != kInvalidEventIndex;
bool tryFindFirstEventIndexByPhase(EventPhase phase, u32& outIndex) {
    const u32 index = findFirstEventIndexByPhase(phase);
        outIndex = kInvalidEventIndex;

    outIndex = index;

bool tryFindLastEventIndexByPhase(EventPhase phase, u32& outIndex) {
    const u32 index = findLastEventIndexByPhase(phase);


bool tryFindFirstEventIndexByName(const char* name, u32& outIndex) {


bool tryFindFirstEventIndexByFlowId(u32 flowId, u32& outIndex) {


bool tryFindLastEventIndexByFlowId(u32 flowId, u32& outIndex) {


u32 orphanAsyncFlowEndCount() {
    return g_orphanAsyncFlowEndCount.load(std::memory_order_acquire);

bool hasOrphanAsyncFlowEnds() {
    return orphanAsyncFlowEndCount() > 0u;

bool tryEventAt(u32 index, ProfileEvent& out) {
    out = eventAt(index);

    return isValidProfileEvent(out);
}

        outEvent = ProfileEvent{};
        return false;


}

u32 lastEventIndex() {
    const u32 count = eventCount();
    return count > 0u ? count - 1u : kInvalidEventIndex;
}

bool tryFirstEvent(ProfileEvent& outEvent) {
    return tryEventAt(0u, outEvent);
}

bool tryLastEvent(ProfileEvent& outEvent) {
    const u32 index = lastEventIndex();
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryEventAt(index, outEvent);
}

const ProfileEvent& lastEvent() {
    const u32 index = lastEventIndex();
    if (index == kInvalidEventIndex) {
        return emptyProfileEvent();
    }
    return eventAt(index);
}

ChromeTraceExportPreflight preflightChromeTraceExport() {
    ChromeTraceExportPreflight preflight{};
    preflight.profilerDisabled = !enabled();
    preflight.eventCount = eventCount();
    preflight.exportableEventCount = exportableEventCount();
    preflight.frameIndex = frameIndex();
    preflight.openAsyncFlowCount = openAsyncFlowCount();
    preflight.activeScopeNestingDepth = scopeNestingDepth();
    preflight.activeFlowNestingDepth = flowNestingDepth();
    preflight.maxScopeNestingDepth = maxNestingDepth();
    preflight.maxFlowNestingDepth = maxFlowNestingDepth();
    preflight.bufferEmpty = isBufferEmpty();
    preflight.scopeNestingUnbalanced = !isScopeNestingBalanced();
    preflight.flowNestingUnbalanced = !isFlowNestingBalanced();
    preflight.hasOpenAsyncFlows = hasOpenAsyncFlows();
    preflight.flowDepthDetached = isFlowDepthDetached();
    preflight.invalidNameEventCount = invalidNameEventCount();
    preflight.ringBufferFull = isBufferFull();
    preflight.hasInvalidNameEvents = hasInvalidNameEvents();
    preflight.crossThreadFlowHandoffPending = isCrossThreadFlowHandoffPending();
    preflight.exportWouldTrimEvents = preflight.eventCount > preflight.exportableEventCount;
    preflight.hasOnlyExportableEvents =
        preflight.eventCount > 0u && preflight.eventCount == preflight.exportableEventCount;
    preflight.orphanAsyncFlowEndCount = orphanAsyncFlowEndCount();
    preflight.hasOrphanAsyncFlowEnds = hasOrphanAsyncFlowEnds();
    return preflight;
}

ProfileScopePreflight preflightProfileScope(const char* name) {
    ProfileScopePreflight preflight{};
    preflight.invalidName = !isValidEventName(name);
    preflight.canEnter = !preflight.profilerDisabled && !preflight.invalidName;

AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name, u32 /*flowId*/) {
    AsyncFlowBeginPreflight preflight{};
    preflight.canBegin = !preflight.profilerDisabled && !preflight.invalidName;

AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name, u32 /*flowId*/) {
    AsyncFlowEndPreflight preflight{};
    preflight.wouldUnderflowOpenCount = openAsyncFlowCount() == 0u;
    preflight.canEnd = !preflight.profilerDisabled && !preflight.invalidName
        && !preflight.wouldUnderflowOpenCount;

NestingAsyncFlowPreflight preflightNestingAsyncFlow() {
    NestingAsyncFlowPreflight preflight{};
bool tryEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventIndexValid(index)) {
        return false;

    outEvent = eventAt(index);
    return isValidProfileEvent(outEvent);

bool tryLastEvent(ProfileEvent& outEvent) {
    const u32 index = lastEventIndex();
    if (index == kInvalidEventIndex) {
    return tryEventAt(index, outEvent);

ProfilerRecordPreflight preflightRecord(const char* name) {
    ProfilerRecordPreflight preflight{};
    preflight.profiler_enabled = enabled();
    preflight.name_valid = isNonEmptyProfileName(name);

ProfilerExportPreflight preflightChromeTraceExport() {
    ProfilerExportPreflight preflight{};
    preflight.event_count = eventCount();
    preflight.has_events = preflight.event_count > 0u;
    preflight.dropped_event_count = droppedEventCount();
    preflight.buffer_full = isBufferFull();
bool tryEventAt(u32 index, const ProfileEvent*& outEvent) {
    static const ProfileEvent kEmpty{};
        outEvent = &kEmpty;

    outEvent = &eventAt(index);
    return true;


ProfilerGuardPreflight preflightGuardState() {
    ProfilerGuardPreflight preflight{};
bool hasLastEvent() {
    return lastEventIndex() != kInvalidEventIndex;



u32 droppedEventCount() {
    return g_droppedEventCount.load(std::memory_order_acquire);

NestingStatePreflight preflightNestingState() {
    NestingStatePreflight preflight{};
    preflight.scopeDepth = currentNestingDepth();
    preflight.flowDepth = currentFlowNestingDepth();
    preflight.openAsyncFlows = g_openAsyncFlowCount.load(std::memory_order_acquire);
    preflight.maxScopeDepth = g_maxNestingDepth.load(std::memory_order_acquire);
    preflight.maxFlowDepth = g_maxFlowNestingDepth.load(std::memory_order_acquire);
    preflight.hasOpenScopes = preflight.scopeDepth > 0u;
    preflight.hasOpenAsyncFlows = preflight.openAsyncFlows > 0u;
    preflight.canEndAsyncFlow = preflight.openAsyncFlows > 0u;

bool hasOpenScopes() {
    return currentNestingDepth() > 0u;

bool hasOpenAsyncFlows() {
    return g_openAsyncFlowCount.load(std::memory_order_acquire) > 0u;

    preflight.bufferEmpty = preflight.eventCount == 0u;
    preflight.canExport = true;
    preflight.hasOpenScopes = hasOpenScopes();



    preflight.hasUnmatchedAsyncFlows = preflight.hasOpenAsyncFlows;

    preflight.hasUnbalancedAsyncFlows = preflight.openAsyncFlows > 0u;

AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name) {
    preflight.profilerDisabled = !g_enabled.load(std::memory_order_acquire);
    preflight.emptyName = !isNonEmptyProfileName(name);

AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name) {
    preflight.orphanEnd = g_openAsyncFlowCount.load(std::memory_order_acquire) == 0u;

ChromeExportPreflight preflightChromeExport() {
    ChromeExportPreflight preflight{};
    preflight.droppedEventCount = droppedEventCount();

    for (u32 i = 0; i < preflight.eventCount; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (isValidProfileEvent(event)) {
            ++preflight.exportableEventCount;
        } else {
            ++preflight.skippedInvalidNameCount;


bool canExportChromeTrace() {
    return preflightChromeExport().canExport();


u32 exportableEventCount() {
    const u32 count = eventCount();
    u32 exportable = 0u;
    for (u32 i = 0; i < count; ++i) {
        if (isValidProfileEvent(eventAt(i))) {
            ++exportable;
    return exportable;

bool isExportEmpty() {
    return exportableEventCount() == 0u;

        outEvent = ProfileEvent{};




void reset() {
    const std::lock_guard<std::mutex> lock(g_exportMutex);
    g_writeHead.store(0u, std::memory_order_release);
    g_eventCount.store(0u, std::memory_order_release);
    g_droppedEventCount.store(0u, std::memory_order_release);
    g_frameIndex.store(0u, std::memory_order_release);
    g_nextScopeId.store(1u, std::memory_order_release);
    g_nextFlowId.store(1u, std::memory_order_release);
    g_maxNestingDepth.store(0u, std::memory_order_release);
    g_maxFlowNestingDepth.store(0u, std::memory_order_release);
    g_openAsyncFlowCount.store(0u, std::memory_order_release);
    g_orphanAsyncFlowEndCount.store(0u, std::memory_order_release);
    g_droppedEventCount.store(0u, std::memory_order_release);
    g_totalRecordedEvents.store(0u, std::memory_order_release);
    threadLocalNestingDepth() = 0u;
    threadLocalFlowNestingDepth() = 0u;
}

u32 remainingEventCapacity() {
    const u32 count = eventCount();
    return count >= kRingCapacity ? 0u : kRingCapacity - count;
}

bool isEmptyProfilerName(const char* name) {
    return name == nullptr || name[0] == '\0';
bool isUsableProfileName(const char* name) {
    return name != nullptr && name[0] != '\0';
    return isValidEventName(name);
}

ProfileNamePreflight preflightProfileName(const char* name) {
    ProfileNamePreflight preflight{};
    preflight.null_name = name == nullptr;
    preflight.empty_name = name != nullptr && name[0] == '\0';
    if (name == nullptr) {
        return preflight;
    }

    preflight.null_name = false;
    if (name[0] == '\0') {
        preflight.empty_name = true;
    }
    return preflight;

ScopePreflight preflightScope(const char* name) {
    ScopePreflight preflight{};
    preflight.profiler_disabled = !enabled();
    const ProfileNamePreflight namePreflight = preflightProfileName(name);
    preflight.null_name = namePreflight.null_name;
    preflight.empty_name = namePreflight.empty_name;
    return preflight;
}

ScopeNestingPreflight preflightScopeNesting() {
    ScopeNestingPreflight preflight{};
    preflight.current_depth = currentNestingDepth();
    preflight.max_depth_seen = g_maxNestingDepth.load(std::memory_order_acquire);
    preflight.profiler_disabled = !g_enabled.load(std::memory_order_acquire);
    return preflight;
}

AsyncFlowBeginPreflight preflightAsyncFlowBegin(const char* name) {
    AsyncFlowBeginPreflight preflight{};
    preflight.null_name = name == nullptr;
    preflight.empty_name = name != nullptr && name[0] == '\0';

AsyncFlowEndPreflight preflightAsyncFlowEnd(const char* name) {
    AsyncFlowEndPreflight preflight{};
    preflight.orphan_end = g_openAsyncFlowCount.load(std::memory_order_acquire) == 0u;
    preflight.max_observed_depth = g_maxNestingDepth.load(std::memory_order_acquire);

AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name) {

    preflight.profiler_disabled = !enabled();
    const ProfileNamePreflight namePreflight = preflightProfileName(name);
    preflight.null_name = namePreflight.null_name;
    preflight.empty_name = namePreflight.empty_name;

AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name) {
    preflight.open_flow_count = g_openAsyncFlowCount.load(std::memory_order_acquire);
    preflight.no_open_flows = preflight.open_flow_count == 0u;

CounterSamplePreflight preflightCounterSample(const char* track) {
    CounterSamplePreflight preflight{};
    const ProfileNamePreflight namePreflight = preflightProfileName(track);
    return preflight;
}

    AsyncFlowEndPreflight preflight{};
    preflight.profiler_disabled = !enabled();
    const ProfileNamePreflight namePreflight = preflightProfileName(name);
    preflight.null_name = namePreflight.null_name;
    preflight.empty_name = namePreflight.empty_name;

    return preflight;
}

ChromeTraceExportPreflight preflightChromeTraceExport() {
    ChromeTraceExportPreflight preflight{};
    preflight.event_count = eventCount();
    preflight.open_async_flow_count = g_openAsyncFlowCount.load(std::memory_order_acquire);
    preflight.has_events = preflight.event_count > 0u;
    preflight.has_unmatched_flows = preflight.open_async_flow_count > 0u;
    preflight.would_emit_empty_trace = !preflight.has_events;
    return preflight;
}

bool shouldSkipProfileScope(const char* name) {
    return preflightProfileName(name).shouldSkip() || !g_enabled.load(std::memory_order_acquire);

bool shouldSkipAsyncFlowBegin(const char* name) {
    return preflightAsyncFlowBegin(name).shouldSkip();

bool shouldSkipAsyncFlowEnd(const char* name) {
    return preflightAsyncFlowEnd(name).shouldSkip();

bool shouldSkipCounterSample(const char* track) {
    return preflightProfileName(track).shouldSkip() || !g_enabled.load(std::memory_order_acquire);

bool tryEventAt(u32 index, ProfileEvent& out) {
    if (!isEventIndexValid(index)) {
        return false;
    out = eventAt(index);
    return isValidProfileEvent(out);

const ProfileEvent* eventAtOrNull(u32 index) {
        return nullptr;
    const ProfileEvent& event = eventAt(index);
    return isValidProfileEvent(event) ? &event : nullptr;
    preflight.profiler_disabled = !enabled();
    preflight.buffer_empty = preflight.event_count == 0u;
    preflight.frame_index = frameIndex();

    if (!preflight.buffer_empty) {
        for (u32 i = 0; i < preflight.event_count; ++i) {
            const ProfileEvent& event = eventAt(i);
            if (event.name == nullptr) {
                ++preflight.null_name_skip_count;
            }
    return preflight;

EventLookupPreflight preflightEventLookup(u32 index) {
    EventLookupPreflight preflight{};
    preflight.requested_index = index;
    preflight.out_of_range = preflight.buffer_empty || index >= preflight.event_count;

bool canLookupEventAt(u32 index) {
    return preflightEventLookup(index).can_lookup();
    preflight.event_count = eventCount();
    preflight.buffer_empty = preflight.event_count == 0u;
    return preflight;
}


bool tryEventAt(u32 index, const ProfileEvent*& event_out) {
    const EventLookupPreflight preflight = preflightEventLookup(index);
    event_out = &eventAt(index);
    return preflight.can_lookup() && isValidProfileEvent(*event_out);
}

u32 nextFlowId() {
    return g_nextFlowId.fetch_add(1u, std::memory_order_acq_rel);
}

void beginAsyncFlow(const char* name, u32 flowId) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(name)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isNonEmptyProfileName(name)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isRecordableName(name)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidProfileName(name)) {
    if (!g_enabled.load(std::memory_order_acquire) || !eventNameIsRecordable(name)) {
        return;
    }

    const u32 flowDepth = pushFlowNestingDepth();
    g_openAsyncFlowCount.fetch_add(1u, std::memory_order_acq_rel);
    recordEvent(name,
                EventPhase::FlowStart,
                flowId,
                currentNestingDepth(),
                flowDepth);
}

void endAsyncFlow(const char* name, u32 flowId) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(name)) {
    if (name == nullptr) {
    if (!isValidEventName(name)) {
        return;
    }

    if (!g_enabled.load(std::memory_order_acquire)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isNonEmptyProfileName(name)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isRecordableName(name)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidProfileName(name)) {
    if (!g_enabled.load(std::memory_order_acquire) || !eventNameIsRecordable(name)) {
        return;
    }

    if (g_openAsyncFlowCount.load(std::memory_order_acquire) == 0u) {
        g_orphanAsyncFlowEndCount.fetch_add(1u, std::memory_order_acq_rel);

    g_openAsyncFlowCount.fetch_sub(1u, std::memory_order_acq_rel);

    const u32 flowDepth = currentFlowNestingDepth();
    const bool enabled = g_enabled.load(std::memory_order_acquire);

    if (!enabled) {
        if (flowDepth > 0u) {
            popFlowNestingDepth();

    recordEvent(name,
                EventPhase::FlowFinish,
                flowId,
                currentNestingDepth(),
                flowDepth);

    if (flowDepth > 0u) {
    if (currentFlowNestingDepth() > 0u) {
        popFlowNestingDepth();
    }
}

void sampleCounter(const char* track, s64 value) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isNonEmptyProfileName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isRecordableName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidProfileName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !eventNameIsRecordable(track)) {
        return;
    }

    recordEvent(track,
                EventPhase::Counter,
                0u,
                currentNestingDepth(),
                currentFlowNestingDepth(),
                CounterValueKind::Int,
                value,
                0.0,
                0u);
}

void sampleCounterFloat(const char* track, f64 value) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isNonEmptyProfileName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isRecordableName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidProfileName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !eventNameIsRecordable(track)) {
        return;
    }

    recordEvent(track,
                EventPhase::Counter,
                0u,
                currentNestingDepth(),
                currentFlowNestingDepth(),
                CounterValueKind::Float,
                0,
                value,
                0u);
}

void sampleCounterSnapshotAtFrame(const char* track, s64 value) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isNonEmptyProfileName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isRecordableName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidProfileName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !eventNameIsRecordable(track)) {
        return;
    }

    recordEvent(track,
                EventPhase::Counter,
                0u,
                currentNestingDepth(),
                currentFlowNestingDepth(),
                CounterValueKind::Int,
                value,
                0.0,
                frameIndex());
}

void sampleCounterFloatSnapshotAtFrame(const char* track, f64 value) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isNonEmptyProfileName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isRecordableName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidProfileName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !eventNameIsRecordable(track)) {
        return;
    }

    recordEvent(track,
                EventPhase::Counter,
                0u,
                currentNestingDepth(),
                currentFlowNestingDepth(),
                CounterValueKind::Float,
                0,
                value,
                frameIndex());
}

ChromeExportPreflight preflightChromeExport() {
    ChromeExportPreflight preflight{};
    preflight.eventCount = eventCount();
    preflight.frameIndex = frameIndex();
    preflight.bufferEmpty = isBufferEmpty();
    preflight.hasEvents = hasEvents();
    preflight.scopeNestingBalanced = isScopeNestingBalanced();
    preflight.flowNestingBalanced = isFlowNestingBalanced();
    preflight.hasOpenAsyncFlows = hasOpenAsyncFlows();
    preflight.canExport = true;
ChromeTraceExportPreflight preflightChromeTraceExport() {
    ChromeTraceExportPreflight preflight{};
    preflight.profilerEnabled = enabled();

    const u32 count = preflight.eventCount;
    for (u32 i = 0; i < count; ++i) {
        if (isValidProfileEvent(eventAt(i))) {
            ++preflight.exportableEventCount;
        }
    preflight.hasExportableEvents = preflight.exportableEventCount > 0u;
bool canExportChromeTrace() {
    return hasEvents();

ChromeExportPreflight preflightChromeTraceExport() {
    preflight.profilerDisabled = !enabled();
    preflight.unbalancedScopeNesting = !isScopeNestingBalanced();
    preflight.emptyBuffer = preflight.eventCount == 0u;
    preflight.bufferFull = isBufferFull();
    preflight.scopeNestingDepth = nestingDepth();
    preflight.flowNestingDepth = flowNestingDepth();
    preflight.openFlowCount = openAsyncFlowCount();
    preflight.unbalancedFlowNesting = !isFlowNestingBalanced();
    preflight.openAsyncFlows = hasOpenAsyncFlows();
    return preflight;
}

ChromeTraceExportRejectReason chromeTraceExportRejectReason() {
    const ChromeTraceExportPreflight preflight = preflightChromeTraceExport();
    if (preflight.emptyBuffer) {
        return ChromeTraceExportRejectReason::EmptyBuffer;
    }
    if (preflight.unbalancedScopeNesting) {
        return ChromeTraceExportRejectReason::UnbalancedScopeNesting;
    if (preflight.openAsyncFlows) {
        return ChromeTraceExportRejectReason::OpenAsyncFlows;
    if (preflight.unbalancedFlowNesting) {
        return ChromeTraceExportRejectReason::UnbalancedFlowNesting;
    if (preflight.bufferFull) {
        return ChromeTraceExportRejectReason::BufferFull;
    return ChromeTraceExportRejectReason::None;

std::string exportChromeTraceJson() {
    const std::lock_guard<std::mutex> lock(g_exportMutex);

    char header[192];
    std::snprintf(header,
                  sizeof(header),
                  "{\"displayTimeUnit\":\"ns\",\"metadata\":{\"name\":\"FUSE CPU profiler\",\"frame\":%u},"
                  "\"traceEvents\":[",
                  g_frameIndex.load(std::memory_order_acquire));

    std::string json = header;
    const u32 count = eventCount();
    bool first = true;

    for (u32 i = 0; i < count; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (!isValidEventName(event.name)) {
        if (!isRecordableName(event.name)) {
        if (!isValidProfileEvent(event)) {
            continue;
        }

        const std::string escapedName = escapeJsonString(event.name);
        const char* phase = chromePhaseToken(event.phase);
        const char* category = chromeCategory(event.phase);
        const u64 timestampUs = event.timestampNs / 1000u;

        char buffer[768];
        switch (event.phase) {
        case EventPhase::Begin:
        case EventPhase::End:
            std::snprintf(buffer,
                          sizeof(buffer),
                          "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                          "\"tid\":%u,\"id\":%u,\"args\":{\"depth\":%u}}",
                          first ? "" : ",",
                          escapedName.c_str(),
                          category,
                          phase,
                          static_cast<unsigned long long>(timestampUs),
                          event.threadId,
                          event.scopeId,
                          event.nestingDepth);
            break;
        case EventPhase::FlowStart:
            if (event.nestingDepth > 0u && event.flowNestingDepth > 0u) {
                std::snprintf(buffer,
                              sizeof(buffer),
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"args\":{\"depth\":%u,\"flow_depth\":%u}}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId,
                              event.nestingDepth,
                              event.flowNestingDepth);
            } else if (event.nestingDepth > 0u) {
                std::snprintf(buffer,
                              sizeof(buffer),
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"args\":{\"depth\":%u}}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId,
                              event.nestingDepth);
            } else if (event.flowNestingDepth > 0u) {
                std::snprintf(buffer,
                              sizeof(buffer),
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"args\":{\"flow_depth\":%u}}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId,
                              event.flowNestingDepth);
            } else {
                std::snprintf(buffer,
                              sizeof(buffer),
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId);
            }
            break;
        case EventPhase::FlowFinish:
            if (event.nestingDepth > 0u && event.flowNestingDepth > 0u) {
                std::snprintf(buffer,
                              sizeof(buffer),
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"bp\":\"e\",\"args\":{\"depth\":%u,\"flow_depth\":%u}}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId,
                              event.nestingDepth,
                              event.flowNestingDepth);
            } else if (event.nestingDepth > 0u) {
                std::snprintf(buffer,
                              sizeof(buffer),
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"bp\":\"e\",\"args\":{\"depth\":%u}}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId,
                              event.nestingDepth);
            } else if (event.flowNestingDepth > 0u) {
                std::snprintf(buffer,
                              sizeof(buffer),
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"bp\":\"e\",\"args\":{\"flow_depth\":%u}}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId,
                              event.flowNestingDepth);
            } else {
                std::snprintf(buffer,
                              sizeof(buffer),
                              "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                              "\"tid\":%u,\"id\":%u,\"bp\":\"e\"}",
                              first ? "" : ",",
                              escapedName.c_str(),
                              category,
                              phase,
                              static_cast<unsigned long long>(timestampUs),
                              event.threadId,
                              event.scopeId);
            }
            break;
        case EventPhase::Counter: {
            char counterHeader[512];
            std::snprintf(counterHeader,
                          sizeof(counterHeader),
                          "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"pid\":1,"
                          "\"tid\":%u,",
                          first ? "" : ",",
                          escapedName.c_str(),
                          category,
                          phase,
                          static_cast<unsigned long long>(timestampUs),
                          event.threadId);
            json += counterHeader;
            json += formatCounterArgsJson(event);
            json += '}';
            first = false;
            continue;
        }
        }
        json += buffer;
        first = false;
    }

    json += "]}";
    return json;
}

bool tryExportChromeTraceJson(std::string& outJson, ChromeTraceExportRejectReason* reason) {
    if (!preflightChromeTraceExport(reason)) {
        outJson.clear();
        return false;
    }

    outJson = exportChromeTraceJson();
    return true;
}

} // namespace fuse::profiler
