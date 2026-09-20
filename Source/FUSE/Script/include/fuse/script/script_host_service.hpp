#pragma once

#include <fuse/script/legacy_script_route.hpp>
#include <fuse/script/script_host.hpp>
#include <fuse/script/script_result.hpp>
#include <fuse/types.hpp>

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

    [[nodiscard]] LegacyScriptDialect last_loaded_dialect() const { return m_lastDialect; }
    [[nodiscard]] usize compat_route_count() const { return m_compatRouteCount; }
    [[nodiscard]] usize fuse_route_count() const { return m_fuseRouteCount; }

private:
    ScriptHostService() = default;

    ScriptHost m_host;
    LegacyScriptDialect m_lastDialect = LegacyScriptDialect::Fuse;
    usize m_compatRouteCount = 0;
    usize m_fuseRouteCount = 0;
};

} // namespace fuse::script
