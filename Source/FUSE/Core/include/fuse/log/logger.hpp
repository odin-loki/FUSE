#pragma once

#include <fuse/types.hpp>

#include <atomic>
#include <cstdarg>

namespace fuse::log {

enum class Level : u8 {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
};

enum class Channel : u32 {
    Core = 1,
    Renderer = 2,
    Physics = 4,
    Ecs = 8,
    Assets = 16,
    Compute = 32,
    Audio = 64,
    Editor = 128,
    All = ~0u,
};

using SinkFn = void (*)(Level level, const char* message, void* userData);

/// Last N formatted lines retained in-process so tests can assert without scraping stderr.
struct Record {
    Level level = Level::Info;
    Channel channel = Channel::Core;
    /// Monotonic (steady clock) nanoseconds when the entry was emitted.
    u64 timestampNs = 0;
    /// Source location for FUSE_LOG_* / logAt(); nullptr and 0 for plain log() calls.
    const char* file = nullptr;
    u32 line = 0;
    char message[128]{};
};

static constexpr u32 kRecordCapacity = 64;

struct RecordSnapshot {
    Record records[kRecordCapacity]{};
    u32 count = 0;
};

/// Async mode configuration (see Logger::startAsync).
struct AsyncOptions {
    /// Ring slots; rounded up to a power of two (minimum 2). Memory is ~capacity * 576 bytes.
    u32 capacity = 1024;
};

/// Counters for the async ring. `enqueued + dropped` is every async emit that passed the filters.
struct AsyncStats {
    u64 enqueued = 0;  ///< Messages that claimed a ring slot.
    u64 delivered = 0; ///< Messages the consumer thread handed to the record ring + sink/stderr.
    u64 dropped = 0;   ///< Messages discarded because the ring was full (overflow policy: drop + count).
    u32 capacity = 0;  ///< Slots in the current ring (0 when async mode is off).
};

/// Longest message (including the terminator) an async emit carries; longer text is truncated.
/// Synchronous emits keep the 1024-byte format buffer.
static constexpr u32 kAsyncMessageBytes = 512;

/// Process-wide logger (U3 / WP-04 / B1.6).
///
/// Two delivery modes:
///  - Synchronous (default): the calling thread formats, records and calls the sink under a mutex,
///    so the sink has run by the time log() returns.
///  - Async (startAsync()): producers format straight into a bounded lock-free MPSC ring (Vyukov
///    per-slot sequence numbers) and return; one consumer thread records and calls the sink.
///    Producer path: no mutex, no heap allocation, never blocks. When the ring is full the message is
///    dropped and counted (droppedCount()). Messages from one thread reach the sink in the order that
///    thread emitted them. Fatal messages flush the ring and are then emitted synchronously.
///    setSink() and snapshotRecords() flush first, so "log, then inspect" still sees the entry.
///
/// Level/channel filters are atomics and never take a lock. The sink is always called by one thread
/// at a time.
class Logger {
public:
    static Logger& instance();

    void setMinLevel(Level level);
    Level minLevel() const;

    void setEnabledChannels(u32 mask);
    u32 enabledChannels() const;

    void setSink(SinkFn sink, void* userData = nullptr);

    void log(Level level, const char* fmt, ...);
    void log(Level level, Channel channel, const char* fmt, ...);
    void logV(Level level, const char* fmt, va_list args);
    void logV(Level level, Channel channel, const char* fmt, va_list args);

    /// Log with a source location (used by the FUSE_LOG_* macros).
    void logAt(Level level, Channel channel, const char* file, u32 line, const char* fmt, ...);
    void logVAt(Level level, Channel channel, const char* file, u32 line, const char* fmt, va_list args);

    RecordSnapshot snapshotRecords() const;

    /// Switch to async delivery with a fresh ring. Returns false when already async or the ring or
    /// consumer thread cannot be created (the logger then stays synchronous).
    bool startAsync(const AsyncOptions& options = {});
    /// Drain everything already enqueued, join the consumer thread and return to synchronous mode.
    /// Safe to call while other threads log (their in-flight emits finish first).
    void stopAsync();
    bool isAsync() const;
    /// Block until every message enqueued before the call has been delivered. No-op when synchronous
    /// or when called from the consumer thread (i.e. from inside a sink).
    void flush();
    /// Total messages dropped on ring overflow since the process started.
    u64 droppedCount() const;
    AsyncStats asyncStats() const;

private:
    Logger() = default;
    ~Logger();

