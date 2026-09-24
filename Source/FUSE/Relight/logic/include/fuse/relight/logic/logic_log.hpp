// FUSE Relight RL-3.5: diagnostics of the Logic graph runtime.
//
// Upstream logs through dxvk::Logger (and ONCE(...) for per-frame messages). The runtime here keeps the messages
// in a bounded, process-wide list so tests and the capture record can read them; logOnce() drops repeats of the
// same text, like ONCE. Messages are echoed to stderr when FUSE_RELIGHT_LOGIC_LOG_ECHO=1 (or setEcho(true)).
#pragma once

#include <string>
#include <vector>

namespace fuse::relight::logic {

enum class LogSeverity : unsigned char { Info, Warning, Error };
const char* logSeverityName(LogSeverity s);

struct LogMessage {
    LogSeverity severity = LogSeverity::Info;
    std::string text;
};

void logMessage(LogSeverity severity, std::string text);
/// Logs `text` the first time it is seen in the process (upstream ONCE(Logger::...)).
void logOnce(LogSeverity severity, std::string text);
/// Returns and clears the stored messages.
std::vector<LogMessage> takeLogMessages();
/// Forgets the logOnce history (tests).
void resetLogOnce();
void setLogEcho(bool echo);

} // namespace fuse::relight::logic
