#pragma once

#include <fuse/log/logger.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

/// Headless console log buffer (B6.11 stub — Qt text view deferred to U6 chrome).
class ConsolePanel {
public:
    using LogLevel = fuse::log::Level;

    struct LogLine {
        LogLevel level = LogLevel::Info;
        std::string text;
        u64 timestampMs = 0;
        u32 repeatCount = 1;
    };

    void addLog(LogLevel level, const char* message, u64 timestampMs = 0);
    void clear();

    void setShowTrace(bool show) { m_showTrace = show; }
    void setShowDebug(bool show) { m_showDebug = show; }
    void setShowInfo(bool show) { m_showInfo = show; }
    void setShowWarnings(bool show) { m_showWarnings = show; }
    void setShowErrors(bool show) { m_showErrors = show; }

    bool showTrace() const { return m_showTrace; }
    bool showDebug() const { return m_showDebug; }
    bool showInfo() const { return m_showInfo; }
    bool showWarnings() const { return m_showWarnings; }
    bool showErrors() const { return m_showErrors; }

    void setTextFilter(const char* filter);
    const std::string& textFilter() const { return m_textFilter; }

    const std::vector<LogLine>& lines() const { return m_lines; }
    std::vector<LogLine> filteredLines() const;

    bool executeCommand(const char* command);
    const std::string& lastExecutedCommand() const { return m_lastExecutedCommand; }

    static const char* levelLabel(LogLevel level);

private:
    static bool levelVisible(LogLevel level, bool showTrace, bool showDebug, bool showInfo,
                             bool showWarnings, bool showErrors);
    static bool matchesTextFilter(const LogLine& line, const std::string& filter);

    std::vector<LogLine> m_lines;
    std::string m_textFilter;
    std::string m_lastExecutedCommand;
    bool m_showTrace = false;
    bool m_showDebug = true;
    bool m_showInfo = true;
    bool m_showWarnings = true;
    bool m_showErrors = true;
};

} // namespace fuse::editor
