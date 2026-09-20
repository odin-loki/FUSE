#pragma once

#include <fuse/types.hpp>

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
    char message[128]{};
};

static constexpr u32 kRecordCapacity = 64;

struct RecordSnapshot {
    Record records[kRecordCapacity]{};
    u32 count = 0;
};

/// Process-wide logger (U3 / WP-04 / B1.6).
/// Thread-safety: a mutex serializes emit, sink, and snapshot (interim; plan wants lock-free).
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

    RecordSnapshot snapshotRecords() const;

private:
    Logger() = default;

    void recordLocked(Level level, Channel channel, const char* message);

    Level m_minLevel = Level::Info;
    u32 m_enabledChannels = static_cast<u32>(Channel::All);
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
