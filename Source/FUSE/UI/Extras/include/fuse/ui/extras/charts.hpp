// FUSE Charts: 2D graphs and charts (Qt Charts / Qt Graphs 2D parity and more) and the 3D graphs (Qt Graphs 3D /
// Data Visualization: bars, scatter, surface, line, area) behind one public API, `fuse::gui::charts`.
//
// A Chart is plain data (axes, series, theme). charts::render() lays it out and emits DrawList commands only (paths,
// rects, gradients, glyphs), so a chart draws through the Vulkan draw-list kernel on the GPU path and through the CPU
// rasteriser otherwise, with the same pixels (parity gate fuse_ui_vk_parity "charts"). Large series are decimated
// per plot pixel column before anything is emitted (min/max envelope on forge::lane SIMD, or LTTB), so a 1M-point
// series costs O(visible pixels) draw commands. Streaming series keep a bounded ring (Series::capacity).
//
// chartView() is the retained widget: hover tooltips, wheel zoom, drag pan, rubber-band zoom (Shift+drag), click
// selection, legend toggles, keyboard point navigation, accessible summary, entry animation, ItemModel binding and
// image export. Colours default to the widget's computed style (text colour, background) so CSS themes apply.
//
// Pending (FORGE text / locale / datetime wrapper): AxisType::DateTime is accepted but labelled numerically; label
// formatting is printf-style and locale independent.
#pragma once

#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/extras/datavis3d.hpp"
#include "fuse/ui/ui.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <cmath>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::charts {

enum class SeriesType : u8 {
    Line, Spline, StepLine, Scatter, Area, StackedArea, PercentArea,
    Bar, StackedBar, PercentBar, HorizontalBar, HorizontalStackedBar, HorizontalPercentBar,
    Pie, Donut, Candlestick, Ohlc, BoxPlot, Histogram, Heatmap, Bubble,
    // beyond Qt Charts
    Waterfall, ///< category bars floating on the running total; `totals` lists indices drawn as the subtotal
    Contour,   ///< isolines (marching squares) of a rows x cols grid in y, `levels` lines (contourFill: heatmap under)
    Radar,     ///< spider chart: y = one value per spoke, labels = spoke names; all radar series share one web
    Treemap,   ///< squarified treemap of y (leaf sizes) with labels
    Sankey,    ///< flow diagram: labels = node names, links = (source, target, value); columns by longest path
    Gauge,     ///< dial: y[0] in [gaugeMin, gaugeMax], colormap = coloured bands (offsets 0..1 along the dial)
    Funnel,    ///< stages y (top to bottom) as centred trapezoids, labels = stage names
};
enum class AxisType : u8 { Value, Log, Category, DateTime /* pending: labelled as numbers */ };
enum class AxisEdge : u8 { Bottom, Left, Top, Right };
enum class LegendPos : u8 { None, Top, Bottom, Left, Right };
enum class Decimation : u8 { Auto, None, MinMax, Lttb };

struct Axis {
    AxisType type = AxisType::Value;
    AxisEdge edge = AxisEdge::Bottom;
    f32 min = NAN, max = NAN;          ///< fixed range (NaN: from the data, rounded to nice ticks)
    f32 viewMin = NAN, viewMax = NAN;  ///< zoom / pan window inside the range (NaN: whole range)
    f32 logBase = 10.f;
    std::vector<std::string> categories; ///< Category: label i sits at value i
    std::string title;
    std::string labelFormat = "%g";      ///< printf format for value labels
    u32 tickCount = 5;
    bool grid = true, visible = true, reversed = false, nice = true;
};

struct Candle { f32 x, open, high, low, close; };
struct Link { u32 source, target; f32 value; };
struct BoxItem { f32 x, lower, q1, median, q3, upper; std::vector<f32> outliers = {}; };

