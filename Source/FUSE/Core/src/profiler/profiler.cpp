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

constexpr u32 kRingCapacity = 4096u;

std::atomic<bool> g_enabled{true};
std::atomic<u32> g_frameIndex{0};
std::atomic<u32> g_nextScopeId{1};
std::atomic<u32> g_nextFlowId{1};

std::array<ProfileEvent, kRingCapacity> g_events{};
std::atomic<u32> g_writeHead{0};
std::atomic<u32> g_eventCount{0};
std::atomic<u32> g_maxNestingDepth{0};
std::atomic<u32> g_maxFlowNestingDepth{0};
std::atomic<u32> g_openAsyncFlowCount{0};
std::atomic<u32> g_orphanAsyncFlowEndCount{0};

std::mutex g_exportMutex;

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

    const u32 count = g_eventCount.load(std::memory_order_acquire);
    if (count < kRingCapacity) {
        g_eventCount.fetch_add(1u, std::memory_order_acq_rel);
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

ProfileScope::ProfileScope(const char* name)
    : m_name(name),
      m_active(g_enabled.load(std::memory_order_acquire) && isValidEventName(name)) {
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
    return currentNestingDepth();
}

u32 flowNestingDepth() {
    return currentFlowNestingDepth();
}

u32 openAsyncFlowCount() {
    return g_openAsyncFlowCount.load(std::memory_order_acquire);
}

bool hasOpenAsyncFlows() {
    return openAsyncFlowCount() > 0u;
}

bool isScopeNestingBalanced() {
    return nestingDepth() == 0u;
}

bool isFlowNestingBalanced() {
    return flowNestingDepth() == 0u;
}

bool hasUnbalancedNesting() {
    return !isScopeNestingBalanced() || !isFlowNestingBalanced();
}

bool isFlowDepthDetached() {
    return flowNestingDepth() != openAsyncFlowCount();
}

bool isCrossThreadFlowHandoffPending() {
    return isFlowDepthDetached() && flowNestingDepth() > 0u;
}

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
}

bool hasEvents() {
    return eventCount() > 0u;
}

bool isBufferEmpty() {
    return eventCount() == 0u;
}

bool isBufferFull() {
    return eventCount() >= kRingCapacity;
}

bool isEventIndexValid(u32 index) {
    return index < eventCount();
}

bool isBlankEventName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return true;
    }

    for (const char* cursor = name; *cursor != '\0'; ++cursor) {
        const char ch = *cursor;
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            return false;
        }
    }
    return true;
}

bool isValidEventName(const char* name) {
    return name != nullptr && name[0] != '\0';
}

bool isValidFlowId(u32 flowId) {
    return flowId != 0u;
}

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
}

bool isValidProfileEvent(const ProfileEvent& event) {
    return isValidEventName(event.name);
}

bool isProfileEventSentinel(const ProfileEvent& event) {
    return event.name == nullptr && event.timestampNs == 0u && event.scopeId == 0u
        && event.phase == EventPhase::Begin;
}

u32 invalidNameEventCount() {
    const u32 total = eventCount();
    const u32 exportable = exportableEventCount();
    return total >= exportable ? total - exportable : 0u;
}

bool hasInvalidNameEvents() {
    return invalidNameEventCount() > 0u;
}

u32 exportableEventCount() {
    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (isValidEventName(eventAt(i).name)) {
            ++count;
        }
    }
    return count;
}

bool isEventExportable(u32 index) {
    return isEventIndexValid(index) && isValidEventName(eventAt(index).name);
}

const ProfileEvent& emptyProfileEvent() {
    static const ProfileEvent kEmpty{};
    return kEmpty;
}

const ProfileEvent& eventAt(u32 index) {
    if (!isEventIndexValid(index)) {
        return emptyProfileEvent();
    }

    const u32 count = eventCount();
    const u32 head = g_writeHead.load(std::memory_order_acquire);
    const u32 start = head >= count ? head - count : 0u;
    const u32 ringIndex = (start + index) % kRingCapacity;
    return g_events[ringIndex];
}

bool tryEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventIndexValid(index)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return isValidProfileEvent(outEvent);
}

bool tryExportableEventAt(u32 index, ProfileEvent& outEvent) {
    if (!isEventExportable(index)) {
        outEvent = ProfileEvent{};
        return false;
    }

    outEvent = eventAt(index);
    return true;
}

bool tryFirstEvent(ProfileEvent& outEvent) {
    const u32 index = firstEventIndex();
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryEventAt(index, outEvent);
}

bool tryLastEvent(ProfileEvent& outEvent) {
    const u32 index = lastEventIndex();
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryEventAt(index, outEvent);
}

bool tryFirstEventByName(const char* name, ProfileEvent& outEvent) {
bool tryExportableFirstEvent(ProfileEvent& outEvent) {
    const u32 index = firstEventIndex();
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);

bool tryExportableLastEvent(ProfileEvent& outEvent) {
    const u32 index = lastEventIndex();


bool tryFindFirstEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByName(name);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);
}

bool tryLastEventByName(const char* name, ProfileEvent& outEvent) {
    outEvent = eventAt(index);
    return isValidProfileEvent(outEvent);

bool tryFindLastEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByName(name);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);
}

