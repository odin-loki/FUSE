#pragma once
// E03 hybrid GPU frame (docs/unification/RENDERER-EXECUTION.md RE-FI-1, TRACK-B-VULKAN.md RE-RUNTIME-3D-RENDER,
// U4-HYBRID-FRAME.md U4-1): Hybrid / World3D / the editor viewport render the real 3D scene through the E02
// scene_renderer::SceneRenderer (FrameComposer) instead of the software PlaceholderRenderer.
//
// One frame (HybridComposer::render on the render thread):
//   beginOverlay(); addSprite(...) / addUi(...)        World2D sprites, then the UI layer (placeholder units -> pixels)
//   World3D::render -> renderWorld3D(world, ctx)        graph.reset(); SceneRenderer::renderScene(world registry, camera)
//                                                       3D opaque + transparent (forward) + post; the E02 UI stage runs
//                                                       our FrameUiSource: "hybrid.sprite_layer" (sprites, then UI quads)
//                                                       -> "frame.ui_composite" over the 3D output
//   (no 3D world: renderEmpty(ctx) records the same frame over an empty registry)
//   RhiContext::submitFrame -> submitFrame(SceneFrameSubmit)   "present.blit" (+ "present.handoff") into the acquired
//                                                       swapchain image, or into the headless RGBA8 target (+ a copy into
//                                                       the host-visible readback buffer); rg::Executor::execute waiting
//                                                       on the acquire semaphore, signalling renderFinished + the slot fence
//
// Frames in flight: the executor ring is 2 deep, so when frame N is recorded frame N-2 has retired and
// SceneRenderer::collectRetired(N - 2) runs (the composer / sprite-layer rings are 3 deep).
// Steady state (static or moving transforms, no new meshes / materials / entities): no heap allocations in
// beginOverlay / add* / renderWorld3D / submitFrame (gate: fuse_hybrid_frame_vk --mode zero_alloc).
#if defined(FUSE_HAS_VULKAN_RHI)

#include <fuse/hybrid/sprite_layer.hpp>
#include <fuse/hybrid/sprite_layer_gpu.hpp>
#include <fuse/renderer/frame/frame_composer.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/scene_renderer/scene_renderer.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/world3d/render_scene.hpp>

#include <memory>
#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
namespace rg {
class Executor;
}
} // namespace fuse::renderer

namespace fuse::world3d {
class World3D;
}

namespace fuse::hybrid {

struct HybridSceneRendererDesc {
    /// Display extent of the 3D frame (0 = 320 x 240, the placeholder extent). A swapchain of another size gets the
    /// frame through a scaling blit (linear); resize() re-creates the frame at a new extent.
    u32 width = 0;
    u32 height = 0;
    u32 renderWidth = 0; ///< 0 = display extent
    u32 renderHeight = 0;
    /// T0 by default: deterministic on every device; Auto takes T2 on ray-query devices.
    renderer::scene_renderer::SceneTier tier = renderer::scene_renderer::SceneTier::T0;
    u32 instanceCapacity = 1024;
    u32 meshCapacity = 64;
    u32 materialCapacity = 256;
    u32 lightCapacity = 64;
    u32 entityCapacity = 4096; ///< >= the render registry's entity indices (World3D::kRegistryCapacity)
    u32 maxQuads = 1024;       ///< sprites + UI quads per frame
    /// Keep an RGBA8 copy of every headless frame in host-visible memory (readbackPixels()).
    bool headlessReadback = true;
    /// Use runtimeComposerDesc() / runtimeFrameSettings() (modest VSM / DDGI / fog sizes, clouds off, no upscaler).
    /// False: `composer` / `settings` below as given.
    bool runtimeDefaults = true;
    renderer::frame::FrameComposerDesc composer{};
    renderer::frame::FrameSettings settings{};
    /// Bindless backend (the descriptor-set path is the one validated on Lavapipe; see test_rp_frame.cpp).
    renderer::BindlessBackendPreference bindlessBackend = renderer::BindlessBackendPreference::DescriptorSet;
};

/// The runtime's composer template and frame settings (what HybridSceneRendererDesc::runtimeDefaults selects).
renderer::frame::FrameComposerDesc runtimeComposerDesc();
renderer::frame::FrameSettings runtimeFrameSettings();

struct HybridSceneFrameStats {
    u64 serial = 0;
    u32 sprites = 0;
    u32 uiQuads = 0;
    u32 instances = 0;     ///< GpuScene instances after the extract
    u32 lights = 0;
    bool uiStageRan = false;
    bool presentedToSwapchain = false;
    bool headless = false;
    bool submitted = false;
};

class HybridSceneRenderer final : public world3d::IWorld3DRenderer {
public:
    HybridSceneRenderer();
    ~HybridSceneRenderer() override;
    HybridSceneRenderer(const HybridSceneRenderer&) = delete;
    HybridSceneRenderer& operator=(const HybridSceneRenderer&) = delete;

