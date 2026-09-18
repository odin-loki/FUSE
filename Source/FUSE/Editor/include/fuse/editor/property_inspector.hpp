#pragma once

#include <fuse/editor/command_stack.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

/// Headless property inspector model (B6.6) — Qt widgets deferred to `fuse_editor`.
class PropertyInspector {
public:
    struct ComponentSection {
        std::string componentName;
        u32 exposedFieldCount = 0;
    };

    void sync(const EditorState& state, EditorScene& scene);

    [[nodiscard]] bool hasSelection() const { return m_target.valid(); }
    [[nodiscard]] ecs::EntityID target() const { return m_target; }
    [[nodiscard]] const std::vector<ComponentSection>& sections() const { return m_sections; }

    bool setTransformPosition(const ecs::vec3& position, EditorScene& scene, CommandStack& cmds);
    bool setSdfBlendAlpha(f32 alpha, EditorScene& scene, CommandStack& cmds);

    /// Mesh material slot helpers for material inspector wiring (B6.7 deepen).
    [[nodiscard]] bool getMeshMaterialId(const EditorScene& scene, u32& out) const;
    /// Catalog-validated mesh slot read — rejects empty catalog and out-of-range ids.
    [[nodiscard]] bool tryGetMeshMaterialId(const EditorScene& scene, u32 catalogCount,
                                            u32& out) const;
    bool setMeshMaterialId(u32 materialId, EditorScene& scene, CommandStack& cmds);
    /// Catalog-validated mesh slot edit — rejects invalid slots (B6.7 deepen).
    bool trySetMeshMaterialId(u32 materialId, u32 catalogCount, EditorScene& scene,
                              CommandStack& cmds);

private:
    void appendSectionIfPresent(const char* componentName, ecs::EntityID id, EditorScene& scene);

    ecs::EntityID m_target = ecs::EntityID::null();
    std::vector<ComponentSection> m_sections;
};

} // namespace fuse::editor
