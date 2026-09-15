#pragma once

#include <fuse/script/script_result.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::script {

class ScriptHost;

enum class ScriptConsoleCommandStatus : u8 {
    Ok,
    UnknownCommand,
    InvalidArgument,
    BackendUnavailable,
};

struct ScriptConsoleCommandResult {
    ScriptConsoleCommandStatus status = ScriptConsoleCommandStatus::UnknownCommand;
    std::string output;

    [[nodiscard]] bool ok() const { return status == ScriptConsoleCommandStatus::Ok; }
};

/// Headless script REPL — built-in command stubs, history buffer, dispatch to `ScriptHost`.
class ScriptConsole {
public:
    static constexpr u32 kDefaultHistoryCapacity = 64;

    ScriptConsole();

    void attach(ScriptHost* host);
    void detach();

    [[nodiscard]] ScriptHost* host() const { return m_host; }

    ScriptConsoleCommandResult execute(const char* line);

    void setHistoryCapacity(u32 capacity);
    [[nodiscard]] u32 historyCapacity() const { return m_historyCapacity; }
    [[nodiscard]] u32 historyCount() const { return static_cast<u32>(m_history.size()); }
    [[nodiscard]] const std::string& historyAt(u32 index) const;

    /// Navigate command history (`previous=true` recalls older entries).
    [[nodiscard]] const std::string& recallHistory(bool previous);
    void resetHistoryNavigation();
    [[nodiscard]] s32 historyNavigationCursor() const { return m_historyCursor; }

    [[nodiscard]] const std::vector<std::string>& outputLines() const { return m_output; }
    void clearOutput();

    using CommandHandler = std::function<ScriptConsoleCommandResult(ScriptConsole&, const char* args)>;

    bool register_command(const char* name, CommandHandler handler);
    bool unregister_command(const char* name);
    [[nodiscard]] usize built_in_command_count() const { return m_builtInHandlers.size(); }
    [[nodiscard]] usize custom_command_count() const { return m_customHandlers.size(); }

private:
    ScriptConsoleCommandResult dispatch_(const char* command, const char* args);
    void pushHistory_(const std::string& line);
    void appendOutput_(const std::string& text);
    void registerBuiltIns_();
    [[nodiscard]] std::string formatCommandList_() const;

    ScriptHost* m_host = nullptr;
    std::vector<std::string> m_history;
    u32 m_historyCapacity = kDefaultHistoryCapacity;
    s32 m_historyCursor = -1;
    std::vector<std::string> m_output;
    std::unordered_map<std::string, CommandHandler> m_builtInHandlers;
    std::unordered_map<std::string, CommandHandler> m_customHandlers;
};

} // namespace fuse::script