struct Series {
    SeriesType type = SeriesType::Line;
    std::string name;
    Color color{};             ///< a == 0: theme palette
    u32 xAxis = 0, yAxis = 1;  ///< indices into Chart::axes
    bool visible = true;
    /// Line / scatter / area / bar / bubble: points (x[i], y[i]); bars on a category axis use x = category index.
    /// Histogram: y holds the raw samples. Pie / donut: y holds slice values, `labels` the slice names.
    std::vector<f32> x, y;
    std::vector<f32> size;     ///< bubble radii (data units of `bubbleScale` px)
    std::vector<std::string> labels;
    std::vector<Candle> candles;
    std::vector<BoxItem> boxes;
    u32 rows = 0, cols = 0;    ///< heatmap grid: y holds rows * cols values row-major; x range = cols, y range = rows
    std::vector<GradientStop> colormap; ///< heatmap (empty: viridis-like)
    u32 bins = 0;              ///< histogram bins (0: Sturges)
    f32 lineWidth = 2.f, markerSize = 0.f, barWidth = 0.8f, holeSize = 0.5f, bubbleScale = 1.f;
    Decimation decimation = Decimation::Auto;
    std::vector<Link> links;   ///< sankey flows (node indices into labels)
    /// Sankey node order within a column: nodeOrder lists node indices top to bottom (missing ones after, by
    /// index); empty: sankeyAutoOrder sorts by the flow-weighted barycentre of the neighbours (fewer crossings),
    /// else index order.
    std::vector<u32> nodeOrder;
    bool sankeyAutoOrder = true;
    /// Treemap hierarchy: parents[i] = parent item of item i (-1 or missing: a root). Groups weigh their leaves'
    /// sum (their own y is ignored), draw a header with their label and nest their children.
    std::vector<i32> parents;
    std::vector<u32> totals;   ///< waterfall: indices shown as the running total (their y is ignored)
    u32 levels = 8;            ///< contour lines
    bool contourFill = false;  ///< contour: paint the heatmap under the isolines
    f32 gaugeMin = 0.f, gaugeMax = 100.f;
    f32 radarMax = NAN;        ///< radar outer ring value (NaN: nice max of all radar series)
    /// Streaming: append() keeps at most `capacity` points (0 = unbounded), dropping the oldest (amortised O(1)).
    std::size_t capacity = 0;
    void append(f32 px, f32 py);
    void append(std::span<const f32> px, std::span<const f32> py);
    std::size_t count() const { return y.size() - m_head; }
    std::span<const f32> xs() const { return std::span<const f32>(x).subspan(std::min(m_head, x.size())); }
    std::span<const f32> ys() const { return std::span<const f32>(y).subspan(m_head); }
    void clear() { x.clear(), y.clear(), m_head = 0; }
    std::size_t m_head = 0; ///< ring start (internal)
};

struct Theme {
    Color background, plot, grid, axis, text, tooltipBg, tooltipText, selection;
    std::vector<Color> palette;
    f32 fontSize = 12.f;
    static Theme dark();
    static Theme light();
    /// Theme from a computed style: text = color, background = background (palette from dark / light by luminance).
    static Theme fromStyle(const ComputedStyle& s);
};

struct Selected { u32 series, index; };

struct Chart {
    std::string title;
    std::vector<Axis> axes; ///< default: [0] bottom value axis, [1] left value axis
    std::vector<Series> series;
    LegendPos legend = LegendPos::Top;
    Theme theme = Theme::dark();
    bool polar = false;     ///< QPolarChart: axis 0 is angular (one turn = its range), axis 1 radial
    bool animate = true;    ///< entry / data-change animation in the widget
    f32 progress = 1.f;     ///< animation 0..1 (bars grow, lines reveal, pies sweep)
    i32 hoverSeries = -1, hoverIndex = -1;
    std::vector<Selected> selection;
    Chart();
    Series& add(SeriesType type, std::string_view name = {});
};

/// Text hooks (the widget passes the UiContext's font). Null: labels are skipped, sizes estimated.
struct TextFns {
    std::function<f32(std::string_view, f32 px)> measure;
    std::function<void(DrawList&, std::string_view, Rect, Color, f32 px, TextAlignment)> draw;
};
struct Layout {
    Rect plot, legend, title;
    std::vector<Rect> legendItems; ///< per series (empty rect when not shown)
};
struct RenderStats {
    std::size_t inputPoints = 0, drawnPoints = 0, commands = 0;
};
struct Hit {
    i32 series = -1, index = -1;
    f32 x = 0, y = 0;   ///< data values
    Vec2 screen{};
    i32 legendItem = -1;
};

