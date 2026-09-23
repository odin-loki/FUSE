#include <fuse/profiler/profiler.hpp>

#include <fuse/platform/thread.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace fuse::profiler {

bool isValidEventName(const char* name);

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

constexpr u32 kMarkerStackCapacity = 32u;

struct PendingMarker {
    const char* name = nullptr;
    u64 startNs = 0;
    u32 scopeId = 0;
    u32 nestingDepth = 0;
};

struct MarkerStack {
    PendingMarker frames[kMarkerStackCapacity]{};
    u32 depth = 0;

    bool push(const PendingMarker& marker) {
        if (depth >= kMarkerStackCapacity) {
            return false;
        }
        frames[depth++] = marker;
        return true;
    }

    bool pop(PendingMarker& out) {
        if (depth == 0u) {
            return false;
        }
        out = frames[--depth];
        return true;
    }

    void clear() {
        depth = 0u;
    }
};

thread_local MarkerStack g_gpuMarkers{};
thread_local MarkerStack g_cudaMarkers{};

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
                 u32 counterSnapshotFrame = 0u,
                 u64 timestampNs = 0u,
                 u64 durationNs = 0u) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }

    // Hot path: the chrome tid is fixed per thread, so hash it once.
    static thread_local const u32 threadId = fuse::platform::chromeTraceThreadId();
    const u32 index = g_writeHead.fetch_add(1u, std::memory_order_acq_rel) % kRingCapacity;
    g_events[index] = ProfileEvent{
        name,
        timestampNs != 0u ? timestampNs : nowNanoseconds(),
        phase,
        threadId,
        scopeId,
        nestingDepth,
        flowNestingDepth,
        counterKind,
        counterIntValue,
        counterFloatValue,
        counterSnapshotFrame,
        durationNs,
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
    case EventPhase::GpuComplete:
    case EventPhase::CudaComplete:
        return "X";
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
    case EventPhase::GpuComplete:
        return "gpu";
    case EventPhase::CudaComplete:
        return "cuda";
    default:
        return "cpu";
    }
}

void beginMarker(MarkerStack& stack, const char* name) {
    if (!g_enabled.load(std::memory_order_acquire) || !isValidEventName(name)) {
        return;
    }

    PendingMarker marker;
    marker.name = name;
    marker.startNs = nowNanoseconds();
    marker.scopeId = g_nextScopeId.fetch_add(1u, std::memory_order_acq_rel);
    marker.nestingDepth = currentNestingDepth();
    stack.push(marker);
}

void endMarker(MarkerStack& stack, EventPhase phase) {
    PendingMarker marker;
    if (!stack.pop(marker)) {
        return;
    }

    const u64 endNs = nowNanoseconds();
    const u64 durationNs = endNs >= marker.startNs ? endNs - marker.startNs : 0u;
    recordEvent(marker.name,
                phase,
                marker.scopeId,
                marker.nestingDepth,
                currentFlowNestingDepth(),
                CounterValueKind::None,
                0,
                0.0,
                0u,
                marker.startNs,
                durationNs);
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

bool isValidEventName(const char* name) {
    return name != nullptr && name[0] != '\0';
}

bool isValidFlowId(u32 flowId) {
    return flowId != 0u;
}

bool isFlowPhaseEvent(const ProfileEvent& event) {
    return event.phase == EventPhase::FlowStart || event.phase == EventPhase::FlowFinish;
}

bool eventNameMatches(const ProfileEvent& event, const char* name) {
    return isValidEventName(name) && isValidEventName(event.name)
        && std::strcmp(event.name, name) == 0;
}

bool eventMatchesFlowId(const ProfileEvent& event, u32 flowId) {
    return isValidFlowId(flowId) && isFlowPhaseEvent(event) && event.scopeId == flowId
        && isValidEventName(event.name);
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
    const u32 index = findFirstEventIndexByName(name);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);
}

bool tryLastEventByName(const char* name, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByName(name);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);
}

bool tryFirstFlowEvent(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findFirstEventIndexByFlowId(flowId);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);
}

