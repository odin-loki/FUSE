#pragma once
// E02 SceneRenderer (docs/unification/RENDERER-EXECUTION.md RE-FI-1, TRACK-B-VULKAN.md RE-RUNTIME-3D-RENDER,
// U4-HYBRID-FRAME.md U4-1): renders an ECS 3D scene through the GPU-driven frame::FrameComposer, so the runtime and
// the editor can stop drawing with the software PlaceholderRenderer. The legacy RendererBootstrap / GBufferRasterPass
// path is untouched (fallback).
//
// Owns the WP-1.1 GpuScene, the UploadQueue (+ its staging buffer, unless the caller hands one in), the ECS
// extractor, the E02 mesh registry and material feed, and the FrameComposer (T0 by default; T2 when
// rt::queryRtCapabilities reports ray query and the tier is Auto).
//
// Frame (renderScene; the order of frame_composer.hpp "Frame protocol"):
//   bindless.setFrameSerial(serial); scene.beginFrame(serial); composer.beginSceneFrame(serial);
//   meshes.flush(scene)                          new / replaced meshes -> GpuScene::addMeshletMesh, meshRemap
//   materialFeed.sync(materials, scene)          MaterialSystem rows whose version moved -> GpuScene::setMaterial
//   extractor.extract(registry, scene)           Transform + Mesh -> instances, Point / Spot / Directional -> lights
//   T0 + DDGI: global SDF boxes from the instances' world AABBs when the instance set / materials changed
//   scene.commit(); composer.commitScene(); upload.flush();
//   composer.beginFrame(desc, settings); outputs = composer.addFrame(graph)
// The caller resets the graph before renderScene, may append passes (readback, addPresent), executes it, and calls
// collectRetired(serial) once the frame retired. The sun is the first DirectionalLight entity (its GpuScene light
// slot); the camera is a frame::FrameCamera (cameraFromRegistry derives one from the active ecs::Camera).
//
// Resize: resize() re-creates the composer at the new extent (full dynamic resolution is RE-FI-11); the caller must
// have retired every frame first.
//
// Steady state (no new entities / meshes / materials, static scene or moving transforms) makes no heap allocations
// in renderScene (gate: fuse_scene_renderer_vk --mode zero_alloc).
#include <fuse/renderer/frame/frame_composer.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_ecs.hpp>
#include <fuse/renderer/scene_renderer/mesh_registry.hpp>
#include <fuse/renderer/scene_renderer/procedural_meshes.hpp>
#include <fuse/renderer/vk/present_path.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::ecs {
class Registry;
}

namespace fuse::renderer {
class BindlessDescriptors;
class GpuAllocator;
class MaterialSystem;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::scene_renderer {

enum class SceneTier : u8 {
    Auto = 0, ///< T2 when rt::queryRtCapabilities(device).usable, else T0
    T0,
    T2,
};

struct SceneRendererDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    /// Optional: an UploadQueue the caller owns (and flushes nothing else into mid-frame). Null: the renderer creates
    /// its own with a `stagingBytes` CpuToGpu staging buffer.
    UploadQueue* upload = nullptr;
    usize stagingBytes = 8u * 1024u * 1024u;
    u32 width = 0;  ///< display extent (FrameGraphOutputs::output with an upscaler)
    u32 height = 0;
    u32 renderWidth = 0; ///< 0 = display extent (no upscaling)
    u32 renderHeight = 0;
    SceneTier tier = SceneTier::Auto;
    u32 instanceCapacity = 1024;
    u32 meshCapacity = 64;
    u32 materialCapacity = 256;
    u32 lightCapacity = 64;
    u32 entityCapacity = 1024; ///< ECS entity indices pre-sized in the extractor
    /// Template for the composer's remaining fields (clusters, VSM, DDGI volume, splats, framesInFlight, language,
    /// frameGen, ...). device / allocator / bindless / upload / scene / tier / extents / capacities are overwritten.
    frame::FrameComposerDesc composer{};
    /// The renderer calls bindless->setFrameSerial / collectRetired with its frame serials.
    bool manageBindlessSerial = true;
    /// T0 DDGI: build the global SDF from the instances' world AABBs (ecs::Mesh::aabb_min / aabb_max, else the mesh
    /// registry bounds) whenever instances are added / removed, a material changed, or (dynamicSdf) any transform moved.
    bool sdfFromScene = true;
    bool dynamicSdf = false;
};

/// Per-frame inputs besides the camera.
struct SceneFrameDesc {
    u64 serial = 0;          ///< 0: the renderer's own counter (+1 per frame)
    u32 frameIndex = 0xFFFFFFFFu; ///< 0xFFFFFFFF: frames rendered so far
    bool resetHistory = false;
    f32 deltaSeconds = 1.f / 60.f;
    /// Sun illuminance for the sky / DDGI / clouds (the scene light keeps colour x intensity). Negative x: the
    /// directional light's colour x intensity.
    f32 sunIlluminance[3] = {-1.f, 0.f, 0.f};
};

struct SceneFrameStats {
    gpu_scene::EcsExtractStats extract{};
    gpu_scene::MaterialFeedStats materials{};
    u32 meshesUploaded = 0;
    bool sdfRebuilt = false;
    u32 sunSlot = 0xFFFFFFFFu;
    gpu_scene::GpuSceneCommitStats commit{};
    bool ok = false;
};

/// Frame image(s) to hand to the presenter / a headless target (addPresent).
struct ScenePresentTargets {
    rg::TextureRef target;      ///< this frame (FrameGraphOutputs::output)
    u32 width = 0;
    u32 height = 0;
    bool swapchain = false;     ///< add the "present.handoff" pass (PRESENT_SRC_KHR)
    /// Frame generation on: the interpolated frame (presented first) goes here (invalid: not presented).
    rg::TextureRef interpolatedTarget;
};

/// Active camera of `registry` (the first ecs::Camera with is_active, else the first one) + its Transform:
/// eye = translation, forward = -Z, fovY / near / far from the component. False when there is none.
bool cameraFromRegistry(ecs::Registry& registry, frame::FrameCamera& out);

class SceneRenderer {
public:
    SceneRenderer() = default;
    ~SceneRenderer();
    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    bool initialize(const SceneRendererDesc& desc);
    /// The caller must have retired every frame (e.g. executor waitIdle).
    void destroy();
    bool valid() const { return m_initialized; }
    const char* reason() const { return m_reason; }
    frame::FrameTier tier() const { return m_tier; }

