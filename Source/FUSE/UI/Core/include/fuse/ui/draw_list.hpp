// FUSE UI (UI2): the backend-neutral draw list (FUSE_UI_PLAN.md §4.9). No Vulkan types. Commands reference
// text in a shared character arena, so recording into a reused list allocates nothing once capacities settle.
// Text uses the built-in 5x7 font until fuse_text (UI3) lands.
#pragma once

#include <fuse/ui/geometry.hpp>

#include <string_view>
#include <vector>

namespace fuse::ui {

enum class DrawOp : u8 {
    FillRect,  ///< rect, color, radius (rounded, analytic AA)
    StrokeRect,///< rect, color, radius, width (border inside the rect)
    Text,      ///< rect.x/y = top-left, color, scale = integer glyph scale, text = arena range
    PushClip,  ///< rect (intersected with the current clip)
    PopClip,
};

struct DrawCmd {
    DrawOp op = DrawOp::FillRect;
    Rect rect{};
    Color color{};
    f32 radius = 0.f;
    f32 width = 0.f;
    u32 textOffset = 0;
    u32 textLength = 0;
    u32 scale = 1;
};

class DrawList {
public:
    void clear() {
        m_cmds.clear();
        m_text.clear();
    }
    void fillRect(const Rect& r, Color c, f32 radius = 0.f) {
        if (c.a == 0 || r.empty()) return;
        m_cmds.push_back({DrawOp::FillRect, r, c, radius});
    }
    void strokeRect(const Rect& r, Color c, f32 width, f32 radius = 0.f) {
        if (c.a == 0 || width <= 0.f || r.empty()) return;
        m_cmds.push_back({DrawOp::StrokeRect, r, c, radius, width});
    }
    void text(Vec2 topLeft, std::string_view s, Color c, u32 scale = 1) {
        if (s.empty() || c.a == 0) return;
        DrawCmd cmd{DrawOp::Text, {topLeft.x, topLeft.y, 0.f, 0.f}, c};
        cmd.textOffset = u32(m_text.size());
        cmd.textLength = u32(s.size());
        cmd.scale = scale == 0 ? 1 : scale;
        m_text.insert(m_text.end(), s.begin(), s.end());
        m_cmds.push_back(cmd);
    }
    void pushClip(const Rect& r) { m_cmds.push_back({DrawOp::PushClip, r}); }
    void popClip() { m_cmds.push_back({DrawOp::PopClip}); }

    const std::vector<DrawCmd>& commands() const { return m_cmds; }
    std::string_view textOf(const DrawCmd& c) const { return {m_text.data() + c.textOffset, c.textLength}; }
    usize capacityBytes() const { return m_cmds.capacity() * sizeof(DrawCmd) + m_text.capacity(); }

private:
    std::vector<DrawCmd> m_cmds;
    std::vector<char> m_text;
};

/// Measure a run of built-in-font text at `scale` (single line).
Vec2 measureText(std::string_view s, u32 scale);

} // namespace fuse::ui
