#pragma once

#include <fuse/editor/viewport_panel.hpp>
#include <fuse/types.hpp>

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

    void tick(EditorHost& host, f32 dt);

    ViewportPanel& panel() { return m_panel; }
    const ViewportPanel& panel() const { return m_panel; }

    bool isEmbedded() const { return m_embedded; }
    u32 runtimeTickCount() const { return m_runtimeTickCount; }
    const std::string& lastProjectLabel() const { return m_lastProjectLabel; }

private:
    void applyPendingResize_();

    ViewportPanel m_panel;
    std::mutex m_resizeMutex;
    u32 m_pendingWidth = 0;
    u32 m_pendingHeight = 0;
    bool m_hasPendingResize = false;
    bool m_embedded = false;
    u32 m_runtimeTickCount = 0;
    std::string m_lastProjectLabel;
};

} // namespace fuse::editor
