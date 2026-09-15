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

const char* commandKindLabel(ScriptConsoleCommandKind kind) {
    switch (kind) {
    case ScriptConsoleCommandKind::BuiltIn:
        return "built-in";
    case ScriptConsoleCommandKind::Custom:
        return "custom";
    case ScriptConsoleCommandKind::Unknown:
    default:
        return "unknown";
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
    m_history.setCapacity(capacity);
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

bool ScriptConsole::register_command(const char* name, ScriptConsoleCommandRegistry::CommandHandler handler) {
    return m_commands.register_custom(name, std::move(handler));
}

bool ScriptConsole::unregister_command(const char* name) {
    return m_commands.unregister_custom(name);
}

ScriptConsoleCommandResult ScriptConsole::dispatch_(const char* command, const char* args) {
    return m_commands.dispatch(command, *this, args);
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

    m_history.push(line);
    resetHistoryNavigation();

    ScriptConsoleCommandResult result = dispatch_(command.c_str(), args.c_str());
    if (!result.output.empty()) {
        appendOutput_(result.output);
    }
    return result;
}

void ScriptConsole::registerBuiltIns_() {
    m_commands.register_built_in("help", [](ScriptConsole& console, const char* /*args*/) {
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, console.m_commands.formatCommandList()};
    });

    m_commands.register_built_in("list", [](ScriptConsole& console, const char* /*args*/) {
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, console.m_commands.formatCommandLines()};
    });

    m_commands.register_built_in("describe", [](ScriptConsole& console, const char* args) {
        if (args == nullptr || args[0] == '\0') {
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::InvalidArgument,
                                              "describe requires a command name"};
        }

        const ScriptConsoleCommandKind kind = console.m_commands.lookup_kind(args);
        std::ostringstream out;
        out << args << ": " << commandKindLabel(kind);
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, out.str()};
    });

    m_commands.register_built_in("complete", [](ScriptConsole& console, const char* args) {
        const std::vector<std::string> matches = console.m_commands.commands_with_prefix(args);
        if (matches.empty()) {
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, std::string{}};
        }

        std::ostringstream out;
        for (std::size_t i = 0; i < matches.size(); ++i) {
            if (i > 0) {
                out << ' ';
            }
            out << matches[i];
        }
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, out.str()};
    });

    m_commands.register_built_in("echo", [](ScriptConsole& /*console*/, const char* args) {
        if (args == nullptr || args[0] == '\0') {
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::InvalidArgument,
                                              "echo requires text"};
        }
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, std::string(args)};
    });

    m_commands.register_built_in("clear", [](ScriptConsole& console, const char* /*args*/) {
        console.clearOutput();
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, "output cleared"};
    });

    m_commands.register_built_in("history", [](ScriptConsole& console, const char* args) {
        if (args != nullptr && std::strcmp(args, "clear") == 0) {
            console.m_history.clear();
            console.resetHistoryNavigation();
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, "history cleared"};
        }

        std::ostringstream out;
        out << "history (" << console.historyCount() << ')';
        for (u32 i = 0; i < console.historyCount(); ++i) {
            out << '\n' << i << ": " << console.historyAt(i);
        }
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, out.str()};
    });

    m_commands.register_built_in("backend", [](ScriptConsole& console, const char* /*args*/) {
        if (console.m_host == nullptr || !console.m_host->is_initialized()) {
            return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::BackendUnavailable,
                                              "no script host attached"};
        }

        const ScriptVM& vm = console.m_host->vm();
        std::ostringstream out;
        out << "backend=" << backendLabel(vm.backend_kind())
            << " chunks=" << vm.loaded_chunk_count();
        return ScriptConsoleCommandResult{ScriptConsoleCommandStatus::Ok, out.str()};
    });

    m_commands.register_built_in("load", [](ScriptConsole& console, const char* args) {
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
    });

    m_commands.register_built_in("run", [](ScriptConsole& console, const char* args) {
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
    });
}

} // namespace fuse::script