Layout layout(const Chart& c, Rect bounds, const TextFns* text = nullptr);
void render(const Chart& c, DrawList& dl, Rect bounds, const TextFns* text = nullptr, RenderStats* stats = nullptr);
Hit hitTest(const Chart& c, Rect bounds, Vec2 pos, f32 radius = 10.f, const TextFns* text = nullptr);
/// Resolved [lo, hi] of every axis (data, fixed or zoomed); `full` ignores the zoom window.
std::vector<std::pair<f32, f32>> axisRanges(const Chart& c, bool full = false);
/// Axis value <-> pixel along [a, b] (log / reversed aware).
f32 toPixel(const Axis& ax, f32 lo, f32 hi, f32 v, f32 a, f32 b);
f32 fromPixel(const Axis& ax, f32 lo, f32 hi, f32 p, f32 a, f32 b);
/// Tick values in [lo, hi] (nice steps; log: powers of the base; category: every index that fits).
std::vector<f32> ticks(const Axis& ax, f32 lo, f32 hi);
std::string formatTick(const Axis& ax, f32 v);
/// Zoom `axis` by `factor` (< 1 zooms in) around data value `centre`; pan by data delta. Reset clears the windows.
void zoom(Chart& c, u32 axis, f32 factor, f32 centre);
void pan(Chart& c, u32 axis, f32 delta);
void zoomTo(Chart& c, u32 axis, f32 lo, f32 hi);
void resetZoom(Chart& c);
std::string tooltip(const Chart& c, u32 series, u32 index);
/// Accessible summary ("Line chart, 2 series: ... ") used as the widget's a11y name.
std::string describe(const Chart& c);

/// Data-change transitions: `out` = `to` with every per-point value (x, y, size, candles, boxes, links, totals of
/// matching series) blended from `from` at t in 0..1. Points new in `to` grow from the last old point (0 for bar-like
/// kinds); series whose type changed, or that are new in `to`, are taken from `to` unchanged. The widget runs this on
/// chartChanged(view, true) when the structure (series count and types) is unchanged, so axes, bars, slices, gauges
/// and flows move to their new values instead of replaying the entry animation.
void blend(const Chart& from, const Chart& to, f32 t, Chart& out);

// ---- decimation (also usable on their own) ----
/// Index range [first, last) of sorted `x` overlapping [lo, hi] plus one neighbour each side.
std::pair<std::size_t, std::size_t> visibleRange(std::span<const f32> x, f32 lo, f32 hi);
/// Min / max of `v` (forge::lane SIMD where available).
void minMax(std::span<const f32> v, f32& lo, f32& hi);
/// Per-bucket first / min / max / last indices over `buckets` equal x slices (sorted x): keeps every extreme, so the
/// drawn envelope is pixel-identical to the full series at one bucket per pixel column.
void decimateMinMax(std::span<const f32> x, std::span<const f32> y, u32 buckets, std::vector<u32>& out);
/// Largest-Triangle-Three-Buckets: `threshold` representative indices (first and last kept).
void lttb(std::span<const f32> x, std::span<const f32> y, u32 threshold, std::vector<u32>& out);

// ---- image export ----
/// Renders the chart on the CPU (glyph atlas from `ui`) into `out` (w x h).
void renderImage(UiContext& ui, const Chart& c, int w, int h, Image& out);
/// Writes RGBA8 `img` as PNG (stored deflate). Returns false on I/O failure.
bool writePng(const Image& img, const std::string& path);

