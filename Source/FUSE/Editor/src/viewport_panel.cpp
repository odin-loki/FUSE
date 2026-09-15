#include <fuse/editor/viewport_panel.hpp>

#include <algorithm>

namespace fuse::editor {

void ViewportPanel::setMode(ViewportMode mode) {
    m_mode = mode;
}

void ViewportPanel::setProjectLabel(std::string label) {
    m_projectLabel = std::move(label);
}

void ViewportPanel::setDimensions(u32 width, u32 height) {
    if (width == m_width && height == m_height) {
        return;
    }
    m_width = std::max(1u, width);
    m_height = std::max(1u, height);
    m_needsResize = true;
}

void ViewportPanel::tick(f32 dt) {
    if (m_camera.isFlying) {
        const f32 move = m_camera.moveSpeed * dt;
        m_camera.positionX += move;
    }
    ++m_tickCount;
}

} // namespace fuse::editor
