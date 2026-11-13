// Adapters from the path API of the gui-framework branch (which fuse_ui_extras was written against) to main's
// fuse/ui/path.hpp + fuse/ui/draw_list.hpp. Private to fuse_ui_extras. The branch API, for reference:
//   Path::circle / rect / append, Path::arcTo(Vec2 radii, degrees, large, sweep, to),
//   DrawList::path(path, fill, evenOdd, stroke, strokeWidth), Gradient::at(t).
#pragma once

#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/path.hpp"

#include <algorithm>
#include <span>

namespace fuse::ui::extras_compat {

inline Path& circle(Path& p, Vec2 centre, f32 r) { return p.addCircle(centre, r); }
inline Path& arcTo(Path& p, Vec2 radii, f32 rotationDeg, bool largeArc, bool sweep, Vec2 to) {
    return p.arcTo(radii.x, radii.y, rotationDeg * 0.017453292519943295f, largeArc, sweep, to);
}
inline Path& append(Path& p, const Path& other) { return p.addPath(other); }

/// The branch's DrawList::path: fills `p` (fill.a == 0: no fill), then strokes it (strokeWidth > 0), centred on
/// the outline with round joins and caps.
inline void drawPath(DrawList& dl, const Path& p, Color fill, bool evenOdd = false, Color stroke = {}, f32 strokeWidth = 0.f) {
    if (p.empty()) return;
    if (fill.a > 0.f) dl.fillPath(p, Brush(fill), evenOdd ? FillRule::EvenOdd : FillRule::NonZero);
    if (strokeWidth > 0.f && stroke.a > 0.f) {
        StrokeStyle s;
        s.width = strokeWidth;
        s.join = LineJoin::Round;
        s.cap = LineCap::Round;
        dl.strokePath(p, s, Brush(stroke));
    }
}

/// The branch's Gradient::at: colour of a stop list at `t` (clamped to the first / last stop, straight-alpha lerp).
inline Color stopsAt(std::span<const GradientStop> stops, f32 t) {
    if (stops.empty()) return {};
    if (t <= stops.front().offset) return stops.front().color;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].offset) {
            const GradientStop& a = stops[i - 1];
            const GradientStop& b = stops[i];
            const f32 span = b.offset - a.offset;
            const f32 k = span > 0.f ? (t - a.offset) / span : 1.f;
            return {a.color.r + (b.color.r - a.color.r) * k, a.color.g + (b.color.g - a.color.g) * k,
                    a.color.b + (b.color.b - a.color.b) * k, a.color.a + (b.color.a - a.color.a) * k};
        }
    }
    return stops.back().color;
}

} // namespace fuse::ui::extras_compat
