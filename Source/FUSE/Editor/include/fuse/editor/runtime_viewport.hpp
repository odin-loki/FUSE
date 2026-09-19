#pragma once

#include <fuse/editor/runtime_embed_session.hpp>
#include <fuse/editor/viewport_panel.hpp>
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
    void requestResize(u32 width, u32 height);
    void setProjectLabel(std::string label);
    void setProjectRoot(std::string root);

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

#if defined(FUSE_VULKAN_BACKEND)
    struct HeadlessGpuStub;
    std::unique_ptr<HeadlessGpuStub> m_headlessGpu;
#endif
};

} // namespace fuse::editor
