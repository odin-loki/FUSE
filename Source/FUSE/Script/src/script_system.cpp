#include <fuse/script/script_system.hpp>

#include <fuse/ecs/components/script.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/script/script_runtime.hpp>

namespace fuse::script {

namespace {

/// Identity of the behaviour a component asks for: module path bytes + module asset id.
u64 identity_of(const ecs::Script& script) {
    u64 hash = 14695981039346656037ull;
    for (usize i = 0; i < ecs::Script::kPathCapacity && script.script_path[i] != '\0'; ++i) {
        hash ^= static_cast<u8>(script.script_path[i]);
        hash *= 1099511628211ull;
    }
    for (u32 shift = 0; shift < 64u; shift += 8u) {
        hash ^= static_cast<u8>(script.module.value >> shift);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool wants_behaviour(const ecs::Script& script) {
    return script.enabled && (script.script_path[0] != '\0' || script.module.valid());
}

} // namespace

ScriptSystem::~ScriptSystem() { shutdown(); }

bool ScriptSystem::init(ecs::Registry& registry, ScriptRuntime& runtime, usize expected_entities) {
    if (m_registry != nullptr) {
        return m_registry == &registry && m_runtime == &runtime;
    }
    if (!runtime.is_initialized()) {
        m_lastError = "script runtime is not initialized";
        return false;
    }
    m_registry = &registry;
    m_runtime = &runtime;
    m_tracked.reserve(expected_entities);
    m_index.reserve(expected_entities);
    m_toAttach.reserve(expected_entities);
    m_modulePath.reserve(ecs::Script::kPathCapacity);
    m_stats = {};
    m_lastError.clear();
    return true;
}

void ScriptSystem::shutdown() {
    if (m_registry == nullptr) {
        return;
    }
    exitPlay();
    m_registry = nullptr;
    m_runtime = nullptr;
    m_resolver = nullptr;
    m_resolverUser = nullptr;
}

void ScriptSystem::set_module_resolver(ScriptModuleResolver resolver, void* user) {
    m_resolver = resolver;
    m_resolverUser = user;
}

void ScriptSystem::enterPlay() {
    if (m_registry == nullptr) {
        return;
    }
    if (m_playing) {
        // Re-entering play retries behaviours whose load failed (e.g. a fixed script file).
        for (usize i = m_tracked.size(); i-- > 0;) {
            if (!m_tracked[i].attached) {
                release_tracked(i);
            }
        }
    }
    m_playing = true;
    sync();
}

void ScriptSystem::exitPlay() {
    detach_all();
    m_playing = false;
}

const ScriptSystem::Tracked* ScriptSystem::find_tracked(ecs::EntityID entity) const {
    const auto it = m_index.find(key_of(entity));
    return it != m_index.end() ? &m_tracked[it->second] : nullptr;
}

bool ScriptSystem::is_attached(ecs::EntityID entity) const {
    const Tracked* tracked = find_tracked(entity);
    return tracked != nullptr && tracked->attached;
}

bool ScriptSystem::resolve_module(const ecs::Script& script, std::string& out_path) {
    if (script.script_path[0] != '\0') {
        out_path.assign(script.path());
        return true;
    }
    if (script.module.valid() && m_resolver != nullptr && m_resolver(m_resolverUser, script.module, out_path) &&
        !out_path.empty()) {
        return true;
    }
    m_lastError = "unresolved script module asset " + std::to_string(script.module.value);
    return false;
}

void ScriptSystem::release_tracked(usize index) {
    const Tracked tracked = m_tracked[index];
    // Unlink first: on_destroy may run script code that re-enters the registry.
    m_index.erase(key_of(tracked.entity));
    if (index + 1u != m_tracked.size()) {
        m_tracked[index] = m_tracked.back();
        m_index[key_of(m_tracked[index].entity)] = static_cast<u32>(index);
    }
    m_tracked.pop_back();

    if (tracked.attached) {
        --m_attachedCount;
        if (tracked.pending_start) {
            --m_pendingStarts;
        }
        ++m_stats.detaches;
        m_runtime->detach(tracked.entity);
    }
    if (ecs::Script* script = m_registry->get<ecs::Script>(tracked.entity)) {
        script->started = false;
        script->lua_ref = ecs::Script::kNoRef;
    }
}

void ScriptSystem::detach_all() {
    while (!m_tracked.empty()) {
        release_tracked(m_tracked.size() - 1u);
    }
    m_pendingStarts = 0;
}

void ScriptSystem::attach_entity(ecs::EntityID entity) {
    const ecs::Script* script = m_registry->get<ecs::Script>(entity);
    if (script == nullptr || !m_registry->alive(entity) || find_tracked(entity) != nullptr) {
        return;
    }
    Tracked tracked;
    tracked.entity = entity;
    tracked.identity = identity_of(*script);
    tracked.stamp = m_stamp;

    if (resolve_module(*script, m_modulePath)) {
        bool loaded = m_runtime->has_module(m_modulePath.c_str());
        if (!loaded) {
            // Running the chunk may touch the registry: `script` is not used past this point.
            const ScriptLoadResult result = m_runtime->load_module_file(m_modulePath.c_str());
            loaded = result.ok();
            if (!loaded) {
                m_lastError = result.message != nullptr ? result.message : "script module load failed";
            }
        }
        if (loaded && m_registry->alive(entity)) {
            if (m_runtime->attach(entity, m_modulePath.c_str())) {
                tracked.attached = true;
            } else {
                m_lastError = "cannot attach module " + m_modulePath + " (already attached or not loaded)";
            }
        }
    }

    ecs::Script* live = m_registry->get<ecs::Script>(entity);
    if (tracked.attached && live != nullptr) {
        const u32 count = live->property_count < ecs::Script::kMaxProperties
                              ? live->property_count
                              : static_cast<u32>(ecs::Script::kMaxProperties);
        for (u32 i = 0; i < count; ++i) {
            if (live->properties[i].type != ecs::ScriptProperty::Type::None) {
                (void)m_runtime->set_instance_property(entity, live->properties[i]);
            }
        }
        live->lua_ref = m_runtime->instance_ref(entity);
        live->started = false;
        tracked.pending_start = true;
        ++m_pendingStarts;
        ++m_attachedCount;
        ++m_stats.attaches;
    } else {
        ++m_stats.load_failures;
    }
    m_index[key_of(entity)] = static_cast<u32>(m_tracked.size());
    m_tracked.push_back(tracked);
}

void ScriptSystem::sync() {
    if (!m_playing || m_registry == nullptr || m_runtime == nullptr) {
        return;
    }
    ++m_stats.syncs;
    if (++m_stamp == 0u) {
        m_stamp = 1u;
    }
    m_toAttach.clear();
    m_registry->each<ecs::Script>([this](ecs::EntityID entity, ecs::Script& script) {
        if (!wants_behaviour(script)) {
            return; // not stamped: a tracked behaviour is released below
        }
        const auto it = m_index.find(key_of(entity));
        if (it != m_index.end()) {
            Tracked& tracked = m_tracked[it->second];
            if (tracked.identity == identity_of(script)) {
                tracked.stamp = m_stamp;
                return;
            }
            // Module changed: released below (not stamped), then attached again.
        }
        m_toAttach.push_back(entity);
    });
    // Component removed or disabled, module changed, or entity destroyed (dead entities are not
    // visited by `each`).
    for (usize i = m_tracked.size(); i-- > 0;) {
        if (m_tracked[i].stamp != m_stamp) {
            release_tracked(i);
        }
    }
    for (const ecs::EntityID entity : m_toAttach) {
        attach_entity(entity);
    }
}

void ScriptSystem::update(f32 dt) {
    if (!m_playing || m_registry == nullptr) {
        return;
    }
    sync();
    m_runtime->update(dt);
    if (m_pendingStarts == 0u) {
        return;
    }
    for (Tracked& tracked : m_tracked) {
        if (!tracked.pending_start) {
            continue;
        }
        tracked.pending_start = false;
        --m_pendingStarts;
        if (ecs::Script* script = m_registry->get<ecs::Script>(tracked.entity)) {
            script->started = m_runtime->instance_started(tracked.entity);
        }
    }
}

void ScriptSystem::on_scene_loaded() {
    detach_all();
    sync();
}

ecs::RegistrySerialiseResult ScriptSystem::load_scene(const std::string& path) {
    ecs::RegistrySerialiseResult result;
    if (m_registry == nullptr) {
        result.error = "script system not initialized";
        return result;
    }
    // Destroy the old scene's behaviours while their entities still exist.
    detach_all();
    result = ecs::RegistrySerialiser::load(path, *m_registry);
    sync();
    return result;
}

void ScriptSystem::destroy_entity(ecs::EntityID entity) {
    if (m_registry == nullptr) {
        return;
    }
    const auto it = m_index.find(key_of(entity));
    if (it != m_index.end()) {
        release_tracked(it->second);
    }
    m_registry->destroy_entity(entity);
}

bool ScriptSystem::dispatch_collision(ecs::EntityID entity, ecs::EntityID other, const ecs::vec3& point) {
    if (!m_playing || !is_attached(entity)) {
        return false;
    }
    m_runtime->dispatch_collision(entity, other, point);
    return true;
}

bool ScriptSystem::dispatch_trigger_enter(ecs::EntityID entity, ecs::EntityID other) {
    if (!m_playing || !is_attached(entity)) {
        return false;
    }
    m_runtime->dispatch_trigger_enter(entity, other);
    return true;
}

usize ScriptSystem::dispatch_contacts(std::span<const ScriptContactEvent> events) {
    usize dispatched = 0;
    for (const ScriptContactEvent& event : events) {
        if (event.kind == ScriptContactEvent::Kind::Collision) {
            dispatched += dispatch_collision(event.a, event.b, event.point) ? 1u : 0u;
            dispatched += dispatch_collision(event.b, event.a, event.point) ? 1u : 0u;
        } else {
            dispatched += dispatch_trigger_enter(event.a, event.b) ? 1u : 0u;
            dispatched += dispatch_trigger_enter(event.b, event.a) ? 1u : 0u;
        }
    }
    return dispatched;
}

} // namespace fuse::script