    /// Creates the allocator, bindless heap, executor, SceneRenderer (+ the builtin meshes of world3d::BuiltinMesh and
    /// a default material palette), the sprite layer and the headless target on `device`. False (reason()) on the
    /// stub backend, without a device, or when the composer / kernels cannot be built.
    bool initialize(renderer::VulkanDevice& device, const HybridSceneRendererDesc& desc = {});
    /// Waits for the GPU and releases everything. Idempotent.
    void shutdown();
    bool valid() const { return m_valid; }
    const char* reason() const { return m_reason; }

    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    /// Re-creates the frame at a new display extent (waits for the GPU first).
    bool resize(u32 width, u32 height);

    renderer::scene_renderer::SceneRenderer& sceneRenderer() { return *m_scene; }
    const renderer::scene_renderer::SceneRenderer& sceneRenderer() const { return *m_scene; }

    // --- overlay (2D sprites + UI), recorded before the 3D frame ------------------------------------------------
    void beginOverlay();
    /// False when the per-frame quad capacity is exhausted.
    bool addSprite(const SpriteQuad& quad);
    bool addUi(const SpriteQuad& quad);
    u32 spriteCount() const { return m_spriteCount; }
    u32 uiCount() const { return m_uiCount; }
    /// Sprites then UI quads (what the kernel composites, in order).
    const SpriteQuad* overlayQuads() const { return m_overlay.data(); }
    u32 overlayCount() const { return m_spriteCount + m_uiCount; }

    // --- 3D -------------------------------------------------------------------------------------------------------
    /// World3D::render hook: renders the world's registry (World3D::registry) with its camera (RenderCamera3D, else
    /// the registry's active ecs::Camera, else a default view) and material table.
    bool renderWorld3D(world3d::World3D& world, frame::FrameCtx& ctx) override;
    /// The frame without a 3D world (sky + overlay).
    bool renderEmpty(frame::FrameCtx& ctx);
    /// Records a frame of `registry` seen by `camera` (renderWorld3D / renderEmpty end here).
    bool renderRegistry(ecs::Registry& registry, const renderer::frame::FrameCamera& camera, frame::FrameCtx& ctx);
    /// True between a render* call and the submit that consumes it.
    bool frameRecorded() const { return m_recorded; }

    // --- submission (RhiContext::setSceneFrameSource) ----------------------------------------------------------------
    renderer::SceneFrameSource frameSource() { return renderer::SceneFrameSource{&HybridSceneRenderer::submitThunk, this}; }
    /// Appends the present blit (swapchain image or headless target + readback copy) and executes the frame's graph
    /// with the slot's semaphores / fence. False (nothing submitted) when no frame was recorded.
    bool submitFrame(const renderer::SceneFrameSubmit& submit);
    /// Submits the recorded frame without an RhiContext (headless target, own fence-less submission).
    bool submitHeadless();

