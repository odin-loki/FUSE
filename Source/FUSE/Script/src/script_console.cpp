#include <fuse/script/script_console.hpp>

#include <fuse/script/script_host.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace fuse::script {

namespace {

std::string trim(const std::string& text) {
    const auto begin = std::find_if_not(text.begin(), text.end(),
                                        [](unsigned char ch) { return std::isspace(ch) != 0; });
    const auto end = std::find_if_not(text.rbegin(), text.rend(),
                                      [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

bool splitCommandLine(const std::string& line, std::string& command, std::string& args) {
    const std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return false;
    }

    const std::size_t space = trimmed.find_first_of(" \t");
    if (space == std::string::npos) {
        command = trimmed;
        args.clear();
        return true;
    }

    command = trimmed.substr(0, space);
    args = trim(trimmed.substr(space + 1));
    return true;
}

const char* backendLabel(ScriptBackendKind kind) {
    switch (kind) {
    case ScriptBackendKind::Lua:
        return "lua";
    case ScriptBackendKind::Null:
    default:
        return "null";
    }
}

} // namespace

void ScriptConsole::attach(ScriptHost* host) {
    m_host = host;
}

void ScriptConsole::detach() {
    m_host = nullptr;
}

ScriptConsole::ScriptConsole() {
    registerBuiltIns_();
}

void ScriptConsole::setHistoryCapacity(u32 capacity) {
    m_historyCapacity = capacity == 0 ? 1u : capacity;
    while (m_history.size() > m_historyCapacity) {
        m_history.erase(m_history.begin());
        if (m_historyCursor > 0) {
            --m_historyCursor;
        }
    }
    if (m_historyCursor >= static_cast<s32>(m_history.size())) {
        m_historyCursor = static_cast<s32>(m_history.size());
    }
}

const std::string& ScriptConsole::historyAt(u32 index) const {
    static const std::string kEmpty;
    if (index >= m_history.size()) {
        return kEmpty;
    }
    return m_history[index];
}

void ScriptConsole::pushHistory_(const std::string& line) {
    const std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return;
    }

    if (!m_history.empty() && m_history.back() == trimmed) {
        return;
    }

    m_history.push_back(trimmed);
    if (m_history.size() > m_historyCapacity) {
        m_history.erase(m_history.begin());
    }
    m_historyCursor = static_cast<s32>(m_history.size());
}

const std::string& ScriptConsole::recallHistory(bool previous) {
    static const std::string kEmpty;

    if (m_history.empty()) {
        return kEmpty;
    }

    if (previous) {
        if (m_historyCursor <= 0) {
            m_historyCursor = 0;
        } else {
            --m_historyCursor;
        }
        return m_history[static_cast<std::size_t>(m_historyCursor)];
    }

    if (m_historyCursor >= static_cast<s32>(m_history.size()) - 1) {
        m_historyCursor = static_cast<s32>(m_history.size());
        return kEmpty;
    }

    ++m_historyCursor;
    if (m_historyCursor >= static_cast<s32>(m_history.size())) {
        m_historyCursor = static_cast<s32>(m_history.size());
        return kEmpty;
    }

    return m_history[static_cast<std::size_t>(m_historyCursor)];
}

void ScriptConsole::resetHistoryNavigation() {
    m_historyCursor = static_cast<s32>(m_history.size());
}

void ScriptConsole::clearOutput() {
    m_output.clear();
}

void ScriptConsole::appendOutput_(const std::string& text) {
    if (text.empty()) {
        return;
    }
    m_output.push_back(text);
}

bool ScriptConsole::register_command(const char* name, CommandHandler handler) {
    if (name == nullptr || !handler) {
        return false;
    }

    const std::string key(name);
    if (m_builtInHandlers.find(key) != m_builtInHandlers.end()) {
        return false;
    }

    m_customHandlers[key] = std::move(handler);
    return true;
}

bool ScriptConsole::unregister_command(const char* name) {
    if (name == nullptr) {
        return false;
    }
    return m_customHandlers.erase(std::string(name)) > 0;
}

ScriptConsoleCommandResult ScriptConsole::dispatch_(const char* command, const char* args) {
    if (command == nullptr || command[0] == '\0') {
        return {ScriptConsoleCommandStatus::InvalidArgument, "empty command"};
    }

    const std::string key(command);
    const auto custom_it = m_customHandlers.find(key);
    if (custom_it != m_customHandlers.end()) {
        return custom_it->second(*this, args != nullptr ? args : "");
    }

    const auto built_in_it = m_builtInHandlers.find(key);
    if (built_in_it != m_builtInHandlers.end()) {
        return built_in_it->second(*this, args != nullptr ? args : "");
    }

    return {ScriptConsoleCommandStatus::UnknownCommand, "unknown command: " + key};
}

ScriptConsoleCommandResult ScriptConsole::execute(const char* line) {
    if (line == nullptr) {
        return {ScriptConsoleCommandStatus::InvalidArgument, "line is null"};
    }

    std::string command;
    std::string args;
    if (!splitCommandLine(line, command, args)) {
        return {ScriptConsoleCommandStatus::InvalidArgument, "empty line"};
    }

    pushHistory_(line);
    resetHistoryNavigation();

    ScriptConsoleCommandResult result = dispatch_(command.c_str(), args.c_str());
    if (!result.output.empty()) {
        appendOutput_(result.output);
    }
    return result;
}

std::string ScriptConsole::formatCommandList_() const {
    std::ostringstream out;
    out << "commands:";
    for (const auto& entry : m_builtInHandlers) {
        out << ' ' << entry.first;
    }
    for (const auto& entry : m_customHandlers) {
        out << ' ' << entry.first;
    }
    return out.str();
}

void ScriptConsole::registerBuiltIns_() {
    m_builtInHandlers["help"] = [](ScriptConsole& console, const char* /*args*/) {
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, console.formatCommandList_()};
    };