// ---- widget ----
/// QChartView: hover -> tooltip + node value = point index (ValueChanged), click toggles selection (Toggled),
/// wheel zooms x (Ctrl: y too) around the pointer, drag pans, Shift+drag rubber-band zooms, legend click toggles a
/// series, Left / Right / Home / End walk points (screen readers hear the tooltip), Escape resets the zoom.
NodeHandle chartView(UiContext& ui, NodeHandle parent, std::string_view id = {});
/// The widget's chart (call chartChanged after editing).
Chart* chartOf(UiContext& ui, NodeHandle view);
/// Repaints and refreshes the accessible summary. animateChange (and Chart::animate): when the series structure is
/// unchanged since the last chartChanged the view tweens from the previous values to the new ones (blend, 0.35 s
/// ease-out); otherwise it replays the entry animation. Series over 200k points snap (no snapshot is kept).
void chartChanged(UiContext& ui, NodeHandle view, bool animateChange = false);
/// The chart as painted this frame (mid-transition: the blended values; otherwise chartOf()).
const Chart* chartShown(const UiContext& ui, NodeHandle view);
/// Hovered point (series, index), -1 none.
Hit chartHovered(const UiContext& ui, NodeHandle view);
std::string chartTooltip(const UiContext& ui, NodeHandle view);
/// Model binding (QVXYModelMapper / QBarModelMapper): column `xColumn` (-1: row index) against each `yColumns`
/// column, one series each (named by the header). Re-read with chartModelChanged after the model changes.
struct ModelMapping {
    const widgets::ItemModel* model = nullptr;
    i32 xColumn = 0;
    std::vector<u32> yColumns;
    SeriesType type = SeriesType::Line;
};
void bindModel(UiContext& ui, NodeHandle view, ModelMapping m);
void chartModelChanged(UiContext& ui, NodeHandle view);
bool exportImage(UiContext& ui, NodeHandle view, const std::string& pngPath, int w = 0, int h = 0);

// ---- 3D graphs (Qt Graphs 3D / Data Visualization): the dv3d scene behind the charts API ----
// Bars, scatter, surface, line and area in an orbit-camera box with 3D axes (ranges, titles, tick labels), hover
// picking, click selection, theming from a charts::Theme, and large-data thinning (screen-cell scatter, sub-pixel
// line steps, strided grids). Painted as BSP-ordered DrawList paths: the same GPU / CPU path as the 2D charts.
using Vec3 = dv3d::Vec3;
using Kind3D = dv3d::Kind;
using Camera3D = dv3d::Camera;
using Axis3D = dv3d::Axis;
using Chart3D = dv3d::Scene;
using Stats3D = dv3d::Stats;
/// Colours a 3D chart from a 2D theme: background, grid, a palette-derived colormap and series colours.
void applyTheme(Chart3D& c, const Theme& t);
/// Accessible summary ("3D surface, 16 x 16 values, 0 to 1").
std::string describe(const Chart3D& c);
void render(const Chart3D& c, DrawList& dl, Rect bounds, const Theme& t, std::vector<dv3d::ProjectedItem>* items = nullptr, Stats3D* stats = nullptr);
void renderImage(UiContext& ui, const Chart3D& c, const Theme& t, int w, int h, Image& out);
/// Q3DSurface / Q3DScatter / Q3DBars / 3D line / 3D area view: drag orbits, wheel zooms, arrows rotate, Home resets
/// the camera, hover tooltips, click / Ctrl+click selection (Toggled), Escape clears it. Themed from its computed style.
NodeHandle chart3dView(UiContext& ui, NodeHandle parent, Kind3D kind, std::string_view id = {});
Chart3D* chart3dOf(UiContext& ui, NodeHandle view);
/// Repaints and refreshes the accessible summary after editing chart3dOf(); with animateChange (and
/// Chart3D::animate) the data tweens from what is on screen when its structure is unchanged (dv3d::blend).
void chart3dChanged(UiContext& ui, NodeHandle view, bool animateChange = true);
/// The scene being painted (mid-transition: the blended data).
const Chart3D* chart3dShown(const UiContext& ui, NodeHandle view);
inline void blend(const Chart3D& from, const Chart3D& to, f32 t, Chart3D& out) { dv3d::blend(from, to, t, out); }
i32 chart3dHovered(const UiContext& ui, NodeHandle view);
std::vector<std::string> chart3dTooltip(UiContext& ui, NodeHandle view);
const std::vector<u32>& chart3dSelection(const UiContext& ui, NodeHandle view);
bool exportImage3d(UiContext& ui, NodeHandle view, const std::string& pngPath, int w = 0, int h = 0);

} // namespace fuse::ui::charts
