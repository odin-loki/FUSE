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
};

using SinkFn = void (*)(Level level, const char* message, void* userData);

/// Minimal process-wide logger (U3 / WP-04). Thread-safe enough for tests and smoke.
class Logger {
public:
    static Logger& instance();

    void setMinLevel(Level level);
    Level minLevel() const;

    void setSink(SinkFn sink, void* userData = nullptr);

    void log(Level level, const char* fmt, ...);
    void logV(Level level, const char* fmt, va_list args);

private:
    Logger() = default;

    Level m_minLevel = Level::Info;
    SinkFn m_sink = nullptr;
    void* m_sinkUser = nullptr;
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

} // namespace fuse::log
