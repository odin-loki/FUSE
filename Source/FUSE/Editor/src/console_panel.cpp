#include <fuse/editor/console_panel.hpp>

#include <chrono>
#include <cstring>

namespace fuse::editor {

namespace {

u64 defaultTimestampMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace

void ConsolePanel::addLog(LogLevel level, const char* message, u64 timestampMs) {
    if (message == nullptr) {
        return;
    }

    const u64 stamp = timestampMs != 0 ? timestampMs : defaultTimestampMs();

    if (!m_lines.empty()) {
        LogLine& last = m_lines.back();
        if (last.level == level && last.text == message) {
            ++last.repeatCount;
            last.timestampMs = stamp;
            return;
        }
    }

    LogLine line;
    line.level = level;
    line.text = message;
    line.timestampMs = stamp;
    m_lines.push_back(std::move(line));
}

void ConsolePanel::clear() {
    m_lines.clear();
}

void ConsolePanel::setTextFilter(const char* filter) {
    m_textFilter = filter != nullptr ? filter : "";
}

std::vector<ConsolePanel::LogLine> ConsolePanel::filteredLines() const {
    std::vector<LogLine> filtered;
    filtered.reserve(m_lines.size());

    for (const LogLine& line : m_lines) {
        if (!levelVisible(line.level, m_showTrace, m_showDebug, m_showInfo, m_showWarnings,
                          m_showErrors)) {
            continue;
        }
        if (!matchesTextFilter(line, m_textFilter)) {
            continue;
        }
        filtered.push_back(line);
    }

    return filtered;
}

bool ConsolePanel::executeCommand(const char* command) {
    if (command == nullptr) {
        m_lastExecutedCommand.clear();
        return false;
    }

    m_lastExecutedCommand = command;
    addLog(LogLevel::Info, ("exec: " + m_lastExecutedCommand).c_str());
    return true;
}

const char* ConsolePanel::levelLabel(LogLevel level) {
    switch (level) {
    case LogLevel::Trace:
        return "Trace";
    case LogLevel::Debug:
        return "Debug";
    case LogLevel::Info:
        return "Info";
    case LogLevel::Warn:
        return "Warn";
    case LogLevel::Error:
        return "Error";
    default:
        return "Unknown";
    }
}

bool ConsolePanel::levelVisible(LogLevel level, bool showTrace, bool showDebug, bool showInfo,
                                bool showWarnings, bool showErrors) {
    switch (level) {
    case LogLevel::Trace:
        return showTrace;
    case LogLevel::Debug:
        return showDebug;
    case LogLevel::Info:
        return showInfo;
    case LogLevel::Warn:
        return showWarnings;
    case LogLevel::Error:
        return showErrors;
    default:
        return true;
    }
}

bool ConsolePanel::matchesTextFilter(const LogLine& line, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }
    return line.text.find(filter) != std::string::npos;
}

} // namespace fuse::editor
