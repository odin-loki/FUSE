#include <fuse/script/script_console_command_registry.hpp>

#include <fuse/script/script_console.hpp>

#include <sstream>

namespace fuse::script {

bool ScriptConsoleCommandRegistry::register_built_in(const char* name, CommandHandler handler) {
    if (name == nullptr || !handler) {
        return false;
    }

    const std::string key(name);
    if (m_builtInHandlers.find(key) != m_builtInHandlers.end()) {
        return false;
    }

    m_builtInHandlers[key] = std::move(handler);
    return true;
}

bool ScriptConsoleCommandRegistry::register_custom(const char* name, CommandHandler handler) {
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

bool ScriptConsoleCommandRegistry::unregister_custom(const char* name) {
    if (name == nullptr) {
        return false;
    }
    return m_customHandlers.erase(std::string(name)) > 0;
}

ScriptConsoleCommandResult ScriptConsoleCommandRegistry::dispatch(const char* name, ScriptConsole& console,
                                                                const char* args) const {
    if (name == nullptr || name[0] == '\0') {
        return {ScriptConsoleCommandStatus::InvalidArgument, "empty command"};
    }

    const std::string key(name);
    const auto custom_it = m_customHandlers.find(key);
    if (custom_it != m_customHandlers.end()) {
        return custom_it->second(console, args != nullptr ? args : "");
    }

    const auto built_in_it = m_builtInHandlers.find(key);
    if (built_in_it != m_builtInHandlers.end()) {
        return built_in_it->second(console, args != nullptr ? args : "");
    }

    return {ScriptConsoleCommandStatus::UnknownCommand, "unknown command: " + key};
}

std::vector<std::string> ScriptConsoleCommandRegistry::command_names() const {
    std::vector<std::string> names;
    names.reserve(m_builtInHandlers.size() + m_customHandlers.size());

    for (const auto& entry : m_builtInHandlers) {
        names.push_back(entry.first);
    }
    for (const auto& entry : m_customHandlers) {
        names.push_back(entry.first);
    }

    return names;
}

std::string ScriptConsoleCommandRegistry::formatCommandList() const {
    std::ostringstream out;
    out << "commands:";
    for (const auto& name : command_names()) {
        out << ' ' << name;
    }
    return out.str();
}

std::string ScriptConsoleCommandRegistry::formatCommandLines() const {
    std::ostringstream out;
    bool first = true;
    for (const auto& name : command_names()) {
        if (!first) {
            out << '\n';
        }
        out << name;
        first = false;
    }
    return out.str();
}

} // namespace fuse::script
