#pragma once

#include <fuse/script/script_callback.hpp>
#include <fuse/script/script_result.hpp>
#include <fuse/script/script_vm.hpp>
#include <fuse/types.hpp>

#include <unordered_map>
#include <vector>

namespace fuse::script {

struct ScriptCallbackRegistration {
    ScriptCallbackId id = kInvalidScriptCallback;
    ScriptEventKind kind = ScriptEventKind::OnStart;
    ScriptCallbackFn fn;
};

/// Game-thread script host facade — callback registry + VM lifecycle (Lua-ready stub backend).
class ScriptHost {
public:
    bool init();
    void shutdown();

    [[nodiscard]] bool is_initialized() const { return m_initialized; }

    ScriptVM& vm() { return m_vm; }
    const ScriptVM& vm() const { return m_vm; }

    ScriptCallbackId register_callback(ScriptEventKind kind, ScriptCallbackFn fn);
    bool unregister_callback(ScriptCallbackId id);
    void clear_callbacks();

    [[nodiscard]] usize callback_count(ScriptEventKind kind) const;
    [[nodiscard]] usize callback_count() const { return m_callbacks.size(); }

    void dispatch(ScriptEventKind kind, const ScriptCallbackContext& ctx);

    /// Convenience for per-frame `OnUpdate` handlers — sets `ctx.dt` and `ctx.entity`.
    void dispatch_update(f32 dt, ecs::EntityID entity = ecs::EntityID::null());

    ScriptLoadResult load_string(const char* source, const char* chunk_name = "chunk");
    ScriptLoadResult load_file(const char* path);

private:
    bool m_initialized = false;
    ScriptVM m_vm;
    ScriptCallbackId m_nextCallbackId = 1;
    std::unordered_map<ScriptCallbackId, ScriptCallbackRegistration> m_callbacks;
    std::vector<ScriptCallbackId> m_callbackOrder;
};

} // namespace fuse::script
