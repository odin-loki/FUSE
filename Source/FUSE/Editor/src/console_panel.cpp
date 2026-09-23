#include <fuse/editor/console_panel.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iterator>

namespace fuse::editor {

namespace {

std::string toLower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

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

bool ConsolePanel::parseCommandLine(std::string_view line, ParsedCommand& out) {
    out = {};
    std::vector<std::string> tokens;
    std::string token;
    bool inToken = false;
    bool inQuotes = false;

    for (usize i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (inQuotes) {
            if (ch == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) {
                token += line[++i];
            } else if (ch == '"') {
                inQuotes = false;
            } else {
                token += ch;
            }
            continue;
        }
        if (ch == '"') {
            inQuotes = true;
            inToken = true;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (inToken) {
                tokens.push_back(std::move(token));
                token.clear();
                inToken = false;
            }
            continue;
        }
        token += ch;
        inToken = true;
    }

    if (inQuotes) {
        return false;
    }
    if (inToken) {
        tokens.push_back(std::move(token));
    }
    if (tokens.empty()) {
        return false;
    }

    out.name = toLower(tokens.front());
    out.args.assign(std::make_move_iterator(tokens.begin() + 1), std::make_move_iterator(tokens.end()));
    return true;
}

ConsolePanel::ConsolePanel() {
    registerBuiltins_();
}

void ConsolePanel::registerBuiltins_() {
    m_commands.emplace("help", [](const std::vector<std::string>&, ConsolePanel& console) {
        std::string names;
        for (const std::string& name : console.commandNames()) {
            names += names.empty() ? name : " " + name;
        }
        console.addLog(LogLevel::Info, ("commands: " + names).c_str());
        return true;
    });
    m_commands.emplace("clear", [](const std::vector<std::string>&, ConsolePanel& console) {
        console.clear();
        return true;
    });
}

void ConsolePanel::registerCommand(std::string name, CommandHandler handler) {
    name = toLower(std::move(name));
    if (name.empty() || !handler) {
        return;
    }
    m_commands[std::move(name)] = std::move(handler);
}

bool ConsolePanel::hasCommand(std::string_view name) const {
    return m_commands.count(toLower(std::string(name))) != 0u;
}

std::vector<std::string> ConsolePanel::commandNames() const {
    std::vector<std::string> names;
    names.reserve(m_commands.size());
    for (const auto& entry : m_commands) {
        names.push_back(entry.first); // std::map keeps them sorted
    }
    return names;
}

bool ConsolePanel::executeCommand(const char* command) {
    m_historyCursor = m_history.size();

    if (command == nullptr) {
        m_lastExecutedCommand.clear();
        return false;
    }

    ParsedCommand parsed;
    if (!parseCommandLine(command, parsed)) {
        if (command[0] != '\0') {
            addLog(LogLevel::Error, (std::string("malformed command: ") + command).c_str());
        }
        return false;
    }

    m_lastExecutedCommand = command;
    if (m_history.empty() || m_history.back() != m_lastExecutedCommand) {
        m_history.push_back(m_lastExecutedCommand);
        if (m_history.size() > kMaxCommandHistory) {
            m_history.erase(m_history.begin());
        }
    }
    m_historyCursor = m_history.size();

    addLog(LogLevel::Info, ("exec: " + m_lastExecutedCommand).c_str());

    const auto it = m_commands.find(parsed.name);
    if (it == m_commands.end()) {
        addLog(LogLevel::Error, ("unknown command: " + parsed.name).c_str());
        return false;
    }

    // Copy: the handler may re-register commands (invalidating `it`).
    const CommandHandler handler = it->second;
    return handler(parsed.args, *this);
}

std::string ConsolePanel::historyPrevious() {
    if (m_history.empty()) {
        return {};
    }
    if (m_historyCursor > 0u) {
        --m_historyCursor;
    }
    return m_history[m_historyCursor];
}

std::string ConsolePanel::historyNext() {
    if (m_historyCursor + 1u >= m_history.size()) {
        m_historyCursor = m_history.size();
        return {};
    }
    ++m_historyCursor;
    return m_history[m_historyCursor];
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
