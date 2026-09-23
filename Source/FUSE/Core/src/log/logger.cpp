#include <fuse/log/logger.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <new>
#include <thread>

namespace fuse::log {

namespace {
const char* levelPrefix(Level level) {
    switch (level) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    case Level::Fatal: return "FATAL";
    }
    return "LOG";
}

void copyMessage(char* dest, usize cap, const char* src) {
    if (cap == 0u) {
        return;
    }
    usize n = 0;
    if (src != nullptr) {
        while (n + 1u < cap && src[n] != '\0') {
            dest[n] = src[n];
            ++n;
        }
    }
    dest[n] = '\0';
}

u64 steadyNowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

/// Serializes the synchronous path, the record ring, the sink and consumer delivery.
std::mutex g_logMutex;

// ---- async ring --------------------------------------------------------------------------------
//
// Bounded MPSC queue with per-slot sequence numbers (D. Vyukov's bounded queue, single consumer).
// Slot i of lap L is free for position p = L*cap + i when seq == p, published when seq == p + 1, and
// released back to producers by the consumer with seq = p + cap.
//
// Producer: CAS-claim `tail` only if the slot at `tail` is free, format directly into the slot,
// publish with seq = p + 1. A slot that is not free means the ring is full: the message is dropped
// and counted, the producer never waits. Positions are claimed in each thread's program order and
// consumed in position order, so per-producer order is preserved.
//
// Consumer: one thread reads positions in order, waiting (not the producers) for a claimed slot to be
// published. It parks on `wakeWord` when idle; producers only touch the futex when `consumerParked`
// is set (Dekker handshake with seq_cst on both sides).

struct alignas(64) Slot {
    std::atomic<u64> seq{0};
    u64 timestampNs = 0;
    const char* file = nullptr;
    u32 line = 0;
    u32 channel = 0;
    u8 level = 0;
    char message[kAsyncMessageBytes]{};
};

struct alignas(64) Ring {
    Slot* slots = nullptr;
    u64 mask = 0;
    u32 capacity = 0;
    alignas(64) std::atomic<u64> tail{0};     // next position producers claim
    alignas(64) std::atomic<u64> consumed{0}; // positions fully delivered (consumer-written)
};

struct AsyncState {
    std::mutex control; // start/stop only, never on the producer path
    std::thread consumer;
    std::atomic<Ring*> ring{nullptr};
    std::atomic<bool> active{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> draining{false}; // stopAsync() between clearing `active` and freeing the ring
    std::atomic<u32> inflight{0};
    std::atomic<bool> consumerParked{false};
    std::atomic<u32> wakeWord{0};
    std::atomic<u64> generation{0};  // bumped by every startAsync(); flush() keys on it (no ABA on ring)
    u64 enqueuedBase = 0;            // positions claimed by rings already stopped (under `control`)
    std::atomic<u64> delivered{0};
    std::atomic<u64> dropped{0};
};

AsyncState g_async;
thread_local bool t_isConsumer = false;

void wakeConsumer() {
    g_async.wakeWord.fetch_add(1u, std::memory_order_seq_cst);
    g_async.wakeWord.notify_one();
}

u32 roundUpPow2(u32 v) {
    u32 p = 2u;
    while (p < v && p < (1u << 30)) {
        p <<= 1u;
    }
    return p;
}
} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    stopAsync();
}

void Logger::setMinLevel(Level level) {
    m_minLevel.store(static_cast<u8>(level), std::memory_order_relaxed);
}

Level Logger::minLevel() const {
    return static_cast<Level>(m_minLevel.load(std::memory_order_relaxed));
}

void Logger::setEnabledChannels(u32 mask) {
    m_enabledChannels.store(mask, std::memory_order_relaxed);
}

u32 Logger::enabledChannels() const {
    return m_enabledChannels.load(std::memory_order_relaxed);
}

void Logger::setSink(SinkFn sink, void* userData) {
    // Messages logged before the switch go to the sink that was current when they were logged.
    flush();
    std::lock_guard<std::mutex> lock(g_logMutex);
    m_sink = sink;
    m_sinkUser = userData;
}

void Logger::log(Level level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logV(level, Channel::Core, fmt, args);
    va_end(args);
}

void Logger::log(Level level, Channel channel, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logV(level, channel, fmt, args);
    va_end(args);
}

