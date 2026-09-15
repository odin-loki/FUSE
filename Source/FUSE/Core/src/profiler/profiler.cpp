#include <fuse/profiler/profiler.hpp>

#include <fuse/platform/thread.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace fuse::profiler {

namespace {

constexpr u32 kRingCapacity = 4096u;

std::atomic<bool> g_enabled{true};
std::atomic<u32> g_frameIndex{0};
std::atomic<u32> g_nextScopeId{1};

std::array<ProfileEvent, kRingCapacity> g_events{};
std::atomic<u32> g_writeHead{0};
std::atomic<u32> g_eventCount{0};
std::atomic<u32> g_maxNestingDepth{0};

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

void recordEvent(const char* name, EventPhase phase, u32 scopeId, u32 nestingDepth) {
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
    };

    const u32 count = g_eventCount.load(std::memory_order_acquire);
    if (count < kRingCapacity) {
        g_eventCount.fetch_add(1u, std::memory_order_acq_rel);
    }
}

} // namespace

ProfileScope::ProfileScope(const char* name)
    : m_name(name), m_active(g_enabled.load(std::memory_order_acquire)) {
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

u32 maxNestingDepth() {
    return g_maxNestingDepth.load(std::memory_order_acquire);
}

const ProfileEvent& eventAt(u32 index) {
    const u32 count = eventCount();
    if (count == 0u) {
        static const ProfileEvent kEmpty{};
        return kEmpty;
    }

    const u32 clamped = index < count ? index : count - 1u;
    const u32 head = g_writeHead.load(std::memory_order_acquire);
    const u32 start = head >= count ? head - count : 0u;
    const u32 ringIndex = (start + clamped) % kRingCapacity;
    return g_events[ringIndex];
}

void reset() {
    const std::lock_guard<std::mutex> lock(g_exportMutex);
    g_writeHead.store(0u, std::memory_order_release);
    g_eventCount.store(0u, std::memory_order_release);
    g_frameIndex.store(0u, std::memory_order_release);
    g_nextScopeId.store(1u, std::memory_order_release);
    g_maxNestingDepth.store(0u, std::memory_order_release);
    threadLocalNestingDepth() = 0u;
}

std::string exportChromeTraceJson() {
    const std::lock_guard<std::mutex> lock(g_exportMutex);

    std::string json =
        "{\"displayTimeUnit\":\"ns\",\"metadata\":{\"name\":\"FUSE CPU profiler\"},\"traceEvents\":[";
    const u32 count = eventCount();
    bool first = true;

    for (u32 i = 0; i < count; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.name == nullptr) {
            continue;
        }

        const char phase = event.phase == EventPhase::Begin ? 'B' : 'E';
        const u64 timestampUs = event.timestampNs / 1000u;

        char buffer[640];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s{\"name\":\"%s\",\"cat\":\"cpu\",\"ph\":\"%c\",\"ts\":%llu,\"pid\":1,"
                      "\"tid\":%u,\"id\":%u,\"args\":{\"depth\":%u}}",
                      first ? "" : ",",
                      event.name,
                      phase,
                      static_cast<unsigned long long>(timestampUs),
                      event.threadId,
                      event.scopeId,
                      event.nestingDepth);
        json += buffer;
        first = false;
    }

    json += "]}";
    return json;
}

} // namespace fuse::profiler
