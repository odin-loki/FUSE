#include <fuse/script/script_console_command_registry.hpp>

#include <fuse/script/script_console.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace fuse::script {

namespace {

bool containsWhitespace(const char* text) {
    if (text == nullptr) {
        return true;
    }
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        if (std::isspace(static_cast<unsigned char>(*cursor)) != 0) {
            return true;
        }
    }
    return false;
}

} // namespace

bool ScriptConsoleCommandRegistry::is_valid_command_name(const char* name) {
    return name != nullptr && name[0] != '\0' && !containsWhitespace(name);
}

bool ScriptConsoleCommandRegistry::register_built_in(const char* name, CommandHandler handler) {
    if (!is_valid_command_name(name) || !handler) {
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
    if (!is_valid_command_name(name) || !handler) {
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

    std::string message = "unknown command: " + key;
    const std::vector<std::string> suggestions = suggest_commands(key.c_str(), 3);
    if (!suggestions.empty()) {
        message += " (did you mean:";
        for (const auto& suggestion : suggestions) {
            message += ' ';
            message += suggestion;
        }
        message += ')';
    }

    return {ScriptConsoleCommandStatus::UnknownCommand, message};
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

bool ScriptConsoleCommandRegistry::has_commands_with_prefix(const char* prefix) const {
    return !commands_with_prefix(prefix).empty();
}

usize ScriptConsoleCommandRegistry::prefix_match_count(const char* prefix) const {
    return commands_with_prefix(prefix).size();
}

std::string ScriptConsoleCommandRegistry::longest_common_prefix(const char* prefix) const {
    const std::vector<std::string> matches = commands_with_prefix(prefix);
    if (matches.empty()) {
        return {};
    }
    if (matches.size() == 1) {
        return matches.front();
    }

    std::string common = matches.front();
    for (std::size_t i = 1; i < matches.size(); ++i) {
        const std::string& candidate = matches[i];
        const std::size_t limit = std::min(common.size(), candidate.size());
        std::size_t shared = 0;
        while (shared < limit && common[shared] == candidate[shared]) {
            ++shared;
        }
        common.resize(shared);
        if (common.empty()) {
            break;
        }
    }

    return common;
}

std::string ScriptConsoleCommandRegistry::unique_prefix_match(const char* partial) const {
    if (partial == nullptr) {
        partial = "";
    }

    const std::string needle(partial);
    if (has_command(needle.c_str())) {
        return needle;
    }

    const std::vector<std::string> matches = commands_with_prefix(needle.c_str());
    if (matches.size() == 1) {
        return matches.front();
    }

    return {};
}

std::vector<std::string> ScriptConsoleCommandRegistry::suggest_commands(const char* name,
                                                                          u32 max_suggestions) const {
    if (name == nullptr || name[0] == '\0' || max_suggestions == 0) {
        return {};
    }

    const std::string needle(name);
    const std::vector<std::string> names = command_names();
    std::vector<std::string> suggestions;
    suggestions.reserve(max_suggestions);

    auto try_add = [&](const std::string& candidate) {
        if (suggestions.size() >= max_suggestions) {
            return;
        }
        if (std::find(suggestions.begin(), suggestions.end(), candidate) != suggestions.end()) {
            return;
        }
        suggestions.push_back(candidate);
    };

    for (const auto& candidate : commands_with_prefix(needle.c_str())) {
        try_add(candidate);
    }

    if (suggestions.size() < max_suggestions && needle.size() > 1) {
        for (u32 prefix_len = static_cast<u32>(needle.size() - 1); prefix_len > 0; --prefix_len) {
            for (const auto& candidate : commands_with_prefix(needle.substr(0, prefix_len).c_str())) {
                try_add(candidate);
            }
            if (suggestions.size() >= max_suggestions) {
                break;
            }
        }
    }

    if (suggestions.size() < max_suggestions) {
        for (const auto& candidate : names) {
            if (candidate.find(needle) != std::string::npos ||
                needle.find(candidate) != std::string::npos) {
                try_add(candidate);
            }
            if (suggestions.size() >= max_suggestions) {
                break;
            }
        }
    }

    return suggestions;
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
