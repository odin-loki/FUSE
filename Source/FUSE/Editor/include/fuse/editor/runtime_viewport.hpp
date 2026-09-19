#pragma once

#include <fuse/editor/runtime_embed_session.hpp>
#include <fuse/editor/viewport_panel.hpp>
#include <fuse/editor/viewport_swapchain_handoff.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <mutex>
#include <string>

namespace fuse::editor {

class EditorHost;

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
    void mirrorEditorEntities_(EditorHost& host);
    void tickHeadlessPresentStub_(EditorHost& host, f32 dt);

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

#if defined(FUSE_VULKAN_BACKEND)
    void* m_headlessGpuStub = nullptr;
#endif
};

} // namespace fuse::editor
