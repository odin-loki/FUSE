// Relight options: logging through the FUSE logger (fuse::log), prefixed "[relight.options]".
// Shipping builds (FUSE_NO_LOGGING) drop info and warnings; errors are always emitted.
#pragma once

#include <fuse/log/logger.hpp>

#include <cstdarg>
#include <cstdio>

namespace fuse::relight::options::detail {

#if defined(__GNUC__) || defined(__clang__)
#define FUSE_RELIGHT_OPTIONS_PRINTF(fmtIndex, argIndex) __attribute__((format(printf, fmtIndex, argIndex)))
#else
#define FUSE_RELIGHT_OPTIONS_PRINTF(fmtIndex, argIndex)
#endif

inline void logMessageV(::fuse::log::Level level, const char* fmt, va_list args) {
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    ::fuse::log::Logger::instance().log(level, ::fuse::log::Channel::Core, "[relight.options] %s", buffer);
}

inline void logInfo(const char* fmt, ...) FUSE_RELIGHT_OPTIONS_PRINTF(1, 2);
inline void logWarn(const char* fmt, ...) FUSE_RELIGHT_OPTIONS_PRINTF(1, 2);
inline void logError(const char* fmt, ...) FUSE_RELIGHT_OPTIONS_PRINTF(1, 2);

inline void logInfo(const char* fmt, ...) {
#if defined(FUSE_NO_LOGGING) && FUSE_NO_LOGGING
    (void)fmt;
#else
    va_list args;
    va_start(args, fmt);
    logMessageV(::fuse::log::Level::Info, fmt, args);
    va_end(args);
#endif
}

inline void logWarn(const char* fmt, ...) {
#if defined(FUSE_NO_LOGGING) && FUSE_NO_LOGGING
    (void)fmt;
#else
    va_list args;
    va_start(args, fmt);
    logMessageV(::fuse::log::Level::Warn, fmt, args);
    va_end(args);
#endif
}

inline void logError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    logMessageV(::fuse::log::Level::Error, fmt, args);
    va_end(args);
}

} // namespace fuse::relight::options::detail
