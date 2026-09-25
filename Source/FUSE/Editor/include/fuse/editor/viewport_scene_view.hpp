#pragma once

#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/viewport_panel.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/ecs/systems/culling_system.hpp>
#include <fuse/ecs/systems/scene_build_system.hpp>
#include <fuse/spatial/bvh.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::editor {

struct ViewportPickResult {
    bool hit = false;
    ecs::EntityID entity = ecs::EntityID::null();
    spatial::BVHLeafType type = spatial::BVHLeafType::Mesh;
    /// Distance along the (unit) pick ray to the entity's surface.
    f32 distance = 0.f;
    ecs::vec3 point{};
};

/// The editor viewport's view of an `EditorScene` (B6.3): once per frame it runs the transform
/// pass, keeps a spatial BVH over every Mesh / SDFObject, culls against the viewport camera and
/// gathers `ecs::SceneData` exactly as the runtime `SceneManager::buildFrame` does, so inspector
/// edits made before `buildFrame` are visible in that same frame. It also answers entity picks
/// (front-most surface under a pixel) through the same BVH.
class ViewportSceneView {
public:
    static constexpr f32 kDefaultPickDistance = 10000.f;

    /// Transform pass + BVH sync + cull + scene gather for this frame.
    const ecs::SceneData& buildFrame(EditorScene& scene, const ViewportPanel& viewport);

    [[nodiscard]] const ecs::SceneData& frame() const { return m_frame; }
    [[nodiscard]] const ecs::CullResult& cullResult() const { return m_cull; }
    [[nodiscard]] const ecs::Camera& camera() const { return m_camera; }
    [[nodiscard]] const spatial::BVH& bvh() const { return m_bvh; }
    [[nodiscard]] u32 frameCount() const { return m_frameCount; }
    [[nodiscard]] u32 bvhRebuildCount() const { return m_bvhRebuilds; }

    /// Brings world matrices and the BVH up to date without gathering a frame.
    void refreshSpatial(EditorScene& scene);

    /// Front-most entity surface hit by `ray`: the BVH prunes by bounds, each candidate is refined
    /// exactly (mesh: oriented local AABB; SDF: sphere trace of `ecs::sdf_object_distance`).
    ViewportPickResult pickRay(EditorScene& scene, const ViewportRay& ray,
                               f32 maxDistance = kDefaultPickDistance);
    /// Pick through window pixel (px, py) of `viewport`.
    ViewportPickResult pick(EditorScene& scene, const ViewportPanel& viewport, f32 px, f32 py);
    /// Click-select: pick and make the hit the sole / primary selection (clears on a miss).
    ViewportPickResult pickAndSelect(EditorScene& scene, const ViewportPanel& viewport, f32 px, f32 py,
                                     EditorState& state);

private:
    void syncBvh_(ecs::Registry& registry);

    ecs::SceneData m_frame{};
    ecs::CullResult m_cull{};
    ecs::Camera m_camera{};
    spatial::BVH m_bvh{};
    std::vector<spatial::BVHLeaf> m_leaves;
    std::vector<spatial::BVHLeaf> m_lastLeaves;
    u32 m_frameCount = 0;
    u32 m_bvhRebuilds = 0;
};

} // namespace fuse::editor
