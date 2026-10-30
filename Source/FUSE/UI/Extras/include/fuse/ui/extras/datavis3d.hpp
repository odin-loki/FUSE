// 3D data visualisation basics (Qt Data Visualization parity: Q3DSurface, Q3DScatter, Q3DBars). The scene is
// projected on the CPU (orbit camera, perspective or orthographic), depth-sorted (painter's order, back faces culled)
// and emitted as DrawList paths with Lambert shading, so it draws through the Vulkan draw-list kernel when the app
// renders on Vulkan and through the CPU path rasteriser otherwise (bit-identical; parity case "dataviz3d").
// Not modelled: a depth buffer (intersecting surfaces resolve by face centroid), textures, per-pixel lighting.
#pragma once

#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/ui.hpp"

#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::dv3d {

struct Vec3 {
    f32 x = 0, y = 0, z = 0;
};
/// Surface / Bars / Area: a rows x cols grid. Scatter / Line: `points`. Area draws each row as a filled ribbon from 0
/// to its values along x (one series per row, Qt Graphs-style 3D area); Line joins `points` in order (a NaN point
/// breaks the line).
enum class Kind : u8 { Surface, Scatter, Bars, Line, Area };

/// Orbit camera: yaw around the vertical axis, pitch above the floor (degrees); zoom scales the view.
struct Camera {
    f32 yaw = -40.f, pitch = 25.f, zoom = 1.f;
    bool perspective = true;
};
/// Axis range (NaN = from the data) and title. Axis 0 = x (columns), 1 = y (values, up), 2 = z (rows).
struct Axis {
    f32 min = NAN, max = NAN;
    std::string title;
    u32 ticks = 4;
};

struct Scene {
    Kind kind = Kind::Surface;
    /// Surface / Bars: rows x cols values, row-major (row = z, column = x).
    u32 rows = 0, cols = 0;
    std::vector<f32> values;
    /// Scatter: points in data space, optional per-point colours (else the colormap by y).
    std::vector<Vec3> points;
    std::vector<Color> pointColors;
    f32 pointSize = 5.f;
    /// Value -> colour (offsets 0..1 over the y range). Empty: blue -> cyan -> green -> yellow -> red.
    std::vector<GradientStop> colormap;
    Color barColor{}; ///< a == 0: bars use the colormap
    bool wireframe = true, shading = true, grid = true;
    Axis axis[3];
    Camera camera;
    i32 highlight = -1; ///< item drawn highlighted (surface vertex, point or bar index)
    std::vector<u32> selection; ///< selected items (drawn highlighted; the widget toggles them on click)
    /// Line / Area colours (Area: per row; empty: palette-free colormap by row / value).
    std::vector<Color> seriesColors;
    f32 lineWidth = 2.f;
    /// Large data: Scatter / Line above `maxPoints` are thinned in screen space (one point per marker-sized pixel
    /// cell, nearest kept; lines drop sub-pixel steps), grids above maxGrid x maxGrid cells draw a strided sample.
    /// Picking still reports original indices.
    u32 maxPoints = 20000, maxGrid = 160;
    Color background{}; ///< a > 0: fills the target first (theme)
    Color gridColor{};  ///< a > 0: overrides render()'s gridColor (theme)
};
/// Render counters (large-data gates): items in the scene and items actually drawn.
struct Stats {
    std::size_t input = 0, drawn = 0;
};

/// A drawn item's screen position (picking): surface vertex / point / bar top centre.
struct ProjectedItem {
    u32 index;
    Vec2 screen;
    f32 depth; ///< larger = nearer the camera
};

/// Resolved data ranges (axis min / max or the data's).
void ranges(const Scene& s, f32 lo[3], f32 hi[3]);
/// Data point -> screen position in `target` (depth: larger is nearer).
Vec2 project(const Scene& s, Rect target, Vec3 data, f32* depth = nullptr);
/// Draws the scene into `dl` within `target`; `items` receives the pickable item positions.
void render(const Scene& s, DrawList& dl, Rect target, Color gridColor, std::vector<ProjectedItem>* items = nullptr, Stats* stats = nullptr);
/// Tick values of axis `a` (0 x, 1 y, 2 z) over the resolved range (nice steps, `Axis::ticks` of them at most + 1).
std::vector<f32> axisTicks(const Scene& s, int a);
/// One piece of a polygon in back-to-front paint order (bspOrder): `source` indexes the input, `pts` are the piece's
/// camera-space vertices (the whole polygon, or a part of it after a BSP split).
struct BspFragment {
    u32 source;
    std::vector<Vec3> pts;
    /// cut[i]: edge pts[i] -> pts[i + 1] lies on a BSP splitting plane (not an edge of the source polygon). Empty when
    /// the fragment is the whole polygon.
    std::vector<bool> cut = {};
};
/// Back-to-front paint order of camera-space polygons (x right, y up, z towards the viewer) by a BSP tree: polygons
/// straddling a splitting plane are cut, so interpenetrating and cyclically overlapping faces still paint correctly
/// (a depth sort cannot). Perspective: eye at (0, 0, eyeDist); orthographic: looking down -z. Used by render() for
/// surface quads and bar faces.
std::vector<BspFragment> bspOrder(const std::vector<std::vector<Vec3>>& polys, bool perspective, f32 eyeDist = 5.f);
/// Index of the item nearest `pos` within `radius` px (nearest the camera on ties), -1 none.
i32 pick(const std::vector<ProjectedItem>& items, Vec2 pos, f32 radius = 10.f);

} // namespace fuse::ui::dv3d

namespace fuse::ui::widgets {
/// Q3DSurface / Q3DScatter / Q3DBars widget: drag orbits, wheel zooms, arrows rotate, Home resets; hovering an
/// item sets the node value to its index and shows a tooltip (Qt: selectedPointChanged / selectedBarChanged).
NodeHandle dataViz3d(UiContext& ui, NodeHandle parent, dv3d::Kind kind, std::string_view id = {});
void dataViz3dSetGrid(UiContext& ui, NodeHandle w, u32 rows, u32 cols, std::span<const f32> values);
void dataViz3dSetPoints(UiContext& ui, NodeHandle w, std::span<const dv3d::Vec3> points, std::span<const Color> colors = {});
/// Direct access to the scene (call ui.invalidate(w) after changing it).
dv3d::Scene* dataViz3dScene(UiContext& ui, NodeHandle w);
void dataViz3dSetCamera(UiContext& ui, NodeHandle w, const dv3d::Camera& c);
dv3d::Camera dataViz3dCamera(const UiContext& ui, NodeHandle w);
/// Selected items (click toggles, Ctrl+click adds, Escape clears; Toggled signal: value = index, flag = selected).
const std::vector<u32>& dataViz3dSelection(const UiContext& ui, NodeHandle w);
/// Hovered item index, -1 none.
i32 dataViz3dHovered(const UiContext& ui, NodeHandle w);
/// Tooltip lines of the hovered item (empty when none).
std::vector<std::string> dataViz3dTooltip(UiContext& ui, NodeHandle w);
} // namespace fuse::ui::widgets
