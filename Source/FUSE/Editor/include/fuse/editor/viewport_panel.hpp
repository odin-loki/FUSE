#pragma once

#include <fuse/types.hpp>

#include <string>

namespace fuse::editor {

enum class ViewportMode {
    View3D,
    View2D,
    Hybrid,
};

struct ViewportCamera {
    f32 positionX = 0.f;
    f32 positionY = 2.f;
    f32 positionZ = -5.f;
    f32 yaw = 0.f;
    f32 pitch = 0.f;
    f32 moveSpeed = 10.f;
    f32 lookSensitivity = 0.2f;
    bool isFlying = false;
};

/// Headless viewport panel API (B6.3). Qt shell embeds a surface later.
class ViewportPanel {
public:
    void setMode(ViewportMode mode);
    ViewportMode mode() const { return m_mode; }

    ViewportCamera& camera() { return m_camera; }
    const ViewportCamera& camera() const { return m_camera; }

    void setProjectLabel(std::string label);
    const std::string& projectLabel() const { return m_projectLabel; }

    void setDimensions(u32 width, u32 height);
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }

    bool needsResize() const { return m_needsResize; }
    void clearResizeFlag() { m_needsResize = false; }

    void tick(f32 dt);
    u32 tickCount() const { return m_tickCount; }

private:
    ViewportMode m_mode = ViewportMode::View3D;
    ViewportCamera m_camera;
    std::string m_projectLabel;
    u32 m_width = 1;
    u32 m_height = 1;
    u32 m_tickCount = 0;
    bool m_needsResize = false;
};

} // namespace fuse::editor