void Logger::logV(Level level, const char* fmt, va_list args) {
    logV(level, Channel::Core, fmt, args);
}

void Logger::logV(Level level, Channel channel, const char* fmt, va_list args) {
    logVAt(level, channel, nullptr, 0u, fmt, args);
}

void Logger::logAt(Level level, Channel channel, const char* file, u32 line, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logVAt(level, channel, file, line, fmt, args);
    va_end(args);
}

void Logger::recordLocked(Level level, Channel channel, const char* file, u32 line, u64 timestampNs,
                          const char* message) {
    Record& rec = m_records[m_recordWrite % kRecordCapacity];
    rec.level = level;
    rec.channel = channel;
    rec.timestampNs = timestampNs;
    rec.file = file;
    rec.line = line;
    copyMessage(rec.message, sizeof(rec.message), message);
    ++m_recordWrite;
    if (m_recordCount < kRecordCapacity) {
        ++m_recordCount;
    }
}

void Logger::deliverLocked(Level level, Channel channel, const char* file, u32 line, u64 timestampNs,
                           const char* message) {
    recordLocked(level, channel, file, line, timestampNs, message);
    if (m_sink) {
        m_sink(level, message, m_sinkUser);
        return;
    }
    if (file != nullptr) {
        std::fprintf(stderr, "[%s] %s:%u: %s\n", levelPrefix(level), file, line, message);
    } else {
        std::fprintf(stderr, "[%s] %s\n", levelPrefix(level), message);
    }
}

void Logger::logVAt(Level level, Channel channel, const char* file, u32 line, const char* fmt, va_list args) {
#if defined(FUSE_SHIPPING) && FUSE_SHIPPING
    if (level < Level::Fatal) {
        return;
    }
#endif
    if (static_cast<u8>(level) < m_minLevel.load(std::memory_order_relaxed)) {
        return;
    }
    if ((static_cast<u32>(channel) & m_enabledChannels.load(std::memory_order_relaxed)) == 0u) {
        return;
    }

    if (level < Level::Fatal && g_async.active.load(std::memory_order_acquire)) {
        if (emitAsync(level, channel, file, line, fmt, args)) {
            return;
        }
    }
    if (t_isConsumer) {
        // A sink logging from the consumer thread already holds the delivery lock: queue it behind the
        // current message instead of deadlocking (Fatal included; the ring outlives the consumer).
        enqueue(g_async.ring.load(std::memory_order_acquire), level, channel, file, line, fmt, args);
        return;
    }
    if (g_async.draining.load(std::memory_order_seq_cst)) {
        // stopAsync() is still delivering this thread's earlier async messages; keep per-thread order.
        std::lock_guard<std::mutex> control(g_async.control);
    }
    if (level == Level::Fatal) {
        // The process may be about to die: everything queued before the fatal line goes out first.
        flush();
    }
    emitSync(level, channel, file, line, fmt, args);
}

void Logger::emitSync(Level level, Channel channel, const char* file, u32 line, const char* fmt, va_list args) {
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt ? fmt : "", args);

    std::lock_guard<std::mutex> lock(g_logMutex);
    // Timestamp under the lock so ring order and timestamp order always agree.
    deliverLocked(level, channel, file, line, steadyNowNs(), buffer);
}

bool Logger::emitAsync(Level level, Channel channel, const char* file, u32 line, const char* fmt, va_list args) {
    // Announce the emit before re-checking `active`, so stopAsync() (which clears `active` and then
    // waits for inflight == 0) never frees the ring under a producer.
    g_async.inflight.fetch_add(1u, std::memory_order_seq_cst);
    if (!g_async.active.load(std::memory_order_seq_cst)) {
        g_async.inflight.fetch_sub(1u, std::memory_order_release);
        return false;
    }
    enqueue(g_async.ring.load(std::memory_order_acquire), level, channel, file, line, fmt, args);
    g_async.inflight.fetch_sub(1u, std::memory_order_release);
    return true;
}

