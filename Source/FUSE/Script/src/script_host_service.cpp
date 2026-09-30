#include <fuse/script/script_host_service.hpp>

#include <fuse/script/script_cook.hpp>

#include "script_lua_compat.hpp"

#include <string>

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
    m_cookedLoadCount = 0;
    m_lastError.clear();
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
        return m_host.load_string(source, chunk_name);
    }

    ++m_fuseRouteCount;
    return m_host.load_string(source, chunk_name);
}

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
namespace {

struct BufferRun {
    const char* data = nullptr;
    usize size = 0;
    const char* chunk_name = nullptr;
    int load_status = LUA_OK;
    std::string* parse_error = nullptr;
};

void run_buffer_fn(lua_State* L, void* user) {
    auto* run = static_cast<BufferRun*>(user);
    run->load_status = luaL_loadbuffer(L, run->data, run->size, run->chunk_name);
    if (run->load_status != LUA_OK) {
        const char* error = lua_tostring(L, -1);
        run->parse_error->assign(error != nullptr ? error : "lua load error");
        return;
    }
    lua_call(L, 0, 0);
}

} // namespace
#endif

ScriptLoadResult ScriptHostService::load_lua_buffer(const void* data, usize size, const char* chunk_name) {
    if (!ensure_initialized()) {
        return {ScriptLoadStatus::BackendUnavailable, "script host service not initialized"};
    }
    if (data == nullptr || size == 0 || chunk_name == nullptr) {
        return {ScriptLoadStatus::InvalidArgument, "empty buffer or chunk name"};
    }
    ++m_fuseRouteCount;
    m_lastDialect = LegacyScriptDialect::Fuse;
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    ScriptVM& vm = m_host.vm();
    if (!vm.has_lua_backend()) {
        return {ScriptLoadStatus::BackendUnavailable, "lua backend unavailable"};
    }
    std::string parse_error;
    BufferRun run;
    run.data = static_cast<const char*>(data);
    run.size = size;
    run.chunk_name = chunk_name;
    run.parse_error = &parse_error;
    const ScriptLoadResult result = vm.run_protected(run_buffer_fn, &run);
    if (run.load_status != LUA_OK) {
        m_lastError = parse_error;
        return {run.load_status == LUA_ERRSYNTAX ? ScriptLoadStatus::ParseError : ScriptLoadStatus::RuntimeError,
                m_lastError.c_str()};
    }
    if (!result.ok()) {
        m_lastError = result.message != nullptr ? result.message : "lua runtime error";
        return {result.status, m_lastError.c_str()};
    }
    return {ScriptLoadStatus::Ok, nullptr};
#else
    return {ScriptLoadStatus::BackendUnavailable, "lua backend unavailable"};
#endif
}

ScriptLoadResult ScriptHostService::load_cooked(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return {ScriptLoadStatus::InvalidArgument, "path is empty"};
    }
    CookedScript cooked;
    if (!load_cooked_script(path, cooked, &m_lastError)) {
        return {ScriptLoadStatus::FileNotFound, m_lastError.c_str()};
    }
    ++m_cookedLoadCount;
    if (cooked.kind == CookedScriptKind::LegacyTorqueScript) {
        const std::string source(cooked.payload.begin(), cooked.payload.end());
        const ScriptLoadResult result = load_chunk(source.c_str(), cooked.chunk_name.c_str());
        if (!result.ok()) {
            m_lastError = result.message != nullptr ? result.message : "legacy chunk failed";
            return {result.status, m_lastError.c_str()};
        }
        return result;
    }
    return load_lua_buffer(cooked.payload.data(), cooked.payload.size(), cooked.chunk_name.c_str());
}

} // namespace fuse::script