bool tryFirstFlowEvent(u32 flowId, ProfileEvent& outEvent) {
    outEvent = eventAt(index);
    return isValidProfileEvent(outEvent);

bool tryFindFirstFlowEvent(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByFlowId(flowId);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);
}

bool tryLastFlowEvent(u32 flowId, ProfileEvent& outEvent) {
    outEvent = eventAt(index);
    return isValidProfileEvent(outEvent);

bool tryFindLastFlowEvent(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByFlowId(flowId);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);
    outEvent = eventAt(index);
    return isValidProfileEvent(outEvent);
}

u32 firstEventIndex() {
    return hasEvents() ? 0u : kInvalidEventIndex;
}

u32 findFirstEventIndexByPhase(EventPhase phase) {
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.phase == phase && isValidEventName(event.name)) {
            return i;
        }
    }
    return kInvalidEventIndex;
}

u32 findLastEventIndexByPhase(EventPhase phase) {
    const u32 total = eventCount();
    for (u32 i = total; i > 0u; --i) {
        const ProfileEvent& event = eventAt(i - 1u);
        if (event.phase == phase && isValidEventName(event.name)) {
            return i - 1u;
        }
    }
    return kInvalidEventIndex;
}

u32 countEventsByPhase(EventPhase phase) {
    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.phase == phase && isValidEventName(event.name)) {
            ++count;
        }
    }
    return count;
}

u32 findFirstEventIndexByName(const char* name) {
    if (!isValidEventName(name)) {
        return kInvalidEventIndex;
    }

    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (eventNameMatches(event, name)) {
        if (eventNameMatches(event.name, name)) {
            return i;
        }
    }
    return kInvalidEventIndex;
}

