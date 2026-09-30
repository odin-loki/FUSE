#pragma once

// Script cook (MP-B7.3-SCRIPT-COMPONENT / UNI-U3-SCRIPT-1): `CookAssetKind::Script` in fuse_project
// calls `cook_script_file`. Lua sources are syntax-checked with the vendored Lua compiler
// (luaL_loadbuffer) and written as precompiled bytecode (lua_dump) in a `.fusescript` container;
// legacy TorqueScript sources (`.cs`, `.tscript`) are tagged as `t3d:` chunks and passed through
// unchanged for the quarantined Compat VM (ScriptHostService routes them by that prefix).
//
// `.fusescript` layout (little-endian host order, like every FUSE cooked format):
//   u32 magic 'FSCR' | u32 version (1) | u8 kind | u8[3] reserved (0)
//   u32 chunk-name bytes | u32 payload bytes | u64 FNV-1a 64 of the payload
//   chunk name (no NUL) | payload
// Lua bytecode is specific to the Lua version and the platform's number/size_t layout: cook per
// target platform. Bytecode is trusted build output (Lua does not verify it), so the payload hash
// guards against truncation / corruption before a VM ever sees it.
//
// fuse_script_cook links only fuse_core and Lua (no ECS, no Compat VMs), so the offline cooker can
// use it without pulling the runtime script host in.

#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fuse::script {

inline constexpr u32 kCookedScriptMagic = 0x52435346u; // 'FSCR'
inline constexpr u32 kCookedScriptVersion = 1u;
inline constexpr const char* kCookedScriptExtension = ".fusescript";

enum class CookedScriptKind : u8 {
    LuaBytecode = 1,        ///< payload = lua_dump of the compiled chunk
    LegacyTorqueScript = 2, ///< payload = original TorqueScript text; chunk name carries `t3d:`
};

enum class ScriptCookFailure : u8 {
    None,
    InvalidArgument,
    SourceUnreadable,
    SyntaxError,         ///< Lua source failed to compile; `message` holds file:line: reason
    CompilerUnavailable, ///< built without a Lua backend (FUSE_SCRIPT_ENABLE_LUA=OFF)
    WriteFailed,
};

struct ScriptCookResult {
    bool ok = false;
    ScriptCookFailure failure = ScriptCookFailure::InvalidArgument;
    CookedScriptKind kind = CookedScriptKind::LuaBytecode;
    u32 byte_count = 0; ///< bytes written to the output file
    std::string chunk_name;
    std::string message;
};

struct CookedScript {
    CookedScriptKind kind = CookedScriptKind::LuaBytecode;
    std::string chunk_name;
    std::vector<u8> payload;
};

/// True when this build can compile Lua (the cook needs it for `.lua` sources).
[[nodiscard]] bool script_compiler_available();

/// Compile `source` without running it. On success `bytecode` holds the lua_dump output; on a
/// syntax error `failure == SyntaxError` and `message` is "Lua syntax error: <chunk>:<line>: <reason>".
ScriptCookResult compile_lua_source(std::string_view source, const char* chunk_name, std::vector<u8>& bytecode);

/// `.cs` / `.tscript` (case-insensitive): legacy TorqueScript, passed through as a `t3d:` chunk.
[[nodiscard]] bool is_legacy_script_path(std::string_view path);
/// Ends with `.fusescript` (case-insensitive).
[[nodiscard]] bool is_cooked_script_path(std::string_view path);

/// Cook one script source to `output_path` (.fusescript). Writes nothing on failure.
ScriptCookResult cook_script_file(const std::string& input_path, const std::string& output_path);

/// Serialise / parse the container (validates magic, version, kind, sizes and payload hash).
[[nodiscard]] std::vector<u8> encode_cooked_script(const CookedScript& script);
bool decode_cooked_script(const u8* data, usize size, CookedScript& out, std::string* error = nullptr);
bool load_cooked_script(const std::string& path, CookedScript& out, std::string* error = nullptr);

} // namespace fuse::script
