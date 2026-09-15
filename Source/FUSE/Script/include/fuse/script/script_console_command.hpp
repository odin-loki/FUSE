#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::script {

class ScriptConsole;

enum class ScriptConsoleCommandStatus : u8 {
    Ok,
    UnknownCommand,
    InvalidArgument,
    BackendUnavailable,
};

enum class ScriptConsoleCommandKind : u8 {
    Unknown,
    BuiltIn,
    Custom,
};

struct ScriptConsoleCommandResult {
    ScriptConsoleCommandStatus status = ScriptConsoleCommandStatus::UnknownCommand;
    std::string output;

    [[nodiscard]] bool ok() const { return status == ScriptConsoleCommandStatus::Ok; }
};

} // namespace fuse::script
