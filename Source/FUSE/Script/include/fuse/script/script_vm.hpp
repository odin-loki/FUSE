#pragma once

#include <fuse/script/script_result.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::script {

enum class ScriptBackendKind : u8 { Null, Lua };

/// Null/stub script VM — records loads and reserves a Lua swap-in point for B7.3 follow-up.
class ScriptVM {
public:
    bool init();
    void shutdown();

    [[nodiscard]] bool is_initialized() const { return m_initialized; }
    [[nodiscard]] ScriptBackendKind backend_kind() const { return m_backend; }

    ScriptLoadResult load_string(const char* source, const char* chunk_name = "chunk");
    ScriptLoadResult load_file(const char* path);

    [[nodiscard]] usize loaded_chunk_count() const { return m_loadedChunks.size(); }
    [[nodiscard]] const std::vector<std::string>& loaded_chunks() const { return m_loadedChunks; }

    /// True when `FUSE_SCRIPT_LUA=1` and the VM initialized a Lua state.
    [[nodiscard]] bool has_lua_backend() const;

private:
    bool m_initialized = false;
    ScriptBackendKind m_backend = ScriptBackendKind::Null;
    std::vector<std::string> m_loadedChunks;

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    struct LuaState;
    LuaState* m_lua = nullptr;
#endif
};

} // namespace fuse::script
