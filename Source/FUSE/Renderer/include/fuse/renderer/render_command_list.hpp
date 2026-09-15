#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

enum class RenderCommandKind : u8 {
    Clear3D = 1,
    DrawSprite2D = 2,
};

struct Clear3DCommand {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
};

struct DrawSprite2DCommand {
    float x = 0.f;
    float y = 0.f;
    float rotation = 0.f;
    u8 r = 255;
    u8 g = 255;
    u8 b = 255;
};

struct RenderCommand {
    RenderCommandKind kind = RenderCommandKind::Clear3D;
    Clear3DCommand clear3D{};
    DrawSprite2DCommand drawSprite2D{};
};

/// Per-frame render commands produced on the game/render thread (architecture §5.3).
/// Jobs write staging data; the render thread merges into this list before RHI submit.
class RenderCommandList {
public:
    void reset();
    void clear3D(float r, float g, float b);
    void drawSprite2D(float x, float y, float rotation, u8 r, u8 g, u8 b);

    const std::vector<RenderCommand>& commands() const { return m_commands; }
    u32 commandCount() const { return static_cast<u32>(m_commands.size()); }

private:
    std::vector<RenderCommand> m_commands;
};

} // namespace fuse::renderer