    void recordLocked(Level level, Channel channel, const char* file, u32 line, u64 timestampNs,
                      const char* message);

    bool emitAsync(Level level, Channel channel, const char* file, u32 line, const char* fmt, va_list args);
    void enqueue(void* ring, Level level, Channel channel, const char* file, u32 line, const char* fmt,
                 va_list args);
    void runConsumer();
    void deliverLocked(Level level, Channel channel, const char* file, u32 line, u64 timestampNs,
                       const char* message);
    void emitSync(Level level, Channel channel, const char* file, u32 line, const char* fmt, va_list args);

    std::atomic<u8> m_minLevel{static_cast<u8>(Level::Info)};
    std::atomic<u32> m_enabledChannels{static_cast<u32>(Channel::All)};
    SinkFn m_sink = nullptr;
    void* m_sinkUser = nullptr;
    Record m_records[kRecordCapacity]{};
    u32 m_recordWrite = 0;
    u32 m_recordCount = 0;
};

inline void info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Logger::instance().logV(Level::Info, fmt, args);
    va_end(args);
}

inline void warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Logger::instance().logV(Level::Warn, fmt, args);
    va_end(args);
}

inline void debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Logger::instance().logV(Level::Debug, fmt, args);
    va_end(args);
}

inline void error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Logger::instance().logV(Level::Error, fmt, args);
    va_end(args);
}

inline void fatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    Logger::instance().logV(Level::Fatal, fmt, args);
    va_end(args);
}

} // namespace fuse::log

/// Channel-Core logging with the call site's file and line recorded in the entry.
#define FUSE_LOG_AT(level, channel, ...)                                                          \
    ::fuse::log::Logger::instance().logAt((level), (channel), __FILE__,                           \
                                          static_cast<::fuse::u32>(__LINE__), __VA_ARGS__)
#if defined(FUSE_NO_LOGGING) && FUSE_NO_LOGGING
namespace fuse::log::detail {
/// Shipping (B7.8): arguments are still evaluated, but no call, format string or file name is emitted.
template <typename... Args>
inline void discardLog(Args&&...) {}
} // namespace fuse::log::detail
#define FUSE_LOG_TRACE(...) ::fuse::log::detail::discardLog(__VA_ARGS__)
#define FUSE_LOG_DEBUG(...) ::fuse::log::detail::discardLog(__VA_ARGS__)
#define FUSE_LOG_INFO(...) ::fuse::log::detail::discardLog(__VA_ARGS__)
#define FUSE_LOG_WARN(...) ::fuse::log::detail::discardLog(__VA_ARGS__)
#define FUSE_LOG_ERROR(...) ::fuse::log::detail::discardLog(__VA_ARGS__)
#else
#define FUSE_LOG_TRACE(...) FUSE_LOG_AT(::fuse::log::Level::Trace, ::fuse::log::Channel::Core, __VA_ARGS__)
#define FUSE_LOG_DEBUG(...) FUSE_LOG_AT(::fuse::log::Level::Debug, ::fuse::log::Channel::Core, __VA_ARGS__)
#define FUSE_LOG_INFO(...) FUSE_LOG_AT(::fuse::log::Level::Info, ::fuse::log::Channel::Core, __VA_ARGS__)
#define FUSE_LOG_WARN(...) FUSE_LOG_AT(::fuse::log::Level::Warn, ::fuse::log::Channel::Core, __VA_ARGS__)
#define FUSE_LOG_ERROR(...) FUSE_LOG_AT(::fuse::log::Level::Error, ::fuse::log::Channel::Core, __VA_ARGS__)
#endif
#define FUSE_LOG_FATAL(...) FUSE_LOG_AT(::fuse::log::Level::Fatal, ::fuse::log::Channel::Core, __VA_ARGS__)
