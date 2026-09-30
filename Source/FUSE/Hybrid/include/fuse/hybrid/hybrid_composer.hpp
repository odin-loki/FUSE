#pragma once

#include <fuse/frame/frame_barrier.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/cooked_asset_bindings.hpp>
#include <fuse/hybrid/mesh_sdf_preview_stub.hpp>
#include <fuse/hybrid/placeholder_renderer.hpp>
#include <fuse/hybrid/project_flags.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <memory>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/hybrid/hybrid_scene_renderer.hpp>
#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/rhi_context.hpp>
#endif

namespace fuse::hybrid {

/// Hybrid compositor (U4 v1): 3D opaque → 2D scene → UI overlay ordering.
///
/// E03: with a Vulkan device the frame is GPU-rendered (enableGpuScene / HybridRendererBootstrap): World3D's ECS
/// registry through the E02 SceneRenderer (3D opaque + transparent + post), World2D sprites and the UI layer as quads
/// of the "hybrid.sprite_layer" kernel composited by the SceneRenderer's UI stage, and the result blitted into the
/// acquired swapchain image or a headless target (RhiContext::submitFrame routes to HybridSceneRenderer). The software
/// PlaceholderRenderer is then off by default and stays the fallback when no device exists (stub backend, no ICD) or
/// the GPU scene cannot be created.
class HybridComposer {
public:
    HybridComposer();
    ~HybridComposer();
    HybridComposer(const HybridComposer&) = delete;
    HybridComposer& operator=(const HybridComposer&) = delete;

    void setProjectFlags(const DimensionFlags& flags);
    const DimensionFlags& projectFlags() const { return m_flags; }

    void attachWorld2D(world2d::World2D* world);
    void attachWorld3D(world3d::World3D* world);

    world2d::World2D* world2D() { return m_world2D; }
    world3d::World3D* world3D() { return m_world3D; }

    PlaceholderRenderer& renderer() { return m_renderer; }
    const PlaceholderRenderer& renderer() const { return m_renderer; }

    CookedAssetBindings& cookedAssets() { return m_cookedAssets; }
    const CookedAssetBindings& cookedAssets() const { return m_cookedAssets; }

    MeshSdfPreviewCatalog& previewCatalog() { return m_previewCatalog; }
    const MeshSdfPreviewCatalog& previewCatalog() const { return m_previewCatalog; }

    void tick(frame::FrameCtx& ctx);
    void render(frame::FrameCtx& ctx);

    /// When false, skip software RGBA writes — RHI mirror remains for headless CI fallback tests.
    /// An explicit call wins over the GPU scene's default (placeholder off while the GPU scene renders).
    void setSoftwarePlaceholderEnabled(bool enabled) {
        m_softwarePlaceholderEnabled = enabled;
        m_softwarePlaceholderExplicit = true;
    }
    bool softwarePlaceholderEnabled() const { return m_softwarePlaceholderEnabled; }
    u32 softwarePlaceholderSkippedFrames() const { return m_softwarePlaceholderSkippedFrames; }
    u32 meshPreviewDraws() const { return m_meshPreviewDraws; }
    u32 sdfPreviewDraws() const { return m_sdfPreviewDraws; }

    u32 frameCount() const { return m_frameCount; }
    const frame::FrameBarrier& frameBarrier() const { return m_barrier; }

    /// True while frames are GPU-rendered through the scene renderer (a Vulkan device exists and the GPU scene
    /// initialised). False: the PlaceholderRenderer fallback (and the legacy RHI command mirror) draws.
    bool gpuSceneActive() const;
    /// Frames the GPU scene path submitted.
    u64 gpuSceneFrames() const { return m_gpuSceneFrames; }

#if defined(FUSE_HAS_VULKAN_RHI)
    /// E03: request the GPU scene path. It is created on the first render() (on rhiContext()'s device, sized to the
    /// real swapchain when one exists, else desc / 320 x 240); without a device it stays off and the placeholder
    /// renders. Returns false only when a previous attempt already failed on this context.
    bool enableGpuScene(const HybridSceneRendererDesc& desc = {});
    /// Drops the GPU scene (waits for its frames) and restores the placeholder default.
    void disableGpuScene();
    /// The GPU scene renderer (null until the first render() created it, or when unavailable).
    HybridSceneRenderer* gpuScene() { return m_gpuScene.get(); }
    const HybridSceneRenderer* gpuScene() const { return m_gpuScene.get(); }
    /// Why the GPU scene is not active ("not requested", the init failure, ...).
    const char* gpuSceneStatus() const { return m_gpuSceneStatus; }

    bool hasRhiRecording() const { return true; }
    const renderer::RenderCommandList& lastCommandList() const { return m_commandList; }
    /// Inject shared RhiContext from RendererBootstrap (B2.10). Null restores lazy create.
    void setSharedRhiContext(renderer::RhiContext* context);
    renderer::RhiContext* rhiContext();
    const renderer::RhiContext* rhiContext() const;
#else
    bool hasRhiRecording() const { return false; }
#endif

private:
    void applyProjectFlags();
#if defined(FUSE_HAS_VULKAN_RHI)
    void ensureRhiContext();
    void recordClear3D(float r, float g, float b);
    void recordSprite2D(float x, float y, float rotation, u8 r, u8 g, u8 b);
    void ensureGpuScene();
    void releaseGpuScene();
#endif

    DimensionFlags m_flags;
    world2d::World2D* m_world2D = nullptr;
    world3d::World3D* m_world3D = nullptr;
    frame::FrameBarrier m_barrier;
    PlaceholderRenderer m_renderer;
    u32 m_frameCount = 0;
    bool m_softwarePlaceholderEnabled = true;
    bool m_softwarePlaceholderExplicit = false;
    u64 m_gpuSceneFrames = 0;
    u32 m_softwarePlaceholderSkippedFrames = 0;
    CookedAssetBindings m_cookedAssets;
    MeshSdfPreviewCatalog m_previewCatalog;
    u32 m_meshPreviewDraws = 0;
    u32 m_sdfPreviewDraws = 0;
#if defined(FUSE_HAS_VULKAN_RHI)
    renderer::RenderCommandList m_commandList;
    renderer::RhiContext* m_sharedRhiContext = nullptr;
    std::unique_ptr<renderer::RhiContext> m_ownedRhiContext;
    // Declared after the owned context: destroyed first (its device outlives it).
    std::unique_ptr<HybridSceneRenderer> m_gpuScene;
    HybridSceneRendererDesc m_gpuSceneDesc{};
    bool m_gpuSceneRequested = false;
    bool m_gpuSceneFailed = false;
    const char* m_gpuSceneStatus = "not requested";
#endif
};

} // namespace fuse::hybrid
