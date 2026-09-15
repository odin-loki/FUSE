#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/script/script_callback.hpp>
#include <fuse/types.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::script {

using ScriptInstanceId = u32;
static constexpr ScriptInstanceId kInvalidScriptInstance = 0;

/// Per-script OnUpdate registry — registration order tick, per-instance dt accumulation,
/// enable/disable, and error isolation (one failing script does not stop others).
class ScriptUpdateRegistry {
public:
    ScriptInstanceId register_script(const char* name, ScriptCallbackFn on_update);
    bool unregister_script(ScriptInstanceId id);
    void clear_scripts();

    void set_enabled(ScriptInstanceId id, bool enabled);
    [[nodiscard]] bool is_enabled(ScriptInstanceId id) const;
    [[nodiscard]] f64 accumulated_dt(ScriptInstanceId id) const;
    [[nodiscard]] const char* script_name(ScriptInstanceId id) const;
    [[nodiscard]] usize script_count() const { return m_scripts.size(); }

    /// Tick enabled scripts in registration order; accumulates `dt` per instance.
    void tick(f32 dt, ecs::EntityID entity = ecs::EntityID::null());

    [[nodiscard]] usize error_count() const { return m_errorCount; }
    [[nodiscard]] const char* last_error() const { return m_lastError.c_str(); }
    void clear_errors();

private:
    struct ScriptInstance {
        ScriptInstanceId id = kInvalidScriptInstance;
        std::string name;
        bool enabled = true;
        f64 accumulated_dt = 0.0;
        ScriptCallbackFn on_update;
    };

    ScriptInstanceId m_nextId = 1;
    std::unordered_map<ScriptInstanceId, ScriptInstance> m_scripts;
    std::vector<ScriptInstanceId> m_scriptOrder;
    usize m_errorCount = 0;
    std::string m_lastError;
};

} // namespace fuse::script