    m_builtInHandlers["echo"] = [](ScriptConsole& /*console*/, const char* args) {
        if (args == nullptr || args[0] == '\0') {
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::InvalidArgument,
                                              "echo requires text"};
        }
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, std::string(args)};
    };

    m_builtInHandlers["clear"] = [](ScriptConsole& console, const char* /*args*/) {
        console.clearOutput();
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, "output cleared"};
    };

    m_builtInHandlers["history"] = [](ScriptConsole& console, const char* /*args*/) {
        std::ostringstream out;
        out << "history (" << console.historyCount() << ')';
        for (u32 i = 0; i < console.historyCount(); ++i) {
            out << '\n' << i << ": " << console.historyAt(i);
        }
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, out.str()};
    };

    m_builtInHandlers["backend"] = [](ScriptConsole& console, const char* /*args*/) {
        if (console.m_host == nullptr || !console.m_host->is_initialized()) {
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::BackendUnavailable,
                                              "no script host attached"};
        }

        const ScriptVM& vm = console.m_host->vm();
        std::ostringstream out;
        out << "backend=" << backendLabel(vm.backend_kind())
            << " chunks=" << vm.loaded_chunk_count();
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, out.str()};
    };

    m_builtInHandlers["load"] = [](ScriptConsole& console, const char* args) {
        if (args == nullptr || args[0] == '\0') {
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::InvalidArgument,
                                              "load requires a file path"};
        }
        if (console.m_host == nullptr || !console.m_host->is_initialized()) {
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::BackendUnavailable,
                                              "no script host attached"};
        }

        const ScriptLoadResult load_result = console.m_host->load_file(args);
        if (!load_result.ok()) {
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::BackendUnavailable,
                                              load_result.message != nullptr ? load_result.message
                                                                             : "load failed"};
        }

        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok,
                                          std::string("loaded ") + args};
    };

    m_builtInHandlers["run"] = [](ScriptConsole& console, const char* args) {
        if (args == nullptr || args[0] == '\0') {
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::InvalidArgument,
                                              "run requires lua source"};
        }
        if (console.m_host == nullptr || !console.m_host->is_initialized()) {
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::BackendUnavailable,
                                              "no script host attached"};
        }

        const ScriptLoadResult load_result = console.m_host->load_string(args, "repl");
        if (!load_result.ok()) {
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::BackendUnavailable,
                                              load_result.message != nullptr ? load_result.message
                                                                             : "run failed"};
        }

        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, "ok"};
    };
}

} // namespace fuse::script
