#pragma once

// MP-B7.3-LUA-API: ready-made backends for the Input.* and Scene.* script tables.

#include <fuse/ecs/entity.hpp>
#include <fuse/script/script_engine_api.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::platform {
class ActionMap;
class GamepadState;
class InputState;
class PlayerController;
} // namespace fuse::platform

namespace fuse::spatial {
class BVH;
}

namespace fuse::script {

/// `Input.*` over a frame's InputState, ActionMap and GamepadState (any may be null: queries on
/// a missing source read as idle; actions are unknown without a map). Non-owning.
class InputScriptBackend final : public ScriptInputBackend {
public:
    InputScriptBackend(const platform::InputState* input, const platform::ActionMap* actions,
                       const platform::GamepadState* gamepads)
        : m_input(input), m_actions(actions), m_gamepads(gamepads) {}
    explicit InputScriptBackend(const platform::PlayerController& player);

    [[nodiscard]] bool key_pressed(platform::Key key) const override;
    [[nodiscard]] bool key_held(platform::Key key) const override;
    [[nodiscard]] bool key_released(platform::Key key) const override;
    [[nodiscard]] bool mouse_pressed(platform::MouseButton button) const override;
    [[nodiscard]] bool mouse_held(platform::MouseButton button) const override;
    [[nodiscard]] bool mouse_released(platform::MouseButton button) const override;
    void mouse_delta(f32& dx, f32& dy) const override;
    void mouse_position(f32& x, f32& y) const override;
    [[nodiscard]] bool has_action(std::string_view action) const override;
    [[nodiscard]] bool action_pressed(std::string_view action) const override;
    [[nodiscard]] bool action_held(std::string_view action) const override;
    [[nodiscard]] bool action_released(std::string_view action) const override;
    [[nodiscard]] f32 action_axis(std::string_view action) const override;
    [[nodiscard]] bool gamepad_connected(u32 pad) const override;

private:
    const platform::InputState* m_input = nullptr;
    const platform::ActionMap* m_actions = nullptr;
    const platform::GamepadState* m_gamepads = nullptr;
};

/// `Scene.*` over a Registry: an entity name table (filled by `Entity.create(name)` and
/// `set_name`), sphere queries (through `bvh` when given — it must hold the Mesh / SDFObject
/// leaves, as SceneManager::spatialBvh() does — plus a scan of Transform-only entities; without a
/// BVH everything is scanned), and the active camera (Camera::is_active on exactly one entity).
class RegistrySceneBackend final : public ScriptSceneBackend {
public:
    explicit RegistrySceneBackend(ecs::Registry& registry, const spatial::BVH* bvh = nullptr)
        : m_registry(registry), m_bvh(bvh) {}

    void set_bvh(const spatial::BVH* bvh) { m_bvh = bvh; }
    /// Name an entity (replaces its previous name; a name maps to one entity, the latest).
    bool set_name(ecs::EntityID entity, std::string_view name);
    void clear_name(ecs::EntityID entity);
    [[nodiscard]] std::string name_of(ecs::EntityID entity) const;
    /// Called after the active camera changes (e.g. to forward it to a SceneManager).
    void set_camera_changed_callback(std::function<void(ecs::EntityID)> callback) {
        m_cameraChanged = std::move(callback);
    }

    ecs::EntityID find_entity(std::string_view name) override;
    usize query_sphere(const ecs::vec3& center, f32 radius, std::vector<ecs::EntityID>& out) override;
    bool set_active_camera(ecs::EntityID camera) override;
    [[nodiscard]] ecs::EntityID active_camera() const override;
    void on_entity_created(ecs::EntityID entity, std::string_view name) override { (void)set_name(entity, name); }

private:
    ecs::Registry& m_registry;
    const spatial::BVH* m_bvh = nullptr;
    std::unordered_map<std::string, ecs::EntityID> m_byName;
    std::unordered_map<u32, std::string> m_names; ///< entity index -> name
    std::function<void(ecs::EntityID)> m_cameraChanged;
};

} // namespace fuse::script
