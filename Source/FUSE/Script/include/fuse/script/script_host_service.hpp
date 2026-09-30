#pragma once

#include <fuse/script/legacy_script_route.hpp>
#include <fuse/script/script_host.hpp>
#include <fuse/script/script_result.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::script {

/// Game-thread singleton facade toward prestarter U3+ single FUSE script host.
class ScriptHostService {
public:
    static ScriptHostService& instance();

    bool ensure_initialized();
    void shutdown();

    [[nodiscard]] bool is_initialized() const { return m_host.is_initialized(); }
    [[nodiscard]] ScriptHost& host() { return m_host; }
    [[nodiscard]] const ScriptHost& host() const { return m_host; }

    /// Route chunk-name prefixes; t3d/t2d execute on the quarantined Compat VM when linked.
    ScriptLoadResult load_chunk(const char* source, const char* chunk_name);

    /// Run a cooked `.fusescript` (script_cook.hpp): Lua bytecode executes on the FUSE VM; a legacy
    /// TorqueScript payload goes through `load_chunk` under its `t3d:` chunk name (Compat VM route).
    ScriptLoadResult load_cooked(const char* path);
    /// Execute a Lua source or precompiled-bytecode buffer (may contain NULs) on the FUSE VM.
    ScriptLoadResult load_lua_buffer(const void* data, usize size, const char* chunk_name);

    [[nodiscard]] LegacyScriptDialect last_loaded_dialect() const { return m_lastDialect; }
    [[nodiscard]] usize compat_route_count() const { return m_compatRouteCount; }
    [[nodiscard]] usize fuse_route_count() const { return m_fuseRouteCount; }
    [[nodiscard]] usize cooked_load_count() const { return m_cookedLoadCount; }

private:
    ScriptHostService() = default;

    ScriptHost m_host;
    LegacyScriptDialect m_lastDialect = LegacyScriptDialect::Fuse;
    usize m_compatRouteCount = 0;
    usize m_fuseRouteCount = 0;
    usize m_cookedLoadCount = 0;
    std::string m_lastError;
};

} // namespace fuse::script
