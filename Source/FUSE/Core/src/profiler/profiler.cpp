#include <fuse/profiler/profiler.hpp>

#include <fuse/platform/thread.hpp>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

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
std::atomic<u32> g_totalEventsWritten{0};
std::atomic<u64> g_totalRecorded{0};
std::atomic<u32> g_maxNestingDepth{0};
std::atomic<u32> g_maxFlowNestingDepth{0};
std::atomic<u32> g_openAsyncFlowCount{0};
std::atomic<u32> g_orphanAsyncFlowEndCount{0};
std::atomic<u32> g_rejectedInvalidNameCount{0};
std::atomic<u32> g_droppedEventCount{0};
std::atomic<u32> g_ignoredAsyncFlowEndCount{0};

std::mutex g_exportMutex;

bool isRecordableName(const char* name) {
    return name != nullptr && name[0] != '\0';
std::atomic<u32> g_totalRecordedEvents{0};


bool isNonEmptyProfileName(const char* name) {
    return name != nullptr && *name != '\0';
std::atomic<u32> g_orphanFlowEndCount{0};


bool isWhitespaceOnlyEventName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        if (!std::isspace(static_cast<unsigned char>(*cursor))) {
    return true;

void noteRejectedEventName(const char* name) {
    g_rejectedInvalidNameCount.fetch_add(1u, std::memory_order_acq_rel);

bool shouldRejectEventName(const char* name) {
    if (name == nullptr) {
        noteRejectedEventName(name);
    if (name[0] == '\0') {
    if (isWhitespaceOnlyEventName(name)) {


bool isAsyncFlowPhase(EventPhase phase) {
    return phase == EventPhase::FlowStart || phase == EventPhase::FlowFinish;
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

    const u32 count = g_eventCount.load(std::memory_order_acquire);
    if (count >= kRingCapacity) {
        g_droppedEventCount.fetch_add(1u, std::memory_order_acq_rel);
    } else {
        g_eventCount.fetch_add(1u, std::memory_order_acq_rel);
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
    g_totalEventsWritten.fetch_add(1u, std::memory_order_acq_rel);
    g_totalRecorded.fetch_add(1u, std::memory_order_acq_rel);

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

EventNameRejectReason diagnoseEventNameRejectReason(const char* name) {
    if (name == nullptr) {
        return EventNameRejectReason::Null;
    }
    if (name[0] == '\0') {
        return EventNameRejectReason::Empty;
    return EventNameRejectReason::None;
u32 countUnbalancedFlowPairsInBuffer() {
    std::unordered_map<u32, std::pair<u32, u32>> flowPairCounts;
    const u32 total = g_eventCount.load(std::memory_order_acquire);
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (!isValidEventName(event.name) || !isFlowEventPhase(event.phase) || !isValidFlowId(event.scopeId)) {
            continue;

        auto& counts = flowPairCounts[event.scopeId];
        if (event.phase == EventPhase::FlowStart) {
            ++counts.first;
        } else {
            ++counts.second;

    u32 unbalanced = 0u;
    for (const auto& entry : flowPairCounts) {
        if (entry.second.first != entry.second.second) {
            ++unbalanced;
    return unbalanced;
bool isAsyncFlowPhase(EventPhase phase) {
    return phase == EventPhase::FlowStart || phase == EventPhase::FlowFinish;

bool eventNameMatches(const ProfileEvent& event, const char* name) {
    if (!isValidEventName(name) || !isValidEventName(event.name)) {
        return false;
    return std::strcmp(event.name, name) == 0;

bool isExportableFlowEvent(const ProfileEvent& event, u32 flowId) {
    return isAsyncFlowPhase(event.phase) && event.scopeId == flowId && isValidEventName(event.name);
bool eventMatchesName(const ProfileEvent& event, const char* name) {
    return name != nullptr && name[0] != '\0' && event.name != nullptr && event.name[0] != '\0'
        && std::strcmp(event.name, name) == 0;

bool eventMatchesFlowId(const ProfileEvent& event, u32 flowId) {
    return (event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish)
        && event.scopeId == flowId && event.name != nullptr && event.name[0] != '\0';
bool isFlowEventPhase(EventPhase phase) {

bool isValidFlowLookupId(u32 flowId) {
    return flowId != 0u;

    return isValidEventName(event.name) && isFlowEventPhase(event.phase) && event.scopeId == flowId;
bool isFlowPhase(EventPhase phase) {
bool isFlowPhaseEvent(const ProfileEvent& event) {
    return event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish;

bool eventNameMatches(const char* lhs, const char* rhs) {
    if (lhs == nullptr || rhs == nullptr) {
    return std::strcmp(lhs, rhs) == 0;
bool eventNameEquals(const char* eventName, const char* name) {
    if (eventName == nullptr || eventName[0] == '\0' || name == nullptr || name[0] == '\0') {
    return std::strcmp(eventName, name) == 0;

bool eventNameMatches(const char* eventName, const char* queryName) {
    if (eventName == nullptr || eventName[0] == '\0' || queryName == nullptr || queryName[0] == '\0') {
    return std::strcmp(eventName, queryName) == 0;

bool eventNameEquals(const char* lhs, const char* rhs) {
    return lhs != nullptr && rhs != nullptr && std::strcmp(lhs, rhs) == 0;

    return isValidEventName(event.name) && isValidEventName(name) && std::strcmp(event.name, name) == 0;

bool eventFlowIdMatches(const ProfileEvent& event, u32 flowId) {
    if (!isValidFlowId(flowId) || !isValidEventName(event.name)) {

bool isAsyncFlowEventForId(const ProfileEvent& event, u32 flowId) {
        && event.scopeId == flowId;
}

} // namespace

bool isBlankEventName(const char* name);
bool isBlankEventName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return true;
    }

    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        if (*cursor != ' ' && *cursor != '\t' && *cursor != '\n' && *cursor != '\r') {
            return false;
        }
    return true;

bool isValidEventName(const char* name);
bool isValidEventName(const char* name) {
    return name != nullptr && name[0] != '\0';

EventNameRejectReason diagnoseEventNameRejectReason(const char* name) {
    if (name == nullptr) {
        return EventNameRejectReason::Null;
    if (name[0] == '\0') {
        return EventNameRejectReason::Empty;
    return EventNameRejectReason::None;
    return diagnoseEventNameRejectReason(name) == EventNameRejectReason::None;

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

void recordRejectedInvalidName() {
    g_rejectedInvalidNameCount.fetch_add(1u, std::memory_order_acq_rel);

void recordOrphanAsyncFlowEnd() {
    g_orphanAsyncFlowEndCount.fetch_add(1u, std::memory_order_acq_rel);
bool isWhitespaceOnlyEventName(const char* name);

bool shouldRecordEventName(const char* name) {
    return isValidEventName(name) && !isWhitespaceOnlyEventName(name);

namespace {

bool isAsyncFlowPhase(EventPhase phase) {
    return phase == EventPhase::FlowStart || phase == EventPhase::FlowFinish;

bool eventNameMatches(const ProfileEvent& event, const char* name) {
    return isValidEventName(event.name) && isValidEventName(name) && std::strcmp(event.name, name) == 0;

bool isAsyncFlowEventForId(const ProfileEvent& event, u32 flowId) {
    return isAsyncFlowPhase(event.phase) && isValidEventName(event.name) && event.scopeId == flowId;

} // namespace

    return isValidEventName(name) && isValidEventName(event.name) && std::strcmp(event.name, name) == 0;

bool eventFlowIdMatches(const ProfileEvent& event, u32 flowId) {
    if (flowId == 0u) {

    return (event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish)
        && event.scopeId == flowId;

    return isValidEventName(event.name) && isValidEventName(name)
        && std::strcmp(event.name, name) == 0;

bool isFlowEventPhase(EventPhase phase) {

bool eventMatchesFlowId(const ProfileEvent& event, u32 flowId) {
    return flowId != 0u && isFlowEventPhase(event.phase) && event.scopeId == flowId
        && isValidEventName(event.name);

bool eventMatchesName(const ProfileEvent& event, const char* name) {
    if (!isValidEventName(name) || !isValidEventName(event.name)) {
    return std::strcmp(event.name, name) == 0;

bool isFlowPhase(EventPhase phase) {

bool eventMatchesFlow(const ProfileEvent& event, u32 flowId) {
    return isFlowPhase(event.phase) && event.scopeId == flowId && isValidEventName(event.name);






    if (event.phase != EventPhase::FlowStart && event.phase != EventPhase::FlowFinish) {
    return isValidEventName(event.name) && event.scopeId == flowId;


bool isFlowPhase(EventPhase phase) {
    return phase == EventPhase::FlowStart || phase == EventPhase::FlowFinish;
}

bool eventNameMatches(const ProfileEvent& event, const char* name) {
    return isValidEventName(event.name) && isValidEventName(name) && std::strcmp(event.name, name) == 0;
}

ProfileScope::ProfileScope(const char* name)
    : m_name(name),
      m_active(g_enabled.load(std::memory_order_acquire) && isValidEventName(name)) {
      m_active(g_enabled.load(std::memory_order_acquire) && name != nullptr) {
      m_active(name != nullptr && g_enabled.load(std::memory_order_acquire)) {
      m_active(g_enabled.load(std::memory_order_acquire) && isNonEmptyProfileName(name)) {
      m_active(g_enabled.load(std::memory_order_acquire) && isRecordableName(name)) {
      m_active(g_enabled.load(std::memory_order_acquire) && isValidProfileName(name)) {
      m_active(g_enabled.load(std::memory_order_acquire) && eventNameIsRecordable(name)) {
    if (g_enabled.load(std::memory_order_acquire) && !isValidEventName(name)) {
        recordRejectedInvalidName();
    }
      m_active(g_enabled.load(std::memory_order_acquire) && !shouldRejectEventName(name)) {
      m_active(wouldRecordWithName(name)) {
      m_active(g_enabled.load(std::memory_order_acquire) && shouldRecordEventName(name)) {
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

bool isValidEventName(const char* name) {
    return name != nullptr && name[0] != '\0';
}

bool canRecordScope(const char* name) {
    return enabled() && isValidEventName(name);
}

bool canBeginAsyncFlow(const char* name) {
    return enabled() && isValidEventName(name);
}

bool canEndAsyncFlow(const char* name) {
    return enabled() && isValidEventName(name)
           && g_openAsyncFlowCount.load(std::memory_order_acquire) > 0u;

bool canSampleCounter(const char* track) {
    return enabled() && isValidEventName(track);

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

u32 ringBufferCapacity() {
u32 exportableEventCount() {
    const u32 count = eventCount();
    u32 exportable = 0u;
    for (u32 i = 0u; i < count; ++i) {
        if (isValidProfileEvent(eventAt(i))) {
            ++exportable;
    return exportable;


u32 totalEventsWritten() {
    return g_totalEventsWritten.load(std::memory_order_acquire);
}

    const u32 written = totalEventsWritten();
    const u32 retained = eventCount();
    return written > retained ? written - retained : 0u;

bool isRingSaturated() {
    return eventCount() >= kRingCapacity;
u32 droppedEventCount() {
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
u32 orphanAsyncFlowEndCount() {
    return g_orphanAsyncFlowEndCount.load(std::memory_order_acquire);
}

bool hasOpenAsyncFlows() {
    return openAsyncFlowCount() > 0u;
}

u32 orphanAsyncFlowEndCount() {
    return g_orphanAsyncFlowEndCount.load(std::memory_order_acquire);
}

bool hasOrphanAsyncFlowEnds() {
    return orphanAsyncFlowEndCount() > 0u;

u32 rejectedInvalidNameCount() {
    return g_rejectedInvalidNameCount.load(std::memory_order_acquire);

bool hasRejectedInvalidNames() {
    return rejectedInvalidNameCount() > 0u;

u32 remainingEventCapacity() {
    const u32 count = eventCount();
    return count >= kRingCapacity ? 0u : kRingCapacity - count;
bool hasActiveScope() {
    return scopeNestingDepth() > 0u;

bool hasActiveFlowDepth() {
    return flowNestingDepth() > 0u;
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

bool isProfilerGuardStateBalanced() {
    return isScopeNestingBalanced() && isFlowNestingBalanced() && !hasOpenAsyncFlows();
}

bool isProfilerGuardStateBalanced() {
    return isScopeNestingBalanced() && isFlowNestingBalanced() && !hasOpenAsyncFlows();
}

bool isValidEventName(const char* name) {
    return name != nullptr && name[0] != '\0';
}

bool isProfilerGuardStateBalanced() {
    return isScopeNestingBalanced() && isFlowNestingBalanced() && !hasOpenAsyncFlows();
}

bool isGuardStateBalanced() {
    return isScopeNestingBalanced() && isFlowNestingBalanced() && !hasOpenAsyncFlows();
}

bool isProfilerNestingPreflightOk() {
    return isScopeNestingBalanced() && isFlowNestingBalanced() && !hasOpenAsyncFlows();
}

bool isValidEventName(const char* name) {
    return name != nullptr && name[0] != '\0';
}

NestingPreflight preflightNesting() {
    NestingPreflight preflight{};
    preflight.scopeDepth = nestingDepth();
    preflight.flowDepth = flowNestingDepth();
    preflight.openFlowCount = openAsyncFlowCount();
    preflight.scopeBalanced = preflight.scopeDepth == 0u;
    preflight.flowBalanced = preflight.flowDepth == 0u;
    preflight.hasOpenFlows = preflight.openFlowCount > 0u;
    return preflight;
}

ProfileScopePreflight preflightProfileScope(const char* name) {
    ProfileScopePreflight preflight{};
    preflight.emptyName = !isValidEventName(name);
    preflight.disabled = !enabled();
    return preflight;
}

AsyncFlowPreflight preflightBeginAsyncFlow(const char* name) {
    AsyncFlowPreflight preflight{};
    preflight.emptyName = !isValidEventName(name);
    preflight.disabled = !enabled();
    return preflight;
}

AsyncFlowPreflight preflightEndAsyncFlow(const char* name) {
    AsyncFlowPreflight preflight{};
    preflight.emptyName = !isValidEventName(name);
    preflight.disabled = !enabled();
    preflight.orphanEnd = openAsyncFlowCount() == 0u;
    return preflight;
}

ExportPreflight preflightExport() {
    ExportPreflight preflight{};
    preflight.disabled = !enabled();
    preflight.bufferedEventCount = eventCount();
    preflight.emptyBuffer = preflight.bufferedEventCount == 0u;
    preflight.frameIndex = frameIndex();

    for (u32 i = 0; i < preflight.bufferedEventCount; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (isValidEventName(event.name)) {
            ++preflight.exportableEventCount;
        } else {
            ++preflight.skippedInvalidNames;
        }
    }

    return preflight;
}

EventLookupPreflight preflightEventAt(u32 index) {
    EventLookupPreflight preflight{};
    preflight.index = index;
    preflight.eventCount = eventCount();
    preflight.emptyBuffer = preflight.eventCount == 0u;
    preflight.indexOutOfRange = index >= preflight.eventCount;

    if (!preflight.canLookup()) {
        return preflight;
    }

    const ProfileEvent& event = eventAt(index);
    preflight.invalidEvent = !isValidProfileEvent(event);
    return preflight;
}

EventLookupPreflight preflightLastEvent() {
    const u32 index = lastEventIndex();
    if (index == kInvalidEventIndex) {
        EventLookupPreflight preflight{};
        preflight.emptyBuffer = true;
        preflight.indexOutOfRange = true;
        return preflight;
    }

    return preflightEventAt(index);
}

bool canEnterProfileScope(const char* name) {
    return preflightProfileScope(name).canEnter();
}

bool canBeginAsyncFlow(const char* name) {
    return preflightBeginAsyncFlow(name).canBegin();
}

bool canEndAsyncFlow(const char* name) {
    return preflightEndAsyncFlow(name).canEnd();
}

bool canExportChromeTrace() {
    return preflightExport().canExport();
}

bool canLookupEventAt(u32 index) {
    return preflightEventAt(index).canLookup();
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

bool isFlowOpenCountAttached() {
    return !isFlowDepthDetached();
}

bool canEndAsyncFlow() {
    return openAsyncFlowCount() > 0u;
}

bool hasActiveScopes() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveFlows() {
    return flowNestingDepth() > 0u;
}

void reconcileDetachedFlowDepth() {
    if (!isFlowDepthDetached()) {
        return;
    }

    threadLocalFlowNestingDepth() = openAsyncFlowCount();
}

u32 orphanAsyncFlowEndCount() {
    return g_orphanAsyncFlowEndCount.load(std::memory_order_acquire);
}

bool hasResidualFlowNestingDepth() {
    return flowNestingDepth() > 0u && openAsyncFlowCount() == 0u;
}

bool needsFlowNestingCleanup() {
    return isFlowDepthDetached() || (flowNestingDepth() > 0u && !hasOpenAsyncFlows());
}

bool wouldIgnoreOrphanAsyncFlowEnd() {
    return openAsyncFlowCount() == 0u;
}

bool wouldRecordEvent(const char* name) {
    return enabled() && isValidEventName(name);
}

void reconcileDetachedFlowNesting() {
    if (!isFlowDepthDetached()) {
        return;
    }

    threadLocalFlowNestingDepth() = openAsyncFlowCount();
}

bool hasActiveScope() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveAsyncFlowNesting() {
    return flowNestingDepth() > 0u;
}

u32 flowDepthMismatch() {
    const u32 localDepth = flowNestingDepth();
    const u32 openCount = openAsyncFlowCount();
    return localDepth > openCount ? localDepth - openCount : openCount - localDepth;
}

bool reconcileDetachedFlowDepth() {
    if (!isFlowDepthDetached()) {
        return false;
    }

    while (flowNestingDepth() > openAsyncFlowCount()) {
        popFlowNestingDepth();
    }
    return true;
}

bool canEndAsyncFlow() {
    return enabled() && openAsyncFlowCount() > 0u;
}

void reconcileDetachedFlowNestingDepth() {
    if (!isFlowDepthDetached()) {
        return;
    }

    u32& depth = threadLocalFlowNestingDepth();
    const u32 openCount = openAsyncFlowCount();
    while (depth > openCount) {
        popFlowNestingDepth();
        depth = threadLocalFlowNestingDepth();
    }
}

bool hasActiveScope() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveAsyncFlowNesting() {
    return flowNestingDepth() > 0u;
}

bool isProfilerGuardStateBalanced() {
    return isScopeNestingBalanced() && isFlowNestingBalanced() && !hasOpenAsyncFlows();
}

u32 ignoredAsyncFlowEndCount() {
    return g_ignoredAsyncFlowEndCount.load(std::memory_order_acquire);
}

bool hasIgnoredAsyncFlowEnds() {
    return ignoredAsyncFlowEndCount() > 0u;
}

u32 orphanAsyncFlowEndCount() {
    return g_orphanAsyncFlowEndCount.load(std::memory_order_acquire);
}

bool hasActiveScope() {
    return scopeNestingDepth() > 0u;
}

u32 flowDepthMismatch() {
    const u32 flowDepth = flowNestingDepth();
    const u32 openCount = openAsyncFlowCount();
    return flowDepth >= openCount ? flowDepth - openCount : openCount - flowDepth;
}

void reconcilePendingFlowHandoff() {
    if (!isCrossThreadFlowHandoffPending()) {
        return;
    }

    popFlowNestingDepth();
}

bool hasActiveScopeNesting() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveFlowNesting() {
    return flowNestingDepth() > 0u;
}

bool hasActiveProfilingNesting() {
    return hasActiveScopeNesting() || hasActiveFlowNesting();
}

bool hasActiveScopeNesting() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveFlowNesting() {
    return flowNestingDepth() > 0u;
}

bool hasNestedAsyncFlowContext() {
    return hasActiveScopeNesting() && hasActiveFlowNesting();
}

bool hasActiveScopes() {
    return scopeNestingDepth() > 0u;
}

bool isFlowNestingConsistent() {
    return flowNestingDepth() == openAsyncFlowCount();
}

bool hasActiveScope() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveAsyncFlowNesting() {
    return flowNestingDepth() > 0u;
}

bool isAsyncFlowIdPaired(u32 flowId) {
    u32 startCount = 0u;
    u32 finishCount = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (!isExportableFlowEvent(event, flowId)) {
            continue;
        }
        if (event.phase == EventPhase::FlowStart) {
            ++startCount;
        } else {
            ++finishCount;
        }
    }
    return startCount == finishCount;
}

bool hasUnpairedAsyncFlowsInBuffer() {
    return unpairedAsyncFlowIdCount() > 0u;
}

u32 unpairedAsyncFlowIdCount() {
    u32 unpairedCount = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (!isAsyncFlowPhase(event.phase) || !isValidEventName(event.name)) {
            continue;
        }

        const u32 flowId = event.scopeId;
        bool seen = false;
        for (u32 j = 0u; j < i; ++j) {
            const ProfileEvent& prior = eventAt(j);
            if (isAsyncFlowPhase(prior.phase) && prior.scopeId == flowId) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }

        if (!isAsyncFlowIdPaired(flowId)) {
            ++unpairedCount;
        }
    }
    return unpairedCount;
}

bool isScopeNameBalancedInBuffer(const char* name) {
    if (!isValidEventName(name)) {
        return true;
    }

    u32 beginCount = 0u;
    u32 endCount = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (!eventNameMatches(event, name)) {
            continue;
        }
        if (event.phase == EventPhase::Begin) {
            ++beginCount;
        } else if (event.phase == EventPhase::End) {
            ++endCount;
        }
    }
    return beginCount == endCount;
}

bool hasActiveScope() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveAsyncFlowNesting() {
    return flowNestingDepth() > 0u;
}

bool hasActiveScope() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveAsyncFlowNesting() {
    return flowNestingDepth() > 0u;
}

bool hasActiveScope() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveAsyncFlowNesting() {
    return flowNestingDepth() > 0u;
}

bool isNestingPreflightClean() {
    return isScopeNestingBalanced() && isFlowNestingBalanced() && !isFlowDepthDetached()
        && !isCrossThreadFlowHandoffPending();
}

bool hasActiveScope() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveAsyncFlowNesting() {
    return flowNestingDepth() > 0u;
}

bool hasActiveScope() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveAsyncFlowNesting() {
    return flowNestingDepth() > 0u;
}

bool isFlowIdOpen(u32 flowId) {
    u32 openStarts = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (!isValidEventName(event.name) || !isFlowPhaseEvent(event) || event.scopeId != flowId) {
            continue;
        }

        if (event.phase == EventPhase::FlowStart) {
            ++openStarts;
        } else {
            if (openStarts > 0u) {
                --openStarts;
            }
        }
    }
    return openStarts > 0u;
}

bool hasActiveScopeNesting() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveFlowNesting() {
    return flowNestingDepth() > 0u;
}

bool hasNestedAsyncFlowContext() {
    return hasActiveScopeNesting() && hasActiveFlowNesting();
}

bool hasActiveScope() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveAsyncFlowNesting() {
    return flowNestingDepth() > 0u;
}

bool hasActiveScope() {
    return scopeNestingDepth() > 0u;
}

bool hasActiveAsyncFlowNesting() {
    return flowNestingDepth() > 0u;
}

bool hasNestingCleanupPending() {
    return hasUnbalancedNesting() || isFlowDepthDetached() || isCrossThreadFlowHandoffPending();
}

bool isFlowPhase(EventPhase phase) {
    return phase == EventPhase::FlowStart || phase == EventPhase::FlowFinish;
}

bool eventMatchesName(const ProfileEvent& event, const char* name) {
    return isValidEventName(event.name) && isValidEventName(name)
        && std::strcmp(event.name, name) == 0;
}

bool eventMatchesFlowId(const ProfileEvent& event, u32 flowId) {
    return flowId != 0u && isFlowPhase(event.phase) && event.scopeId == flowId
        && isValidEventName(event.name);
}

bool hasMatchingFlowFinish(const ProfileEvent& flowStart) {
    if (flowStart.phase != EventPhase::FlowStart || !isValidEventName(flowStart.name)) {
        return false;
    }

    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.phase == EventPhase::FlowFinish && event.scopeId == flowStart.scopeId
            && isValidEventName(event.name)) {
            return true;
        }
    }
    return false;
}

bool hasMatchingFlowStart(const ProfileEvent& flowFinish) {
    if (flowFinish.phase != EventPhase::FlowFinish || !isValidEventName(flowFinish.name)) {
        return false;
    }

    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.phase == EventPhase::FlowStart && event.scopeId == flowFinish.scopeId
            && isValidEventName(event.name)) {
            return true;
        }
    }
    return false;
}

u32 orphanFlowStartCount() {
    u32 orphans = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.phase == EventPhase::FlowStart && isValidEventName(event.name)
            && !hasMatchingFlowFinish(event)) {
            ++orphans;
        }
    }
    return orphans;
}

u32 orphanFlowFinishCount() {
    u32 orphans = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.phase == EventPhase::FlowFinish && isValidEventName(event.name)
            && !hasMatchingFlowStart(event)) {
            ++orphans;
        }
    }
    return orphans;
}

bool hasOrphanFlowEvents() {
    return orphanFlowStartCount() > 0u || orphanFlowFinishCount() > 0u;
}

bool wouldSkipProfileScope(const char* name) {
    return !g_enabled.load(std::memory_order_acquire) || !isValidEventName(name);
}

bool wouldSkipAsyncFlowBegin(const char* name) {
    return !g_enabled.load(std::memory_order_acquire) || !isValidEventName(name);
}

bool wouldSkipAsyncFlowEnd(const char* name) {
    return !g_enabled.load(std::memory_order_acquire) || !isValidEventName(name)
        || g_openAsyncFlowCount.load(std::memory_order_acquire) == 0u;
}

bool wouldSkipCounterSample(const char* track) {
    return !g_enabled.load(std::memory_order_acquire) || !isValidEventName(track);
}

bool wouldSkipChromeTraceExport() {
    return !enabled();
}

bool wouldSkipChromeTraceExportSafely() {
    return !preflightChromeTraceExport().canExportSafely();
}

bool wouldSkipProfileScope(const char* name) {
    return !enabled() || !isValidEventName(name);
}

bool wouldSkipAsyncFlowBegin(const char* name) {
    return !enabled() || !isValidEventName(name);
}

bool wouldSkipAsyncFlowEnd(const char* name, u32 /*flowId*/) {
    if (!enabled() || !isValidEventName(name)) {
        return true;
    }
    return openAsyncFlowCount() == 0u;
}

bool wouldSkipCounterSample(const char* track) {
    return !enabled() || !isValidEventName(track);
}

bool wouldSkipChromeTraceExport() {
    return !enabled();
}

bool hasEvents() {
    return eventCount() > 0u;
}

bool hasExportableEvents() {
    return hasEvents();
}

bool hasOpenAsyncFlows() {
    return openAsyncFlowCount() > 0u;
    return exportableEventCount() > 0u;
}

bool isBufferEmpty() {
    return eventCount() == 0u;
}

bool isBufferFull() {
    return eventCount() >= kRingCapacity;
bool hasOpenAsyncFlows() {
    return openAsyncFlowCount() > 0u;

    return eventCount() >= ringCapacity();

bool hasRingBufferWrapped() {
    return totalWriteCount() > kRingCapacity;
}

u32 totalWriteCount() {
    return g_writeHead.load(std::memory_order_acquire);
}

u32 droppedEventCount() {
    const u32 writes = totalWriteCount();
    return writes > kRingCapacity ? writes - kRingCapacity : 0u;
}

u32 nonExportableEventCount() {
    const u32 total = eventCount();
    u32 nonExportable = 0u;
    for (u32 i = 0u; i < total; ++i) {
        if (!isValidEventName(eventAt(i).name)) {
            ++nonExportable;
        }
    }
    return nonExportable;
}

u32 remainingEventCapacity() {
    const u32 count = eventCount();
    return count >= kRingCapacity ? 0u : kRingCapacity - count;
}

bool isEventIndexValid(u32 index) {
    if (index == kInvalidEventIndex) {
        return false;
    }
    return index < eventCount();

bool isBlankEventName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return true;

    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        const char ch = *cursor;
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            return false;

bool isEmptyEventName(const char* name) {
    return name == nullptr || name[0] == '\0';
}

bool isBlankEventName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
bool isNullOrEmptyEventName(const char* name) {
    return name == nullptr || name[0] == '\0';
}

    if (isNullOrEmptyEventName(name)) {
        return true;
    }

    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            break;
        default:
            return false;
bool isInvalidEventIndex(u32 index) {
    return index == kInvalidEventIndex;
        if (!std::isspace(static_cast<unsigned char>(*cursor))) {
        }
    return true;
        const char ch = *cursor;
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {

bool isValidEventName(const char* name) {

        if (*cursor != ' ' && *cursor != '\t' && *cursor != '\n' && *cursor != '\r') {
    if (name == nullptr || name[0] == '\0') {
    }

    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        if (!std::isspace(static_cast<unsigned char>(*cursor))) {
            return true;

    return !isNullOrEmptyEventName(name);

bool wouldRecordWithName(const char* name) {
    return g_enabled.load(std::memory_order_acquire) && isValidEventName(name) && !isBlankEventName(name);

    return !isBlankEventName(name);
}

bool isFirstEventIndex(u32 index) {
    return hasEvents() && index == 0u;
bool isNullOrEmptyEventName(const char* name) {
    return !isValidEventName(name);
}

bool wouldRecordWithName(const char* name) {
    return enabled() && isValidEventName(name);
bool isNullEventName(const char* name) {
    return name == nullptr;

bool isEmptyEventName(const char* name) {
    return name != nullptr && name[0] == '\0';

bool isBlankEventName(const char* name) {
    return isNullEventName(name) || isEmptyEventName(name) || isWhitespaceOnlyEventName(name);

bool tryValidateEventName(const char* name, EventNameRejectReason& outReason) {
    if (isNullEventName(name)) {
        outReason = EventNameRejectReason::Null;
        return false;
    if (isEmptyEventName(name)) {
        outReason = EventNameRejectReason::Empty;
    if (isWhitespaceOnlyEventName(name)) {
        outReason = EventNameRejectReason::Blank;
    outReason = EventNameRejectReason::None;
    return true;

EventNameRejectReason eventNameRejectReason(const char* name) {
    EventNameRejectReason reason = EventNameRejectReason::None;
    tryValidateEventName(name, reason);
    return reason;

bool wouldRecordEventName(const char* name) {
    return enabled() && tryValidateEventName(name, reason);

u32 rejectedInvalidNameCount() {
    return g_rejectedInvalidNameCount.load(std::memory_order_acquire);

bool hasRejectedInvalidNames() {
    return rejectedInvalidNameCount() > 0u;
    if (name == nullptr) {
        return EventNameRejectReason::Null;
    if (name[0] == '\0') {
        return EventNameRejectReason::Empty;
    return EventNameRejectReason::None;

const char* eventNameRejectReasonLabel(EventNameRejectReason reason) {
    switch (reason) {
    case EventNameRejectReason::None:
        return "none";
    case EventNameRejectReason::Null:
        return "null";
    case EventNameRejectReason::Empty:
        return "empty";
    return "unknown";

bool wouldRecordEvent(const char* name) {

bool canBeginAsyncFlow(const char* name) {
    return wouldRecordEvent(name);

bool canEndAsyncFlow(const char* name) {
    return wouldRecordEvent(name) && openAsyncFlowCount() > 0u;

bool canSampleCounter(const char* track) {
    return wouldRecordEvent(track);
bool isWhitespaceOnlyEventName(const char* name) {
    if (name == nullptr || name[0] == '\0') {

    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        if (!std::isspace(static_cast<unsigned char>(*cursor))) {

bool eventNameMatches(const char* lhs, const char* rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return lhs == rhs;
    return std::strcmp(lhs, rhs) == 0;

bool isLookupNameValid(const char* name) {
    return isValidEventName(name);

    return name == nullptr || name[0] == '\0';

        const char ch = *cursor;
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
bool isValidFlowId(u32 flowId) {
    return flowId != 0u;

bool isFlowEventPhase(EventPhase phase) {
    return phase == EventPhase::FlowStart || phase == EventPhase::FlowFinish;

bool eventMatchesName(const ProfileEvent& event, const char* name) {
    if (!isValidEventName(name) || !isValidEventName(event.name)) {

    return std::strcmp(event.name, name) == 0;

bool eventMatchesFlowId(const ProfileEvent& event, u32 flowId) {
    if (!isValidFlowId(flowId) || !isFlowEventPhase(event.phase) || !isValidEventName(event.name)) {

    return event.scopeId == flowId;
bool eventNameMatches(const char* eventName, const char* name) {
    if (!isValidEventName(eventName) || !isValidEventName(name)) {
    return std::strcmp(eventName, name) == 0;

bool isFlowPhaseEvent(const ProfileEvent& event) {
    return event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish;

bool eventFlowIdMatches(const ProfileEvent& event, u32 flowId) {
    return isFlowPhaseEvent(event) && isValidEventName(event.name) && event.scopeId == flowId;

bool isFlowEventForId(const ProfileEvent& event, u32 flowId) {
    return isValidEventName(event.name)
        && (event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish)
        && event.scopeId == flowId;
namespace {

bool eventNameMatches(const ProfileEvent& event, const char* name) {

bool isFlowPhase(EventPhase phase) {

    return flowId != 0u && isFlowPhase(event.phase) && event.scopeId == flowId;

} // namespace




    return isValidEventName(name) && isValidEventName(event.name)
        && std::strcmp(event.name, name) == 0;


    return isValidFlowId(flowId) && isFlowPhaseEvent(event) && isValidEventName(event.name)



bool profileEventNameEquals(const ProfileEvent& event, const char* name) {


bool profileEventMatchesFlowId(const ProfileEvent& event, u32 flowId) {
    if (flowId == 0u) {

    if (event.phase != EventPhase::FlowStart && event.phase != EventPhase::FlowFinish) {

    return isValidEventName(event.name) && event.scopeId == flowId;



bool isAsyncFlowPhase(EventPhase phase) {



    return isAsyncFlowPhase(event.phase) && event.scopeId == flowId && isValidEventName(event.name);




    return isValidEventName(event.name) && isValidEventName(name) && std::strcmp(event.name, name) == 0;

bool isAsyncFlowEvent(const ProfileEvent& event) {

bool flowEventMatchesId(const ProfileEvent& event, u32 flowId) {
    return isValidEventName(event.name) && isAsyncFlowEvent(event) && event.scopeId == flowId;

bool eventNameMatches(const char* eventName, const char* queryName) {
    return isValidEventName(eventName) && isValidEventName(queryName)
        && std::strcmp(eventName, queryName) == 0;



bool isValidProfileEvent(const ProfileEvent& event) {
    return tryValidateEventName(event.name, reason);


    return shouldRecordEventName(event.name);
}

bool isLastEventIndex(u32 index) {
    const u32 count = eventCount();
    return count > 0u && index == count - 1u;
}

u32 countEventsWithPhase(EventPhase phase) {
bool isFlowPhaseEvent(const ProfileEvent& event) {
    return event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish;
}

bool eventNameMatches(const ProfileEvent& event, const char* name) {
    if (!isValidEventName(name) || !isValidEventName(event.name)) {
        return false;
    return std::strcmp(event.name, name) == 0;

u32 invalidNameEventCount() {
    return nonExportableEventCount();
}

bool hasInvalidNameEvents() {
    return invalidNameEventCount() > 0u;

u32 nonExportableEventCount() {
    const u32 total = eventCount();
    const u32 exportable = exportableEventCount();
    return total >= exportable ? total - exportable : 0u;

u32 exportableEventCount() {
    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (eventAt(i).phase == phase) {
        if (isEventExportable(i)) {
        if (shouldRecordEventName(eventAt(i).name)) {
            ++count;
        }
    }
    return count;
}

bool isNullEventName(const char* name) {
    return name == nullptr;
}

bool isEmptyEventName(const char* name) {
    return name != nullptr && name[0] == '\0';

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
    if (name == nullptr || name[0] == '\0') {
        return false;

    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case '\f':
        case '\v':
            continue;
        default:
            return true;

    return !isEmptyEventName(name);
    return !isBlankEventName(name);
    return !isNullEventName(name) && !isEmptyEventName(name);


    return isValidEventName(name);

bool isValidProfilerName(const char* name) {

bool isBlankEventName(const char* name) {

            break;



bool wouldRecordEventName(const char* name) {
    return g_enabled.load(std::memory_order_acquire) && isValidEventName(name);

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

bool canRecordEvent(const char* name) {
    return enabled() && isValidEventName(name);

bool canBeginAsyncFlow(const char* name) {
    return canRecordEvent(name);

bool canEndAsyncFlow(const char* name) {
    return canRecordEvent(name) && openAsyncFlowCount() > 0u;

bool isExportableProfileEvent(const ProfileEvent& event) {
    return isValidProfileEvent(event);

u32 exportableEventCount() {
    u32 count = 0u;
    for (u32 i = 0u; i < total; ++i) {
        if (isValidEventName(eventAt(i).name)) {
            ++count;
    return count;

u32 invalidEventCount() {
    return total >= exportableEventCount() ? total - exportableEventCount() : 0u;

        if (!isValidEventName(eventAt(i).name)) {


u32 nonExportableEventCount() {

bool isEventExportable(u32 index) {
    return isEventIndexValid(index) && isValidEventName(eventAt(index).name);
    return isRecordableName(event.name);
    if (!isEventIndexValid(index)) {

    EventNameRejectReason reason = EventNameRejectReason::None;
    return tryValidateEventName(eventAt(index).name, reason);

EventLookupRejectReason eventLookupRejectReason(u32 index) {
    if (isBufferEmpty()) {
        return EventLookupRejectReason::EmptyBuffer;
        return EventLookupRejectReason::OutOfRange;

    EventNameRejectReason nameReason = EventNameRejectReason::None;
    if (!tryValidateEventName(eventAt(index).name, nameReason)) {
        return EventLookupRejectReason::InvalidEvent;
    return EventLookupRejectReason::None;

bool tryCanLookupEventAt(u32 index, EventLookupRejectReason& outReason) {
    outReason = eventLookupRejectReason(index);
    return outReason == EventLookupRejectReason::None;

bool tryExportableEventAt(u32 index, ProfileEvent& outEvent) {
    EventLookupRejectReason lookupReason = EventLookupRejectReason::None;
    if (!tryCanLookupEventAt(index, lookupReason)) {
        outEvent = ProfileEvent{};

    outEvent = eventAt(index);
    return isEventExportable(index);

bool tryFirstExportableEvent(ProfileEvent& outEvent) {
    const u32 index = firstEventIndex();
    if (index == kInvalidEventIndex) {

    return tryExportableEventAt(index, outEvent);

bool tryLastExportableEvent(ProfileEvent& outEvent) {
    const u32 index = lastEventIndex();

u32 droppedEventCount() {
    return g_droppedEventCount.load(std::memory_order_acquire);

bool hasDroppedEvents() {
    return droppedEventCount() > 0u;

    return isEventIndexValid(index) && shouldRecordEventName(eventAt(index).name);
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
namespace {

NestingStateRejectReason diagnoseNestingStateRejectReason() {
    if (!isScopeNestingBalanced()) {
        return NestingStateRejectReason::UnbalancedScopeNesting;
    }
    if (isFlowDepthDetached()) {
        return NestingStateRejectReason::FlowDepthDetached;
    if (hasOpenAsyncFlows()) {
        return NestingStateRejectReason::OpenAsyncFlows;
    if (!isFlowNestingBalanced()) {
        return NestingStateRejectReason::UnbalancedFlowNesting;
    return NestingStateRejectReason::None;

ChromeTraceExportRejectReason diagnoseChromeTraceExportRejectReason() {
    if (!enabled()) {
        return ChromeTraceExportRejectReason::ProfilerDisabled;
    if (exportableEventCount() == 0u) {
        return ChromeTraceExportRejectReason::NoExportableEvents;
        return ChromeTraceExportRejectReason::UnbalancedScopeNesting;
        return ChromeTraceExportRejectReason::FlowDepthDetached;
        return ChromeTraceExportRejectReason::OpenAsyncFlows;
        return ChromeTraceExportRejectReason::UnbalancedFlowNesting;
    return ChromeTraceExportRejectReason::None;

} // namespace




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

bool isEventLookupPreflightOk(u32 index) {
    if (!isEventIndexValid(index)) {
        return false;

    return isValidProfileEvent(eventAt(index));

bool isEventNameValid(const char* name) {
    return isValidEventName(name);










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
u32 nonExportableEventCount() {
    const u32 total = eventCount();
    const u32 exportable = exportableEventCount();
    return total >= exportable ? total - exportable : 0u;

bool hasNonExportableEvents() {
    return nonExportableEventCount() > 0u;

bool isFirstEventIndex(u32 index) {
    const u32 first = firstEventIndex();
    return first != kInvalidEventIndex && index == first;
u32 droppedEventCount() {
    const u32 head = g_writeHead.load(std::memory_order_acquire);
    return head > kRingCapacity ? head - kRingCapacity : 0u;

    return hasEvents() && index == 0u;

bool isLastEventIndex(u32 index) {
    const u32 last = lastEventIndex();
    return last != kInvalidEventIndex && index == last;

u32 countEventsWithPhase(EventPhase phase) {
u32 countEventsByPhase(EventPhase phase) {
    u32 count = 0u;
    for (u32 i = 0u; i < total; ++i) {
        if (eventAt(i).phase == phase) {
            ++count;
    return count;

u32 findFirstEventIndexWithPhase(EventPhase phase) {
            return i;
    return kInvalidEventIndex;
u32 exportableFirstEventIndex() {
    const u32 count = eventCount();
    for (u32 i = 0u; i < count; ++i) {
        if (isEventExportable(i)) {

u32 exportableLastEventIndex() {

    for (u32 i = count; i > 0u; --i) {
        const u32 index = i - 1u;
        if (isEventExportable(index)) {
            return index;

u32 findFirstEventIndex(EventPhase phase) {

u32 findLastEventIndex(EventPhase phase) {
    for (u32 i = total; i > 0u; --i) {
        if (eventAt(i - 1u).phase == phase) {
            return i - 1u;

bool tryFindEventByName(const char* name, u32 startIndex, u32& outIndex, ProfileEvent& outEvent) {
    if (!isValidEventName(name)) {
        outIndex = kInvalidEventIndex;
        outEvent = ProfileEvent{};

    if (startIndex >= total) {

    for (u32 i = startIndex; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.name != nullptr && std::strcmp(event.name, name) == 0) {
            outIndex = i;
            outEvent = event;
            return isValidProfileEvent(outEvent);


    case NestingStateRejectReason::FlowDepthDetached:
        return "flow_depth_detached";


bool tryFindFirstEventWithPhase(EventPhase phase, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexWithPhase(phase);
    if (index == kInvalidEventIndex) {

    return tryEventAt(index, outEvent);

u32 findFirstEventIndexByPhase(EventPhase phase) {

bool tryFindFirstEventByPhase(EventPhase phase, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByPhase(phase);


bool tryEventPhaseAt(u32 index, EventPhase& outPhase) {
    ProfileEvent event{};
    if (!tryEventAt(index, event)) {
        outPhase = EventPhase::Begin;

    outPhase = event.phase;
    return true;
    u32 exportable = 0u;
            ++exportable;
    return total - exportable;

u32 totalEventsWritten() {
    return g_totalEventsWritten.load(std::memory_order_acquire);

bool hasRingWrapped() {
    return totalEventsWritten() > kRingCapacity;


u32 lastExportableEventIndex() {

u32 findLastEventIndexByPhase(EventPhase phase) {
        if (eventAt(index).phase == phase) {





u32 findFirstEventIndexByName(const char* name) {

u32 countEventsOfPhase(EventPhase phase) {

u32 firstEventIndexOfPhase(EventPhase phase) {

u32 lastEventIndexOfPhase(EventPhase phase) {

bool isEventAtPhase(u32 index, EventPhase phase) {
    return isEventIndexValid(index) && eventAt(index).phase == phase;

const ProfileEvent& emptyProfileEvent() {
    static const ProfileEvent kEmpty{};
    return kEmpty;
    return isNonEmptyProfileName(event.name);


bool isEmptyProfileEvent(const ProfileEvent& event) {
    return &event == &emptyProfileEvent() || event.name == nullptr;
}

const ProfileEvent& eventAt(u32 index) {
    if (index == kInvalidEventIndex || !isEventIndexValid(index)) {
        return emptyProfileEvent();

    const u32 count = eventCount();
    if (count == 0u || index >= count) {

    return emptyProfileEventStub();


    return count > 0u && index < count;
















    const u32 count = eventCount();
    const u32 head = g_writeHead.load(std::memory_order_acquire);
    const u32 start = head >= count ? head - count : 0u;
    const u32 ringIndex = (start + index) % kRingCapacity;
    return g_events[ringIndex];
}

bool canLookupEventAt(u32 index) {
    EventLookupRejectReason reason = EventLookupRejectReason::None;
    return tryCanLookupEventAt(index, reason);
}

bool tryCanLookupEventAt(u32 index, EventLookupRejectReason& outReason) {
    if (eventCount() == 0u) {
        outReason = EventLookupRejectReason::EmptyBuffer;
        return false;
    if (index >= eventCount()) {
        outReason = EventLookupRejectReason::OutOfRange;

    const ProfileEvent& candidate = eventAt(index);
    if (!isValidProfileEvent(candidate)) {
        outReason = EventLookupRejectReason::InvalidEvent;

    outReason = EventLookupRejectReason::None;
    return true;



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
    return "unknown";
const char* eventNameAt(u32 index) {

    if (!isEventExportable(index)) {
        return nullptr;
    return eventAt(index).name;

bool peekEventAt(u32 index, ProfileEvent& outEvent) {
namespace {

bool isAsyncFlowPhase(EventPhase phase) {
    return phase == EventPhase::FlowStart || phase == EventPhase::FlowFinish;
}

bool eventNameMatches(const ProfileEvent& event, const char* name) {
    return isValidEventName(name) && isValidEventName(event.name) && std::strcmp(event.name, name) == 0;
}


struct FlowPairCounts {
    u32 startCount = 0u;
    u32 finishCount = 0u;
};

void accumulateFlowPairCounts(std::unordered_map<u32, FlowPairCounts>& counts) {
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (!isValidEventName(event.name) || !isAsyncFlowPhase(event.phase)) {
            continue;
        }

        FlowPairCounts& pair = counts[event.scopeId];
        if (event.phase == EventPhase::FlowStart) {
            ++pair.startCount;
        } else {
            ++pair.finishCount;
        }
    }

} // namespace

bool tryEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventIndexValid(index)) {
        outEvent = ProfileEvent{};

    outEvent = eventAt(index);

bool tryEventAt(u32 index, ProfileEvent& outEvent) {
bool tryRecordedEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventIndexValid(index)) {
        return nullptr;

    const ProfileEvent& event = eventAt(index);
    return isValidEventName(event.name) ? event.name : nullptr;
}

bool tryEventAt(u32 index, ProfileEvent& outEvent) {
    EventLookupRejectReason reason = EventLookupRejectReason::None;
    if (!tryCanLookupEventAt(index, reason)) {
    if (!isEventLookupPreflightOk(index)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return true;
}

bool tryEventAt(u32 index, ProfileEvent& outEvent) {
    if (!tryRecordedEventAt(index, outEvent)) {
        return false;
    }

    return isValidProfileEvent(outEvent);

bool tryExportableEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventExportable(index)) {

    return true;

bool tryEventPhaseAt(u32 index, EventPhase& outPhase) {
    ProfileEvent event{};
    if (!tryEventAt(index, event)) {
        outPhase = EventPhase::Begin;
        return false;
    }

    outPhase = event.phase;
    return true;
}

bool tryEventAtPhase(u32 index, EventPhase phase, ProfileEvent& outEvent) {
    if (!tryEventAt(index, outEvent)) {
        return false;
    }

    return outEvent.phase == phase;
}

bool tryExportableEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventExportable(index)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return true;
}

bool tryEventPhaseAt(u32 index, EventPhase& outPhase) {
    if (!isEventIndexValid(index)) {
        return false;
    }

    outPhase = eventAt(index).phase;
    return true;

bool isFirstEventIndex(u32 index) {
    return hasEvents() && index == firstEventIndex();

bool isLastEventIndex(u32 index) {
    const u32 last = lastEventIndex();
    return last != kInvalidEventIndex && index == last;

u32 countEventsByPhase(EventPhase phase) {
    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (eventAt(i).phase == phase) {
            ++count;
    return count;

bool tryExportableEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventExportable(index)) {
bool tryFindFirstEventByPhase(EventPhase phase, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByPhase(phase);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};

    return tryExportableEventAt(index, outEvent);

bool tryFindLastEventByPhase(EventPhase phase, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByPhase(phase);


bool tryFindFirstEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByName(name);


bool tryFindLastEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByName(name);

bool tryFirstExportableEvent(ProfileEvent& outEvent) {
        if (tryExportableEventAt(i, outEvent)) {


bool tryLastExportableEvent(ProfileEvent& outEvent) {
    for (u32 i = total; i > 0u; --i) {
        if (tryExportableEventAt(i - 1u, outEvent)) {





bool tryFindFirstEventByFlowId(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByFlowId(flowId);


bool tryFindLastEventByFlowId(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByFlowId(flowId);




bool tryFindFirstExportableEventByName(const char* name, ProfileEvent& outEvent) {


bool tryFindFirstExportableEventByFlowId(u32 flowId, ProfileEvent& outEvent) {

bool tryFirstExportableEventByName(const char* name, ProfileEvent& outEvent) {


bool tryLastExportableEventByName(const char* name, ProfileEvent& outEvent) {


bool tryFirstExportableEventByFlowId(u32 flowId, ProfileEvent& outEvent) {


bool tryLastExportableEventByFlowId(u32 flowId, ProfileEvent& outEvent) {

bool tryFirstEventByName(const char* name, ProfileEvent& outEvent) {

    outEvent = eventAt(index);

bool tryLastEventByName(const char* name, ProfileEvent& outEvent) {


bool tryFirstEventByFlowId(u32 flowId, ProfileEvent& outEvent) {


bool tryLastEventByFlowId(u32 flowId, ProfileEvent& outEvent) {

bool tryFindExportableEventByName(const char* name, ProfileEvent& outEvent) {


bool tryFindExportableEventByFlowId(u32 flowId, ProfileEvent& outEvent) {


bool tryFirstEvent(ProfileEvent& outEvent) {
    const u32 index = firstEventIndex();
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return true;
}

bool tryExportableEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventExportable(index)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return true;
}

bool tryExportableEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventExportable(index)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return true;
}

bool tryEventAtPhase(u32 index, EventPhase expectedPhase, ProfileEvent& outEvent) {
    if (!tryEventAt(index, outEvent)) {
        return false;
    }

    if (outEvent.phase != expectedPhase) {
        outEvent = ProfileEvent{};
        return false;
    }

    return true;
}

bool tryExportableEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventExportable(index)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return true;
}

bool tryExportableEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventExportable(index)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return true;
}

bool tryEventAtPhase(u32 index, EventPhase phase, ProfileEvent& outEvent) {
    if (!isEventIndexValid(index)) {
        outEvent = ProfileEvent{};
        return false;
    }

    const ProfileEvent& event = eventAt(index);
    if (event.phase != phase || !isValidProfileEvent(event)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = event;
    return true;
}

bool tryExportableEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventExportable(index)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return true;
}

EventLookupRejectReason eventLookupRejectReason(u32 index) {
    if (isBufferEmpty()) {
        return EventLookupRejectReason::EmptyBuffer;
    }
    if (!isEventIndexValid(index)) {
        return EventLookupRejectReason::OutOfRange;
    }
    if (!isValidProfileEvent(eventAt(index))) {
        return EventLookupRejectReason::NotExportable;
    }
    return EventLookupRejectReason::None;
}

EventLookupRejectReason exportableEventLookupRejectReason(u32 index) {
    if (isBufferEmpty()) {
        return EventLookupRejectReason::EmptyBuffer;
    }
    if (!isEventIndexValid(index)) {
        return EventLookupRejectReason::OutOfRange;
    }
    if (!isEventExportable(index)) {
        return EventLookupRejectReason::NotExportable;
    }
    return EventLookupRejectReason::None;
}

const char* eventLookupRejectReasonLabel(EventLookupRejectReason reason) {
    switch (reason) {
    case EventLookupRejectReason::None:
        return "none";
    case EventLookupRejectReason::EmptyBuffer:
        return "empty_buffer";
    case EventLookupRejectReason::OutOfRange:
        return "out_of_range";
    case EventLookupRejectReason::NotExportable:
        return "not_exportable";
    }
    return "unknown";
}

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









bool hasExportableEvents() {
    const u32 count = eventCount();
    for (u32 i = 0; i < count; ++i) {
        if (isValidProfileEvent(eventAt(i))) {

bool isChromeTraceExportEmpty() {
    return !hasExportableEvents();






}

u32 lastEventIndex() {
    const u32 count = eventCount();
    return count > 0u ? count - 1u : kInvalidEventIndex;
}

bool tryFirstEvent(ProfileEvent& outEvent) {
    return tryEventAt(0u, outEvent);
bool isLastEventIndexValid() {
    return lastEventIndex() != kInvalidEventIndex;
}

bool tryLastEvent(ProfileEvent& outEvent) {
    const u32 index = lastEventIndex();
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryEventAt(index, outEvent);
}

bool hasExportableEvents() {
bool tryFirstExportableEvent(ProfileEvent& outEvent) {
    const u32 index = exportableFirstEventIndex();
    if (index == kInvalidEventIndex) {
bool tryEventAtReverse(u32 reverseIndex, ProfileEvent& outEvent) {
    const u32 count = eventCount();
    if (reverseIndex >= count) {
    const u32 index = firstEventIndex();
    const u32 index = firstExportableEventIndex();
bool tryFindFirstEventByPhase(EventPhase phase, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByPhase(phase);
bool tryFirstEventOfPhase(EventPhase phase, ProfileEvent& outEvent) {
    const u32 index = firstEventIndexOfPhase(phase);
bool tryLastExportableEvent(ProfileEvent& outEvent) {
    const u32 index = lastEventIndex();
bool tryExportableFirstEvent(ProfileEvent& outEvent) {
bool tryFirstEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByName(name);
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);

bool tryLastExportableEvent(ProfileEvent& outEvent) {
    const u32 index = lastExportableEventIndex();

}

    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;


bool tryFirstEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByName(name);


bool tryLastEventByName(const char* name, ProfileEvent& outEvent) {

bool tryExportableLastEvent(ProfileEvent& outEvent) {
    const u32 index = lastEventIndex();


bool tryFindFirstEventIndexByName(const char* name, u32& outIndex) {
        outIndex = kInvalidEventIndex;

    outIndex = index;
    return true;

bool tryFindFirstEventIndexByFlowId(u32 flowId, u32& outIndex) {
    const u32 index = findFirstEventIndexByFlowId(flowId);


bool tryFindFirstEventByName(const char* name, ProfileEvent& outEvent) {

    outEvent = eventAt(index);
    return isValidProfileEvent(outEvent);

bool tryFindLastEventByName(const char* name, ProfileEvent& outEvent) {





















    const u32 index = findLastEventIndexByName(name);


bool tryFirstFlowStartById(u32 flowId, ProfileEvent& outEvent) {
    if (flowId == 0u) {



















    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (isValidEventName(event.name) && event.phase == EventPhase::FlowStart && event.scopeId == flowId) {
            outEvent = event;


bool tryLastFlowFinishById(u32 flowId, ProfileEvent& outEvent) {

    for (u32 i = total; i > 0u; --i) {
        const ProfileEvent& event = eventAt(i - 1u);
        if (isValidEventName(event.name) && event.phase == EventPhase::FlowFinish && event.scopeId == flowId) {



bool tryFirstEventByFlowId(u32 flowId, ProfileEvent& outEvent) {


bool tryFindFirstEventByFlowId(u32 flowId, ProfileEvent& outEvent) {


bool tryFirstFlowEvent(u32 flowId, ProfileEvent& outEvent) {






bool tryLastEventByFlowId(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByFlowId(flowId);



bool tryFindLastEventByFlowId(u32 flowId, ProfileEvent& outEvent) {



bool tryLastFlowEvent(u32 flowId, ProfileEvent& outEvent) {


bool tryFindFirstEventByFlow(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByFlow(flowId);


bool tryFindLastEventByFlow(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByFlow(flowId);









































































































































bool tryFindFirstFlowEvent(u32 flowId, ProfileEvent& outEvent) {


bool tryFindLastFlowEvent(u32 flowId, ProfileEvent& outEvent) {











    return tryEventAt(index, outEvent);







u32 firstEventIndex() {
    return hasEvents() ? 0u : kInvalidEventIndex;
}

    const u32 index = exportableLastEventIndex();
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;

    return tryExportableEventAt(index, outEvent);

bool tryFindFirstEventByPhase(EventPhase phase, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByPhase(phase);

    return tryEventAt(index, outEvent);

bool tryFindLastEventByPhase(EventPhase phase, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByPhase(phase);


bool tryFindFirstEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByName(name);


bool tryFindLastEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByName(name);



bool tryExportableLastEvent(ProfileEvent& outEvent) {
    const u32 index = lastEventIndex();



    outEvent = eventAt(index);
    return isValidProfileEvent(outEvent);





u32 firstEventIndex() {
    return hasEvents() ? 0u : kInvalidEventIndex;

u32 firstExportableEventIndex() {
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (isEventExportable(i)) {
            return i;
        }
    return kInvalidEventIndex;

u32 lastExportableEventIndex() {
    if (total == 0u) {

    for (u32 i = total; i > 0u; --i) {
        const u32 index = i - 1u;
        if (isEventExportable(index)) {
            return index;

u32 findFirstEventIndexByPhase(EventPhase phase) {
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (isEventExportable(i)) {
            return i;
        }
    }
    return kInvalidEventIndex;
}

u32 lastExportableEventIndex() {
    const u32 total = eventCount();
    for (u32 i = total; i > 0u; --i) {
        if (isEventExportable(i - 1u)) {
            return i - 1u;
        }
    }
    return kInvalidEventIndex;
}

u32 findFirstEventIndexByPhase(EventPhase phase) {
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.phase == phase && shouldRecordEventName(event.name)) {
            return i;
    }
bool tryFindFirstEventIndexByPhase(EventPhase phase, u32& outIndex) {
        outIndex = kInvalidEventIndex;

    outIndex = index;
    return true;

bool tryFindLastEventIndexByPhase(EventPhase phase, u32& outIndex) {
    const u32 index = findLastEventIndexByPhase(phase);


bool tryFindFirstEventIndexByName(const char* name, u32& outIndex) {
    const u32 index = findFirstEventIndexByName(name);


bool tryFindLastEventIndexByName(const char* name, u32& outIndex) {
    const u32 index = findLastEventIndexByName(name);


bool tryLastExportableEvent(ProfileEvent& outEvent) {
    const u32 total = eventCount();
    for (u32 i = total; i > 0u; --i) {
        if (tryExportableEventAt(i - 1u, outEvent)) {


u32 firstEventIndex() {
    return hasEvents() ? 0u : kInvalidEventIndex;

    return tryExportableEventAt(index, outEvent);

bool tryLastExportableEvent(ProfileEvent& outEvent) {
    const u32 index = exportableLastEventIndex();

    return tryEventAt(count - 1u - reverseIndex, outEvent);

u32 findEventIndex(EventPhase phase, u32 startIndex) {
    for (u32 i = startIndex; i < count; ++i) {
        if (eventAt(i).phase == phase) {
            return i;
        const ProfileEvent& event = eventAt(i - 1u);
        if (event.phase == phase && shouldRecordEventName(event.name)) {
            return i - 1u;
        }
    return kInvalidEventIndex;

u32 countEventsByPhase(EventPhase phase) {
    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
            ++count;
    return count;

bool tryFindEventByScopeId(u32 scopeId, ProfileEvent& outEvent) {
        const ProfileEvent& event = eventAt(i);
        if (event.scopeId == scopeId && isValidProfileEvent(event)) {
            outEvent = event;
            return true;


    const u32 index = lastEventIndex();

    outEvent = eventAt(index);
    return isExportableProfileEvent(outEvent);

    const u32 index = lastExportableEventIndex();


const char* eventNameAt(u32 index) {
    if (!isEventIndexValid(index)) {
        return nullptr;
    return eventAt(index).name;

bool tryEventPhaseAt(u32 index, EventPhase& outPhase) {
        outPhase = EventPhase::Begin;

    outPhase = eventAt(index).phase;
}

    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;

    return tryEventAt(index, outEvent);

bool tryFindFirstEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByName(name);




bool tryLastEventOfPhase(EventPhase phase, ProfileEvent& outEvent) {
    const u32 index = lastEventIndexOfPhase(phase);


bool tryFindEventByName(const char* name, u32& outIndex) {
    if (!isValidEventName(name)) {
        outIndex = kInvalidEventIndex;

        if (event.name != nullptr && std::strcmp(event.name, name) == 0) {
            outIndex = i;


u32 firstEventIndex() {
    return hasEvents() ? 0u : kInvalidEventIndex;

u32 lastEventIndex() {
    const u32 count = eventCount();
    for (u32 i = 0; i < count; ++i) {
        if (isValidProfileEvent(eventAt(i))) {

bool canExportChromeTrace() {


u32 countEventsByPhase(EventPhase phase) {
    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (eventAt(i).phase == phase) {
        if (event.phase == phase && shouldRecordEventName(event.name)) {
            ++count;
    return count;

u32 findFirstEventIndexByName(const char* name) {
    if (!shouldRecordEventName(name)) {
        return kInvalidEventIndex;

        const ProfileEvent& event = eventAt(i);
        if (shouldRecordEventName(event.name) && event.name != nullptr
            && std::strcmp(event.name, name) == 0) {
            return i;

u32 findLastEventIndexByName(const char* name) {

    for (u32 i = total; i > 0u; --i) {
        const ProfileEvent& event = eventAt(i - 1u);
            return i - 1u;

u32 countEventsByName(const char* name) {
        return 0u;

            ++count;
        }
    }
    return count;
}

u32 findFirstEventIndexOfPhase(EventPhase phase) {
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (eventAt(i).phase == phase) {
            return i;
        }
    return kInvalidEventIndex;

u32 firstExportableEventIndex() {
        if (isEventExportable(i)) {

u32 lastExportableEventIndex() {
    if (total == 0u) {

    for (u32 i = total; i > 0u; --i) {
        if (isEventExportable(i - 1u)) {
bool isScopePairBalancedInBuffer() {
    return countEventsByPhase(EventPhase::Begin) == countEventsByPhase(EventPhase::End);

bool isFlowPairBalancedInBuffer() {
    return countEventsByPhase(EventPhase::FlowStart) == countEventsByPhase(EventPhase::FlowFinish);

u32 findFirstEventIndexByName(const char* name) {
    if (!isValidEventName(name)) {

        const ProfileEvent& event = eventAt(i);
        if (isValidEventName(event.name) && std::strcmp(event.name, name) == 0) {

u32 findLastEventIndexByName(const char* name) {

        const ProfileEvent& event = eventAt(i - 1u);


        const u32 index = i - 1u;
        if (isEventExportable(index)) {
            return index;










bool hasUnpairedScopeEventsInBuffer() {
    return scopeBeginEndEventDelta() > 0u;

bool hasUnpairedAsyncFlowEventsInBuffer() {
    return asyncFlowStartFinishEventDelta() > 0u;

u32 scopeBeginEndEventDelta() {
    const u32 beginCount = countEventsByPhase(EventPhase::Begin);
    const u32 endCount = countEventsByPhase(EventPhase::End);
    return beginCount >= endCount ? beginCount - endCount : endCount - beginCount;

u32 asyncFlowStartFinishEventDelta() {
    const u32 startCount = countEventsByPhase(EventPhase::FlowStart);
    const u32 finishCount = countEventsByPhase(EventPhase::FlowFinish);
    return startCount >= finishCount ? startCount - finishCount : finishCount - startCount;

    if (!isLookupNameValid(name)) {

        if (isValidEventName(event.name) && eventNameMatches(event.name, name)) {



        if (event.name != nullptr && std::strcmp(event.name, name) == 0) {





            return i - 1u;

u32 lastEventIndexByPhase(EventPhase phase) {
u32 countEventsByName(const char* name) {
        return 0u;

    u32 count = 0u;
            ++count;
    return count;

u32 droppedEventCount() {
    return g_droppedEventCount.load(std::memory_order_acquire);

bool tryFindEventIndexByName(const char* name, u32& outIndex) {











u32 findFirstEventIndexByFlow(u32 flowId) {
    if (flowId == 0u) {

        if (isExportableFlowEvent(event, flowId)) {

u32 findLastEventIndexByFlow(u32 flowId) {


u32 countEventsByFlow(u32 flowId) {







u32 findFirstEventIndexByFlowId(u32 flowId) {
        if (isFlowPhase(event.phase) && event.scopeId == flowId && isValidEventName(event.name)) {

u32 findLastEventIndexByFlowId(u32 flowId) {

u32 countEventsByFlowId(u32 flowId) {

        if (eventNameMatches(event.name, name)) {






        if (isAsyncFlowPhase(event.phase) && event.scopeId == flowId && isValidEventName(event.name)) {





bool tryFindFirstEventIndexByPhase(EventPhase phase, u32& outIndex) {
    const u32 index = findFirstEventIndexByPhase(phase);
    if (index == kInvalidEventIndex) {
        outIndex = kInvalidEventIndex;
        return false;

    outIndex = index;
    return true;

bool tryFindLastEventIndexByPhase(EventPhase phase, u32& outIndex) {
    const u32 index = findLastEventIndexByPhase(phase);


bool tryFindFirstEventIndexByName(const char* name, u32& outIndex) {
    const u32 index = findFirstEventIndexByName(name);


u32 findFirstEventIndexByScopeId(u32 scopeId) {
        if (event.scopeId == scopeId && isValidEventName(event.name)) {

u32 countEventsByScopeId(u32 scopeId) {



bool tryFirstExportableEvent(ProfileEvent& outEvent) {
    const u32 index = firstExportableEventIndex();
        outEvent = ProfileEvent{};

    return tryExportableEventAt(index, outEvent);

bool tryLastExportableEvent(ProfileEvent& outEvent) {
    const u32 index = lastExportableEventIndex();



        if (!isValidEventName(event.name)) {
            continue;
        if ((event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish)
            && event.scopeId == flowId) {



u32 exportableFirstEventIndex() {

u32 exportableLastEventIndex() {

bool hasScopeBeginEndMismatch() {
    return countEventsByPhase(EventPhase::Begin) != countEventsByPhase(EventPhase::End);

bool hasFlowStartFinishMismatch() {
    return countEventsByPhase(EventPhase::FlowStart) != countEventsByPhase(EventPhase::FlowFinish);

    const u64 total = g_totalRecorded.load(std::memory_order_acquire);
    const u32 retained = eventCount();
    return total > static_cast<u64>(retained) ? static_cast<u32>(total - static_cast<u64>(retained)) : 0u;
bool hasEventsByPhase(EventPhase phase) {
    return findFirstEventIndexByPhase(phase) != kInvalidEventIndex;

bool tryFindFirstEventByPhase(EventPhase phase, ProfileEvent& outEvent) {


bool tryFindLastEventByPhase(EventPhase phase, ProfileEvent& outEvent) {


        if (isValidEventName(event.name) && std::string(event.name) == name) {




bool namesMatch(const char* lhs, const char* rhs) {
    if (lhs == rhs) {
    if (lhs == nullptr || rhs == nullptr) {
    return std::strcmp(lhs, rhs) == 0;


        if (isValidEventName(event.name) && namesMatch(event.name, name)) {












u32 orphanAsyncFlowEndCount() {
    return g_orphanAsyncFlowEndCount.load(std::memory_order_acquire);

bool hasOrphanAsyncFlowEnds() {
    return orphanAsyncFlowEndCount() > 0u;
bool eventNameMatches(const ProfileEvent& event, const char* name) {
    return event.name != nullptr && std::strcmp(event.name, name) == 0;

bool isFlowPhaseEvent(const ProfileEvent& event) {
    return event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish;


        if (isValidEventName(event.name) && eventNameMatches(event, name)) {





        if (isValidEventName(event.name) && isFlowPhaseEvent(event) && event.scopeId == flowId) {



        if (eventNameMatches(event, name)) {






        if (isValidEventName(event.name) && isAsyncFlowPhase(event.phase) && event.scopeId == flowId) {





bool hasFlowStartEvent(u32 flowId) {

        if (isValidEventName(event.name) && event.phase == EventPhase::FlowStart && event.scopeId == flowId) {

bool hasFlowFinishEvent(u32 flowId) {

        if (isValidEventName(event.name) && event.phase == EventPhase::FlowFinish && event.scopeId == flowId) {

bool isFlowPairRecorded(u32 flowId) {
    return hasFlowStartEvent(flowId) && hasFlowFinishEvent(flowId);

































u32 countDanglingFlowBegins() {
    std::unordered_map<u32, FlowPairCounts> counts;
    accumulateFlowPairCounts(counts);

    u32 dangling = 0u;
    for (const auto& entry : counts) {
        if (entry.second.startCount > entry.second.finishCount) {
            dangling += entry.second.startCount - entry.second.finishCount;
    return dangling;

u32 countOrphanFlowEnds() {

    u32 orphan = 0u;
        if (entry.second.finishCount > entry.second.startCount) {
            orphan += entry.second.finishCount - entry.second.startCount;
    return orphan;

bool isFlowPairingConsistent() {
    return countDanglingFlowBegins() == 0u && countOrphanFlowEnds() == 0u;

bool isNestingStateConsistent() {
    return isScopeNestingBalanced() && isFlowNestingBalanced() && !isFlowDepthDetached();

        if (eventMatchesName(eventAt(i), name)) {


        if (eventMatchesName(eventAt(i - 1u), name)) {

    if (!isValidFlowId(flowId)) {

        if (eventMatchesFlowId(eventAt(i), flowId)) {


        if (eventMatchesFlowId(eventAt(i - 1u), flowId)) {





u32 countFlowStartsById(u32 flowId) {

        if (event.phase == EventPhase::FlowStart && eventMatchesFlowId(event, flowId)) {

u32 countFlowFinishesById(u32 flowId) {

        if (event.phase == EventPhase::FlowFinish && eventMatchesFlowId(event, flowId)) {

bool isAsyncFlowPairBalanced(u32 flowId) {

    return countFlowStartsById(flowId) == countFlowFinishesById(flowId);

bool tryFirstEventByName(const char* name, ProfileEvent& outEvent) {


bool tryLastEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByName(name);


bool tryFirstFlowEventById(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByFlowId(flowId);


bool tryLastFlowEventById(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByFlowId(flowId);










        if (eventFlowIdMatches(event, flowId)) {








        if (isAsyncFlowEventForId(event, flowId)) {



u32 countBufferedFlowStartsByFlowId(u32 flowId) {
        if (event.phase == EventPhase::FlowStart && isAsyncFlowEventForId(event, flowId)) {

u32 countBufferedFlowFinishesByFlowId(u32 flowId) {
        if (event.phase == EventPhase::FlowFinish && isAsyncFlowEventForId(event, flowId)) {

bool isBufferedFlowIdBalanced(u32 flowId) {
    return countBufferedFlowStartsByFlowId(flowId) == countBufferedFlowFinishesByFlowId(flowId);

bool hasUnbalancedBufferedFlowPairs() {
    std::unordered_map<u32, s32> openByFlowId;
        if (!isAsyncFlowPhase(event.phase) || !isValidEventName(event.name)) {

        s32& balance = openByFlowId[event.scopeId];
        if (event.phase == EventPhase::FlowStart) {
            ++balance;
        } else {
            --balance;

    for (const auto& entry : openByFlowId) {
        if (entry.second != 0) {

bool tryFindFirstEventByName(const char* name, ProfileEvent& outEvent) {


bool tryFindLastEventByName(const char* name, ProfileEvent& outEvent) {


bool tryFindFirstEventByFlowId(u32 flowId, ProfileEvent& outEvent) {


bool tryFindLastEventByFlowId(u32 flowId, ProfileEvent& outEvent) {


        if (eventNameMatches(eventAt(i), name)) {


        if (eventNameMatches(eventAt(i - 1u), name)) {




        if (eventFlowIdMatches(eventAt(i), flowId)) {


        if (eventFlowIdMatches(eventAt(i - 1u), flowId)) {












namespace {

    return isValidEventName(name) && isValidEventName(event.name) && std::strcmp(event.name, name) == 0;

bool isFlowPhase(EventPhase phase) {
    return phase == EventPhase::FlowStart || phase == EventPhase::FlowFinish;

u32 countOrphanEndsForPhases(EventPhase beginPhase, EventPhase endPhase) {
    std::unordered_map<u32, u32> openIds;
    u32 orphanEnds = 0u;
        if (!isValidEventName(event.name) || event.scopeId == 0u) {

        if (event.phase == beginPhase) {
            openIds[event.scopeId]++;
        } else if (event.phase == endPhase) {
            if (openIds[event.scopeId] == 0u) {
                ++orphanEnds;
                openIds[event.scopeId]--;
    return orphanEnds;

} // namespace

    if (wouldSkipNameLookup(name)) {






    if (wouldSkipFlowIdLookup(flowId)) {






bool wouldSkipNameLookup(const char* name) {
    return !isValidEventName(name) || isBufferEmpty();

bool wouldSkipFlowIdLookup(u32 flowId) {
    return flowId == 0u || isBufferEmpty();

bool isRecordedScopePairingConsistent() {
    return orphanScopeEndCount() == 0u;

bool isRecordedAsyncFlowPairingConsistent() {
    return orphanFlowEndCount() == 0u;

bool isRecordedNestingConsistent() {
    return isRecordedScopePairingConsistent() && isRecordedAsyncFlowPairingConsistent();

u32 orphanScopeEndCount() {
    return countOrphanEndsForPhases(EventPhase::Begin, EventPhase::End);

u32 orphanFlowEndCount() {
    return countOrphanEndsForPhases(EventPhase::FlowStart, EventPhase::FlowFinish);







        if (isFlowEventForId(event, flowId)) {





u32 openFlowEventCountForId(u32 flowId) {

    u32 starts = 0u;
    u32 finishes = 0u;
        if (!isFlowEventForId(event, flowId)) {

            ++starts;
            ++finishes;

    return starts >= finishes ? starts - finishes : 0u;

bool isFlowIdBalanced(u32 flowId) {



    return starts == finishes;


























bool tryFirstEventByFlowId(u32 flowId, ProfileEvent& outEvent) {


bool tryLastEventByFlowId(u32 flowId, ProfileEvent& outEvent) {




        if (eventMatchesName(eventAt(index), name)) {




        if (eventMatchesFlowId(eventAt(index), flowId)) {








        if (eventMatchesName(event, name)) {





bool hasEventWithName(const char* name) {
    return countEventsByName(name) > 0u;

        if (eventMatchesFlow(event, flowId)) {


        if (eventMatchesFlow(eventAt(i), flowId)) {

bool hasFlowEvent(u32 flowId) {
    return countEventsByFlow(flowId) > 0u;







        if (isAsyncFlowPhase(event.phase) && isValidEventName(event.name) && event.scopeId == flowId) {























    if (!isValidEventName(name) || !isValidEventName(event.name)) {

    return std::strcmp(event.name, name) == 0;







    if (!isValidFlowLookupId(flowId)) {

        if (isFlowEventPhase(event.phase) && event.scopeId == flowId && isValidEventName(event.name)) {






        if (event.phase == EventPhase::FlowStart && event.scopeId == flowId && isValidEventName(event.name)) {


        if (event.phase == EventPhase::FlowFinish && event.scopeId == flowId && isValidEventName(event.name)) {

bool isFlowPairedInBuffer(u32 flowId) {

    u32 startCount = 0u;
    u32 finishCount = 0u;
        if (event.scopeId != flowId || !isValidEventName(event.name)) {

            ++startCount;
        } else if (event.phase == EventPhase::FlowFinish) {
            ++finishCount;
    return startCount == 1u && finishCount == 1u;





    return isValidEventName(event.name) && isValidEventName(name)
        && std::strcmp(event.name, name) == 0;




















bool tryFindFirstEventIndexByFlow(u32 flowId, u32& outIndex) {
    const u32 index = findFirstEventIndexByFlow(flowId);



    outEvent = eventAt(index);
    return isValidProfileEvent(outEvent);



bool tryFindFirstEventByFlow(u32 flowId, ProfileEvent& outEvent) {


bool tryFindLastEventByFlow(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByFlow(flowId);








        if (isFlowPhaseEvent(event) && event.scopeId == flowId && isValidEventName(event.name)) {









bool tryFirstFlowEventByFlowId(u32 flowId, ProfileEvent& outEvent) {

    return isValidProfileEvent(outEvent) && isFlowPhaseEvent(outEvent);









        if (eventMatchesFlowId(event, flowId)) {










bool tryFindLastEventIndexByName(const char* name, u32& outIndex) {


bool tryFindFirstEventIndexByFlowId(u32 flowId, u32& outIndex) {


bool tryFindLastEventIndexByFlowId(u32 flowId, u32& outIndex) {



bool isAsyncFlowPhase(EventPhase phase) {










bool tryExportableFirstEvent(ProfileEvent& outEvent) {
    const u32 index = firstEventIndex();


bool tryExportableLastEvent(ProfileEvent& outEvent) {
    const u32 index = lastEventIndex();









































































        if (eventNameEquals(event.name, name)) {




















    return isValidEventName(event.name) && isValidEventName(name) && std::strcmp(event.name, name) == 0;

bool eventMatchesFlowId(const ProfileEvent& event, u32 flowId) {
    return isValidEventName(event.name)
        && (event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish)
        && event.scopeId == flowId;





























        if (profileEventNameEquals(event, name)) {




        if (profileEventNameEquals(eventAt(i), name)) {


        if (profileEventMatchesFlowId(event, flowId)) {




        if (profileEventMatchesFlowId(eventAt(i), flowId)) {









u32 openAsyncFlowCountForId(u32 flowId) {
    u32 openStarts = 0u;
        if (!eventMatchesFlowId(event, flowId)) {

            ++openStarts;

    return openStarts > finishes ? openStarts - finishes : 0u;

bool isAsyncFlowOpen(u32 flowId) {
    return openAsyncFlowCountForId(flowId) > 0u;

        if (isValidEventName(event.name) && eventNameEquals(event.name, name)) {





        if (isFlowPhase(event.phase) && isValidEventName(event.name) && event.scopeId == flowId) {
















bool hasUnpairedFlowEvents() {









bool isFlowIdOpen(u32 flowId) {
    u32 openCount = 0u;
        if (!isFlowPhase(event.phase) || event.scopeId != flowId || !isValidEventName(event.name)) {

            ++openCount;
        } else if (openCount > 0u) {
            --openCount;
    return openCount > 0u;

















        if (flowEventMatchesId(event, flowId)) {



        if (!flowEventMatchesId(event, flowId)) {

            if (openStarts > 0u) {
                --openStarts;
    return openStarts > 0u;


















        if (isAsyncFlowEventForId(event, flowId) && isValidEventName(event.name)) {






bool isAsyncFlowEvent(const ProfileEvent& event, u32 flowId) {
    return (event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish)
        && event.scopeId == flowId && isValidEventName(event.name);

    return isValidEventName(event.name) && std::strcmp(event.name, name) == 0;







        if (isAsyncFlowEvent(event, flowId)) {


        if (isAsyncFlowEvent(eventAt(i), flowId)) {

bool eventNameMatches(const char* eventName, const char* searchName) {
    if (!isValidEventName(eventName) || !isValidEventName(searchName)) {
    return std::strcmp(eventName, searchName) == 0;




















u32 lastEventIndex() {
    const u32 count = eventCount();
    for (u32 i = count; i > 0u; --i) {
        const u32 index = i - 1u;
        if (eventAt(index).phase == phase) {
            return index;
        }
    }
    return kInvalidEventIndex;
}

bool tryFindLastEventByPhase(EventPhase phase, ProfileEvent& outEvent) {
    const u32 index = lastEventIndexByPhase(phase);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryEventAt(index, outEvent);
}

u32 firstExportableEventIndex() {
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (isEventExportable(i)) {
            return i;
        }
    }
    return kInvalidEventIndex;
}

u32 lastExportableEventIndex() {
    const u32 total = eventCount();
    if (total == 0u) {
        return kInvalidEventIndex;
    }

    for (u32 i = total; i > 0u; --i) {
        const u32 index = i - 1u;
        if (isEventExportable(index)) {
            return index;
        }
    }
    return kInvalidEventIndex;
}

u32 lastExportableEventIndex() {
    const u32 count = eventCount();
    for (u32 i = count; i > 0u; --i) {
        const u32 index = i - 1u;
        if (isEventExportable(index)) {
            return index;
        }
    }
    return kInvalidEventIndex;
}

bool tryFindEventByName(const char* name, ProfileEvent& outEvent) {
    if (!isValidEventName(name)) {
        outEvent = ProfileEvent{};
        return false;
    }

    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.name != nullptr && std::string(event.name) == name) {
            outEvent = event;
            return isValidProfileEvent(outEvent);
        }
    }

    outEvent = ProfileEvent{};
    return false;
}

bool tryFirstEventOfPhase(EventPhase phase, ProfileEvent& outEvent) {
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.phase == phase && isValidProfileEvent(event)) {
            outEvent = event;
            return true;
        }
    }

    outEvent = ProfileEvent{};
    return false;
}

u32 firstExportableEventIndex() {
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (isEventExportable(i)) {
            return i;
        }
    }
    return kInvalidEventIndex;
}

u32 lastExportableEventIndex() {
    const u32 total = eventCount();
    if (total == 0u) {
        return kInvalidEventIndex;
    }

    for (u32 i = total; i > 0u; --i) {
        const u32 index = i - 1u;
        if (isEventExportable(index)) {
            return index;
        }
    }
    return kInvalidEventIndex;
}

const ProfileEvent& lastEvent() {
    const u32 index = lastEventIndex();
    if (index == kInvalidEventIndex) {
        return emptyProfileEvent();
    }
    return eventAt(index);
}

NestingStatePreflight preflightNestingState() {
    NestingStatePreflight preflight{};
    preflight.scopeDepth = scopeNestingDepth();
    preflight.flowDepth = flowNestingDepth();
    preflight.openAsyncFlows = openAsyncFlowCount();
    preflight.maxScopeDepth = maxNestingDepth();
    preflight.maxFlowDepth = maxFlowNestingDepth();
    preflight.scopeBalanced = isScopeNestingBalanced();
    preflight.flowBalanced = isFlowNestingBalanced();
    preflight.flowDepthDetached = isFlowDepthDetached();
    preflight.hasOpenAsyncFlows = hasOpenAsyncFlows();
    preflight.activeScopeNestingDepth = scopeNestingDepth();
    preflight.activeFlowNestingDepth = flowNestingDepth();
    preflight.openAsyncFlowCount = openAsyncFlowCount();
    preflight.maxScopeNestingDepth = maxNestingDepth();
    preflight.maxFlowNestingDepth = maxFlowNestingDepth();
    preflight.scopeNestingUnbalanced = !isScopeNestingBalanced();
    preflight.flowNestingUnbalanced = !isFlowNestingBalanced();
    preflight.crossThreadFlowHandoffPending = isCrossThreadFlowHandoffPending();
ProfileScopePreflight preflightProfileScope(const char* name) {
    ProfileScopePreflight preflight{};
    preflight.profilerDisabled = !enabled();
    preflight.invalidName = !isValidEventName(name);
    return preflight;
}

AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name) {
    AsyncFlowBeginPreflight preflight{};
    preflight.emptyName = isEmptyEventName(name);

AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name) {
    AsyncFlowEndPreflight preflight{};
    preflight.orphanEnd = openAsyncFlowCount() == 0u;
EventNamePreflight preflightEventName(const char* name) {
    EventNamePreflight preflight{};
    preflight.nullName = name == nullptr;
    preflight.emptyName = name != nullptr && name[0] == '\0';

AsyncFlowPreflight preflightAsyncFlowBegin(const char* name) {
    AsyncFlowPreflight preflight{};

AsyncFlowPreflight preflightAsyncFlowEnd(const char* name) {
    AsyncFlowPreflight preflight = preflightAsyncFlowBegin(name);
    preflight.noOpenFlows = openAsyncFlowCount() == 0u;
u32 totalWriteCount() {
    return g_writeHead.load(std::memory_order_acquire);

u32 droppedEventCount() {
    const u32 writes = totalWriteCount();
    const u32 stored = eventCount();
    return writes > stored ? writes - stored : 0u;

u32 remainingEventCapacity() {
    return stored < kRingCapacity ? kRingCapacity - stored : 0u;

bool hasDroppedEvents() {
    return droppedEventCount() > 0u;

bool hasRingWrapped() {
    return totalWriteCount() >= kRingCapacity;

u32 orphanAsyncFlowEndCount() {
    return g_orphanFlowEndCount.load(std::memory_order_acquire);

bool hasOrphanAsyncFlowEnds() {
    return orphanAsyncFlowEndCount() > 0u;

bool wouldIgnoreOrphanAsyncFlowEnd() {
    return openAsyncFlowCount() == 0u;

bool canEndAsyncFlow() {
    return !wouldIgnoreOrphanAsyncFlowEnd();

NestingStateRejectReason nestingStateRejectReason() {
    if (!isScopeNestingBalanced()) {
        return NestingStateRejectReason::UnbalancedScopeNesting;
    if (!isFlowNestingBalanced()) {
        return NestingStateRejectReason::UnbalancedFlowNesting;
    if (hasOpenAsyncFlows()) {
        return NestingStateRejectReason::OpenAsyncFlows;
    if (isFlowDepthDetached()) {
        return NestingStateRejectReason::FlowDepthDetached;
    return NestingStateRejectReason::None;

bool preflightProfilerState(NestingStateRejectReason* reason) {
    const NestingStateRejectReason stateReason = nestingStateRejectReason();
    if (reason != nullptr) {
        *reason = stateReason;
    return stateReason == NestingStateRejectReason::None;

void reconcileDetachedFlowDepth() {
    if (openAsyncFlowCount() == 0u && flowNestingDepth() > 0u) {
        threadLocalFlowNestingDepth() = 0u;
    if (hasActiveScope()) {
        return NestingStateRejectReason::ActiveScope;
    if (isCrossThreadFlowHandoffPending()) {
        return NestingStateRejectReason::CrossThreadFlowHandoffPending;
    if (hasActiveAsyncFlowNesting()) {
        return NestingStateRejectReason::ActiveFlowNesting;

const char* nestingStateRejectReasonLabel(NestingStateRejectReason reason) {
    switch (reason) {
    case NestingStateRejectReason::None:
        return "none";
    case NestingStateRejectReason::ActiveScope:
        return "active_scope";
    case NestingStateRejectReason::ActiveFlowNesting:
        return "active_flow_nesting";
    case NestingStateRejectReason::OpenAsyncFlows:
        return "open_async_flows";
    case NestingStateRejectReason::FlowDepthDetached:
        return "flow_depth_detached";
    case NestingStateRejectReason::CrossThreadFlowHandoffPending:
        return "cross_thread_flow_handoff_pending";
    return "unknown";

ChromeTraceExportRejectReason chromeTraceExportRejectReason() {
    if (!enabled()) {
        return ChromeTraceExportRejectReason::ProfilerDisabled;
        return ChromeTraceExportRejectReason::CrossThreadFlowHandoffPending;
        return ChromeTraceExportRejectReason::FlowDepthDetached;
    if (hasUnbalancedNesting()) {
        return ChromeTraceExportRejectReason::UnbalancedNesting;
    return ChromeTraceExportRejectReason::None;

const char* chromeTraceExportRejectReasonLabel(ChromeTraceExportRejectReason reason) {
    case ChromeTraceExportRejectReason::None:
    case ChromeTraceExportRejectReason::ProfilerDisabled:
        return "profiler_disabled";
    case ChromeTraceExportRejectReason::UnbalancedNesting:
        return "unbalanced_nesting";
    case ChromeTraceExportRejectReason::FlowDepthDetached:
    case ChromeTraceExportRejectReason::CrossThreadFlowHandoffPending:

NestingAsyncFlowPreflight preflightNestingAndAsyncFlow() {
    NestingAsyncFlowPreflight preflight{};
    preflight.scopeNestingBalanced = isScopeNestingBalanced();
    preflight.flowNestingBalanced = isFlowNestingBalanced();
    preflight.emptyName = !isValidEventName(name);


AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name, u32 flowId) {
    preflight.orphanFinish = openAsyncFlowCount() == 0u;
    (void)flowId;
ScopeNestingPreflight preflightScopeNesting() {
    ScopeNestingPreflight preflight{};
    preflight.activeDepth = scopeNestingDepth();
    preflight.maxDepth = maxNestingDepth();
    preflight.balanced = isScopeNestingBalanced();

AsyncFlowPreflight preflightAsyncFlow() {
    preflight.openCount = openAsyncFlowCount();
    preflight.activeFlowDepth = flowNestingDepth();
    preflight.balanced = isFlowNestingBalanced();
    preflight.depthDetached = isFlowDepthDetached();
    preflight.crossThreadHandoffPending = isCrossThreadFlowHandoffPending();
    preflight.hasOpenFlows = hasOpenAsyncFlows();
NestingPreflight preflightNesting() {
    NestingPreflight preflight{};
    return preflight;
}

ChromeTraceExportPreflight preflightChromeTraceExport() {
    ChromeTraceExportPreflight preflight{};
    preflight.profilerDisabled = !enabled();
    preflight.eventCount = eventCount();
    preflight.exportableEventCount = exportableEventCount();
    preflight.totalEventsWritten = totalEventsWritten();
    preflight.droppedEventCount = droppedEventCount();
    preflight.nonExportableEventCount = nonExportableEventCount();
    preflight.firstExportableEventIndex = firstExportableEventIndex();
    preflight.lastExportableEventIndex = lastExportableEventIndex();
    preflight.frameIndex = frameIndex();
    preflight.openAsyncFlowCount = openAsyncFlowCount();
    preflight.activeScopeNestingDepth = scopeNestingDepth();
    preflight.activeFlowNestingDepth = flowNestingDepth();
    preflight.maxScopeNestingDepth = maxNestingDepth();
    preflight.maxFlowNestingDepth = maxFlowNestingDepth();
    for (u32 i = 0u; i < preflight.eventCount; ++i) {
        if (isValidEventName(eventAt(i).name)) {
            ++preflight.exportableEventCount;
        }
    preflight.firstExportableEventIndex = firstExportableEventIndex();
    preflight.lastExportableEventIndex = lastExportableEventIndex();
    preflight.bufferEmpty = isBufferEmpty();
    preflight.ringSaturated = isRingSaturated();
    preflight.bufferFull = isBufferFull();
    preflight.ringBufferWrapped = hasRingBufferWrapped();
    preflight.droppedEventCount = droppedEventCount();
    preflight.nonExportableEventCount = nonExportableEventCount();
    preflight.remainingEventCapacity = remainingEventCapacity();
    preflight.rejectedInvalidNameCount = rejectedInvalidNameCount();
    preflight.orphanAsyncFlowEndCount = orphanAsyncFlowEndCount();
    preflight.hasRingWrapped = hasRingWrapped();
    preflight.remainingCapacity = remainingEventCapacity();
    preflight.allEventsExportable =
        preflight.eventCount == 0u || preflight.exportableEventCount == preflight.eventCount;
    preflight.nonExportableEventCount =
        preflight.eventCount > preflight.exportableEventCount
            ? preflight.eventCount - preflight.exportableEventCount
            : 0u;
    preflight.ringCapacity = ringCapacity();
    preflight.scopeNestingUnbalanced = !isScopeNestingBalanced();
    preflight.flowNestingUnbalanced = !isFlowNestingBalanced();
    preflight.hasOpenAsyncFlows = hasOpenAsyncFlows();
    preflight.flowDepthDetached = isFlowDepthDetached();
    preflight.invalidNameEventCount = invalidNameEventCount();
    preflight.nonExportableEventCount = preflight.invalidNameEventCount;
    preflight.firstExportableEventIndex = firstExportableEventIndex();
    preflight.lastExportableEventIndex = lastExportableEventIndex();
    preflight.ringBufferFull = isBufferFull();
    preflight.hasInvalidNameEvents = hasInvalidNameEvents();
    preflight.crossThreadFlowHandoffPending = isCrossThreadFlowHandoffPending();
    preflight.exportWouldTrimEvents = preflight.eventCount > preflight.exportableEventCount;
    preflight.hasOnlyExportableEvents =
        preflight.eventCount > 0u && preflight.eventCount == preflight.exportableEventCount;
    preflight.orphanAsyncFlowEndCount = orphanAsyncFlowEndCount();
    preflight.hasOrphanAsyncFlowEnds = hasOrphanAsyncFlowEnds();
    preflight.bufferFull = isBufferFull();
    preflight.scopeBeginEventCount = countEventsWithPhase(EventPhase::Begin);
    preflight.scopeEndEventCount = countEventsWithPhase(EventPhase::End);
    preflight.asyncFlowStartEventCount = countEventsWithPhase(EventPhase::FlowStart);
    preflight.asyncFlowFinishEventCount = countEventsWithPhase(EventPhase::FlowFinish);
    preflight.nonExportableEventCount =
        preflight.eventCount > preflight.exportableEventCount
            ? preflight.eventCount - preflight.exportableEventCount
            : 0u;
    preflight.hasRejectedInvalidNames = hasRejectedInvalidNames();
    preflight.nonExportableEventCount = nonExportableEventCount();
    preflight.scopeBeginEventCount = countEventsByPhase(EventPhase::Begin);
    preflight.scopeEndEventCount = countEventsByPhase(EventPhase::End);
    preflight.flowStartEventCount = countEventsByPhase(EventPhase::FlowStart);
    preflight.flowFinishEventCount = countEventsByPhase(EventPhase::FlowFinish);
    preflight.counterEventCount = countEventsByPhase(EventPhase::Counter);
    preflight.firstEventIndex = firstEventIndex();
    preflight.lastEventIndex = lastEventIndex();
    preflight.ringCapacity = ringCapacity();
    preflight.droppedEventCount = droppedEventCount();
    preflight.isBufferFull = isBufferFull();
    preflight.invalidEventCount = invalidEventCount();
    preflight.needsFlowNestingCleanup = needsFlowNestingCleanup();
    preflight.hasExportWarnings = preflight.hasUnbalancedNesting() || preflight.flowDepthDetached
                                  || preflight.hasOpenAsyncFlows;
    preflight.lastExportableEventIndex = lastExportableEventIndex();
    preflight.totalWriteCount = totalWriteCount();
    preflight.remainingEventCapacity = remainingEventCapacity();
    preflight.hasDroppedEvents = hasDroppedEvents();
    preflight.hasRingWrapped = hasRingWrapped();
    preflight.rejectReason = chromeTraceExportRejectReason();
    preflight.scopeBeginCount = countEventsByPhase(EventPhase::Begin);
    preflight.scopeEndCount = countEventsByPhase(EventPhase::End);
    preflight.flowStartCount = countEventsByPhase(EventPhase::FlowStart);
    preflight.flowFinishCount = countEventsByPhase(EventPhase::FlowFinish);
    preflight.scopePairImbalancedInBuffer = !isScopePairBalancedInBuffer();
    preflight.flowPairImbalancedInBuffer = !isFlowPairBalancedInBuffer();
    preflight.hasDroppedEvents = preflight.droppedEventCount > 0u;
    preflight.ignoredAsyncFlowEndCount = ignoredAsyncFlowEndCount();
    preflight.hasIgnoredAsyncFlowEnds = hasIgnoredAsyncFlowEnds();
    preflight.hasUnpairedScopeEvents = hasUnpairedScopeEventsInBuffer();
    preflight.hasUnpairedAsyncFlowEvents = hasUnpairedAsyncFlowEventsInBuffer();
    preflight.scopeBeginEndMismatch = hasScopeBeginEndMismatch();
    preflight.flowStartFinishMismatch = hasFlowStartFinishMismatch();
    return preflight;

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

    for (u32 i = 0; i < preflight.eventCount; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (isValidProfileEvent(event)) {
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




    return preflightChromeTraceExport().canExport();

    preflight.emptyBuffer = isBufferEmpty();
    preflight.unbalancedScopeNesting = !isScopeNestingBalanced();
    preflight.unbalancedFlowNesting = !isFlowNestingBalanced();
    preflight.bufferTruncated = isBufferFull();
    for (u32 i = 0u; i < count; ++i) {

bool hasExportableEvents() {
    return exportableEventCount() > 0u;

bool isChromeTraceExportEmpty() {
    return !hasExportableEvents();






    return diagnoseChromeTraceExportRejectReason() == ChromeTraceExportRejectReason::None;
}

bool preflightChromeTraceNesting(ChromeTraceExportRejectReason* reason) {
    const ChromeTraceExportRejectReason rejectReason = diagnoseChromeTraceExportRejectReason();
    if (reason != nullptr) {
        *reason = rejectReason;
    return rejectReason == ChromeTraceExportRejectReason::None;

const char* chromeTraceExportRejectReasonLabel(ChromeTraceExportRejectReason reason) {
    switch (reason) {
    case ChromeTraceExportRejectReason::None:
        return "none";
    case ChromeTraceExportRejectReason::ProfilerDisabled:
        return "profiler_disabled";
    case ChromeTraceExportRejectReason::NoExportableEvents:
        return "no_exportable_events";
    case ChromeTraceExportRejectReason::UnbalancedScopeNesting:
        return "unbalanced_scope_nesting";
    case ChromeTraceExportRejectReason::UnbalancedFlowNesting:
        return "unbalanced_flow_nesting";
    case ChromeTraceExportRejectReason::OpenAsyncFlows:
        return "open_async_flows";
    case ChromeTraceExportRejectReason::FlowDepthDetached:
        return "flow_depth_detached";
    return "unknown";




    return preflightChromeTraceExport().canExportTrace();

void reconcileDetachedFlowDepth() {
    if (isFlowDepthDetached() && openAsyncFlowCount() == 0u) {
        threadLocalFlowNestingDepth() = 0u;

bool canEndAsyncFlow() {
    return openAsyncFlowCount() > 0u;

bool wouldIgnoreOrphanAsyncFlowEnd() {
    return openAsyncFlowCount() == 0u;

ChromeTraceExportRejectReason chromeTraceExportRejectReason() {
    if (!enabled()) {
        return ChromeTraceExportRejectReason::ProfilerDisabled;
    if (exportableEventCount() == 0u) {
        return ChromeTraceExportRejectReason::NoExportableEvents;
    if (hasDroppedEvents()) {
        return ChromeTraceExportRejectReason::BufferOverflow;
    if (!isScopeNestingBalanced()) {
        return ChromeTraceExportRejectReason::UnbalancedScopeNesting;
    if (!isFlowNestingBalanced()) {
        return ChromeTraceExportRejectReason::UnbalancedFlowNesting;
    if (hasOpenAsyncFlows()) {
        return ChromeTraceExportRejectReason::OpenAsyncFlows;
    if (isFlowDepthDetached()) {
        return ChromeTraceExportRejectReason::FlowDepthDetached;
    return ChromeTraceExportRejectReason::None;

    return chromeTraceExportRejectReason() == ChromeTraceExportRejectReason::None;

bool tryExportChromeTraceJson(std::string& outJson, ChromeTraceExportRejectReason* reason) {
    const ChromeTraceExportRejectReason rejectReason = chromeTraceExportRejectReason();

    if (rejectReason != ChromeTraceExportRejectReason::None) {
        outJson.clear();

    outJson = exportChromeTraceJson();

    preflight.profilerDisabled = !enabled();


    preflight.hasActiveProfilingNesting = hasActiveProfilingNesting();
    preflight.firstExportableEventIndex = firstExportableEventIndex();
    preflight.hasActiveScopeNesting = hasActiveScopeNesting();
    preflight.hasActiveFlowNesting = hasActiveFlowNesting();
    preflight.hasNestedAsyncFlowContext = hasNestedAsyncFlowContext();
    preflight.hasActiveScopes = hasActiveScopes();
    preflight.danglingFlowBeginCount = countDanglingFlowBegins();
    preflight.orphanFlowEndCount = countOrphanFlowEnds();
    preflight.hasUnpairedFlowEvents =
        preflight.danglingFlowBeginCount > 0u || preflight.orphanFlowEndCount > 0u;
    preflight.nestingStateConsistent = isNestingStateConsistent();
    preflight.unbalancedFlowPairCount = countUnbalancedFlowPairsInBuffer();
    preflight.hasUnbalancedFlowPairsInBuffer = preflight.unbalancedFlowPairCount > 0u;

EventNameLookupPreflight preflightEventLookupByName(const char* name) {
    EventNameLookupPreflight preflight{};
    preflight.nameValid = isValidEventName(name);
    preflight.bufferEmpty = isBufferEmpty();
    if (!preflight.nameValid || preflight.bufferEmpty) {

    preflight.matchCount = countEventsByName(name);
    preflight.firstMatchIndex = findFirstEventIndexByName(name);
    preflight.lastMatchIndex = findLastEventIndexByName(name);

FlowIdLookupPreflight preflightFlowLookupById(u32 flowId) {
    FlowIdLookupPreflight preflight{};
    preflight.flowIdValid = isValidFlowId(flowId);
    if (!preflight.flowIdValid || preflight.bufferEmpty) {

    preflight.flowStartCount = countFlowStartsById(flowId);
    preflight.flowFinishCount = countFlowFinishesById(flowId);
    preflight.firstFlowEventIndex = findFirstEventIndexByFlowId(flowId);
    preflight.lastFlowEventIndex = findLastEventIndexByFlowId(flowId);
    preflight.pairBalanced = preflight.flowStartCount == preflight.flowFinishCount;

NestingConsistencyPreflight preflightNestingConsistency() {
    NestingConsistencyPreflight preflight{};
    preflight.scopeNestingBalanced = isScopeNestingBalanced();
    preflight.flowNestingBalanced = isFlowNestingBalanced();
    preflight.flowDepthAttached = !isFlowDepthDetached();
    preflight.openAsyncFlowCount = openAsyncFlowCount();
    preflight.activeScopeNestingDepth = scopeNestingDepth();
    preflight.activeFlowNestingDepth = flowNestingDepth();
    preflight.hasActiveScope = hasActiveScope();
    preflight.hasActiveAsyncFlowNesting = hasActiveAsyncFlowNesting();
    preflight.hasUnbalancedBufferedFlowPairs = hasUnbalancedBufferedFlowPairs();



    preflight.orphanScopeEndCount = orphanScopeEndCount();
    preflight.orphanFlowEndCount = orphanFlowEndCount();
    preflight.recordedScopePairingConsistent = isRecordedScopePairingConsistent();
    preflight.recordedFlowPairingConsistent = isRecordedAsyncFlowPairingConsistent();



    preflight.unpairedAsyncFlowIdCount = unpairedAsyncFlowIdCount();
    preflight.hasUnpairedAsyncFlowsInBuffer = preflight.unpairedAsyncFlowIdCount > 0u;



    preflight.hasUnpairedFlowEvents = preflight.flowStartEventCount != preflight.flowFinishEventCount;







    const u32 total = preflight.eventCount;
    for (u32 i = 0u; i < total; ++i) {
        if (!isValidEventName(event.name)) {
            continue;

        switch (event.phase) {
        case EventPhase::Begin:
            ++preflight.recordedScopeBeginCount;
            break;
        case EventPhase::End:
            ++preflight.recordedScopeEndCount;
        case EventPhase::FlowStart:
            ++preflight.recordedFlowStartCount;
        case EventPhase::FlowFinish:
            ++preflight.recordedFlowFinishCount;
        case EventPhase::Counter:
    preflight.hasUnpairedRecordedScopes =
        preflight.recordedScopeBeginCount != preflight.recordedScopeEndCount;
    preflight.hasUnpairedRecordedFlows =
        preflight.recordedFlowStartCount != preflight.recordedFlowFinishCount;







    preflight.scopeEventsUnbalanced = preflight.scopeBeginEventCount != preflight.scopeEndEventCount;
    preflight.flowEventsUnbalanced = preflight.flowStartEventCount != preflight.flowFinishEventCount;






    preflight.hasUnpairedScopeEvents =
        preflight.scopeBeginEventCount != preflight.scopeEndEventCount;
        preflight.flowStartEventCount != preflight.flowFinishEventCount;




ScopeNestingPreflight preflightScopeNesting() {
    ScopeNestingPreflight preflight{};
    preflight.activeDepth = scopeNestingDepth();
    preflight.maxDepth = maxNestingDepth();
    preflight.balanced = isScopeNestingBalanced();

AsyncFlowPreflight preflightAsyncFlow() {
    AsyncFlowPreflight preflight{};
    preflight.activeDepth = flowNestingDepth();
    preflight.maxDepth = maxFlowNestingDepth();
    preflight.openCount = openAsyncFlowCount();
    preflight.balanced = isFlowNestingBalanced();
    preflight.depthDetached = isFlowDepthDetached();
    preflight.crossThreadHandoffPending = isCrossThreadFlowHandoffPending();
    preflight.orphanFlowStartCount = orphanFlowStartCount();
    preflight.orphanFlowFinishCount = orphanFlowFinishCount();
    preflight.hasOrphanFlowEvents = hasOrphanFlowEvents();




    preflight.maxScopeNestingDepth = maxNestingDepth();
    preflight.maxFlowNestingDepth = maxFlowNestingDepth();
    preflight.scopeNestingUnbalanced = !isScopeNestingBalanced();
    preflight.flowNestingUnbalanced = !isFlowNestingBalanced();
    preflight.hasOpenAsyncFlows = hasOpenAsyncFlows();
    preflight.flowDepthDetached = isFlowDepthDetached();
    preflight.crossThreadFlowHandoffPending = isCrossThreadFlowHandoffPending();

NestingPreflight preflightNesting() {
    NestingPreflight preflight{};



    return preflight;
}

void reset() {
    const std::lock_guard<std::mutex> lock(g_exportMutex);
    g_writeHead.store(0u, std::memory_order_release);
    g_eventCount.store(0u, std::memory_order_release);
    g_droppedEventCount.store(0u, std::memory_order_release);
    g_totalEventsWritten.store(0u, std::memory_order_release);
    g_totalRecorded.store(0u, std::memory_order_release);
    g_frameIndex.store(0u, std::memory_order_release);
    g_nextScopeId.store(1u, std::memory_order_release);
    g_nextFlowId.store(1u, std::memory_order_release);
    g_maxNestingDepth.store(0u, std::memory_order_release);
    g_maxFlowNestingDepth.store(0u, std::memory_order_release);
    g_openAsyncFlowCount.store(0u, std::memory_order_release);
    g_orphanAsyncFlowEndCount.store(0u, std::memory_order_release);
    g_droppedEventCount.store(0u, std::memory_order_release);
    g_totalRecordedEvents.store(0u, std::memory_order_release);
    g_rejectedInvalidNameCount.store(0u, std::memory_order_release);
    g_orphanFlowEndCount.store(0u, std::memory_order_release);
    g_ignoredAsyncFlowEndCount.store(0u, std::memory_order_release);
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
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    if (!isValidEventName(name)) {
        recordRejectedInvalidName();
    if (!g_enabled.load(std::memory_order_acquire) || shouldRejectEventName(name)) {
    if (!wouldRecordWithName(name)) {
    if (!g_enabled.load(std::memory_order_acquire) || !shouldRecordEventName(name)) {
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
        recordRejectedInvalidName();
    if (!g_enabled.load(std::memory_order_acquire) || shouldRejectEventName(name)) {
    if (!wouldRecordWithName(name)) {
    if (!g_enabled.load(std::memory_order_acquire) || !shouldRecordEventName(name)) {

        return;
    }

    if (g_openAsyncFlowCount.load(std::memory_order_acquire) == 0u) {
        g_orphanAsyncFlowEndCount.fetch_add(1u, std::memory_order_acq_rel);
        recordOrphanAsyncFlowEnd();
        g_orphanFlowEndCount.fetch_add(1u, std::memory_order_acq_rel);
        g_ignoredAsyncFlowEndCount.fetch_add(1u, std::memory_order_acq_rel);
        return;
    }

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
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    if (!isValidEventName(track)) {
        recordRejectedInvalidName();
    if (!g_enabled.load(std::memory_order_acquire) || shouldRejectEventName(track)) {
    if (!wouldRecordWithName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !shouldRecordEventName(track)) {
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
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    if (!isValidEventName(track)) {
        recordRejectedInvalidName();
    if (!g_enabled.load(std::memory_order_acquire) || shouldRejectEventName(track)) {
    if (!wouldRecordWithName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !shouldRecordEventName(track)) {
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
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    if (!isValidEventName(track)) {
        recordRejectedInvalidName();
    if (!g_enabled.load(std::memory_order_acquire) || shouldRejectEventName(track)) {
    if (!wouldRecordWithName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !shouldRecordEventName(track)) {
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
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }
    if (!isValidEventName(track)) {
        recordRejectedInvalidName();
    if (!g_enabled.load(std::memory_order_acquire) || shouldRejectEventName(track)) {
    if (!wouldRecordWithName(track)) {
    if (!g_enabled.load(std::memory_order_acquire) || !shouldRecordEventName(track)) {
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

ChromeTraceExportRejectReason chromeTraceExportRejectReason() {
    const ChromeTraceExportPreflight preflight = preflightChromeTraceExport();
    if (preflight.emptyBuffer) {
        return ChromeTraceExportRejectReason::EmptyBuffer;
    if (preflight.unbalancedScopeNesting) {
        return ChromeTraceExportRejectReason::UnbalancedScopeNesting;
    if (preflight.openAsyncFlows) {
        return ChromeTraceExportRejectReason::OpenAsyncFlows;
    if (preflight.unbalancedFlowNesting) {
        return ChromeTraceExportRejectReason::UnbalancedFlowNesting;
    if (preflight.bufferFull) {
        return ChromeTraceExportRejectReason::BufferFull;
    return ChromeTraceExportRejectReason::None;

bool isValidChromeTraceExport(const std::string& json) {
    if (json.empty() || json.front() != '{' || json.back() != '}') {
        return false;

    return json.find("\"displayTimeUnit\":\"ns\"") != std::string::npos &&
           json.find("\"metadata\":{\"name\":\"FUSE CPU profiler\"") != std::string::npos &&
           json.find("\"traceEvents\":[") != std::string::npos;
bool isChromeExportPreflightOk() {
    return isProfilerNestingPreflightOk();

std::string exportChromeTraceJson() {
    std::string json;
    tryExportChromeTraceJson(json);
    return json;
}

bool tryExportChromeTraceJson(std::string& outJson) {
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
    bool exportedAny = false;

    for (u32 i = 0; i < count; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (!isValidEventName(event.name)) {
        if (!isRecordableName(event.name)) {
        if (!isValidProfileEvent(event)) {
        if (!shouldRecordEventName(event.name)) {
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
            exportedAny = true;
            continue;
        }
        }
        json += buffer;
        first = false;
        exportedAny = true;
    }

    json += "]}";
    outJson = std::move(json);
    return exportedAny;
}

bool tryExportChromeTraceJson(std::string& outJson, ChromeTraceExportRejectReason* reason) {
    if (!preflightChromeTraceExport(reason)) {
        outJson.clear();
        return false;
    }

    outJson = exportChromeTraceJson();
    return true;
}

bool tryExportChromeTraceJson(std::string& outJson, ChromeTraceExportRejectReason* reason) {
    if (!preflightChromeTraceExport(reason)) {
        outJson.clear();
        return false;
    }

    outJson = exportChromeTraceJson();
    return true;
}

bool tryExportChromeTraceJson(std::string& outJson, ChromeTraceExportRejectReason* reason) {
    if (!preflightChromeTraceNesting(reason)) {
        outJson.clear();
        return false;
    }

    outJson = exportChromeTraceJson();
    return true;
}

bool tryExportChromeTraceJson(std::string& outJson, ChromeTraceExportRejectReason* reason) {
    if (!preflightChromeTraceNesting(reason)) {
        outJson.clear();
        return false;
    }

    outJson = exportChromeTraceJson();
    return true;
}

const char* eventNameRejectReasonLabel(EventNameRejectReason reason) {
    switch (reason) {
    case EventNameRejectReason::None:
        return "none";
    case EventNameRejectReason::Null:
        return "null";
    case EventNameRejectReason::Empty:
        return "empty";
    case EventNameRejectReason::Blank:
        return "blank";
    }
    return "unknown";
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

const char* nestingStateRejectReasonLabel(NestingStateRejectReason reason) {
    switch (reason) {
    case NestingStateRejectReason::None:
        return "none";
    case NestingStateRejectReason::UnbalancedScopeNesting:
        return "unbalanced_scope_nesting";
    case NestingStateRejectReason::UnbalancedFlowNesting:
        return "unbalanced_flow_nesting";
    case NestingStateRejectReason::OpenAsyncFlows:
        return "open_async_flows";
    case NestingStateRejectReason::FlowDepthDetached:
        return "flow_depth_detached";
    }
    return "unknown";
}

const char* chromeTraceExportRejectReasonLabel(ChromeTraceExportRejectReason reason) {
    switch (reason) {
    case ChromeTraceExportRejectReason::None:
        return "none";
    case ChromeTraceExportRejectReason::ProfilerDisabled:
        return "profiler_disabled";
    case ChromeTraceExportRejectReason::NoExportableEvents:
        return "no_exportable_events";
    case ChromeTraceExportRejectReason::BufferOverflow:
        return "buffer_overflow";
    case ChromeTraceExportRejectReason::UnbalancedScopeNesting:
        return "unbalanced_scope_nesting";
    case ChromeTraceExportRejectReason::UnbalancedFlowNesting:
        return "unbalanced_flow_nesting";
    case ChromeTraceExportRejectReason::OpenAsyncFlows:
        return "open_async_flows";
    case ChromeTraceExportRejectReason::FlowDepthDetached:
        return "flow_depth_detached";
    }
    return "unknown";
}

} // namespace fuse::profiler
