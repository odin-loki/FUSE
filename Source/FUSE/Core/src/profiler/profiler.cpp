#include <fuse/profiler/profiler.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace fuse::profiler {

namespace {

constexpr u32 kRingCapacity = 4096u;

std::atomic<bool> g_enabled{true};
std::atomic<u32> g_frameIndex{0};

std::array<ProfileEvent, kRingCapacity> g_events{};
std::atomic<u32> g_writeHead{0};
std::atomic<u32> g_eventCount{0};

std::mutex g_exportMutex;

u64 nowNanoseconds() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

u32 currentThreadId() {
    const auto id = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return static_cast<u32>(id);
}

void recordEvent(const char* name, EventPhase phase) {
    if (!g_enabled.load(std::memory_order_acquire)) {
        return;
    }

    const u32 index = g_writeHead.fetch_add(1u, std::memory_order_acq_rel) % kRingCapacity;
    g_events[index] = ProfileEvent{name, nowNanoseconds(), phase, currentThreadId()};

    const u32 count = g_eventCount.load(std::memory_order_acquire);
    if (count < kRingCapacity) {
        g_eventCount.fetch_add(1u, std::memory_order_acq_rel);
    }
}

} // namespace

ProfileScope::ProfileScope(const char* name) : m_name(name), m_active(g_enabled.load(std::memory_order_acquire)) {
    if (m_active) {
        recordEvent(m_name, EventPhase::Begin);
    }
}

ProfileScope::~ProfileScope() {
    if (m_active) {
        recordEvent(m_name, EventPhase::End);
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
}

std::string exportChromeTraceJson() {
    const std::lock_guard<std::mutex> lock(g_exportMutex);

    std::string json = "{\"traceEvents\":[";
    const u32 count = eventCount();
    bool first = true;

    for (u32 i = 0; i < count; ++i) {
        const ProfileEvent& event = eventAt(i);
        if (event.name == nullptr) {
            continue;
        }

        const char phase = event.phase == EventPhase::Begin ? 'B' : 'E';
        const u64 timestampUs = event.timestampNs / 1000u;

        char buffer[512];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s{\"name\":\"%s\",\"cat\":\"cpu\",\"ph\":\"%c\",\"ts\":%llu,\"pid\":1,\"tid\":%u}",
                      first ? "" : ",",
                      event.name,
                      phase,
                      static_cast<unsigned long long>(timestampUs),
                      event.threadId);
        json += buffer;
        first = false;
    }

    json += "]}";
    return json;
}

} // namespace fuse::profiler
