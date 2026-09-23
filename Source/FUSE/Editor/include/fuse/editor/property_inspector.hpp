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
#include <fuse/scene/scene.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

/// Headless property inspector model (B6.6) — Qt widgets deferred to `fuse_editor`.
class PropertyInspector {
public:
    /// One labelled, formatted field row of a component section.
    struct Field {
        std::string name;
        std::string value;
    };

    struct ComponentSection {
        std::string componentName;
        u32 exposedFieldCount = 0;
        std::vector<Field> fields{};
        /// True when no typed field layout is known for the component (module-registered type):
        /// the section then shows its size and raw bytes read-only.
        bool generic = false;
    };

    /// Build the section for one component instance given its registered name, type size and
    /// raw bytes (type-erased; used for every `ecs::ComponentTypes` entry the entity carries).
    [[nodiscard]] static ComponentSection describeComponent(const char* componentName, usize size,
                                                            const void* data);

    void sync(const EditorState& state, EditorScene& scene);
    /// Bind the inspector to a `runtimeScene` entity selected by index (P5).
    void syncRuntime(const EditorState& state, const scene::Scene& scene);

    [[nodiscard]] bool hasSelection() const { return m_target.valid(); }
    [[nodiscard]] ecs::EntityID target() const { return m_target; }
    [[nodiscard]] const std::vector<ComponentSection>& sections() const { return m_sections; }

    /// Selected runtime-scene object name (mission entity), independent of ECS components.
    [[nodiscard]] bool getName(const scene::Scene& scene, std::string& out) const;
    bool setName(std::string name, scene::Scene& scene, CommandQueue& queue);
    bool setName(std::string name, scene::Scene& scene, CommandStack& cmds);

    bool setTransformPosition(const ecs::vec3& position, EditorScene& scene, CommandStack& cmds);
    bool setSdfBlendAlpha(f32 alpha, EditorScene& scene, CommandStack& cmds);
    /// SDF primitive type edit (`sdf.shape` = type + params, undoable via the editor command
    /// apply path). Params the new type needs but the old one left at zero are filled from the
    /// old shape's size (`sdfParamsForType`), so e.g. Sphere -> Box keeps a same-size box.
    bool setSdfType(ecs::SDFPrimitive type, EditorScene& scene, CommandStack& cmds);

    /// Params for switching `params` (authored for `from`) to primitive `to`.
    [[nodiscard]] static ecs::vec3 sdfParamsForType(ecs::SDFPrimitive from, ecs::SDFPrimitive to,
                                                    const ecs::vec3& params);
    /// "type;x,y,z" text used by the `sdf.shape` command (round-trip float precision).
    [[nodiscard]] static std::string formatSdfShape(ecs::SDFPrimitive type, const ecs::vec3& params);
    [[nodiscard]] static bool parseSdfShape(const std::string& text, ecs::SDFPrimitive& type, ecs::vec3& params);
    bool setDirectionalIntensity(f32 intensity, EditorScene& scene, CommandStack& cmds);
    bool setSpotIntensity(f32 intensity, EditorScene& scene, CommandStack& cmds);

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

    ecs::EntityID m_target = ecs::EntityID::null();
    std::vector<ComponentSection> m_sections;
};

} // namespace fuse::editor
