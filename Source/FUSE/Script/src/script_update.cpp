#include <fuse/script/script_update.hpp>

#include <exception>
#include <utility>

namespace fuse::script {

ScriptInstanceId ScriptUpdateRegistry::register_script(const char* name, ScriptCallbackFn on_update) {
    if (!on_update) {
        return kInvalidScriptInstance;
    }

    ScriptInstance instance;
    instance.id = m_nextId++;
    instance.name = (name != nullptr) ? name : "";
    instance.on_update = std::move(on_update);

    const ScriptInstanceId id = instance.id;
    m_scripts.emplace(id, std::move(instance));
    m_scriptOrder.push_back(id);
    return id;
}

bool ScriptUpdateRegistry::unregister_script(ScriptInstanceId id) {
    if (id == kInvalidScriptInstance) {
        return false;
    }

    const auto it = m_scripts.find(id);
    if (it == m_scripts.end()) {
        return false;
    }

    m_scripts.erase(it);
    for (auto order_it = m_scriptOrder.begin(); order_it != m_scriptOrder.end(); ++order_it) {
        if (*order_it == id) {
            m_scriptOrder.erase(order_it);
            break;
        }
    }
    return true;
}

void ScriptUpdateRegistry::clear_scripts() {
    m_scripts.clear();
    m_scriptOrder.clear();
}

void ScriptUpdateRegistry::set_enabled(ScriptInstanceId id, bool enabled) {
    const auto it = m_scripts.find(id);
    if (it != m_scripts.end()) {
        it->second.enabled = enabled;
    }
}

bool ScriptUpdateRegistry::is_enabled(ScriptInstanceId id) const {
    const auto it = m_scripts.find(id);
    return it != m_scripts.end() && it->second.enabled;
}

f64 ScriptUpdateRegistry::accumulated_dt(ScriptInstanceId id) const {
    const auto it = m_scripts.find(id);
    return (it != m_scripts.end()) ? it->second.accumulated_dt : 0.0;
}

const char* ScriptUpdateRegistry::script_name(ScriptInstanceId id) const {
    const auto it = m_scripts.find(id);
    return (it != m_scripts.end()) ? it->second.name.c_str() : "";
}

void ScriptUpdateRegistry::tick(f32 dt, ecs::EntityID entity) {
    ScriptCallbackContext ctx;
    ctx.entity = entity;
    ctx.dt = dt;

    for (const ScriptInstanceId id : m_scriptOrder) {
        const auto it = m_scripts.find(id);
        if (it == m_scripts.end() || !it->second.enabled || !it->second.on_update) {
            continue;
        }

        ScriptInstance& instance = it->second;
        instance.accumulated_dt += static_cast<f64>(dt);

        try {
            instance.on_update(ctx);
        } catch (const std::exception& ex) {
            ++m_errorCount;
            m_lastError = instance.name.empty() ? ex.what() : (instance.name + ": " + ex.what());
        } catch (...) {
            ++m_errorCount;
            m_lastError = instance.name.empty() ? "unknown script error" : (instance.name + ": unknown error");
        }
    }
}

void ScriptUpdateRegistry::clear_errors() {
    m_errorCount = 0;
    m_lastError.clear();
}

} // namespace fuse::script
