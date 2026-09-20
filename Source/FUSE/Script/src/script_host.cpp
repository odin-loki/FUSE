#include <fuse/script/script_host.hpp>
#include <fuse/script/legacy_script_route.hpp>

#if defined(FUSE_HAS_COMPAT_TS) && FUSE_HAS_COMPAT_TS
#include <fuse/compat/ts.hpp>
#endif

namespace fuse::script {

bool ScriptHost::init() {
    if (m_initialized) {
        return true;
    }

    if (!m_vm.init()) {
        return false;
    }

    m_initialized = true;
    return true;
}

void ScriptHost::shutdown() {
    clear_callbacks();
    m_updateRegistry.clear_scripts();
    m_updateRegistry.clear_errors();
    m_vm.shutdown();
    m_initialized = false;
    m_nextCallbackId = 1;
    m_lastLoadError.clear();
    m_lastCompatOutput.clear();
}

ScriptCallbackId ScriptHost::register_callback(ScriptEventKind kind, ScriptCallbackFn fn) {
    if (!m_initialized || !fn) {
        return kInvalidScriptCallback;
    }

    const ScriptCallbackId id = m_nextCallbackId++;
    ScriptCallbackRegistration registration;
    registration.id = id;
    registration.kind = kind;
    registration.fn = std::move(fn);

    m_callbacks.emplace(id, std::move(registration));
    m_callbackOrder.push_back(id);
    return id;
}

bool ScriptHost::unregister_callback(ScriptCallbackId id) {
    if (id == kInvalidScriptCallback) {
        return false;
    }

    const auto it = m_callbacks.find(id);
    if (it == m_callbacks.end()) {
        return false;
    }

    m_callbacks.erase(it);
    for (auto order_it = m_callbackOrder.begin(); order_it != m_callbackOrder.end(); ++order_it) {
        if (*order_it == id) {
            m_callbackOrder.erase(order_it);
            break;
        }
    }
    return true;
}

void ScriptHost::clear_callbacks() {
    m_callbacks.clear();
    m_callbackOrder.clear();
}

usize ScriptHost::callback_count(ScriptEventKind kind) const {
    usize count = 0;
    for (const ScriptCallbackId id : m_callbackOrder) {
        const auto it = m_callbacks.find(id);
        if (it != m_callbacks.end() && it->second.kind == kind) {
            ++count;
        }
    }
    return count;
}

void ScriptHost::dispatch(ScriptEventKind kind, const ScriptCallbackContext& ctx) {
    if (!m_initialized) {
        return;
    }

    for (const ScriptCallbackId id : m_callbackOrder) {
        const auto it = m_callbacks.find(id);
        if (it == m_callbacks.end() || it->second.kind != kind || !it->second.fn) {
            continue;
        }
        it->second.fn(ctx);
    }
}

void ScriptHost::dispatch_update(f32 dt, ecs::EntityID entity) {
    ScriptCallbackContext ctx;
    ctx.entity = entity;
    ctx.dt = dt;
    dispatch(ScriptEventKind::OnUpdate, ctx);
}

void ScriptHost::tick_update_scripts(f32 dt, ecs::EntityID entity) {
    if (!m_initialized) {
        return;
    }
    m_updateRegistry.tick(dt, entity);
}

ScriptLoadResult ScriptHost::load_string(const char* source, const char* chunk_name) {
    if (!m_initialized) {
        return {ScriptLoadStatus::BackendUnavailable, "script host not initialized"};
    }

    const LegacyChunkRoute route = parse_legacy_chunk_route(chunk_name);
    if (legacy_dialect_is_compat_stub(route.dialect)) {
        if (source == nullptr) {
            m_lastLoadError = "source is null";
            return {ScriptLoadStatus::InvalidArgument, m_lastLoadError.c_str()};
        }

#if defined(FUSE_HAS_COMPAT_TS) && FUSE_HAS_COMPAT_TS
        const fuse::compat::Dialect dialect =
            (route.dialect == LegacyScriptDialect::T3dTorqueScript)
                ? fuse::compat::Dialect::T3d
                : fuse::compat::Dialect::T2d;
        const char* name = (chunk_name != nullptr && chunk_name[0] != '\0') ? chunk_name : "chunk";
        const fuse::compat::ExecResult result = fuse::compat::eval(dialect, source, name);
        if (result.ok) {
            m_lastCompatOutput = result.output;
            m_lastLoadError.clear();
            return {ScriptLoadStatus::Ok, nullptr};
        }
        m_lastLoadError = result.error.empty() ? "compat TorqueScript parse error" : result.error;
        return {ScriptLoadStatus::ParseError, m_lastLoadError.c_str()};
#else
        m_lastLoadError = "compat TorqueScript VM is not linked (FUSE_BUILD_COMPAT_TS=OFF)";
        return {ScriptLoadStatus::BackendUnavailable, m_lastLoadError.c_str()};
#endif
    }

    return m_vm.load_string(source, chunk_name);
}

ScriptLoadResult ScriptHost::load_file(const char* path) {
    if (!m_initialized) {
        return {ScriptLoadStatus::BackendUnavailable, "script host not initialized"};
    }
    return m_vm.load_file(path);
}

} // namespace fuse::script
