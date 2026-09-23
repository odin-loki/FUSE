#pragma once

#include <fuse/ecs/entity.hpp>
#include <fuse/editor/runtime_embed_session.hpp>
#include <fuse/editor/viewport_panel.hpp>
#include <fuse/editor/viewport_swapchain_handoff.hpp>
#include <fuse/handle_table.hpp>
#include <fuse/io/asset.hpp>
#include <fuse/project/cook_cache.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/types.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fuse::editor {

class EditorHost;

/// Editor-host request for a presenting viewport (fuse_editor Qt host). The game thread creates the
/// viewport renderer's VkInstance with `instanceExtensions` (the UI toolkit's WSI extensions, e.g.
/// VK_KHR_surface + VK_KHR_xcb_surface) and publishes it; the UI adopts it, creates the window
/// surface on it and hands the surface back ("viewport.vk_surface_adopted").
struct WindowPresentRequest {
    std::vector<std::string> instanceExtensions;
    bool enableValidation = false;
};

enum class WindowPresentState : u8 {
    Off = 0,          ///< not requested: headless / software placeholder path
    InstancePending,  ///< requested; the game thread has not created the instance yet
    InstanceReady,    ///< `windowPresentInstance()` may be adopted by the UI
    SurfaceWired,     ///< real VkSwapchainKHR on the UI surface; frames are presented
    Failed,           ///< no Vulkan / WSI: headless fallback (UI keeps its placeholder)
};

/// Counters for the presenting viewport (game thread writes, any thread reads).
struct WindowPresentStats {
    u64 frames = 0;             ///< viewport frames rendered while a surface was wired
    u64 acquiredImages = 0;     ///< swapchain images acquired
    u64 presentedImages = 0;    ///< real vkQueuePresentKHR calls
    u64 swapchainRecreates = 0; ///< resize-driven swapchain rebuilds
    u32 width = 0;              ///< current swapchain extent
    u32 height = 0;
};

/// Bridges the Qt viewport surface to the in-process runtime scene (U6).
/// UI thread posts dimensions; game thread ticks the embedded viewport hook.
class RuntimeViewportHook {
public:
    RuntimeViewportHook() = default;
    ~RuntimeViewportHook();

    void requestResize(u32 width, u32 height);
    void setProjectLabel(std::string label);
    void setProjectRoot(std::string root);

    /// Queue an external `VkSurfaceKHR` for Track B `SwapchainDesc` wiring (Qt/U6 follow-up).
    void setExternalSurfaceHandle(void* vkSurface, u32 width, u32 height,
                                  const char* handoffSource = nullptr, bool qtStubSurface = false,
                                  bool qtRealSurface = false, void* qtVkInstance = nullptr);
    void setPendingQtStubSurface(bool qtStubSurface) { m_pendingQtStubSurface = qtStubSurface; }
    bool pendingQtStubSurface() const { return m_pendingQtStubSurface; }
    const ViewportSwapchainHandoff& swapchainHandoff() const { return m_surfaceHandoff; }
#if defined(FUSE_VULKAN_BACKEND)
    fuse::renderer::SwapchainDesc buildSwapchainDescHandoff() const;
#endif

    void tick(EditorHost& host, f32 dt);

    /// UI thread, before the game thread ticks: render the viewport for a window-system surface.
    void requestWindowSystemPresent(const WindowPresentRequest& request);
    [[nodiscard]] WindowPresentState windowPresentState() const {
        return m_windowPresentState.load(std::memory_order_acquire);
    }
    /// VkInstance (as void*) to adopt; non-null from InstanceReady until the hook is destroyed.
    [[nodiscard]] void* windowPresentInstance() const {
        return m_windowPresentInstance.load(std::memory_order_acquire);
    }
    [[nodiscard]] WindowPresentStats windowPresentStats() const;
    /// Tear-down, with the game thread stopped: drain the GPU and destroy the swapchain on the UI
    /// surface so the UI can destroy the surface (before the instance goes with this hook).
    void releaseWindowSurface();

    ViewportPanel& panel() { return m_panel; }
    const ViewportPanel& panel() const { return m_panel; }

    RuntimeEmbedSession& embedSession() { return m_embedSession; }
    const RuntimeEmbedSession& embedSession() const { return m_embedSession; }

    bool isEmbedded() const { return m_embedded; }
    u32 runtimeTickCount() const { return m_runtimeTickCount; }
    const std::string& lastProjectLabel() const { return m_lastProjectLabel; }
    const std::string& projectRoot() const { return m_embedSession.projectRoot; }

private:
    void applyPendingResize_();
    void ensureWorldLoaded_(EditorHost& host);
    void drainPendingMaterialLoads_();
    void drainPendingShaderLoads_();
    void mirrorEditorEntities_(EditorHost& host);
    void syncEcsToEmbedWorld3D_(EditorHost& host);
    void syncMeshSdfPreviewFromEcs_(EditorHost& host);
    void bindCookedAssetsToHybrid_();
    void tickHeadlessPresentStub_();
    [[nodiscard]] bool windowPresentActive_() const;
    void tickWindowPresent_();

    ViewportPanel m_panel;
    RuntimeEmbedSession m_embedSession;
    std::mutex m_resizeMutex;
    u32 m_pendingWidth = 0;
    u32 m_pendingHeight = 0;
    bool m_hasPendingResize = false;
    bool m_embedded = false;
    u32 m_runtimeTickCount = 0;
    std::string m_lastProjectLabel;
    ViewportSwapchainHandoff m_surfaceHandoff{};
    bool m_pendingQtStubSurface = true;
    fuse::project::CookCache m_materialCookCache;
    fuse::HandleTable<fuse::io::Asset> m_materialAssetTable;
    bool m_materialLoadsPending = false;
    fuse::project::CookCache m_shaderCookCache;
    fuse::HandleTable<fuse::io::Asset> m_shaderAssetTable;
    bool m_shaderLoadsPending = false;
    WindowPresentRequest m_windowPresentRequest;
    std::atomic<WindowPresentState> m_windowPresentState{WindowPresentState::Off};
    std::atomic<void*> m_windowPresentInstance{nullptr};
    std::atomic<u64> m_wpFrames{0};
    std::atomic<u64> m_wpAcquired{0};
    std::atomic<u64> m_wpPresented{0};
    std::atomic<u64> m_wpRecreates{0};
    std::atomic<u32> m_wpWidth{0};
    std::atomic<u32> m_wpHeight{0};
    /// Per-tick entity walk scratch (capacity reused so idle ticks stay allocation-free).
    std::vector<ecs::EntityID> m_entityOrderScratch;

#if defined(FUSE_VULKAN_BACKEND)
    void* m_headlessGpuStub = nullptr;
    /// Manifest of the loaded world; embed worlds attach once the hybrid renderer exists,
    /// which can be after the world load on the same tick.
    fuse::project::ProjectManifest m_loadedManifest{};
#endif
};

} // namespace fuse::editor