    /// Blocks until every submitted frame completed (then readbackPixels() holds the last headless frame).
    bool waitIdle();
    /// RGBA8 (R, G, B, A byte order), width() x height(), top row first: the last completed headless frame.
    /// Null when headlessReadback is off or nothing was rendered headless yet.
    const u8* readbackPixels() const;
    u32 readbackFrames() const { return m_readbackFrames; }

    const HybridSceneFrameStats& lastFrame() const { return m_stats; }
    u64 framesSubmitted() const { return m_framesSubmitted; }
    renderer::rg::Executor* executor() { return m_executor.get(); }
    renderer::GpuAllocator* allocator() { return m_allocator.get(); }
    renderer::VulkanDevice* device() { return m_device; }

private:
    static bool submitThunk(const renderer::SceneFrameSubmit& submit, void* user);
    static void recordCopy(const rg::PassContext& context, void* user);
    static rg::TextureRef recordUi(rg::Graph& graph, const renderer::frame::FrameUiContext& context, void* user);
    bool createTargets();
    void destroyTargets();
    void syncMaterials(const world3d::World3D& world);
    bool execute(void* waitSemaphore, void* signalSemaphore, void* fence, const renderer::SceneFrameSubmit* swapchain);

    renderer::VulkanDevice* m_device = nullptr;
    HybridSceneRendererDesc m_desc{};
    bool m_valid = false;
    const char* m_reason = "not initialised";
    u32 m_width = 0;
    u32 m_height = 0;

    std::unique_ptr<renderer::GpuAllocator> m_allocator;
    std::unique_ptr<renderer::BindlessDescriptors> m_bindless;
    std::unique_ptr<renderer::rg::Executor> m_executor;
    std::unique_ptr<renderer::scene_renderer::SceneRenderer> m_scene;
    SpriteLayerGpu m_sprites;
    rg::Graph m_graph;
    renderer::frame::FrameGraphOutputs m_outputs{};
    bool m_recorded = false;

    std::vector<SpriteQuad> m_overlay; ///< [0, sprites) sprites, then UI quads (sized maxQuads at init)
    std::vector<SpriteQuad> m_uiScratch;
    u32 m_spriteCount = 0;
    u32 m_uiCount = 0;

    renderer::Texture m_headless{};
    u32 m_headlessLayout = 0;
    u8 m_headlessQueue = rg::kNoQueue;
    renderer::Buffer m_readback{};
    struct Copy {
        rg::TextureRef src;
        rg::BufferRef dst;
        u32 width = 0;
        u32 height = 0;
    };
    Copy m_copy{};
    u32 m_readbackFrames = 0;
    bool m_readbackPending = false;

    ecs::Registry* m_emptyRegistry = nullptr;
    std::unique_ptr<ecs::Registry> m_emptyRegistryStorage;
    u64 m_materialVersion = ~0ull;
    const world3d::World3D* m_materialWorld = nullptr;
    u64 m_framesSubmitted = 0;
    HybridSceneFrameStats m_stats{};
};

/// GPU readback check for demos / gates (call after waitIdle, on a headless frame): the pixels where the CPU sprite
/// layer (spriteLayerTexel over overlayQuads()) is opaque must equal the quad colour within `tolerance` / 255.
struct OverlayReadbackCheck {
    bool valid = false;  ///< a headless frame was read back
    u32 covered = 0;     ///< pixels fully covered by the overlay
    u32 mismatched = 0;  ///< channels off by more than the tolerance
};
OverlayReadbackCheck checkOverlayReadback(const HybridSceneRenderer& renderer, u32 tolerance = 1u);

/// RGB of pixel (x, y) of the last headless readback (false when out of range / nothing read back).
bool readbackPixel(const HybridSceneRenderer& renderer, u32 x, u32 y, u8 rgb[3]);

} // namespace fuse::hybrid

#endif // defined(FUSE_HAS_VULKAN_RHI)
