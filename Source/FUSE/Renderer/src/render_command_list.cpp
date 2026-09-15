#include <fuse/renderer/render_command_list.hpp>

namespace fuse::renderer {

void RenderCommandList::reset() {
    m_commands.clear();
}

void RenderCommandList::clear3D(float r, float g, float b) {
    RenderCommand cmd;
    cmd.kind = RenderCommandKind::Clear3D;
    cmd.clear3D.r = r;
    cmd.clear3D.g = g;
    cmd.clear3D.b = b;
    m_commands.push_back(cmd);
}

void RenderCommandList::drawSprite2D(float x, float y, float rotation, u8 r, u8 g, u8 b) {
    RenderCommand cmd;
    cmd.kind = RenderCommandKind::DrawSprite2D;
    cmd.drawSprite2D.x = x;
    cmd.drawSprite2D.y = y;
    cmd.drawSprite2D.rotation = rotation;
    cmd.drawSprite2D.r = r;
    cmd.drawSprite2D.g = g;
    cmd.drawSprite2D.b = b;
    m_commands.push_back(cmd);
}

} // namespace fuse::renderer
