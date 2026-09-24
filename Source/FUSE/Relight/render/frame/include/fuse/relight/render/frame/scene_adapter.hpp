// FUSE Relight RL-4.1: the adapter from Relight's CPU scene (RL-1.7 SceneModel, RL-3.4 replaced scene) to the
// FUSE GPU scene (renderer WP-1.1 gpu_scene::GpuScene).
//
// Per frame: beginFrame(serial), submit() for every committed draw (its SceneModel result, with the RL-3.4
// replacement when there is one), submitLights() once, endFrame() -> the frame's deltas are in the GpuScene
// (commit() uploads them when it has a GPU backend; CPU mirror otherwise).
//
// Mapping:
//   instances   one GPU instance per (RL-1.7 instance id, part): part 0 is the original draw (kept unless a mesh
//               replacement hides it: ReplacedDraw::drawOriginal), parts 1.. the replacement mesh parts placed at
//               part transform x objectToWorld. Created the first frame the key is seen (teleport: previous
//               transform = current), transform updated every frame (the GPU scene keeps the previous one for
//               motion vectors), removed the first frame the key is not submitted (SceneModel's garbage
//               collection already decided which instances live).
//   meshes      one row per geometry identity: the RL-1.7 BLAS id for original draws, the replacement mesh id for
//               parts; the object-space bounds as the bounding sphere. Vertex streams stay 0 until the geometry
//               upload (RL-4.2) fills them.
//   materials   one row per legacy material hash (colour texture 0; untextured draws share row 0) or
//               replacement material record; base colour = D3DMATERIAL9 diffuse, baseColorTexIdx = the bindless
//               shader handle of the draw's colour texture (BindlessImageRegistry) when it has one.
//   lights      the frame's lights (game lights, or RL-3.4's replaced list), matched by identity (game hash /
//               mod record + instance) so a persistent light keeps its slot.
#pragma once

#include <fuse/relight/render/frame/scene_feed.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::relight::render::frame {

struct AdapterStats {
    std::uint64_t serial = 0;
    std::uint32_t draws = 0;
    std::uint32_t instances = 0;        ///< live GPU instances after the frame
    std::uint32_t added = 0, removed = 0, moved = 0;
    std::uint32_t hiddenOriginals = 0;  ///< original draws hidden by a mesh replacement
    std::uint32_t replacementParts = 0;
    std::uint32_t meshes = 0, materials = 0, lights = 0;
    std::uint32_t texturedMaterials = 0; ///< material rows with a bindless colour texture
    renderer::gpu_scene::GpuSceneCommitStats commit;
};

class GpuSceneAdapter final : public IGpuSceneSink {
public:
    using TextureHandleFn = std::function<std::uint32_t(tap::ResourceId)>;

    /// `scene` must be initialised (GPU or CPU mirror); not owned. `textureHandle`: bindless shader handle of a
    /// texture (0: none).
    GpuSceneAdapter(renderer::gpu_scene::GpuScene& scene, TextureHandleFn textureHandle);

    void beginFrame(std::uint64_t serial) override;
    void submit(const AdapterDraw& draw) override;
    void submitLights(const std::vector<AdapterLight>& lights) override;
    /// Removes the instances not submitted this frame and commits the GPU scene (stats()).
    void endFrame() override;
    /// Removes every instance and light (device reset / destroy).
    void clear() override;
    std::uint32_t instanceCount() const override { return m_stats.instances; }

    const AdapterStats& stats() const { return m_stats; }
    /// The GPU instance of (instanceId, part); invalid handle when none.
    renderer::gpu_scene::InstanceHandle instanceOf(std::uint64_t instanceId, std::uint32_t part = 0) const;
    std::uint32_t meshOf(std::uint64_t geometryKey) const;
    std::uint32_t materialOf(std::uint64_t materialKey) const;

private:
    struct InstanceEntry {
        renderer::gpu_scene::InstanceHandle handle;
        std::uint64_t seen = 0;
    };
    struct LightEntry {
        renderer::gpu_scene::LightHandle handle;
        std::uint64_t seen = 0;
    };
    std::uint32_t meshRow(std::uint64_t key, const scene::instances::AxisAlignedBoundingBox& bounds);
    std::uint32_t materialRow(std::uint64_t key, const tap::Color4& diffuse, std::uint32_t textureHandle);
    void placeInstance(std::uint64_t key, std::uint32_t mesh, std::uint32_t material,
                       const scene::instances::Mat4f& objectToWorld, bool teleport, bool transparent);

    renderer::gpu_scene::GpuScene& m_scene;
    TextureHandleFn m_textureHandle;
    std::uint64_t m_serial = 0;
    std::uint64_t m_frame = 0; ///< frames begun (the "seen" stamp)
    std::unordered_map<std::uint64_t, InstanceEntry> m_instances; ///< key: hash(instanceId, part)
    std::unordered_map<std::uint64_t, std::uint32_t> m_meshes;
    std::unordered_map<std::uint64_t, std::uint32_t> m_materials;
    std::unordered_map<std::uint64_t, LightEntry> m_lights;
    AdapterStats m_stats;
};

} // namespace fuse::relight::render::frame
