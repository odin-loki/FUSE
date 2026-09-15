#pragma once

#include <fuse/script/script_console_command.hpp>
#include <fuse/script/script_console_command_registry.hpp>
#include <fuse/script/script_console_history.hpp>
#include <fuse/script/script_result.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::script {

class ScriptHost;

/// Headless script REPL — built-in command stubs, history buffer, dispatch to `ScriptHost`.
class ScriptConsole {
public:
    static constexpr u32 kDefaultHistoryCapacity = ScriptConsoleHistoryBuffer::kDefaultCapacity;

    ScriptConsole();

    void attach(ScriptHost* host);
    void detach();

    [[nodiscard]] ScriptHost* host() const { return m_host; }

    ScriptConsoleCommandResult execute(const char* line);

    void setHistoryCapacity(u32 capacity);
    [[nodiscard]] u32 historyCapacity() const { return m_history.capacity(); }
    [[nodiscard]] u32 historyCount() const { return m_history.count(); }
    [[nodiscard]] const std::string& historyAt(u32 index) const { return m_history.at(index); }

    /// Navigate command history (`previous=true` recalls older entries).
    [[nodiscard]] const std::string& recallHistory(bool previous) { return m_history.recall(previous); }
    void resetHistoryNavigation() { m_history.resetNavigation(); }
    [[nodiscard]] s32 historyNavigationCursor() const { return m_history.navigationCursor(); }

    [[nodiscard]] const std::vector<std::string>& outputLines() const { return m_output; }
    void clearOutput();

    bool register_command(const char* name, ScriptConsoleCommandRegistry::CommandHandler handler);
    bool unregister_command(const char* name);
    [[nodiscard]] bool has_command(const char* name) const { return m_commands.has_command(name); }
    [[nodiscard]] bool is_built_in_command(const char* name) const { return m_commands.is_built_in(name); }
    [[nodiscard]] bool is_custom_command(const char* name) const { return m_commands.is_custom(name); }
    [[nodiscard]] ScriptConsoleCommandKind command_kind(const char* name) const {
        return m_commands.lookup_kind(name);
    }
    [[nodiscard]] usize built_in_command_count() const { return m_commands.built_in_count(); }
    [[nodiscard]] usize custom_command_count() const { return m_commands.custom_count(); }

private:
    ScriptConsoleCommandResult dispatch_(const char* command, const char* args);
    void appendOutput_(const std::string& text);
    void registerBuiltIns_();

    ScriptHost* m_host = nullptr;
    ScriptConsoleHistoryBuffer m_history;
    ScriptConsoleCommandRegistry m_commands;
    std::vector<std::string> m_output;
};

} // namespace fuse::script
