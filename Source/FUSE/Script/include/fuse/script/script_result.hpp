#pragma once

#include <fuse/types.hpp>

namespace fuse::script {

enum class ScriptLoadStatus : u8 {
    Ok,
    FileNotFound,
    ParseError,
    BackendUnavailable,
    InvalidArgument,
    /// Chunk or callback raised a Lua error (includes instruction-budget and memory-limit aborts).
    RuntimeError,
};

struct ScriptLoadResult {
    ScriptLoadStatus status = ScriptLoadStatus::BackendUnavailable;
    const char* message = nullptr;

    [[nodiscard]] bool ok() const { return status == ScriptLoadStatus::Ok; }
};

} // namespace fuse::script
