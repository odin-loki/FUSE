#pragma once

#include <fuse/log/logger.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::editor {

/// Headless console log buffer (B6.11 stub — Qt text view deferred to U6 chrome).
class ConsolePanel {
public:
    using LogLevel = fuse::log::Level;

    static constexpr u32 kMaxCommandHistory = 64;

    /// Command line split into a name and arguments (see `parseCommandLine`).
    struct ParsedCommand {
        std::string name;
        std::vector<std::string> args;
    };

    /// Handler for a registered console command; returns false on failure (usage errors etc.).
    using CommandHandler =
        std::function<bool(const std::vector<std::string>& args, ConsolePanel& console)>;

    struct LogLine {
        LogLevel level = LogLevel::Info;
        std::string text;
        u64 timestampMs = 0;
        u32 repeatCount = 1;
    };

    ConsolePanel();

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

    /// Splits on whitespace; double quotes group a token (`\"` and `\\` escape inside quotes).
    /// Returns false for a blank line or an unterminated quote.
    static bool parseCommandLine(std::string_view line, ParsedCommand& out);

    /// Registers (or replaces) a command. Names are case-insensitive. `help` and `clear` are
    /// built in and can be overridden.
    void registerCommand(std::string name, CommandHandler handler);
    [[nodiscard]] bool hasCommand(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> commandNames() const;

    /// Echoes the line to the log, records it in the history, parses and dispatches it.
    /// Returns false for blank / malformed lines, unknown commands and failing handlers.
    bool executeCommand(const char* command);
    const std::string& lastExecutedCommand() const { return m_lastExecutedCommand; }

    /// Executed lines, oldest first (consecutive duplicates collapsed, capped at
    /// `kMaxCommandHistory`).
    [[nodiscard]] const std::vector<std::string>& commandHistory() const { return m_history; }
    /// Shell-style recall: `historyPrevious` steps to older lines (stops at the oldest),
    /// `historyNext` steps back toward the newest and returns "" past it. Executing resets.
    std::string historyPrevious();
    std::string historyNext();

    static const char* levelLabel(LogLevel level);

private:
    static bool levelVisible(LogLevel level, bool showTrace, bool showDebug, bool showInfo,
                             bool showWarnings, bool showErrors);
    static bool matchesTextFilter(const LogLine& line, const std::string& filter);

    void registerBuiltins_();

    std::vector<LogLine> m_lines;
    std::string m_textFilter;
    std::string m_lastExecutedCommand;
    std::map<std::string, CommandHandler> m_commands;
    std::vector<std::string> m_history;
    usize m_historyCursor = 0;
    bool m_showTrace = false;
    bool m_showDebug = true;
    bool m_showInfo = true;
    bool m_showWarnings = true;
    bool m_showErrors = true;
};

} // namespace fuse::editor
