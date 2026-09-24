// FUSE Relight RL-4.1: what the frame orchestration feeds the FUSE GPU scene per frame (the Vulkan-free half of the
// GPU-scene adapter; scene_adapter.hpp is the GpuScene side).
//
// RenderTap turns every flushed frame of the capture tap into AdapterDraws (RL-1.7 SceneModel result + input, RL-3.4
// replaced draw) and AdapterLights (game lights or RL-3.4's replaced list) and hands them to an IGpuSceneSink:
// GpuSceneAdapter (renderer WP-1.1 gpu_scene::GpuScene) where the renderer's objects can live. Inside d3d9.dll no
// sink is attached yet (render_tap.hpp): fuse_gpu_scene references fuse_rhi's volk globals, whose translation unit
// loads vulkan-1.dll from its static initialiser, i.e. from d3d9.dll's DllMain.
#pragma once

#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/replace/replacement_engine.hpp>
#include <fuse/relight/scene/instances/scene_model.hpp>
#include <fuse/relight/scene/lights/legacy_light.hpp>
#include <fuse/relight/tap/relight_tap.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>

#include <cstdint>
#include <vector>

namespace fuse::relight::render::frame {

/// One committed draw as the GPU scene consumes it.
struct AdapterDraw {
    std::uint64_t instanceId = 0; ///< SceneDrawResult::instanceId (0: none, skipped)
    std::uint64_t blasId = 0;
    bool created = false;         ///< SceneDrawResult::created (a new RL-1.7 instance: no motion)
    scene::instances::Mat4f objectToWorld = scene::instances::identityMatrix();
    scene::instances::AxisAlignedBoundingBox bounds; ///< object space
    hash::Hash64 materialHash = hash::kEmptyHash;
    tap::ResourceId colorTexture = tap::kNoResource;
    tap::Color4 diffuse{1.f, 1.f, 1.f, 1.f};
    bool transparent = false;
    const replace::ReplacedDraw* replaced = nullptr; ///< RL-3.4 (null: no replacement engine)
};

/// Builds an AdapterDraw from the RL-1.7 input and result of a draw.
AdapterDraw adapterDraw(const scene::instances::SceneDrawResult& result, const scene::instances::SceneDrawInput& input,
                        const scene::LegacyMaterialRecord& material, tap::ResourceId colorTexture);

struct AdapterLight {
    std::uint64_t key = 0; ///< identity (game light hash, or mod record + instance)
    renderer::gpu_scene::GpuLight light;
};
/// Game lights (TranslatedFrame::lights) as GPU lights.
std::vector<AdapterLight> adapterLights(const std::vector<scene::LightRecord>& lights);
/// RL-3.4's replaced light list.
std::vector<AdapterLight> adapterLights(const std::vector<replace::ReplacedLight>& lights);

/// Where a frame's scene goes (GpuSceneAdapter).
class IGpuSceneSink {
public:
    virtual ~IGpuSceneSink() = default;
    virtual void beginFrame(std::uint64_t serial) = 0;
    virtual void submit(const AdapterDraw& draw) = 0;
    virtual void submitLights(const std::vector<AdapterLight>& lights) = 0;
    virtual void endFrame() = 0;
    virtual void clear() = 0;
    /// Live GPU instances after the last endFrame.
    virtual std::uint32_t instanceCount() const = 0;
};

} // namespace fuse::relight::render::frame
