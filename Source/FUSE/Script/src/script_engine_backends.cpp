// MP-B7.3-LUA-API: InputScriptBackend (Input.*) and RegistrySceneBackend (Scene.*).

#include <fuse/script/script_engine_backends.hpp>

#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/ecs/systems/culling_system.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/platform/action_map.hpp>
#include <fuse/platform/gamepad.hpp>
#include <fuse/platform/input.hpp>
#include <fuse/spatial/bvh.hpp>

#include <algorithm>

namespace fuse::script {

// ---- Input ------------------------------------------------------------------------------------

InputScriptBackend::InputScriptBackend(const platform::PlayerController& player)
    : m_input(&player.input()), m_actions(&player.actions()), m_gamepads(&player.gamepads()) {}

bool InputScriptBackend::key_pressed(platform::Key key) const { return m_input != nullptr && m_input->keyPressed(key); }
bool InputScriptBackend::key_held(platform::Key key) const { return m_input != nullptr && m_input->keyDown(key); }
bool InputScriptBackend::key_released(platform::Key key) const {
    return m_input != nullptr && m_input->keyReleased(key);
}

bool InputScriptBackend::mouse_pressed(platform::MouseButton button) const {
    return m_input != nullptr && m_input->mousePressed(button);
}
bool InputScriptBackend::mouse_held(platform::MouseButton button) const {
    return m_input != nullptr && m_input->mouseDown(button);
}
bool InputScriptBackend::mouse_released(platform::MouseButton button) const {
    return m_input != nullptr && m_input->mouseReleased(button);
}

void InputScriptBackend::mouse_delta(f32& dx, f32& dy) const {
    dx = m_input != nullptr ? static_cast<f32>(m_input->mouseDeltaX()) : 0.f;
    dy = m_input != nullptr ? static_cast<f32>(m_input->mouseDeltaY()) : 0.f;
}

void InputScriptBackend::mouse_position(f32& x, f32& y) const {
    x = m_input != nullptr ? static_cast<f32>(m_input->mouseX()) : 0.f;
    y = m_input != nullptr ? static_cast<f32>(m_input->mouseY()) : 0.f;
}

bool InputScriptBackend::has_action(std::string_view action) const {
    return m_actions != nullptr && m_actions->find(action) != platform::ActionMap::kInvalidAction;
}
bool InputScriptBackend::action_pressed(std::string_view action) const {
    return m_actions != nullptr && m_actions->pressed(action);
}
bool InputScriptBackend::action_held(std::string_view action) const {
    return m_actions != nullptr && m_actions->held(action);
}
bool InputScriptBackend::action_released(std::string_view action) const {
    return m_actions != nullptr && m_actions->released(action);
}
f32 InputScriptBackend::action_axis(std::string_view action) const {
    return m_actions != nullptr ? m_actions->axis(action) : 0.f;
}
bool InputScriptBackend::gamepad_connected(u32 pad) const {
    return m_gamepads != nullptr && m_gamepads->connected(pad);
}

// ---- Scene ------------------------------------------------------------------------------------

bool RegistrySceneBackend::set_name(ecs::EntityID entity, std::string_view name) {
    if (!m_registry.alive(entity) || name.empty()) {
        return false;
    }
    clear_name(entity);
    std::string key(name);
    // The name moves to this entity; the previous holder (if any) loses it.
    if (const auto it = m_byName.find(key); it != m_byName.end()) {
        m_names.erase(it->second.index);
    }
    m_byName[key] = entity;
    m_names[entity.index] = std::move(key);
    return true;
}

void RegistrySceneBackend::clear_name(ecs::EntityID entity) {
    const auto it = m_names.find(entity.index);
    if (it == m_names.end()) {
        return;
    }
    const auto byName = m_byName.find(it->second);
    if (byName != m_byName.end() && byName->second.index == entity.index) {
        m_byName.erase(byName);
    }
    m_names.erase(it);
}

std::string RegistrySceneBackend::name_of(ecs::EntityID entity) const {
    const auto it = m_names.find(entity.index);
    if (it == m_names.end() || !m_registry.alive(entity)) {
        return {};
    }
    const auto byName = m_byName.find(it->second);
    return byName != m_byName.end() && byName->second == entity ? it->second : std::string();
}

ecs::EntityID RegistrySceneBackend::find_entity(std::string_view name) {
    const auto it = m_byName.find(std::string(name));
    if (it == m_byName.end()) {
        return ecs::EntityID::null();
    }
    if (!m_registry.alive(it->second)) {
        // Destroyed since it was named: forget it (and never hand out the stale handle).
        m_names.erase(it->second.index);
        m_byName.erase(it);
        return ecs::EntityID::null();
    }
    return it->second;
}

namespace {

/// World matrix of a transform even before TransformSystem ran (dirty roots are recomputed on a
/// copy; dirty children keep their last matrix).
ecs::Transform world_ready(const ecs::Transform& t) {
    ecs::Transform copy = t;
    if (copy.dirty && ecs::TransformSystem::is_root_transform(copy)) {
        ecs::TransformSystem::recompute_world_matrix(copy, ecs::mat4::identity());
    }
    return copy;
}

bool sphere_hits_box(const ecs::vec3& c, f32 radius, const spatial::AABB& box) {
    const auto axis = [](f32 v, f32 lo, f32 hi) {
        const f32 d = v < lo ? lo - v : (v > hi ? v - hi : 0.f);
        return d * d;
    };
    const f32 d2 = axis(c.x, box.min.x, box.max.x) + axis(c.y, box.min.y, box.max.y) + axis(c.z, box.min.z, box.max.z);
    return d2 <= radius * radius;
}

} // namespace

usize RegistrySceneBackend::query_sphere(const ecs::vec3& center, f32 radius, std::vector<ecs::EntityID>& out) {
    if (!(radius >= 0.f)) {
        return 0;
    }
    const usize before = out.size();
    std::vector<ecs::EntityID> hits;
    if (m_bvh != nullptr) {
        std::vector<spatial::BVHLeaf> leaves;
        m_bvh->query_sphere(center, radius, leaves);
        for (const spatial::BVHLeaf& leaf : leaves) {
            if (m_registry.alive(leaf.entity) && sphere_hits_box(center, radius, leaf.aabb)) {
                hits.push_back(leaf.entity);
            }
        }
    }
    m_registry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform& t) {
        const ecs::Mesh* mesh = m_registry.get<ecs::Mesh>(id);
        const ecs::SDFObject* sdf = m_registry.get<ecs::SDFObject>(id);
        if (m_bvh != nullptr && (mesh != nullptr || sdf != nullptr)) {
            return; // bounded entities come from the BVH
        }
        const ecs::Transform w = world_ready(t);
        bool hit = false;
        if (sdf != nullptr) {
            hit = sphere_hits_box(center, radius, ecs::CullingSystem::world_bounds(w, *sdf));
        }
        if (!hit && mesh != nullptr) {
            hit = sphere_hits_box(center, radius, ecs::CullingSystem::world_bounds(w, *mesh));
        }
        if (!hit && mesh == nullptr && sdf == nullptr) {
            const f32 dx = w.local_to_world.data[12] - center.x;
            const f32 dy = w.local_to_world.data[13] - center.y;
            const f32 dz = w.local_to_world.data[14] - center.z;
            hit = dx * dx + dy * dy + dz * dz <= radius * radius;
        }
        if (hit) {
            hits.push_back(id);
        }
    });
    std::sort(hits.begin(), hits.end(), [](const ecs::EntityID& a, const ecs::EntityID& b) { return a.index < b.index; });
    hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
    out.insert(out.end(), hits.begin(), hits.end());
    return out.size() - before;
}

bool RegistrySceneBackend::set_active_camera(ecs::EntityID camera) {
    if (!m_registry.alive(camera) || m_registry.get<ecs::Camera>(camera) == nullptr) {
        return false;
    }
    m_registry.each<ecs::Camera>([&](ecs::EntityID id, ecs::Camera& c) { c.is_active = id == camera; });
    if (m_cameraChanged) {
        m_cameraChanged(camera);
    }
    return true;
}

ecs::EntityID RegistrySceneBackend::active_camera() const {
    ecs::EntityID found = ecs::EntityID::null();
    m_registry.each<ecs::Camera>([&](ecs::EntityID id, ecs::Camera& c) {
        if (c.is_active && !found.valid()) {
            found = id;
        }
    });
    return found;
}

} // namespace fuse::script
