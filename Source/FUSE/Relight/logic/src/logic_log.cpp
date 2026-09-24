// FUSE Relight RL-3.5: diagnostics of the Logic graph runtime (see logic_log.hpp).
#include <fuse/relight/logic/logic_log.hpp>

#include <fuse/relight/options/option_config.hpp>

#include <cstdio>
#include <mutex>
#include <set>

namespace fuse::relight::logic {

namespace {

constexpr std::size_t kMaxStoredMessages = 1024;

struct LogState {
    std::mutex mutex;
    std::vector<LogMessage> messages;
    std::set<std::string> once;
    int echo = -1; ///< -1: from FUSE_RELIGHT_LOGIC_LOG_ECHO
};
LogState& state() {
    static LogState s;
    return s;
}

bool echoEnabled(LogState& s) {
    if (s.echo < 0) {
        s.echo = options::getEnvironmentVariable("FUSE_RELIGHT_LOGIC_LOG_ECHO") == "1" ? 1 : 0;
    }
    return s.echo == 1;
}

} // namespace

const char* logSeverityName(LogSeverity s) {
    switch (s) {
    case LogSeverity::Info: return "info";
    case LogSeverity::Warning: return "warning";
    case LogSeverity::Error: return "error";
    }
    return "?";
}

void logMessage(LogSeverity severity, std::string text) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (echoEnabled(s)) {
        std::fprintf(stderr, "fuse-relight: logic: %s: %s\n", logSeverityName(severity), text.c_str());
    }
    if (s.messages.size() < kMaxStoredMessages) {
        s.messages.push_back({severity, std::move(text)});
    }
}

void logOnce(LogSeverity severity, std::string text) {
    {
        LogState& s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.once.insert(text).second) {
            return;
        }
    }
    logMessage(severity, std::move(text));
}

std::vector<LogMessage> takeLogMessages() {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    std::vector<LogMessage> out;
    out.swap(s.messages);
    return out;
}

void resetLogOnce() {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.once.clear();
}

void setLogEcho(bool echo) {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.echo = echo ? 1 : 0;
}

} // namespace fuse::relight::logic