u32 findLastEventIndexByName(const char* name) {
    if (!isValidEventName(name)) {
        return kInvalidEventIndex;
    }

    const u32 total = eventCount();
    for (u32 i = total; i > 0u; --i) {
        const ProfileEvent& event = eventAt(i - 1u);
        if (eventNameMatches(event, name)) {
        if (eventNameMatches(event.name, name)) {
            return i - 1u;
        }
    }
    return kInvalidEventIndex;
}

u32 countEventsByName(const char* name) {
    if (!isValidEventName(name)) {
        return 0u;
    }

    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (eventNameMatches(eventAt(i), name)) {
        const ProfileEvent& event = eventAt(i);
        if (eventNameMatches(event.name, name)) {
            ++count;
        }
    }
    return count;
}

bool hasEventsWithName(const char* name) {
    return findFirstEventIndexByName(name) != kInvalidEventIndex;
}

u32 findFirstEventIndexByFlowId(u32 flowId) {
    if (!isValidFlowId(flowId)) {
    if (flowId == 0u) {
        return kInvalidEventIndex;
    }

    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (eventMatchesFlowId(event, flowId)) {
        if (isAsyncFlowPhase(event.phase) && event.scopeId == flowId && isValidEventName(event.name)) {
            return i;
        }
    }
    return kInvalidEventIndex;
}

u32 findLastEventIndexByFlowId(u32 flowId) {
    if (!isValidFlowId(flowId)) {
    if (flowId == 0u) {
        return kInvalidEventIndex;
    }

    const u32 total = eventCount();
    for (u32 i = total; i > 0u; --i) {
        const ProfileEvent& event = eventAt(i - 1u);
        if (eventMatchesFlowId(event, flowId)) {
        if (isAsyncFlowPhase(event.phase) && event.scopeId == flowId && isValidEventName(event.name)) {
            return i - 1u;
        }
    }
    return kInvalidEventIndex;
}

u32 countEventsByFlowId(u32 flowId) {
    if (!isValidFlowId(flowId)) {
    if (flowId == 0u) {
        return 0u;
    }

    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (eventMatchesFlowId(eventAt(i), flowId)) {
        const ProfileEvent& event = eventAt(i);
        if (isAsyncFlowPhase(event.phase) && event.scopeId == flowId && isValidEventName(event.name)) {
            ++count;
        }
    }
    return count;
}

bool hasEventsWithFlowId(u32 flowId) {
    return findFirstEventIndexByFlowId(flowId) != kInvalidEventIndex;
bool tryFindFirstEventIndexByPhase(EventPhase phase, u32& outIndex) {
    const u32 index = findFirstEventIndexByPhase(phase);
    if (index == kInvalidEventIndex) {
        outIndex = kInvalidEventIndex;
        return false;
    }

    outIndex = index;
    return true;

bool tryFindLastEventIndexByPhase(EventPhase phase, u32& outIndex) {
    const u32 index = findLastEventIndexByPhase(phase);


bool tryFindFirstEventIndexByName(const char* name, u32& outIndex) {
    const u32 index = findFirstEventIndexByName(name);


bool tryFindFirstEventIndexByFlowId(u32 flowId, u32& outIndex) {
    const u32 index = findFirstEventIndexByFlowId(flowId);


bool tryFindLastEventIndexByFlowId(u32 flowId, u32& outIndex) {
    const u32 index = findLastEventIndexByFlowId(flowId);


u32 orphanAsyncFlowEndCount() {
    return g_orphanAsyncFlowEndCount.load(std::memory_order_acquire);

bool hasOrphanAsyncFlowEnds() {
    return orphanAsyncFlowEndCount() > 0u;
}

u32 lastEventIndex() {
    const u32 count = eventCount();
    return count > 0u ? count - 1u : kInvalidEventIndex;
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
    preflight.profilerDisabled = !enabled();
    preflight.invalidName = !isValidEventName(name);
    preflight.canEnter = !preflight.profilerDisabled && !preflight.invalidName;
    return preflight;
}

AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name, u32 /*flowId*/) {
    AsyncFlowBeginPreflight preflight{};
    preflight.profilerDisabled = !enabled();
    preflight.invalidName = !isValidEventName(name);
    preflight.canBegin = !preflight.profilerDisabled && !preflight.invalidName;
    return preflight;
}

AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name, u32 /*flowId*/) {
    AsyncFlowEndPreflight preflight{};
    preflight.profilerDisabled = !enabled();
    preflight.invalidName = !isValidEventName(name);
    preflight.wouldUnderflowOpenCount = openAsyncFlowCount() == 0u;
    preflight.canEnd = !preflight.profilerDisabled && !preflight.invalidName
        && !preflight.wouldUnderflowOpenCount;
    return preflight;
}

NestingAsyncFlowPreflight preflightNestingAsyncFlow() {
    NestingAsyncFlowPreflight preflight{};
    preflight.activeScopeNestingDepth = scopeNestingDepth();
    preflight.activeFlowNestingDepth = flowNestingDepth();
    preflight.maxScopeNestingDepth = maxNestingDepth();
    preflight.maxFlowNestingDepth = maxFlowNestingDepth();
    preflight.openAsyncFlowCount = openAsyncFlowCount();
    preflight.scopeNestingUnbalanced = !isScopeNestingBalanced();
    preflight.flowNestingUnbalanced = !isFlowNestingBalanced();
    preflight.hasOpenAsyncFlows = hasOpenAsyncFlows();
    preflight.flowDepthDetached = isFlowDepthDetached();
    preflight.crossThreadFlowHandoffPending = isCrossThreadFlowHandoffPending();
    return preflight;
}

void reset() {
    const std::lock_guard<std::mutex> lock(g_exportMutex);
    g_writeHead.store(0u, std::memory_order_release);
    g_eventCount.store(0u, std::memory_order_release);
    g_frameIndex.store(0u, std::memory_order_release);
    g_nextScopeId.store(1u, std::memory_order_release);
    g_nextFlowId.store(1u, std::memory_order_release);
    g_maxNestingDepth.store(0u, std::memory_order_release);
    g_maxFlowNestingDepth.store(0u, std::memory_order_release);
    g_openAsyncFlowCount.store(0u, std::memory_order_release);
    g_orphanAsyncFlowEndCount.store(0u, std::memory_order_release);
    threadLocalNestingDepth() = 0u;
    threadLocalFlowNestingDepth() = 0u;
}

u32 nextFlowId() {
    return g_nextFlowId.fetch_add(1u, std::memory_order_acq_rel);
}

void beginAsyncFlow(const char* name, u32 flowId) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(name)) {
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
        return;
    }

    if (g_openAsyncFlowCount.load(std::memory_order_acquire) == 0u) {
        g_orphanAsyncFlowEndCount.fetch_add(1u, std::memory_order_acq_rel);
        return;
    }

    g_openAsyncFlowCount.fetch_sub(1u, std::memory_order_acq_rel);

    const u32 flowDepth = currentFlowNestingDepth();
    recordEvent(name,
                EventPhase::FlowFinish,
                flowId,
                currentNestingDepth(),
                flowDepth);
    popFlowNestingDepth();
}

void sampleCounter(const char* track, s64 value) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(track)) {
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

} // namespace fuse::profiler

// --- deepen additive from deepen-b16-profiler-c977 ---
ProfilerRecordPreflight preflightRecord(const char* name) {
    ProfilerRecordPreflight preflight{};
ProfilerExportPreflight preflightChromeTraceExport() {
    ProfilerExportPreflight preflight{};

// --- deepen additive from deepen-b16-profiler-preflights-ea42 ---
bool tryEventAt(u32 index, ProfileEvent& out) {

// --- deepen additive from deepen-b16-profiler-guards-0c1a ---
ChromeExportPreflight preflightChromeExport() {
    ChromeExportPreflight preflight{};

// --- deepen additive from profiler-b16-preflight-deepen-6ec7 ---
ProfileNamePreflight preflightProfileName(const char* name) {
    ProfileNamePreflight preflight{};
ScopeNestingPreflight preflightScopeNesting() {
    ScopeNestingPreflight preflight{};
AsyncFlowBeginPreflight preflightAsyncFlowBegin(const char* name) {
AsyncFlowEndPreflight preflightAsyncFlowEnd(const char* name) {
    return preflightProfileName(name).shouldSkip() || !g_enabled.load(std::memory_order_acquire);
    return preflightAsyncFlowBegin(name).shouldSkip();
    return preflightAsyncFlowEnd(name).shouldSkip();
    return preflightProfileName(track).shouldSkip() || !g_enabled.load(std::memory_order_acquire);

// --- deepen additive from deepen-b16-profiler-preflights-4b82 ---
ScopePreflight preflightScope(const char* name) {
    const ProfileNamePreflight namePreflight = preflightProfileName(name);
    preflight.null_name = namePreflight.null_name;
    preflight.empty_name = namePreflight.empty_name;
AsyncFlowBeginPreflight preflightBeginAsyncFlow(const char* name) {
AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name) {
CounterSamplePreflight preflightCounterSample(const char* track) {
    CounterSamplePreflight preflight{};
    const ProfileNamePreflight namePreflight = preflightProfileName(track);
EventLookupPreflight preflightEventLookup(u32 index) {
    EventLookupPreflight preflight{};
    return preflightEventLookup(index).can_lookup();
bool tryEventAt(u32 index, const ProfileEvent*& event_out) {
    const EventLookupPreflight preflight = preflightEventLookup(index);

// --- deepen additive from deepen-b16-profiler-preflights-8f4e ---
bool tryEventAt(u32 index, const ProfileEvent*& outEvent) {
ProfilerGuardPreflight preflightGuardState() {
    ProfilerGuardPreflight preflight{};

// --- deepen additive from deepen-b16-profiler-preflights-479f ---
NestingStatePreflight preflightNestingState() {
    NestingStatePreflight preflight{};
    return preflightChromeExport().canExport();

// --- deepen additive from deepen-b16-profiler-guards-5d2c ---
    ChromeExportPreflight preflight;

// --- deepen additive from deepen-b16-profiler-export-preflight-9968 ---
ChromeExportPreflight preflightChromeTraceExport() {

// --- deepen additive from deepen-b16-profiler-export-preflights-d73d ---
    return preflightChromeTraceExport().canExport();
    return tryEventAt(0u, outEvent);

// --- deepen additive from deepen-b16-profiler-export-preflight-bcfd ---
ChromeTraceExportRejectReason chromeTraceExportRejectReason() {
    const ChromeTraceExportPreflight preflight = preflightChromeTraceExport();
        return ChromeTraceExportRejectReason::EmptyBuffer;
        return ChromeTraceExportRejectReason::UnbalancedScopeNesting;
        return ChromeTraceExportRejectReason::OpenAsyncFlows;
        return ChromeTraceExportRejectReason::UnbalancedFlowNesting;
        return ChromeTraceExportRejectReason::BufferFull;
    return ChromeTraceExportRejectReason::None;

// --- deepen additive from deepen-b16-profiler-preflights-3a15 ---
    ChromeTraceExportPreflight result{};

// --- deepen additive from deepen-fuse-b16-profiler-11c2 ---
EventNameRejectReason diagnoseEventNameRejectReason(const char* name) {
        return EventNameRejectReason::Null;
        return EventNameRejectReason::Empty;
    return EventNameRejectReason::None;
NestingStateRejectReason diagnoseNestingStateRejectReason() {
        return NestingStateRejectReason::UnbalancedScopeNesting;
        return NestingStateRejectReason::UnbalancedFlowNesting;
        return NestingStateRejectReason::OpenAsyncFlows;
    return NestingStateRejectReason::None;
ChromeTraceExportRejectReason diagnoseChromeTraceExportRejectReason() {
bool tryValidateEventName(const char* name, EventNameRejectReason& outReason) {
    outReason = diagnoseEventNameRejectReason(name);
    return outReason == EventNameRejectReason::None;
const char* eventNameRejectReasonLabel(EventNameRejectReason reason) {
    case EventNameRejectReason::None:
    case EventNameRejectReason::Null:
    case EventNameRejectReason::Empty:
    return diagnoseNestingStateRejectReason() == NestingStateRejectReason::None;
bool preflightProfilerState(NestingStateRejectReason* reason) {
    const NestingStateRejectReason rejectReason = diagnoseNestingStateRejectReason();
    return rejectReason == NestingStateRejectReason::None;
const char* nestingStateRejectReasonLabel(NestingStateRejectReason reason) {
    case NestingStateRejectReason::None:
    case NestingStateRejectReason::UnbalancedScopeNesting:
    case NestingStateRejectReason::UnbalancedFlowNesting:
    case NestingStateRejectReason::OpenAsyncFlows:
    return diagnoseChromeTraceExportRejectReason() == ChromeTraceExportRejectReason::None;
bool preflightChromeTraceExport(ChromeTraceExportRejectReason* reason) {
    const ChromeTraceExportRejectReason rejectReason = diagnoseChromeTraceExportRejectReason();
    return rejectReason == ChromeTraceExportRejectReason::None;
const char* chromeTraceExportRejectReasonLabel(ChromeTraceExportRejectReason reason) {
    case ChromeTraceExportRejectReason::None:
    case ChromeTraceExportRejectReason::UnbalancedScopeNesting:
    case ChromeTraceExportRejectReason::UnbalancedFlowNesting:
    case ChromeTraceExportRejectReason::OpenAsyncFlows:
    EventLookupRejectReason reason = EventLookupRejectReason::None;
    return tryCanLookupEventAt(index, reason);
bool tryCanLookupEventAt(u32 index, EventLookupRejectReason& outReason) {
        outReason = EventLookupRejectReason::EmptyBuffer;
        outReason = EventLookupRejectReason::OutOfRange;
        outReason = EventLookupRejectReason::InvalidEvent;
    outReason = EventLookupRejectReason::None;
const char* eventLookupRejectReasonLabel(EventLookupRejectReason reason) {
    case EventLookupRejectReason::None:
    case EventLookupRejectReason::EmptyBuffer:
    case EventLookupRejectReason::OutOfRange:
    case EventLookupRejectReason::InvalidEvent:
    if (!tryCanLookupEventAt(index, reason)) {
bool tryExportChromeTraceJson(std::string& outJson, ChromeTraceExportRejectReason* reason) {
    if (!preflightChromeTraceExport(reason)) {

// --- deepen additive from deepen-b16-profiler-preflight-guards-b702 ---
    tryExportChromeTraceJson(json);
bool tryExportChromeTraceJson(std::string& outJson) {

// --- deepen additive from deepen-b16-profiler-preflights-acf4 ---
bool isProfilerNestingPreflightOk() {
bool isEventLookupPreflightOk(u32 index) {
    if (!isEventLookupPreflightOk(index)) {
bool isChromeExportPreflightOk() {
    return isProfilerNestingPreflightOk();

// --- deepen additive from deepen-b16-profiler-preflights-3f2f ---
NestingPreflight preflightNesting() {
AsyncFlowPreflight preflightBeginAsyncFlow(const char* name) {
AsyncFlowPreflight preflightEndAsyncFlow(const char* name) {
ExportPreflight preflightExport() {
EventLookupPreflight preflightEventAt(u32 index) {
EventLookupPreflight preflightLastEvent() {
    return preflightEventAt(index);
    return preflightProfileScope(name).canEnter();
    return preflightBeginAsyncFlow(name).canBegin();
    return preflightEndAsyncFlow(name).canEnd();
    return preflightExport().canExport();
    return preflightEventAt(index).canLookup();

// --- deepen additive from deepen-profiler-b16-guards-10ba ---
bool tryEventPhaseAt(u32 index, EventPhase& outPhase) {
    if (!tryEventAt(index, event)) {

// --- deepen additive from deepen-b16-profiler-guards-fb79 ---
bool tryFirstExportableEvent(ProfileEvent& outEvent) {
bool tryLastExportableEvent(ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-guards-5e82 ---
bool tryFindEventByName(const char* name, u32 startIndex, u32& outIndex, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-guards-1296 ---
    return diagnoseEventNameRejectReason(name) == EventNameRejectReason::None;
        return NestingStateRejectReason::FlowDepthDetached;
        return ChromeTraceExportRejectReason::ProfilerDisabled;
        return ChromeTraceExportRejectReason::NoExportableEvents;
        return ChromeTraceExportRejectReason::FlowDepthDetached;
    case NestingStateRejectReason::FlowDepthDetached:
bool preflightChromeTraceNesting(ChromeTraceExportRejectReason* reason) {
    case ChromeTraceExportRejectReason::ProfilerDisabled:
    case ChromeTraceExportRejectReason::NoExportableEvents:
    case ChromeTraceExportRejectReason::FlowDepthDetached:
    if (!preflightChromeTraceNesting(reason)) {

// --- deepen additive from deepen-b16-profiler-guards-3935 ---
bool tryEventAtReverse(u32 reverseIndex, ProfileEvent& outEvent) {
    return tryEventAt(count - 1u - reverseIndex, outEvent);
bool tryFindEventByScopeId(u32 scopeId, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-guards-2ba8 ---
bool tryFindFirstEventWithPhase(EventPhase phase, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-guards-c0f6 ---
bool tryFindFirstEventByPhase(EventPhase phase, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-guards-dcff ---
    return preflightChromeTraceExport().canExportTrace();

// --- deepen additive from deepen-b16-profiler-guards-7793 ---
bool wouldIgnoreOrphanAsyncFlowEnd() {
bool wouldRecordEventName(const char* name) {

// --- deepen additive from deepen-b16-profiler-guards-93c0 ---
EventNamePreflight preflightEventName(const char* name) {
    EventNamePreflight preflight{};
AsyncFlowPreflight preflightAsyncFlowBegin(const char* name) {
AsyncFlowPreflight preflightAsyncFlowEnd(const char* name) {
    AsyncFlowPreflight preflight = preflightAsyncFlowBegin(name);

// --- deepen additive from deepen-b16-profiler-guards-cad0 ---
bool tryEventAtPhase(u32 index, EventPhase expectedPhase, ProfileEvent& outEvent) {
    if (!tryEventAt(index, outEvent)) {

// --- deepen additive from deepen-profiler-b16-guards-33c5 ---
bool wouldRecordEvent(const char* name) {

// --- deepen additive from deepen-b16-profiler-guards-9145 ---
bool tryFindLastEventByPhase(EventPhase phase, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-guards-7722 ---
bool wouldRecordWithName(const char* name) {
bool tryEventAtPhase(u32 index, EventPhase phase, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-guards-6796 ---
bool tryRecordedEventAt(u32 index, ProfileEvent& outEvent) {
    if (!tryRecordedEventAt(index, outEvent)) {

// --- deepen additive from deepen-b16-profiler-guards-61a8 ---
bool tryFirstEventOfPhase(EventPhase phase, ProfileEvent& outEvent) {
bool tryLastEventOfPhase(EventPhase phase, ProfileEvent& outEvent) {
bool tryFindEventByName(const char* name, u32& outIndex) {

// --- deepen additive from deepen-b16-profiler-guards-ce9d ---
bool tryFindEventByName(const char* name, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-guards-8f82 ---
        outReason = EventNameRejectReason::Null;
        outReason = EventNameRejectReason::Empty;
        outReason = EventNameRejectReason::Blank;
    outReason = EventNameRejectReason::None;
EventNameRejectReason eventNameRejectReason(const char* name) {
    EventNameRejectReason reason = EventNameRejectReason::None;
    tryValidateEventName(name, reason);
    return enabled() && tryValidateEventName(name, reason);
    return tryValidateEventName(event.name, reason);
    return tryValidateEventName(eventAt(index).name, reason);
EventLookupRejectReason eventLookupRejectReason(u32 index) {
        return EventLookupRejectReason::EmptyBuffer;
        return EventLookupRejectReason::OutOfRange;
    EventNameRejectReason nameReason = EventNameRejectReason::None;
    if (!tryValidateEventName(eventAt(index).name, nameReason)) {
        return EventLookupRejectReason::InvalidEvent;
    return EventLookupRejectReason::None;
    outReason = eventLookupRejectReason(index);
    return outReason == EventLookupRejectReason::None;
    EventLookupRejectReason lookupReason = EventLookupRejectReason::None;
    if (!tryCanLookupEventAt(index, lookupReason)) {
NestingStateRejectReason nestingStateRejectReason() {
    const NestingStateRejectReason stateReason = nestingStateRejectReason();
    return stateReason == NestingStateRejectReason::None;
    preflight.rejectReason = chromeTraceExportRejectReason();
        return ChromeTraceExportRejectReason::BufferOverflow;
    return chromeTraceExportRejectReason() == ChromeTraceExportRejectReason::None;
    const ChromeTraceExportRejectReason rejectReason = chromeTraceExportRejectReason();
    if (rejectReason != ChromeTraceExportRejectReason::None) {
    case EventNameRejectReason::Blank:
    case ChromeTraceExportRejectReason::BufferOverflow:

// --- deepen additive from deepen-fuse-b16-profiler-e55b ---
bool tryFindLastEventIndexByName(const char* name, u32& outIndex) {
        if (tryExportableEventAt(i - 1u, outEvent)) {

// --- deepen additive from deepen-b16-profiler-guards-5b61 ---
        return EventLookupRejectReason::NotExportable;
EventLookupRejectReason exportableEventLookupRejectReason(u32 index) {
    case EventLookupRejectReason::NotExportable:
        return NestingStateRejectReason::ActiveScope;
        return NestingStateRejectReason::CrossThreadFlowHandoffPending;
        return NestingStateRejectReason::ActiveFlowNesting;
    case NestingStateRejectReason::ActiveScope:
    case NestingStateRejectReason::ActiveFlowNesting:
    case NestingStateRejectReason::CrossThreadFlowHandoffPending:
        return ChromeTraceExportRejectReason::CrossThreadFlowHandoffPending;
        return ChromeTraceExportRejectReason::UnbalancedNesting;
    case ChromeTraceExportRejectReason::UnbalancedNesting:
    case ChromeTraceExportRejectReason::CrossThreadFlowHandoffPending:

// --- deepen additive from deepen-profiler-b16-guards-183a ---
bool tryFindEventIndexByName(const char* name, u32& outIndex) {

// --- deepen additive from deepen-b16-profiler-guards-512e ---
        if (tryExportableEventAt(i, outEvent)) {

// --- deepen additive from deepen-profiler-b16-guards-eb78 ---
bool tryFirstFlowStartById(u32 flowId, ProfileEvent& outEvent) {
bool tryLastFlowFinishById(u32 flowId, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-name-flow-guards-4e15 ---
bool tryFirstFlowEventById(u32 flowId, ProfileEvent& outEvent) {
bool tryLastFlowEventById(u32 flowId, ProfileEvent& outEvent) {
EventNameLookupPreflight preflightEventLookupByName(const char* name) {
    EventNameLookupPreflight preflight{};
FlowIdLookupPreflight preflightFlowLookupById(u32 flowId) {
    FlowIdLookupPreflight preflight{};
NestingConsistencyPreflight preflightNestingConsistency() {
    NestingConsistencyPreflight preflight{};

// --- deepen additive from deepen-b16-profiler-name-flow-e105 ---
bool tryFirstEventByFlowId(u32 flowId, ProfileEvent& outEvent) {
bool tryLastEventByFlowId(u32 flowId, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-name-flow-lookup-cb6d ---
bool tryFindFirstEventByFlowId(u32 flowId, ProfileEvent& outEvent) {
bool tryFindLastEventByFlowId(u32 flowId, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-guards-590c ---
    if (wouldSkipNameLookup(name)) {
    if (wouldSkipFlowIdLookup(flowId)) {
bool wouldSkipNameLookup(const char* name) {
bool wouldSkipFlowIdLookup(u32 flowId) {

// --- deepen additive from b16-profiler-deepen-guards-891a ---
bool tryFindFirstEventByFlow(u32 flowId, ProfileEvent& outEvent) {
bool tryFindLastEventByFlow(u32 flowId, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-name-flow-guards-2034 ---
bool tryFindFirstExportableEventByName(const char* name, ProfileEvent& outEvent) {
bool tryFindFirstExportableEventByFlowId(u32 flowId, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-guards-b5ca ---
bool tryFirstExportableEventByName(const char* name, ProfileEvent& outEvent) {
bool tryLastExportableEventByName(const char* name, ProfileEvent& outEvent) {
bool tryFirstExportableEventByFlowId(u32 flowId, ProfileEvent& outEvent) {
bool tryLastExportableEventByFlowId(u32 flowId, ProfileEvent& outEvent) {
AsyncFlowEndPreflight preflightEndAsyncFlow(const char* name, u32 flowId) {

// --- deepen additive from deepen-b16-profiler-guards-52e0 ---
AsyncFlowPreflight preflightAsyncFlow() {

// --- deepen additive from deepen-b16-profiler-name-flow-lookup-7ab9 ---
bool tryFindExportableEventByName(const char* name, ProfileEvent& outEvent) {
bool tryFindExportableEventByFlowId(u32 flowId, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-guards-2244 ---
bool wouldSkipProfileScope(const char* name) {
bool wouldSkipAsyncFlowBegin(const char* name) {
bool wouldSkipAsyncFlowEnd(const char* name) {
bool wouldSkipCounterSample(const char* track) {
bool wouldSkipChromeTraceExport() {
bool wouldSkipChromeTraceExportSafely() {
    return !preflightChromeTraceExport().canExportSafely();

// --- deepen additive from deepen-b16-profiler-guards-f01b ---
bool wouldSkipAsyncFlowEnd(const char* name, u32 /*flowId*/) {

// --- deepen additive from deepen-b16-profiler-guards-355d ---
bool wouldSkipProfileScope(const char* name, ProfileSkipReason* reason) {
bool wouldSkipAsyncFlowBegin(const char* name, ProfileSkipReason* reason) {
bool wouldSkipAsyncFlowEnd(const char* name, u32 /*flowId*/, ProfileSkipReason* reason) {
bool wouldSkipCounter(const char* track, ProfileSkipReason* reason) {
bool wouldSkipChromeTraceExport(ProfileSkipReason* reason) {
bool wouldSkipChromeTraceExportSafely(ProfileSkipReason* reason) {

// --- deepen additive from b16-profiler-deepen-guards-6464 ---
    return !preflightProfileScope(name).canEnter;
bool wouldSkipBeginAsyncFlow(const char* name, u32 flowId) {
    return !preflightBeginAsyncFlow(name, flowId).canBegin;
bool wouldSkipEndAsyncFlow(const char* name, u32 flowId) {
    return !preflightEndAsyncFlow(name, flowId).canEnd;

// --- deepen additive from deepen-b16-profiler-guards-5803 ---
    return wouldSkipProfileScope(name);
    return wouldSkipProfileScope(name)
    return wouldSkipProfileScope(track);
bool wouldSkipCounterFloatSample(const char* track) {
    return wouldSkipCounterSample(track);
bool wouldSkipCounterSnapshotAtFrame(const char* track) {
bool wouldSkipCounterFloatSnapshotAtFrame(const char* track) {

// --- deepen additive from b16-profiler-deepen-guards-6d64 ---
AsyncFlowNestingPreflight preflightAsyncFlowNesting() {
    AsyncFlowNestingPreflight preflight{};
bool wouldSkipBeginAsyncFlow(const char* name) {
    return !preflightBeginAsyncFlow(name, 0u).canBegin;
bool wouldSkipEndAsyncFlow(const char* name) {
    return !preflightEndAsyncFlow(name, 0u).canEnd;
bool wouldSkipCounter(const char* track) {
    return wouldSkipCounter(track);

// --- deepen additive from deepen-b16-profiler-wouldskip-lookup-c0fe ---
bool wouldSkipProfileRecord(const char* name,
bool tryFindFirstFlowEventById(u32 flowId, ProfileEvent& outEvent) {
bool tryFindLastFlowEventById(u32 flowId, ProfileEvent& outEvent) {
bool wouldSkipProfileScope(const char* name, ProfileRecordSkipReason* reason) {
    return wouldSkipProfileRecord(name, false, reason);
bool wouldSkipAsyncFlowBegin(const char* name, ProfileRecordSkipReason* reason) {
bool wouldSkipAsyncFlowEnd(const char* name, ProfileRecordSkipReason* reason) {
    return wouldSkipProfileRecord(name, true, reason);
bool wouldSkipCounterSample(const char* track, ProfileRecordSkipReason* reason) {
    return wouldSkipProfileRecord(track, false, reason);

// --- deepen additive from deepen-b16-profiler-guards-e7e3 ---
bool wouldSkipScopeRecording(const char* name) {
bool wouldSkipCounterRecording(const char* track) {
ScopeRecordingPreflight preflightScopeRecording(const char* name) {
    ScopeRecordingPreflight preflight{};
CounterRecordingPreflight preflightCounterRecording(const char* track) {
    CounterRecordingPreflight preflight{};

// --- deepen additive from deepen-b16-profiler-wouldskip-0f61 ---
AsyncFlowEndPreflight preflightAsyncFlowEnd(const char* name, u32 /*flowId*/) {
bool wouldSkipProfileScope(const char* name, ProfileScopeSkipReason* reason) {
    const ProfileScopePreflight preflight = preflightProfileScope(name);
bool wouldSkipAsyncFlowBegin(const char* name, AsyncFlowBeginSkipReason* reason) {
    const AsyncFlowBeginPreflight preflight = preflightAsyncFlowBegin(name);
bool wouldSkipAsyncFlowEnd(const char* name, u32 flowId, AsyncFlowEndSkipReason* reason) {
    const AsyncFlowEndPreflight preflight = preflightAsyncFlowEnd(name, flowId);
bool wouldSkipCounterSample(const char* track, CounterSampleSkipReason* reason) {
    const CounterSamplePreflight preflight = preflightCounterSample(track);
bool wouldSkipChromeTraceExport(ChromeTraceExportSkipReason* reason) {
bool wouldSkipChromeTraceExportSafely(ChromeTraceExportSkipReason* reason) {

// --- deepen additive from deepen-b16-profiler-guards-7260 ---
bool wouldSkipScope(const char* name) {

// --- deepen additive from deepen-b16-profiler-wouldskip-lookup-b790 ---
bool wouldSkipRecording(const char* name) {
ProfilerNestingPreflight preflightNesting() {
    ProfilerNestingPreflight preflight{};
bool preflightBeginAsyncFlow(const char* name) {
    return !wouldSkipRecording(name);
bool preflightEndAsyncFlow(const char* name) {
    return !wouldSkipRecording(name) && openAsyncFlowCount() > 0u;
    return wouldSkipRecording(name);
bool wouldSkipAsyncFlow(const char* name) {
    return wouldSkipRecording(track);

// --- deepen additive from deepen-b16-profiler-guards-0dc1 ---
bool wouldSkipAsyncFlowBegin(const char* name, u32 /*flowId*/) {
bool wouldSkipSafeChromeTraceExport() {

// --- deepen additive from b16-profiler-deepen-guards-70eb ---
    return wouldSkipScope(name);
    return wouldSkipScope(name) || openAsyncFlowCount() == 0u;
    return wouldSkipScope(track);
        return ChromeTraceExportRejectReason::UnpairedFlowEvents;
    case ChromeTraceExportRejectReason::UnpairedFlowEvents:

// --- deepen additive from deepen-b16-profiler-wouldskip-68ea ---
bool tryFirstExportableEventByFlow(u32 flowId, ProfileEvent& outEvent) {
bool tryLastExportableEventByFlow(u32 flowId, ProfileEvent& outEvent) {

// --- deepen additive from deepen-b16-profiler-guards-d08f ---
bool wouldSkipProfileScope(const char* name, ProfilerSkipReason* reason) {
bool wouldSkipAsyncFlowBegin(const char* name, ProfilerSkipReason* reason) {
    return wouldSkipProfileScope(name, reason);
bool wouldSkipAsyncFlowEnd(const char* name, ProfilerSkipReason* reason) {
bool wouldSkipCounter(const char* track, ProfilerSkipReason* reason) {
    return wouldSkipProfileScope(track, reason);
bool wouldSkipChromeTraceExport(ProfilerSkipReason* reason) {
bool wouldSkipSafeChromeTraceExport(ProfilerSkipReason* reason) {

// --- deepen additive from deepen-b16-profiler-guards-d3b7 ---
bool wouldSkipRecording() {
    return wouldSkipRecording() || !isValidEventName(name);
    return wouldSkipRecording() || !isValidEventName(name)
    return wouldSkipRecording() || !isValidEventName(track);
    return wouldSkipRecording();
ScopeNestingPreflight preflightScopeNesting(const char* name) {
    preflight.profilerDisabled = wouldSkipRecording();
    preflight.wouldSkip = preflight.profilerDisabled
    preflight.wouldSkip = wouldSkipAsyncFlowBegin(name);
    preflight.wouldSkip = wouldSkipAsyncFlowEnd(name);

// --- deepen additive from deepen-b16-profiler-wouldskip-lookup-ee46 ---
bool wouldSkipScope(const char* name, ProfilerRecordSkipReason* reason) {
bool wouldSkipAsyncFlowBegin(const char* name, ProfilerRecordSkipReason* reason) {
    return wouldSkipScope(name, reason);
bool wouldSkipAsyncFlowEnd(const char* name, ProfilerRecordSkipReason* reason) {
bool wouldSkipCounter(const char* track, ProfilerRecordSkipReason* reason) {
    return wouldSkipScope(track, reason);
    preflight.wouldSkip = wouldSkipAsyncFlowBegin(name, &preflight.skipReason);
    preflight.wouldSkip = wouldSkipAsyncFlowEnd(name, &preflight.skipReason);
bool wouldSkipChromeTraceExport(bool requireBalancedNesting) {
