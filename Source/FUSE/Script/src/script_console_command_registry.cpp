#include <fuse/script/script_console_command_registry.hpp>

#include <fuse/script/script_console.hpp>

#include <algorithm>
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

    m_customHandlers[std::string(name)] = std::move(handler);
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

bool ScriptConsoleCommandRegistry::has_command(const char* name) const {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    const std::string key(name);
    return m_customHandlers.find(key) != m_customHandlers.end() ||
           m_builtInHandlers.find(key) != m_builtInHandlers.end();
}

bool ScriptConsoleCommandRegistry::is_built_in(const char* name) const {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    return m_builtInHandlers.find(std::string(name)) != m_builtInHandlers.end();
}

bool ScriptConsoleCommandRegistry::is_custom(const char* name) const {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }

    return m_customHandlers.find(std::string(name)) != m_customHandlers.end();
}

ScriptConsoleCommandKind ScriptConsoleCommandRegistry::lookup_kind(const char* name) const {
    if (name == nullptr || name[0] == '\0') {
        return ScriptConsoleCommandKind::Unknown;
    }

    const std::string key(name);
    if (m_customHandlers.find(key) != m_customHandlers.end()) {
        return ScriptConsoleCommandKind::Custom;
    }
    if (m_builtInHandlers.find(key) != m_builtInHandlers.end()) {
        return ScriptConsoleCommandKind::BuiltIn;
    }
    return ScriptConsoleCommandKind::Unknown;
}

std::vector<std::string> ScriptConsoleCommandRegistry::commands_with_prefix(const char* prefix) const {
    const std::string needle = prefix != nullptr ? prefix : "";
    std::vector<std::string> matches;

    for (const auto& name : command_names()) {
        if (needle.empty() || name.compare(0, needle.size(), needle) == 0) {
            matches.push_back(name);
        }
    }

    return matches;
}

std::vector<std::string> ScriptConsoleCommandRegistry::command_names() const {
    std::vector<std::string> names;
    names.reserve(m_builtInHandlers.size() + m_customHandlers.size());

    for (const auto& entry : m_builtInHandlers) {
        if (m_customHandlers.find(entry.first) == m_customHandlers.end()) {
            names.push_back(entry.first);
        }
    }
    for (const auto& entry : m_customHandlers) {
        names.push_back(entry.first);
    }

    std::sort(names.begin(), names.end());
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
