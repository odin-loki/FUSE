#pragma once

#include <fuse/script/script_console_command.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::script {

/// Named command registry for `ScriptConsole` — built-in stubs plus custom handlers.
class ScriptConsoleCommandRegistry {
public:
    using CommandHandler = std::function<ScriptConsoleCommandResult(ScriptConsole&, const char* args)>;

    bool register_built_in(const char* name, CommandHandler handler);
    bool register_custom(const char* name, CommandHandler handler);
    bool unregister_custom(const char* name);

    [[nodiscard]] ScriptConsoleCommandResult dispatch(const char* name, ScriptConsole& console,
                                                      const char* args) const;

    [[nodiscard]] usize built_in_count() const { return m_builtInHandlers.size(); }
    [[nodiscard]] usize custom_count() const { return m_customHandlers.size(); }
    [[nodiscard]] std::vector<std::string> command_names() const;
    [[nodiscard]] std::string formatCommandList() const;
    [[nodiscard]] std::string formatCommandLines() const;

private:
    std::unordered_map<std::string, CommandHandler> m_builtInHandlers;
    std::unordered_map<std::string, CommandHandler> m_customHandlers;
};

} // namespace fuse::script
