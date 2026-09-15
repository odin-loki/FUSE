#include <fuse/script/script_vm.hpp>

#include <cstring>
#include <filesystem>
#include <string>

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
#endif

namespace fuse::script {

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
struct ScriptVM::LuaState {
    lua_State* state = nullptr;
};
#endif

bool ScriptVM::has_lua_backend() const {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    return m_lua != nullptr && m_lua->state != nullptr;
#else
    return false;
#endif
}

bool ScriptVM::init() {
    if (m_initialized) {
        return true;
    }

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    m_lua = new LuaState();
    m_lua->state = luaL_newstate();
    if (m_lua->state == nullptr) {
        delete m_lua;
        m_lua = nullptr;
        m_backend = ScriptBackendKind::Null;
        return false;
    }

    luaL_openlibs(m_lua->state);
    m_backend = ScriptBackendKind::Lua;
#else
    m_backend = ScriptBackendKind::Null;
#endif

    m_loadedChunks.clear();
    m_initialized = true;
    return true;
}

void ScriptVM::shutdown() {
#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (m_lua != nullptr) {
        if (m_lua->state != nullptr) {
            lua_close(m_lua->state);
            m_lua->state = nullptr;
        }
        delete m_lua;
        m_lua = nullptr;
    }
#endif

    m_loadedChunks.clear();
    m_initialized = false;
    m_backend = ScriptBackendKind::Null;
}

ScriptLoadResult ScriptVM::load_string(const char* source, const char* chunk_name) {
    if (!m_initialized) {
        return {ScriptLoadStatus::BackendUnavailable, "script VM not initialized"};
    }
    if (source == nullptr) {
        return {ScriptLoadStatus::InvalidArgument, "source is null"};
    }

    const char* name = (chunk_name != nullptr && chunk_name[0] != '\0') ? chunk_name : "chunk";

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (has_lua_backend()) {
        const int load_status = luaL_loadbuffer(m_lua->state, source, std::strlen(source), name);
        if (load_status != LUA_OK) {
            const char* error = lua_tostring(m_lua->state, -1);
            lua_pop(m_lua->state, 1);
            return {ScriptLoadStatus::ParseError, error != nullptr ? error : "lua parse error"};
        }

        const int call_status = lua_pcall(m_lua->state, 0, LUA_MULTRET, 0);
        if (call_status != LUA_OK) {
            const char* error = lua_tostring(m_lua->state, -1);
            lua_pop(m_lua->state, 1);
            return {ScriptLoadStatus::ParseError, error != nullptr ? error : "lua runtime error"};
        }

        m_loadedChunks.emplace_back(name);
        return {ScriptLoadStatus::Ok, nullptr};
    }
#endif

    m_loadedChunks.emplace_back(name);
    return {ScriptLoadStatus::Ok, nullptr};
}

ScriptLoadResult ScriptVM::load_file(const char* path) {
    if (!m_initialized) {
        return {ScriptLoadStatus::BackendUnavailable, "script VM not initialized"};
    }
    if (path == nullptr || path[0] == '\0') {
        return {ScriptLoadStatus::InvalidArgument, "path is empty"};
    }

    const std::filesystem::path file_path(path);
    if (!std::filesystem::exists(file_path)) {
        return {ScriptLoadStatus::FileNotFound, path};
    }

#if defined(FUSE_SCRIPT_LUA) && FUSE_SCRIPT_LUA
    if (has_lua_backend()) {
        const int load_status = luaL_loadfile(m_lua->state, path);
        if (load_status != LUA_OK) {
            const char* error = lua_tostring(m_lua->state, -1);
            lua_pop(m_lua->state, 1);
            return {ScriptLoadStatus::ParseError, error != nullptr ? error : "lua parse error"};
        }

        const int call_status = lua_pcall(m_lua->state, 0, LUA_MULTRET, 0);
        if (call_status != LUA_OK) {
            const char* error = lua_tostring(m_lua->state, -1);
            lua_pop(m_lua->state, 1);
            return {ScriptLoadStatus::ParseError, error != nullptr ? error : "lua runtime error"};
        }

        m_loadedChunks.emplace_back(file_path.filename().string());
        return {ScriptLoadStatus::Ok, nullptr};
    }
#endif

    m_loadedChunks.emplace_back(file_path.filename().string());
    return {ScriptLoadStatus::Ok, nullptr};
}

} // namespace fuse::script