bool tryLastFlowEvent(u32 flowId, ProfileEvent& outEvent) {
    const u32 index = findLastEventIndexByFlowId(flowId);
    if (index == kInvalidEventIndex) {
        outEvent = ProfileEvent{};
        return false;
    }

    return tryExportableEventAt(index, outEvent);
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
        return kInvalidEventIndex;
    }

    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (eventMatchesFlowId(event, flowId)) {
            return i;
        }
    }
    return kInvalidEventIndex;
}

u32 findLastEventIndexByFlowId(u32 flowId) {
    if (!isValidFlowId(flowId)) {
        return kInvalidEventIndex;
    }

    const u32 total = eventCount();
    for (u32 i = total; i > 0u; --i) {
        const ProfileEvent& event = eventAt(i - 1u);
        if (eventMatchesFlowId(event, flowId)) {
            return i - 1u;
        }
    }
    return kInvalidEventIndex;
}

u32 countEventsByFlowId(u32 flowId) {
    if (!isValidFlowId(flowId)) {
        return 0u;
    }

    u32 count = 0u;
    const u32 total = eventCount();
    for (u32 i = 0u; i < total; ++i) {
        if (eventMatchesFlowId(eventAt(i), flowId)) {
            ++count;
        }
    }
    return count;
}

bool hasEventsWithFlowId(u32 flowId) {
    return findFirstEventIndexByFlowId(flowId) != kInvalidEventIndex;
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
    preflight.profilerDisabled = !kChromeTraceExportEnabled || !enabled();
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
    threadLocalNestingDepth() = 0u;
    threadLocalFlowNestingDepth() = 0u;
    g_gpuMarkers.clear();
    g_cudaMarkers.clear();
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

void profile_gpu_begin(const char* name, void* cmdBuffer) {
    (void)cmdBuffer;
    beginMarker(g_gpuMarkers, name);
}

void profile_gpu_end(void* cmdBuffer) {
    (void)cmdBuffer;
    endMarker(g_gpuMarkers, EventPhase::GpuComplete);
}

void profile_cuda_begin(const char* name, void* stream) {
    (void)stream;
    beginMarker(g_cudaMarkers, name);
}

void profile_cuda_end(void* stream) {
    (void)stream;
    endMarker(g_cudaMarkers, EventPhase::CudaComplete);
}

std::string exportChromeTraceJson() {
    if constexpr (!kChromeTraceExportEnabled) {
        return "{\"displayTimeUnit\":\"ns\",\"metadata\":{\"name\":\"FUSE CPU profiler\",\"frame\":0},"
               "\"traceEvents\":[]}";
    }

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

        // Sized for the escaped name so long names are never truncated into invalid JSON.
        std::string bufferStorage(escapedName.size() + 768u, '\0');
        char* buffer = bufferStorage.data();
        const std::size_t bufferSize = bufferStorage.size();
        switch (event.phase) {
        case EventPhase::Begin:
        case EventPhase::End:
            std::snprintf(buffer,
                          bufferSize,
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
                              bufferSize,
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
                              bufferSize,
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
                              bufferSize,
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
                              bufferSize,
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
                              bufferSize,
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
                              bufferSize,
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
                              bufferSize,
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
                              bufferSize,
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
            char* counterHeader = buffer;
            std::snprintf(counterHeader,
                          bufferSize,
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
        case EventPhase::GpuComplete:
        case EventPhase::CudaComplete:
            std::snprintf(buffer,
                          bufferSize,
                          "%s{\"name\":\"%s\",\"cat\":\"%s\",\"ph\":\"%s\",\"ts\":%llu,\"dur\":%llu,\"pid\":1,"
                          "\"tid\":%u,\"id\":%u,\"args\":{\"depth\":%u}}",
                          first ? "" : ",",
                          escapedName.c_str(),
                          category,
                          phase,
                          static_cast<unsigned long long>(timestampUs),
                          static_cast<unsigned long long>(event.durationNs / 1000u),
                          event.threadId,
                          event.scopeId,
                          event.nestingDepth);
            break;
        }
        json += buffer;
        first = false;
    }

    json += "]}";
    return json;
}

} // namespace fuse::profiler
