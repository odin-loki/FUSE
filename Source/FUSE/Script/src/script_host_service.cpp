#include <fuse/script/script_host_service.hpp>

namespace fuse::script {

ScriptHostService& ScriptHostService::instance() {
    static ScriptHostService service;
    return service;
}

bool ScriptHostService::ensure_initialized() {
    if (m_host.is_initialized()) {
        return true;
    }
    return m_host.init();
}

void ScriptHostService::shutdown() {
    m_host.shutdown();
    m_lastDialect = LegacyScriptDialect::Fuse;
    m_compatRouteCount = 0;
    m_fuseRouteCount = 0;
}

ScriptLoadResult ScriptHostService::load_chunk(const char* source, const char* chunk_name) {
    if (!ensure_initialized()) {
        return {ScriptLoadStatus::BackendUnavailable, "script host service not initialized"};
    }
    if (source == nullptr) {
        return {ScriptLoadStatus::InvalidArgument, "source is null"};
    }

    const LegacyChunkRoute route = parse_legacy_chunk_route(chunk_name);
    m_lastDialect = route.dialect;

    if (legacy_dialect_is_compat_stub(route.dialect)) {
        ++m_compatRouteCount;
        return {ScriptLoadStatus::Ok, nullptr};
    }

    ++m_fuseRouteCount;
    return m_host.load_string(source, chunk_name);
}

} // namespace fuse::script