void Logger::enqueue(void* ringPtr, Level level, Channel channel, const char* file, u32 line, const char* fmt,
                     va_list args) {
    auto* ring = static_cast<Ring*>(ringPtr);
    u64 pos = ring->tail.load(std::memory_order_relaxed);
    Slot* slot = nullptr;
    for (;;) {
        Slot& candidate = ring->slots[pos & ring->mask];
        const u64 seq = candidate.seq.load(std::memory_order_acquire);
        const auto diff = static_cast<s64>(seq - pos);
        if (diff == 0) {
            if (ring->tail.compare_exchange_weak(pos, pos + 1u, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                slot = &candidate;
                break;
            }
        } else if (diff < 0) {
            // Full: the slot still holds an undelivered message from the previous lap. Drop, never block.
            g_async.dropped.fetch_add(1u, std::memory_order_relaxed);
            return;
        } else {
            pos = ring->tail.load(std::memory_order_relaxed);
        }
    }

    slot->timestampNs = steadyNowNs();
    slot->file = file;
    slot->line = line;
    slot->channel = static_cast<u32>(channel);
    slot->level = static_cast<u8>(level);
    std::vsnprintf(slot->message, sizeof(slot->message), fmt ? fmt : "", args);
    // seq_cst publish + seq_cst read of consumerParked pairs with the consumer's park handshake.
    slot->seq.store(pos + 1u, std::memory_order_seq_cst);
    if (g_async.consumerParked.load(std::memory_order_seq_cst)) {
        wakeConsumer();
    }
}

void Logger::runConsumer() {
    t_isConsumer = true;
    Ring* ring = g_async.ring.load(std::memory_order_acquire);
    u64 head = 0;
    constexpr u32 kBatch = 64u;
    constexpr u32 kSpinsBeforePark = 256u;
    u32 idleSpins = 0;
    for (;;) {
        Slot* first = &ring->slots[head & ring->mask];
        if (first->seq.load(std::memory_order_acquire) == head + 1u) {
            idleSpins = 0;
            std::lock_guard<std::mutex> lock(g_logMutex);
            for (u32 n = 0; n < kBatch; ++n) {
                Slot& slot = ring->slots[head & ring->mask];
                if (slot.seq.load(std::memory_order_acquire) != head + 1u) {
                    break;
                }
                deliverLocked(static_cast<Level>(slot.level), static_cast<Channel>(slot.channel), slot.file,
                              slot.line, slot.timestampNs, slot.message);
                slot.seq.store(head + ring->capacity, std::memory_order_release);
                ++head;
                g_async.delivered.fetch_add(1u, std::memory_order_relaxed);
                ring->consumed.store(head, std::memory_order_release);
            }
            continue;
        }

        if (g_async.stopRequested.load(std::memory_order_acquire) &&
            ring->tail.load(std::memory_order_acquire) == head) {
            // stopAsync() waited for inflight == 0, so nothing claimed is still unpublished.
            return;
        }

        if (idleSpins < kSpinsBeforePark) {
            ++idleSpins;
            if (idleSpins > 32u) {
                std::this_thread::yield();
            }
            continue;
        }

        // Park. Either the producer sees consumerParked (and bumps wakeWord) or we see its publish.
        const u32 word = g_async.wakeWord.load(std::memory_order_seq_cst);
        g_async.consumerParked.store(true, std::memory_order_seq_cst);
        const bool pending = first->seq.load(std::memory_order_seq_cst) == head + 1u ||
                             g_async.stopRequested.load(std::memory_order_seq_cst);
        if (!pending) {
            g_async.wakeWord.wait(word, std::memory_order_seq_cst);
        }
        g_async.consumerParked.store(false, std::memory_order_seq_cst);
        idleSpins = 0;
    }
}

bool Logger::startAsync(const AsyncOptions& options) {
    std::lock_guard<std::mutex> control(g_async.control);
    if (g_async.active.load(std::memory_order_acquire)) {
        return false;
    }
    const u32 capacity = roundUpPow2(options.capacity);
    auto* ring = new (std::nothrow) Ring();
    Slot* slots = ring ? new (std::nothrow) Slot[capacity] : nullptr;
    if (slots == nullptr) {
        delete ring;
        return false;
    }
    for (u32 i = 0; i < capacity; ++i) {
        slots[i].seq.store(i, std::memory_order_relaxed);
    }
    ring->slots = slots;
    ring->mask = capacity - 1u;
    ring->capacity = capacity;
    g_async.ring.store(ring, std::memory_order_release);
    g_async.stopRequested.store(false, std::memory_order_release);
    g_async.generation.fetch_add(1u, std::memory_order_seq_cst);
    try {
        g_async.consumer = std::thread([this] { runConsumer(); });
    } catch (...) {
        g_async.ring.store(nullptr, std::memory_order_release);
        delete[] slots;
        delete ring;
        return false;
    }
    g_async.active.store(true, std::memory_order_seq_cst);
    return true;
}

void Logger::stopAsync() {
    std::lock_guard<std::mutex> control(g_async.control);
    if (!g_async.active.load(std::memory_order_acquire)) {
        return;
    }
    // New emits go synchronous (after the drain, see logVAt); wait for the ones already inside
    // emitAsync() to publish.
    g_async.draining.store(true, std::memory_order_seq_cst);
    g_async.active.store(false, std::memory_order_seq_cst);
    while (g_async.inflight.load(std::memory_order_seq_cst) != 0u) {
        std::this_thread::yield();
    }
    g_async.stopRequested.store(true, std::memory_order_seq_cst);
    wakeConsumer();
    if (g_async.consumer.joinable()) {
        g_async.consumer.join();
    }
    Ring* ring = g_async.ring.exchange(nullptr, std::memory_order_acq_rel);
    if (ring != nullptr) {
        g_async.enqueuedBase += ring->tail.load(std::memory_order_acquire);
        delete[] ring->slots;
        delete ring;
    }
    g_async.draining.store(false, std::memory_order_seq_cst);
}

bool Logger::isAsync() const {
    return g_async.active.load(std::memory_order_acquire);
}

void Logger::flush() {
    if (t_isConsumer) {
        return;
    }
    // Pin the ring so stopAsync() cannot free it while we read it.
    g_async.inflight.fetch_add(1u, std::memory_order_seq_cst);
    if (!g_async.active.load(std::memory_order_seq_cst)) {
        g_async.inflight.fetch_sub(1u, std::memory_order_release);
        return;
    }
    Ring* ring = g_async.ring.load(std::memory_order_acquire);
    const u64 generation = g_async.generation.load(std::memory_order_seq_cst);
    const u64 target = ring->tail.load(std::memory_order_acquire);
    // Drop the pin while waiting: the consumer may wait for producers, and stopAsync() drains anyway.
    g_async.inflight.fetch_sub(1u, std::memory_order_release);
    u32 spins = 0;
    for (;;) {
        // Re-pin for each read of the ring.
        g_async.inflight.fetch_add(1u, std::memory_order_seq_cst);
        const bool live = g_async.active.load(std::memory_order_seq_cst) &&
                          g_async.generation.load(std::memory_order_seq_cst) == generation;
        const bool done = !live || ring->consumed.load(std::memory_order_acquire) >= target;
        g_async.inflight.fetch_sub(1u, std::memory_order_release);
        if (done) {
            if (!live) {
                // stopAsync() is draining (or has drained) the ring; wait for it to finish.
                std::lock_guard<std::mutex> control(g_async.control);
            }
            return;
        }
        if (g_async.consumerParked.load(std::memory_order_seq_cst)) {
            wakeConsumer();
        }
        if (++spins < 64u) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
}

u64 Logger::droppedCount() const {
    return g_async.dropped.load(std::memory_order_relaxed);
}

AsyncStats Logger::asyncStats() const {
    AsyncStats stats;
    std::lock_guard<std::mutex> control(g_async.control);
    const Ring* ring = g_async.ring.load(std::memory_order_acquire);
    stats.enqueued = g_async.enqueuedBase + (ring ? ring->tail.load(std::memory_order_acquire) : 0u);
    stats.delivered = g_async.delivered.load(std::memory_order_relaxed);
    stats.dropped = g_async.dropped.load(std::memory_order_relaxed);
    stats.capacity = ring ? ring->capacity : 0u;
    return stats;
}

RecordSnapshot Logger::snapshotRecords() const {
    const_cast<Logger*>(this)->flush();
    std::lock_guard<std::mutex> lock(g_logMutex);
    RecordSnapshot snap{};
    snap.count = m_recordCount;
    const u32 first = m_recordWrite - m_recordCount;
    for (u32 i = 0; i < m_recordCount; ++i) {
        snap.records[i] = m_records[(first + i) % kRecordCapacity];
    }
    return snap;
}

} // namespace fuse::log
