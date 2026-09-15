#include <fuse/log/logger.hpp>

#include <cstdio>
#include <mutex>

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

std::mutex g_logMutex;
} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setMinLevel(Level level) {
    m_minLevel = level;
}

Level Logger::minLevel() const {
    return m_minLevel;
}

void Logger::setSink(SinkFn sink, void* userData) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    m_sink = sink;
    m_sinkUser = userData;
}

void Logger::log(Level level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logV(level, fmt, args);
    va_end(args);
}

void Logger::logV(Level level, const char* fmt, va_list args) {
    if (level < m_minLevel) {
        return;
    }

    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (m_sink) {
        m_sink(level, buffer, m_sinkUser);
        return;
    }

    std::fprintf(stderr, "[%s] %s\n", levelPrefix(level), buffer);
}

} // namespace fuse::log
