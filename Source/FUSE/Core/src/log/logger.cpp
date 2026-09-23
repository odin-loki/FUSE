#include <fuse/log/logger.hpp>

#include <chrono>
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

std::mutex g_logMutex;
} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::setMinLevel(Level level) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    m_minLevel = level;
}

Level Logger::minLevel() const {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return m_minLevel;
}

void Logger::setEnabledChannels(u32 mask) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    m_enabledChannels = mask;
}

u32 Logger::enabledChannels() const {
    std::lock_guard<std::mutex> lock(g_logMutex);
    return m_enabledChannels;
}

void Logger::setSink(SinkFn sink, void* userData) {
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

void Logger::logVAt(Level level, Channel channel, const char* file, u32 line, const char* fmt, va_list args) {
#if defined(FUSE_SHIPPING) && FUSE_SHIPPING
    if (level < Level::Fatal) {
        return;
    }
#endif

    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (level < m_minLevel) {
            return;
        }
        if ((static_cast<u32>(channel) & m_enabledChannels) == 0u) {
            return;
        }
    }

    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt ? fmt : "", args);

    std::lock_guard<std::mutex> lock(g_logMutex);
    // Timestamp under the lock so ring order and timestamp order always agree.
    const u64 timestampNs = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                 .count());
    recordLocked(level, channel, file, line, timestampNs, buffer);
    if (m_sink) {
        m_sink(level, buffer, m_sinkUser);
        return;
    }

    if (file != nullptr) {
        std::fprintf(stderr, "[%s] %s:%u: %s\n", levelPrefix(level), file, line, buffer);
    } else {
        std::fprintf(stderr, "[%s] %s\n", levelPrefix(level), buffer);
    }
}

RecordSnapshot Logger::snapshotRecords() const {
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