    /// Re-creates the composer at the new display (and render) extent; the scene, meshes and materials stay.
    bool resize(u32 width, u32 height, u32 renderWidth = 0, u32 renderHeight = 0);

    // --- content ------------------------------------------------------------------------------------------
    MeshRegistry& meshes() { return m_meshes; }
    const MeshRegistry& meshes() const { return m_meshes; }
    /// Material source (optional; null detaches). Its rows are fed to the GpuScene each frame (changed rows only).
    void setMaterialSystem(const MaterialSystem* materials);
    /// Direct material rows for callers without a MaterialSystem (overwritten by the feed when both write a row).
    bool setMaterial(u32 id, const Material& material);
    void setFrameSettings(const frame::FrameSettings& settings) { m_settings = settings; }
    const frame::FrameSettings& frameSettings() const { return m_settings; }
    /// E02 UI / HUD stage (frame::FrameComposer::setUiSource).
    bool setUiSource(const frame::FrameUiSource& source) { return m_composer.setUiSource(source); }

    // --- frame ----------------------------------------------------------------------------------------------
    /// Mirrors the registry into the GPU scene and records the frame into `graph` (reset by the caller). The graph
    /// must execute before the next renderScene. Invalid outputs when the frame could not be recorded.
    frame::FrameGraphOutputs renderScene(ecs::Registry& registry, const frame::FrameCamera& camera, rg::Graph& graph,
                                         const SceneFrameDesc& frame = {});
    /// Appends the blit(s) of the last renderScene's image(s) into present targets (present_path.hpp): output ->
    /// target, and with frame generation presentInterpolated -> interpolatedTarget (presentReal -> target).
    bool addPresent(rg::Graph& graph, const frame::FrameGraphOutputs& outputs, const ScenePresentTargets& targets);
    /// Destroys resources retired at serials <= completedSerial (composer, scene, bindless when managed).
    void collectRetired(u64 completedSerial);

    // --- inspection -------------------------------------------------------------------------------------------
    u64 frameSerial() const { return m_serial; }
    const SceneFrameStats& lastFrame() const { return m_stats; }
    gpu_scene::GpuScene& scene() { return m_scene; }
    const gpu_scene::GpuScene& scene() const { return m_scene; }
    frame::FrameComposer& composer() { return m_composer; }
    const frame::FrameComposer& composer() const { return m_composer; }
    const gpu_scene::GpuSceneEcsExtractor& extractor() const { return m_extractor; }
    UploadQueue& upload() { return *m_upload; }
    /// The T0 DDGI boxes of the last SDF rebuild (world AABBs + surfaces, in registry chunk order).
    const DdgiCpuScene& sdfBoxes() const { return m_sdfWorld; }

private:
    bool createComposer();
    void rebuildSdf(ecs::Registry& registry);

    SceneRendererDesc m_desc{};
    bool m_initialized = false;
    const char* m_reason = "not initialised";
    frame::FrameTier m_tier = frame::FrameTier::T0;

    gpu_scene::GpuScene m_scene;
    gpu_scene::GpuSceneEcsExtractor m_extractor;
    gpu_scene::GpuSceneMaterialFeed m_feed;
    MeshRegistry m_meshes;
    frame::FrameComposer m_composer;
    const MaterialSystem* m_materials = nullptr;

    UploadQueue m_ownUpload;
    UploadQueue* m_upload = nullptr;
    Buffer m_staging{};

    frame::FrameSettings m_settings{};
    SceneFrameStats m_stats{};
    u64 m_serial = 0;
    u32 m_frames = 0;
    u64 m_remapVersion = ~0ull;
    bool m_sdfDirty = true;
    DdgiCpuScene m_sdfWorld;
    std::vector<compute::SdfObject> m_sdfObjects;
    std::vector<gi_gpu::DdgiSurface> m_sdfSurfaces;
    PresentBlit m_blits[2]{};
};

} // namespace fuse::renderer::scene_renderer
