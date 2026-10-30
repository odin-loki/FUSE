// FUSE Charts (2D): layout, axes, series painters, decimation, hit testing, widget. See charts.hpp.
#include "fuse/ui/extras/charts.hpp"
#include "charts_simd.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>

namespace fuse::ui::charts {

// ---------------- series data ----------------
void Series::append(f32 px, f32 py) {
    if (x.size() != y.size()) x.resize(y.size());
    x.push_back(px), y.push_back(py);
    if (capacity && count() > capacity) {
        ++m_head;
        if (m_head >= capacity) { // compact once per `capacity` drops: amortised O(1), storage stays contiguous
            x.erase(x.begin(), x.begin() + std::ptrdiff_t(m_head));
            y.erase(y.begin(), y.begin() + std::ptrdiff_t(m_head));
            m_head = 0;
        }
    }
}
void Series::append(std::span<const f32> px, std::span<const f32> py) {
    const std::size_t n = std::min(px.size(), py.size());
    for (std::size_t i = 0; i < n; ++i) append(px[i], py[i]);
}

Theme Theme::dark() {
    Theme t;
    t.background = Color::rgb(0x16181D), t.plot = Color::rgb(0x1C1F26), t.grid = Color::rgb(0xFFFFFF, 0.09f);
    t.axis = Color::rgb(0x9AA0AC), t.text = Color::rgb(0xD8DBE2), t.tooltipBg = Color::rgb(0x2A2E38, 0.96f);
    t.tooltipText = Color::rgb(0xF2F3F7), t.selection = Color::rgb(0xFFFFFF);
    for (u32 c : {0x4E9BFFu, 0xFF8A3Du, 0x3DDC97u, 0xF25F5Cu, 0xB48CFFu, 0xFFD23Fu, 0x2EC4B6u, 0xFF6FB5u}) t.palette.push_back(Color::rgb(c));
    return t;
}
Theme Theme::light() {
    Theme t;
    t.background = Color::rgb(0xFFFFFF), t.plot = Color::rgb(0xF7F8FA), t.grid = Color::rgb(0x000000, 0.08f);
    t.axis = Color::rgb(0x5A6070), t.text = Color::rgb(0x23262E), t.tooltipBg = Color::rgb(0x23262E, 0.94f);
    t.tooltipText = Color::rgb(0xFFFFFF), t.selection = Color::rgb(0x000000);
    for (u32 c : {0x1F6FEBu, 0xE8590Cu, 0x2F9E44u, 0xC92A2Au, 0x7048E8u, 0xF08C00u, 0x0C8599u, 0xD6336Cu}) t.palette.push_back(Color::rgb(c));
    return t;
}
Theme Theme::fromStyle(const ComputedStyle& s) {
    const f32 lum = 0.2126f * s.background.r + 0.7152f * s.background.g + 0.0722f * s.background.b;
    Theme t = (s.background.a > 0.f && lum > 0.5f) ? light() : dark();
    if (s.background.a > 0.f) {
        t.background = s.background;
        t.plot = Color{s.background.r * 0.94f + 0.03f, s.background.g * 0.94f + 0.03f, s.background.b * 0.94f + 0.03f, 1.f};
    }
    t.text = s.color;
    t.axis = s.color.withAlpha(s.color.a * 0.7f);
    t.fontSize = s.fontSize * 0.85f;
    return t;
}

Chart::Chart() {
    axes.resize(2);
    axes[0].edge = AxisEdge::Bottom;
    axes[1].edge = AxisEdge::Left;
}
Series& Chart::add(SeriesType type, std::string_view name) {
    Series& s = series.emplace_back();
    s.type = type, s.name = std::string(name);
    return s;
}

namespace {
bool isBar(SeriesType t) { return t >= SeriesType::Bar && t <= SeriesType::HorizontalPercentBar; }
bool isHBar(SeriesType t) { return t >= SeriesType::HorizontalBar && t <= SeriesType::HorizontalPercentBar; }
bool isStackedBar(SeriesType t) { return t == SeriesType::StackedBar || t == SeriesType::HorizontalStackedBar; }
bool isPercentBar(SeriesType t) { return t == SeriesType::PercentBar || t == SeriesType::HorizontalPercentBar; }
bool isPie(SeriesType t) { return t == SeriesType::Pie || t == SeriesType::Donut; }
bool isLineLike(SeriesType t) {
    return t == SeriesType::Line || t == SeriesType::Spline || t == SeriesType::StepLine || t == SeriesType::Area ||
           t == SeriesType::StackedArea || t == SeriesType::PercentArea || t == SeriesType::Scatter || t == SeriesType::Bubble;
}
bool isPointSeries(SeriesType t) { return isLineLike(t) && t != SeriesType::StackedArea && t != SeriesType::PercentArea; }
Color seriesColor(const Chart& c, u32 i) {
    const Series& s = c.series[i];
    if (s.color.a > 0.f) return s.color;
    return c.theme.palette.empty() ? Color::rgb(0x4E9BFF) : c.theme.palette[i % c.theme.palette.size()];
}
Color sliceColor(const Chart& c, u32 i) { return c.theme.palette.empty() ? Color::rgb(0x4E9BFF) : c.theme.palette[i % c.theme.palette.size()]; }
Color mix(Color a, Color b, f32 t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t}; }
Color sampleMap(const std::vector<GradientStop>& m, f32 t) {
    static const std::vector<GradientStop> viridis = {{0.f, Color::rgb(0x440154)}, {0.25f, Color::rgb(0x3B528B)}, {0.5f, Color::rgb(0x21918C)},
                                                     {0.75f, Color::rgb(0x5EC962)}, {1.f, Color::rgb(0xFDE725)}};
    const auto& s = m.empty() ? viridis : m;
    t = std::clamp(t, 0.f, 1.f);
    if (t <= s.front().offset) return s.front().color;
    for (std::size_t i = 1; i < s.size(); ++i)
        if (t <= s[i].offset) {
            const f32 d = s[i].offset - s[i - 1].offset;
            return mix(s[i - 1].color, s[i].color, d > 0.f ? (t - s[i - 1].offset) / d : 0.f);
        }
    return s.back().color;
}
f32 fontPx(const Chart& c) { return c.theme.fontSize; }
f32 measure(const TextFns* t, std::string_view s, f32 px) { return t && t->measure ? t->measure(s, px) : f32(s.size()) * px * 0.55f; }
void text(const TextFns* t, DrawList& dl, std::string_view s, Rect r, Color c, f32 px, TextAlignment a) {
    if (t && t->draw && !s.empty()) t->draw(dl, s, r, c, px, a);
}
bool horizontalEdge(AxisEdge e) { return e == AxisEdge::Bottom || e == AxisEdge::Top; }
bool onlyPies(const Chart& c) {
    bool any = false;
    for (const Series& s : c.series) { if (!isPie(s.type)) return false; any = true; }
    return any;
}

struct HistBins { f32 lo = 0, w = 1; std::vector<f32> counts; };
HistBins histogram(const Series& s) {
    HistBins h;
    const auto v = s.ys();
    if (v.empty()) return h;
    f32 lo, hi;
    minMax(v, lo, hi);
    const u32 n = s.bins ? s.bins : u32(std::ceil(std::log2(f32(v.size())) + 1.f));
    h.lo = lo;
    h.w = hi > lo ? (hi - lo) / f32(n) : 1.f;
    h.counts.assign(n, 0.f);
    for (f32 x : v) h.counts[std::min<std::size_t>(n - 1, std::size_t((x - lo) / h.w))] += 1.f;
    return h;
}
struct Stack { std::vector<f32> pos, neg, total; };
Stack stackFor(const Chart& c, SeriesType group, std::size_t n) {
    Stack st;
    st.pos.assign(n, 0.f), st.neg.assign(n, 0.f), st.total.assign(n, 0.f);
    for (const Series& s : c.series)
        if (s.visible && s.type == group)
            for (std::size_t i = 0; i < std::min(n, s.count()); ++i) st.total[i] += std::abs(s.ys()[i]);
    return st;
}
std::size_t maxCount(const Chart& c, SeriesType t) {
    std::size_t n = 0;
    for (const Series& s : c.series) if (s.visible && s.type == t) n = std::max(n, s.count());
    return n;
}
f32 xAt(const Series& s, std::size_t i) { const auto xs = s.xs(); return xs.size() == s.count() ? xs[i] : f32(i); }
f32 niceStep(f32 span, u32 count) {
    const f32 raw = span / f32(std::max(1u, count - 1));
    const f32 mag = std::pow(10.f, std::floor(std::log10(raw)));
    const f32 r = raw / mag;
    return (r <= 1.f ? 1.f : r <= 2.f ? 2.f : r <= 2.5f ? 2.5f : r <= 5.f ? 5.f : 10.f) * mag;
}
} // namespace

// ---------------- ranges and ticks ----------------
std::vector<std::pair<f32, f32>> axisRanges(const Chart& c, bool full) {
    const std::size_t na = c.axes.size();
    std::vector<f32> lo(na, INFINITY), hi(na, -INFINITY);
    std::vector<u8> tight(na, 0); // slot axes (bar categories, heatmap cells, candles, boxes): no nice padding
    auto slot = [&](u32 a) { if (a < na) tight[a] = 1; };
    auto acc = [&](u32 a, f32 v) { if (a < na && std::isfinite(v)) lo[a] = std::min(lo[a], v), hi[a] = std::max(hi[a], v); };
    for (const Series& s : c.series) {
        if (!s.visible || isPie(s.type)) continue;
        const bool h = isHBar(s.type);
        const u32 cat = h ? s.yAxis : s.xAxis, val = h ? s.xAxis : s.yAxis;
        if (s.type == SeriesType::Heatmap) {
            acc(s.xAxis, -0.5f), acc(s.xAxis, f32(s.cols) - 0.5f), acc(s.yAxis, -0.5f), acc(s.yAxis, f32(s.rows) - 0.5f);
            slot(s.xAxis), slot(s.yAxis);
        } else if (s.type == SeriesType::Candlestick || s.type == SeriesType::Ohlc) {
            slot(s.xAxis);
            for (const Candle& k : s.candles) acc(s.xAxis, k.x - 0.5f), acc(s.xAxis, k.x + 0.5f), acc(s.yAxis, k.low), acc(s.yAxis, k.high);
        } else if (s.type == SeriesType::BoxPlot) {
            slot(s.xAxis);
            for (const BoxItem& b : s.boxes) {
                acc(s.xAxis, b.x - 0.5f), acc(s.xAxis, b.x + 0.5f), acc(s.yAxis, b.lower), acc(s.yAxis, b.upper);
                for (f32 o : b.outliers) acc(s.yAxis, o);
            }
        } else if (s.type == SeriesType::Histogram) {
            const HistBins hb = histogram(s);
            acc(s.xAxis, hb.lo), acc(s.xAxis, hb.lo + hb.w * f32(hb.counts.size())), acc(s.yAxis, 0.f);
            for (f32 v : hb.counts) acc(s.yAxis, v);
        } else if (isBar(s.type)) {
            slot(cat);
            for (std::size_t i = 0; i < s.count(); ++i) acc(cat, xAt(s, i) - 0.5f), acc(cat, xAt(s, i) + 0.5f);
            acc(val, 0.f);
            if (isPercentBar(s.type)) acc(val, 100.f);
            else if (isStackedBar(s.type)) {
                Stack st = stackFor(c, s.type, maxCount(c, s.type));
                for (const Series& o : c.series)
                    if (o.visible && o.type == s.type)
                        for (std::size_t i = 0; i < o.count() && i < st.pos.size(); ++i) (o.ys()[i] >= 0 ? st.pos[i] : st.neg[i]) += o.ys()[i];
                for (std::size_t i = 0; i < st.pos.size(); ++i) acc(val, st.pos[i]), acc(val, st.neg[i]);
            } else if (s.count()) {
                f32 a, b;
                minMax(s.ys(), a, b);
                acc(val, a), acc(val, b);
            }
        } else {
            const auto xs = s.xs(), ys = s.ys();
            if (xs.size() == ys.size() && !xs.empty()) { f32 a, b; minMax(xs, a, b); acc(s.xAxis, a), acc(s.xAxis, b); }
            else if (!ys.empty()) acc(s.xAxis, 0.f), acc(s.xAxis, f32(ys.size() - 1));
            if (s.type == SeriesType::PercentArea) { acc(s.yAxis, 0.f), acc(s.yAxis, 100.f); continue; }
            if (s.type == SeriesType::StackedArea) {
                const std::size_t n = maxCount(c, s.type);
                std::vector<f32> tot(n, 0.f);
                for (const Series& o : c.series) if (o.visible && o.type == s.type) for (std::size_t i = 0; i < o.count() && i < n; ++i) tot[i] += o.ys()[i];
                for (f32 v : tot) acc(s.yAxis, v);
                acc(s.yAxis, 0.f);
                continue;
            }
            if (!ys.empty()) { f32 a, b; minMax(ys, a, b); acc(s.yAxis, a), acc(s.yAxis, b); }
            if (s.type == SeriesType::Area) acc(s.yAxis, 0.f);
            if (s.type == SeriesType::Bubble && !s.size.empty()) { // pad by the largest bubble's radius share
                const f32 pad = (hi[s.yAxis] - lo[s.yAxis]) * 0.08f;
                acc(s.yAxis, lo[s.yAxis] - pad), acc(s.yAxis, hi[s.yAxis] + pad);
            }
        }
    }
    std::vector<std::pair<f32, f32>> out(na);
    for (std::size_t a = 0; a < na; ++a) {
        const Axis& ax = c.axes[a];
        f32 l = lo[a], h = hi[a];
        if (ax.type == AxisType::Category && !ax.categories.empty()) l = std::min(std::isfinite(l) ? l : 0.f, -0.5f), h = std::max(std::isfinite(h) ? h : 0.f, f32(ax.categories.size()) - 0.5f);
        if (!std::isfinite(l) || !std::isfinite(h)) l = 0.f, h = 1.f;
        if (ax.type == AxisType::Log) { l = std::max(l, 1e-30f); h = std::max(h, l * ax.logBase); }
        if (h <= l) { const f32 d = std::abs(l) > 0 ? std::abs(l) * 0.1f : 1.f; l -= d, h += d; }
        if (ax.nice && !tight[a] && (ax.type == AxisType::Value || ax.type == AxisType::DateTime)) {
            const f32 step = niceStep(h - l, ax.tickCount);
            l = std::floor(l / step + 1e-4f) * step, h = std::ceil(h / step - 1e-4f) * step;
        }
        if (ax.type == AxisType::Log && ax.nice) {
            const f32 lb = std::log(ax.logBase);
            l = std::pow(ax.logBase, std::floor(std::log(l) / lb + 1e-4f)), h = std::pow(ax.logBase, std::ceil(std::log(h) / lb - 1e-4f));
        }
        if (std::isfinite(ax.min)) l = ax.min;
        if (std::isfinite(ax.max)) h = ax.max;
        if (!full && std::isfinite(ax.viewMin) && std::isfinite(ax.viewMax) && ax.viewMax > ax.viewMin) l = ax.viewMin, h = ax.viewMax;
        out[a] = {l, h};
    }
    return out;
}

f32 toPixel(const Axis& ax, f32 lo, f32 hi, f32 v, f32 a, f32 b) {
    f32 t;
    if (ax.type == AxisType::Log) {
        const f32 l = std::log(std::max(lo, 1e-30f)), h = std::log(std::max(hi, 1e-30f));
        t = (std::log(std::max(v, 1e-30f)) - l) / (h - l);
    } else t = (v - lo) / (hi - lo);
    if (ax.reversed) t = 1.f - t;
    return a + (b - a) * t;
}
f32 fromPixel(const Axis& ax, f32 lo, f32 hi, f32 p, f32 a, f32 b) {
    f32 t = (p - a) / (b - a);
    if (ax.reversed) t = 1.f - t;
    if (ax.type == AxisType::Log) {
        const f32 l = std::log(std::max(lo, 1e-30f)), h = std::log(std::max(hi, 1e-30f));
        return std::exp(l + (h - l) * t);
    }
    return lo + (hi - lo) * t;
}
std::vector<f32> ticks(const Axis& ax, f32 lo, f32 hi) {
    std::vector<f32> t;
    if (!(hi > lo)) return t;
    if (ax.type == AxisType::Category) {
        const i32 step = std::max(1, i32(std::ceil((hi - lo) / 12.f)));
        for (i32 i = std::max(0, i32(std::ceil(lo))); f32(i) <= hi; i += step) t.push_back(f32(i));
        return t;
    }
    if (ax.type == AxisType::Log) {
        const f32 lb = std::log(ax.logBase);
        for (f32 e = std::ceil(std::log(lo) / lb - 1e-4f); e <= std::floor(std::log(hi) / lb + 1e-4f) && t.size() < 40; e += 1.f) t.push_back(std::pow(ax.logBase, e));
        return t;
    }
    const f32 step = niceStep(hi - lo, ax.tickCount);
    for (f32 v = std::ceil(lo / step - 1e-4f) * step; v <= hi + step * 1e-4f && t.size() < 64; v += step) t.push_back(std::abs(v) < step * 1e-5f ? 0.f : v);
    return t;
}
std::string formatTick(const Axis& ax, f32 v) {
    if (ax.type == AxisType::Category) {
        const long i = std::lround(v);
        return i >= 0 && std::size_t(i) < ax.categories.size() ? ax.categories[std::size_t(i)] : std::to_string(i);
    }
    char b[64];
    std::snprintf(b, sizeof b, ax.labelFormat.c_str(), double(v));
    return b;
}

// ---------------- zoom ----------------
void zoomTo(Chart& c, u32 a, f32 lo, f32 hi) {
    if (a >= c.axes.size() || !(hi > lo)) return;
    c.axes[a].viewMin = lo, c.axes[a].viewMax = hi;
}
void zoom(Chart& c, u32 a, f32 factor, f32 centre) {
    if (a >= c.axes.size()) return;
    const auto r = axisRanges(c)[a];
    if (c.axes[a].type == AxisType::Log) {
        const f32 l = std::log(r.first), h = std::log(r.second), m = std::log(std::max(centre, 1e-30f));
        zoomTo(c, a, std::exp(m + (l - m) * factor), std::exp(m + (h - m) * factor));
    } else zoomTo(c, a, centre + (r.first - centre) * factor, centre + (r.second - centre) * factor);
}
void pan(Chart& c, u32 a, f32 d) {
    if (a >= c.axes.size()) return;
    const auto r = axisRanges(c)[a];
    zoomTo(c, a, r.first + d, r.second + d);
}
void resetZoom(Chart& c) { for (Axis& a : c.axes) a.viewMin = a.viewMax = NAN; }

// ---------------- decimation ----------------
std::pair<std::size_t, std::size_t> visibleRange(std::span<const f32> x, f32 lo, f32 hi) {
    std::size_t a = std::size_t(std::lower_bound(x.begin(), x.end(), lo) - x.begin());
    std::size_t b = std::size_t(std::upper_bound(x.begin(), x.end(), hi) - x.begin());
    if (a > 0) --a;
    if (b < x.size()) ++b;
    return {a, std::max(a, b)};
}
void minMax(std::span<const f32> v, f32& lo, f32& hi) {
    if (v.empty()) { lo = hi = 0.f; return; }
    simd::minMaxF32(v.data(), v.size(), lo, hi);
}
void decimateMinMax(std::span<const f32> x, std::span<const f32> y, u32 buckets, std::vector<u32>& out) {
    out.clear();
    const std::size_t n = std::min(x.size(), y.size());
    if (n == 0 || buckets == 0) return;
    if (n <= std::size_t(buckets) * 4) { out.resize(n); std::iota(out.begin(), out.end(), 0u); return; }
    const f32 x0 = x[0], w = (x[n - 1] - x0) / f32(buckets);
    std::size_t i = 0;
    for (u32 b = 0; b < buckets && i < n; ++b) {
        const f32 edge = b + 1 == buckets ? INFINITY : x0 + w * f32(b + 1);
        const std::size_t first = i;
        const std::size_t last = std::size_t(std::lower_bound(x.begin() + std::ptrdiff_t(i), x.begin() + std::ptrdiff_t(n), edge) - x.begin());
        if (last == first) continue;
        std::size_t mn, mx;
        simd::argMinMaxF32(y.data() + first, last - first, mn, mx);
        mn += first, mx += first;
        const u32 idx[4] = {u32(first), u32(std::min(mn, mx)), u32(std::max(mn, mx)), u32(last - 1)};
        for (u32 k : idx) if (out.empty() || out.back() != k) out.push_back(k);
        i = last;
    }
}
void lttb(std::span<const f32> x, std::span<const f32> y, u32 threshold, std::vector<u32>& out) {
    out.clear();
    const std::size_t n = std::min(x.size(), y.size());
    if (threshold >= n || threshold < 3) { out.resize(n); std::iota(out.begin(), out.end(), 0u); return; }
    const double every = double(n - 2) / double(threshold - 2);
    std::size_t a = 0;
    out.push_back(0);
    for (u32 i = 0; i < threshold - 2; ++i) {
        const std::size_t avgStart = std::size_t(double(i + 1) * every) + 1, avgEnd = std::min(n, std::size_t(double(i + 2) * every) + 1);
        double ax = 0, ay = 0;
        for (std::size_t j = avgStart; j < avgEnd; ++j) ax += x[j], ay += y[j];
        const double cnt = double(std::max<std::size_t>(1, avgEnd - avgStart));
        ax /= cnt, ay /= cnt;
        const std::size_t rs = std::size_t(double(i) * every) + 1, re = std::min(n - 1, std::size_t(double(i + 1) * every) + 1);
        double best = -1;
        std::size_t pick = rs;
        for (std::size_t j = rs; j < re; ++j) {
            const double area = std::abs((double(x[a]) - ax) * (double(y[j]) - double(y[a])) - (double(x[a]) - double(x[j])) * (ay - double(y[a])));
            if (area > best) best = area, pick = j;
        }
        out.push_back(u32(pick));
        a = pick;
    }
    out.push_back(u32(n - 1));
}

// ---------------- layout ----------------
Layout layout(const Chart& c, Rect b, const TextFns* tf) {
    Layout L;
    const f32 fs = fontPx(c), lh = fs * 1.4f, pad = 8.f;
    Rect r{b.x + pad, b.y + pad, b.w - 2 * pad, b.h - 2 * pad};
    if (!c.title.empty()) {
        L.title = {r.x, r.y, r.w, fs * 1.8f};
        r.y += fs * 1.9f, r.h -= fs * 1.9f;
    }
    L.legendItems.assign(c.series.size(), Rect{});
    std::vector<std::pair<u32, std::string>> entries;
    for (u32 i = 0; i < c.series.size(); ++i)
        if (!isPie(c.series[i].type)) entries.push_back({i, c.series[i].name.empty() ? "Series " + std::to_string(i + 1) : c.series[i].name});
    if (c.legend != LegendPos::None && !entries.empty()) {
        const f32 sw = fs * 0.9f, gap = fs;
        if (c.legend == LegendPos::Top || c.legend == LegendPos::Bottom) {
            f32 total = 0;
            for (auto& e : entries) total += sw + 6.f + std::ceil(measure(tf, e.second, fs)) + gap;
            f32 x = r.x + std::max(0.f, (r.w - total) * 0.5f);
            const f32 y = c.legend == LegendPos::Top ? r.y : r.bottom() - lh;
            for (auto& e : entries) {
                const f32 w = sw + 6.f + std::ceil(measure(tf, e.second, fs));
                L.legendItems[e.first] = {x, y, w, lh};
                x += w + gap;
            }
            L.legend = {r.x, y, r.w, lh};
            if (c.legend == LegendPos::Top) r.y += lh + 4.f;
            r.h -= lh + 4.f;
        } else {
            f32 wmax = 0;
            for (auto& e : entries) wmax = std::max(wmax, sw + 6.f + std::ceil(measure(tf, e.second, fs)));
            const f32 x = c.legend == LegendPos::Left ? r.x : r.right() - wmax;
            f32 y = r.y;
            for (auto& e : entries) { L.legendItems[e.first] = {x, y, wmax, lh}; y += lh; }
            L.legend = {x, r.y, wmax, y - r.y};
            if (c.legend == LegendPos::Left) r.x += wmax + gap;
            r.w -= wmax + gap;
        }
    }
    if (c.polar) {
        const f32 m = fs * 2.f, s = std::max(1.f, std::min(r.w, r.h) - 2 * m);
        L.plot = {r.x + (r.w - s) * 0.5f, r.y + (r.h - s) * 0.5f, s, s};
        return L;
    }
    if (onlyPies(c)) { L.plot = r; return L; }
    const auto rg = axisRanges(c);
    f32 m[4] = {0, 0, 0, 0}; // bottom left top right
    for (std::size_t a = 0; a < c.axes.size(); ++a) {
        const Axis& ax = c.axes[a];
        if (!ax.visible) continue;
        f32 need;
        if (horizontalEdge(ax.edge)) need = lh + 4.f;
        else {
            f32 w = 0;
            for (f32 t : ticks(ax, rg[a].first, rg[a].second)) w = std::max(w, measure(tf, formatTick(ax, t), fs));
            need = w + 8.f;
        }
        if (!ax.title.empty()) need += lh;
        m[int(ax.edge)] += need;
    }
    m[0] = std::max(m[0], fs * 0.6f), m[2] = std::max(m[2], fs * 0.6f), m[3] = std::max(m[3], fs);
    L.plot = {std::round(r.x + m[1]), std::round(r.y + m[2]), std::max(1.f, std::round(r.w - m[1] - m[3])), std::max(1.f, std::round(r.h - m[0] - m[2]))};
    return L;
}

// ---------------- render ----------------
namespace {
struct Ctx {
    const Chart& c;
    DrawList* dl;
    const TextFns* tf;
    Layout L;
    std::vector<std::pair<f32, f32>> rg;
    RenderStats* stats;
    f32 ap(u32 a, f32 v) const {
        const Axis& ax = c.axes[a];
        return horizontalEdge(ax.edge) ? toPixel(ax, rg[a].first, rg[a].second, v, L.plot.x, L.plot.right())
                                       : toPixel(ax, rg[a].first, rg[a].second, v, L.plot.bottom(), L.plot.y);
    }
    f32 pa(u32 a, f32 p) const {
        const Axis& ax = c.axes[a];
        return horizontalEdge(ax.edge) ? fromPixel(ax, rg[a].first, rg[a].second, p, L.plot.x, L.plot.right())
                                       : fromPixel(ax, rg[a].first, rg[a].second, p, L.plot.bottom(), L.plot.y);
    }
    Vec2 map(const Series& s, f32 x, f32 y) const {
        const Rect& p = L.plot;
        if (c.polar) {
            const f32 ang = toPixel(c.axes[s.xAxis], rg[s.xAxis].first, rg[s.xAxis].second, x, 0.f, 6.2831853f) - 1.5707963f;
            const f32 rad = std::max(0.f, toPixel(c.axes[s.yAxis], rg[s.yAxis].first, rg[s.yAxis].second, y, 0.f, p.w * 0.5f));
            return {p.x + p.w * 0.5f + std::cos(ang) * rad, p.y + p.h * 0.5f + std::sin(ang) * rad};
        }
        // a vertical x axis (horizontal bars put categories on y) swaps the roles
        if (!horizontalEdge(c.axes[s.xAxis].edge)) return {ap(s.yAxis, y), ap(s.xAxis, x)};
        return {ap(s.xAxis, x), ap(s.yAxis, y)};
    }
    f32 unitPx(u32 a) const { return (horizontalEdge(c.axes[a].edge) ? L.plot.w : L.plot.h) / std::max(1e-12f, rg[a].second - rg[a].first); }
};

void marker(DrawList& dl, Vec2 p, f32 r, Color c, Color ring = {}) {
    Path m;
    m.circle(p, r);
    dl.path(m, c, false, ring, ring.a > 0 ? 1.5f : 0.f);
}

void drawAxes(Ctx& x) {
    const Chart& c = x.c;
    DrawList& dl = *x.dl;
    const Rect& p = x.L.plot;
    const f32 fs = fontPx(c), lh = fs * 1.4f;
    if (c.polar) {
        const Vec2 ctr{p.x + p.w * 0.5f, p.y + p.h * 0.5f};
        const f32 R = p.w * 0.5f;
        Path bg;
        bg.circle(ctr, R);
        dl.path(bg, c.theme.plot);
        const Axis& ra = c.axes[1];
        for (f32 t : ticks(ra, x.rg[1].first, x.rg[1].second)) {
            const f32 r = toPixel(ra, x.rg[1].first, x.rg[1].second, t, 0.f, R);
            if (r <= 0.5f) continue;
            Path ring;
            ring.circle(ctr, r);
            dl.path(ring, {}, false, c.theme.grid, 1.f);
            text(x.tf, dl, formatTick(ra, t), {ctr.x + 3.f, ctr.y - r, 80.f, lh}, c.theme.axis, fs, TextAlignment::Start);
        }
        const Axis& aa = c.axes[0];
        for (f32 t : ticks(aa, x.rg[0].first, x.rg[0].second)) {
            if (t >= x.rg[0].second - 1e-6f * std::max(1.f, std::abs(x.rg[0].second))) continue;
            const f32 ang = toPixel(aa, x.rg[0].first, x.rg[0].second, t, 0.f, 6.2831853f) - 1.5707963f;
            dl.line(ctr, {ctr.x + std::cos(ang) * R, ctr.y + std::sin(ang) * R}, c.theme.grid, 1.f);
            const std::string s = formatTick(aa, t);
            const f32 w = measure(x.tf, s, fs);
            text(x.tf, dl, s, {ctr.x + std::cos(ang) * (R + fs) - w * 0.5f, ctr.y + std::sin(ang) * (R + fs) - lh * 0.5f, w + 2, lh}, c.theme.axis, fs, TextAlignment::Center);
        }
        Path outline;
        outline.circle(ctr, R);
        dl.path(outline, {}, false, c.theme.axis, 1.f);
        return;
    }
    dl.rect(p, c.theme.plot);
    f32 used[4] = {0, 0, 0, 0};
    for (std::size_t a = 0; a < c.axes.size(); ++a) {
        const Axis& ax = c.axes[a];
        if (!ax.visible) continue;
        const auto [lo, hi] = x.rg[a];
        const bool h = horizontalEdge(ax.edge);
        const int e = int(ax.edge);
        f32 labelW = 0;
        for (f32 t : ticks(ax, lo, hi)) {
            const f32 v = h ? toPixel(ax, lo, hi, t, p.x, p.right()) : toPixel(ax, lo, hi, t, p.bottom(), p.y);
            if (ax.grid && used[e] == 0.f) {
                if (h) dl.line({std::round(v) + 0.5f, p.y}, {std::round(v) + 0.5f, p.bottom()}, c.theme.grid, 1.f);
                else dl.line({p.x, std::round(v) + 0.5f}, {p.right(), std::round(v) + 0.5f}, c.theme.grid, 1.f);
            }
            const std::string s = formatTick(ax, t);
            const f32 w = measure(x.tf, s, fs);
            labelW = std::max(labelW, w);
            switch (ax.edge) {
            case AxisEdge::Bottom: text(x.tf, dl, s, {v - w * 0.5f - 1, p.bottom() + used[0] + 3.f, w + 2, lh}, c.theme.axis, fs, TextAlignment::Center); break;
            case AxisEdge::Top: text(x.tf, dl, s, {v - w * 0.5f - 1, p.y - used[2] - lh - 2.f, w + 2, lh}, c.theme.axis, fs, TextAlignment::Center); break;
            case AxisEdge::Left: text(x.tf, dl, s, {p.x - used[1] - w - 6.f, v - lh * 0.5f, w + 2, lh}, c.theme.axis, fs, TextAlignment::End); break;
            case AxisEdge::Right: text(x.tf, dl, s, {p.right() + used[3] + 5.f, v - lh * 0.5f, w + 2, lh}, c.theme.axis, fs, TextAlignment::Start); break;
            }
        }
        switch (ax.edge) {
        case AxisEdge::Bottom: dl.line({p.x, p.bottom() + used[0] + 0.5f}, {p.right(), p.bottom() + used[0] + 0.5f}, c.theme.axis, 1.f); break;
        case AxisEdge::Top: dl.line({p.x, p.y - used[2] - 0.5f}, {p.right(), p.y - used[2] - 0.5f}, c.theme.axis, 1.f); break;
        case AxisEdge::Left: dl.line({p.x - used[1] - 0.5f, p.y}, {p.x - used[1] - 0.5f, p.bottom()}, c.theme.axis, 1.f); break;
        case AxisEdge::Right: dl.line({p.right() + used[3] + 0.5f, p.y}, {p.right() + used[3] + 0.5f, p.bottom()}, c.theme.axis, 1.f); break;
        }
        f32 need = h ? lh + 4.f : labelW + 8.f;
        if (!ax.title.empty()) {
            const f32 tw = measure(x.tf, ax.title, fs);
            switch (ax.edge) {
            case AxisEdge::Bottom: text(x.tf, dl, ax.title, {p.x + (p.w - tw) * 0.5f, p.bottom() + used[0] + need, tw + 2, lh}, c.theme.text, fs, TextAlignment::Center); break;
            case AxisEdge::Top: text(x.tf, dl, ax.title, {p.x + (p.w - tw) * 0.5f, p.y - used[2] - need - lh, tw + 2, lh}, c.theme.text, fs, TextAlignment::Center); break;
            // vertical-axis titles sit horizontally above the axis (rotated glyph runs wait for the text wrapper)
            case AxisEdge::Left: text(x.tf, dl, ax.title, {std::max(0.f, p.x - used[1] - need), p.y - lh - 2.f, tw + 2, lh}, c.theme.text, fs, TextAlignment::Start); break;
            case AxisEdge::Right: text(x.tf, dl, ax.title, {p.right() + used[3] + 4.f, p.y - lh - 2.f, tw + 2, lh}, c.theme.text, fs, TextAlignment::Start); break;
            }
            need += lh;
        }
        used[e] += need;
    }
}

// Screen points of a point series, restricted to the visible x window and decimated to the plot width.
void seriesPoints(Ctx& x, const Series& s, std::vector<Vec2>& pts, std::vector<u32>& idxOut) {
    pts.clear(), idxOut.clear();
    const auto ys = s.ys();
    thread_local std::vector<f32> ix;
    std::span<const f32> xs = s.xs();
    if (xs.size() != ys.size()) {
        ix.resize(ys.size());
        std::iota(ix.begin(), ix.end(), 0.f);
        xs = ix;
    }
    const bool sorted = !x.c.polar && std::is_sorted(xs.begin(), xs.end());
    std::size_t a = 0, b = ys.size();
    if (sorted) std::tie(a, b) = visibleRange(xs, x.rg[s.xAxis].first, x.rg[s.xAxis].second);
    const std::span<const f32> vx = xs.subspan(a, b - a), vy = ys.subspan(a, b - a);
    if (x.stats) x.stats->inputPoints += vx.size();
    thread_local std::vector<u32> idx;
    const u32 width = u32(std::max(1.f, x.L.plot.w));
    Decimation d = s.decimation;
    if (d == Decimation::Auto)
        d = sorted && vx.size() > std::size_t(width) * 4 && s.type != SeriesType::Scatter && s.type != SeriesType::Bubble ? Decimation::MinMax : Decimation::None;
    if (d == Decimation::MinMax && sorted) decimateMinMax(vx, vy, width, idx);
    else if (d == Decimation::Lttb) lttb(vx, vy, width * 2, idx);
    else { idx.resize(vx.size()); std::iota(idx.begin(), idx.end(), 0u); }
    pts.reserve(idx.size()), idxOut.reserve(idx.size());
    for (u32 i : idx) {
        if (!std::isfinite(vy[i])) continue;
        pts.push_back(x.map(s, vx[i], vy[i]));
        idxOut.push_back(u32(a + i));
    }
    if (x.stats) x.stats->drawnPoints += pts.size();
}

void drawStackedArea(Ctx& x, u32 si, std::vector<f32>& stackBase) {
    const Chart& c = x.c;
    const Series& s = c.series[si];
    const Color col = seriesColor(c, si);
    const std::size_t n = s.count();
    std::vector<f32> tot;
    if (s.type == SeriesType::PercentArea) {
        tot.assign(n, 0.f);
        for (const Series& o : c.series) if (o.visible && o.type == s.type) for (std::size_t i = 0; i < std::min(n, o.count()); ++i) tot[i] += o.ys()[i];
    }
    if (stackBase.size() < n) stackBase.resize(n, 0.f);
    std::vector<Vec2> top, bot;
    for (std::size_t i = 0; i < n; ++i) {
        f32 v = s.ys()[i];
        const f32 b0 = stackBase[i];
        if (!tot.empty()) v *= tot[i] != 0.f ? 100.f / tot[i] : 0.f;
        bot.push_back(x.map(s, xAt(s, i), b0 * c.progress));
        top.push_back(x.map(s, xAt(s, i), (b0 + v) * c.progress));
        stackBase[i] = b0 + v;
    }
    if (x.stats) x.stats->drawnPoints += n, x.stats->inputPoints += n;
    if (top.size() < 2) return;
    Path p;
    p.moveTo(top[0]);
    for (std::size_t i = 1; i < top.size(); ++i) p.lineTo(top[i]);
    for (std::size_t i = bot.size(); i-- > 0;) p.lineTo(bot[i]);
    p.close();
    x.dl->path(p, col.withAlpha(0.55f * col.a));
    Path l;
    l.moveTo(top[0]);
    for (std::size_t i = 1; i < top.size(); ++i) l.lineTo(top[i]);
    x.dl->path(l, {}, false, col, s.lineWidth);
}

void drawLineLike(Ctx& x, u32 si) {
    const Chart& c = x.c;
    DrawList& dl = *x.dl;
    const Series& s = c.series[si];
    const Color col = seriesColor(c, si);
    std::vector<Vec2> pts;
    std::vector<u32> idx;
    seriesPoints(x, s, pts, idx);
    if (pts.empty()) return;
    const bool reveal = c.progress < 1.f;
    if (reveal) dl.pushClip({x.L.plot.x - 20.f, x.L.plot.y - 20.f, (x.L.plot.w + 40.f) * c.progress, x.L.plot.h + 40.f});
    if (s.type == SeriesType::Scatter || s.type == SeriesType::Bubble) {
        const f32 r = s.markerSize > 0 ? s.markerSize : 4.f;
        for (std::size_t k = 0; k < pts.size(); ++k) {
            if (s.type == SeriesType::Bubble) {
                const std::size_t si2 = idx[k] + s.m_head;
                const f32 rr = si2 < s.size.size() ? std::max(1.f, s.size[si2] * s.bubbleScale) : r;
                marker(dl, pts[k], rr, col.withAlpha(0.55f * col.a), col);
            } else marker(dl, pts[k], r, col);
        }
    } else {
        Path p;
        p.moveTo(pts[0]);
        if (s.type == SeriesType::Spline && pts.size() > 2) {
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) { // Catmull-Rom as cubic Béziers (QSplineSeries)
                const Vec2 p0 = pts[i ? i - 1 : 0], p1 = pts[i], p2 = pts[i + 1], p3 = pts[std::min(i + 2, pts.size() - 1)];
                p.cubicTo({p1.x + (p2.x - p0.x) / 6.f, p1.y + (p2.y - p0.y) / 6.f}, {p2.x - (p3.x - p1.x) / 6.f, p2.y - (p3.y - p1.y) / 6.f}, p2);
            }
        } else if (s.type == SeriesType::StepLine) {
            for (std::size_t i = 1; i < pts.size(); ++i) p.lineTo({pts[i].x, pts[i - 1].y}).lineTo(pts[i]);
        } else {
            for (std::size_t i = 1; i < pts.size(); ++i) p.lineTo(pts[i]);
        }
        if (s.type == SeriesType::Area) {
            Path f;
            if (c.polar) {
                f = p;
                f.close();
                dl.path(f, col.withAlpha(0.3f * col.a));
            } else {
                const f32 base = x.ap(s.yAxis, std::max(0.f, x.rg[s.yAxis].first));
                f.moveTo({pts[0].x, base});
                for (const Vec2& q : pts) f.lineTo(q);
                f.lineTo({pts.back().x, base}).close();
                dl.gradientPath(f, Gradient::linear({0.f, x.L.plot.y}, {0.f, x.L.plot.bottom()}, {{0.f, col.withAlpha(0.55f * col.a)}, {1.f, col.withAlpha(0.06f * col.a)}}));
            }
        }
        if (s.type == SeriesType::Line && pts.size() > 512) {
            // dense (decimated) envelopes: stroke in short runs so each path's coverage bounds stay small; the CPU
            // rasteriser and the GPU kernel both scale with path bounds x segments
            for (std::size_t i = 0; i + 1 < pts.size(); i += 96) {
                Path run;
                run.moveTo(pts[i]);
                for (std::size_t k = i + 1; k <= std::min(i + 96, pts.size() - 1); ++k) run.lineTo(pts[k]);
                dl.path(run, {}, false, col, s.lineWidth);
            }
        } else dl.path(p, {}, false, col, s.lineWidth);
        if (s.markerSize > 0) for (const Vec2& q : pts) marker(dl, q, s.markerSize, col);
    }
    if (reveal) dl.popClip();
}

Rect barRect(bool horiz, f32 c0, f32 c1, f32 v0, f32 v1) {
    return horiz ? Rect{std::min(v0, v1), std::min(c0, c1), std::abs(v1 - v0), std::abs(c1 - c0)}
                 : Rect{std::min(c0, c1), std::min(v0, v1), std::abs(c1 - c0), std::abs(v1 - v0)};
}
// bars of `si`: rects in data order (shared by render and hit testing)
void barRects(const Ctx& x, u32 si, std::vector<Rect>& out) {
    out.clear();
    const Chart& c = x.c;
    const Series& s = c.series[si];
    const bool horiz = isHBar(s.type);
    const u32 cat = horiz ? s.yAxis : s.xAxis, val = horiz ? s.xAxis : s.yAxis;
    if (s.type == SeriesType::Bar || s.type == SeriesType::HorizontalBar) {
        u32 k = 0, n = 0;
        for (u32 o = 0; o < c.series.size(); ++o) if (c.series[o].visible && c.series[o].type == s.type) { if (o == si) k = n; ++n; }
        const f32 slot = s.barWidth / f32(std::max(1u, n));
        for (std::size_t i = 0; i < s.count(); ++i) {
            const f32 xc = xAt(s, i) - s.barWidth * 0.5f + slot * (f32(k) + 0.5f);
            const f32 base = c.axes[val].type == AxisType::Log ? x.rg[val].first : std::clamp(0.f, x.rg[val].first, x.rg[val].second);
            out.push_back(barRect(horiz, x.ap(cat, xc - slot * 0.46f), x.ap(cat, xc + slot * 0.46f), x.ap(val, base), x.ap(val, base + (s.ys()[i] - base) * c.progress)));
        }
        return;
    }
    const Stack st0 = stackFor(c, s.type, maxCount(c, s.type));
    std::vector<f32> pos(st0.pos.size(), 0.f), neg(st0.pos.size(), 0.f);
    for (u32 o = 0; o <= si; ++o) {
        const Series& q = c.series[o];
        if (!q.visible || q.type != s.type) continue;
        for (std::size_t i = 0; i < q.count() && i < pos.size(); ++i) {
            f32 v = q.ys()[i];
            if (isPercentBar(s.type)) v = st0.total[i] > 0 ? v / st0.total[i] * 100.f : 0.f;
            f32& base = v >= 0 ? pos[i] : neg[i];
            if (o == si) {
                const f32 xc = xAt(s, i);
                out.push_back(barRect(horiz, x.ap(cat, xc - s.barWidth * 0.5f), x.ap(cat, xc + s.barWidth * 0.5f), x.ap(val, base * c.progress), x.ap(val, (base + v) * c.progress)));
            }
            base += v;
        }
    }
}

void drawPie(Ctx& x, u32 si, Rect area) {
    const Chart& c = x.c;
    DrawList& dl = *x.dl;
    const Series& s = c.series[si];
    const auto v = s.ys();
    f32 total = 0;
    for (f32 a : v) total += std::max(0.f, a);
    if (total <= 0) return;
    const f32 fs = fontPx(c), lh = fs * 1.4f;
    const Vec2 ctr{area.x + area.w * 0.5f, area.y + area.h * 0.5f};
    const f32 R = std::min(area.w, area.h) * 0.5f - lh * 1.2f, hole = s.type == SeriesType::Donut ? R * s.holeSize : 0.f;
    if (R <= 2) return;
    if (!s.name.empty()) text(x.tf, dl, s.name, {area.x, area.y, area.w, lh}, c.theme.text, fs, TextAlignment::Center);
    f32 a0 = -1.5707963f;
    const f32 sweep = 6.2831853f * c.progress;
    for (std::size_t i = 0; i < v.size(); ++i) {
        const f32 da = std::max(0.f, v[i]) / total * sweep;
        if (da <= 0) continue;
        const f32 a1 = a0 + da, mid = (a0 + a1) * 0.5f;
        const bool hov = c.hoverSeries == i32(si) && c.hoverIndex == i32(i);
        const f32 ex = hov ? R * 0.06f : 0.f; // exploded hover slice (QPieSlice::setExploded)
        const Vec2 o{ctr.x + std::cos(mid) * ex, ctr.y + std::sin(mid) * ex};
        auto at = [&](f32 ang, f32 r) { return Vec2{o.x + std::cos(ang) * r, o.y + std::sin(ang) * r}; };
        Path p;
        const bool large = da > 3.14159265f;
        if (da >= 6.2831f) { // full circle: two half arcs
            if (hole > 0) { p.circle(o, R); Path in; in.circle(o, hole); p.append(in); dl.path(p, sliceColor(c, u32(i)), true); }
            else { p.circle(o, R); dl.path(p, sliceColor(c, u32(i))); }
        } else {
            if (hole > 0) {
                p.moveTo(at(a0, hole)).lineTo(at(a0, R)).arcTo({R, R}, 0.f, large, true, at(a1, R)).lineTo(at(a1, hole)).arcTo({hole, hole}, 0.f, large, false, at(a0, hole));
            } else p.moveTo(o).lineTo(at(a0, R)).arcTo({R, R}, 0.f, large, true, at(a1, R));
            p.close();
            dl.path(p, sliceColor(c, u32(i)), false, c.theme.background, 1.5f);
        }
        if (c.progress >= 1.f && i < s.labels.size() && da > 0.15f) {
            char pct[16];
            std::snprintf(pct, sizeof pct, " %.0f%%", double(v[i] / total * 100.f));
            const std::string t = s.labels[i] + pct;
            const f32 w = measure(x.tf, t, fs);
            const Vec2 lp = at(mid, R + lh * 0.6f);
            text(x.tf, dl, t, {std::cos(mid) >= 0 ? lp.x : lp.x - w, lp.y - lh * 0.5f, w + 2, lh}, c.theme.text, fs, TextAlignment::Start);
        }
        a0 = a1;
    }
    if (x.stats) x.stats->inputPoints += v.size(), x.stats->drawnPoints += v.size();
}

void drawFinancial(Ctx& x, u32 si) {
    const Series& s = x.c.series[si];
    DrawList& dl = *x.dl;
    const Color up = s.color.a > 0 ? s.color : Color::rgb(0x26A69A), down = Color::rgb(0xEF5350);
    for (std::size_t i = 0; i < s.candles.size(); ++i) {
        const Candle& k = s.candles[i];
        Color col = k.close >= k.open ? up : down;
        if (x.c.hoverSeries == i32(si) && x.c.hoverIndex == i32(i)) col = mix(col, Color::rgb(0xFFFFFF), 0.3f);
        const f32 xc = std::round(x.ap(s.xAxis, k.x)) + 0.5f, hw = std::max(1.f, x.unitPx(s.xAxis) * s.barWidth * 0.35f);
        const f32 yh = x.ap(s.yAxis, k.high), yl = x.ap(s.yAxis, k.low), yo = x.ap(s.yAxis, k.open), yc = x.ap(s.yAxis, k.close);
        if (s.type == SeriesType::Candlestick) {
            dl.line({xc, yh}, {xc, yl}, col, 1.f);
            dl.rect({xc - hw, std::min(yo, yc), hw * 2, std::max(1.f, std::abs(yc - yo))}, col);
        } else {
            dl.line({xc, yh}, {xc, yl}, col, 1.5f);
            dl.line({xc - hw, yo}, {xc, yo}, col, 1.5f);
            dl.line({xc, yc}, {xc + hw, yc}, col, 1.5f);
        }
    }
    if (x.stats) x.stats->inputPoints += s.candles.size(), x.stats->drawnPoints += s.candles.size();
}
void drawBoxes(Ctx& x, u32 si) {
    const Chart& c = x.c;
    DrawList& dl = *x.dl;
    const Series& s = c.series[si];
    const Color col = seriesColor(c, si);
    for (const BoxItem& b : s.boxes) {
        const f32 xc = std::round(x.ap(s.xAxis, b.x)) + 0.5f, hw = std::max(2.f, x.unitPx(s.xAxis) * s.barWidth * 0.3f);
        const f32 yl = x.ap(s.yAxis, b.lower), y1 = x.ap(s.yAxis, b.q1), ym = x.ap(s.yAxis, b.median), y3 = x.ap(s.yAxis, b.q3), yu = x.ap(s.yAxis, b.upper);
        dl.line({xc, yl}, {xc, y1}, col, 1.f);
        dl.line({xc, y3}, {xc, yu}, col, 1.f);
        dl.line({xc - hw * 0.5f, yl}, {xc + hw * 0.5f, yl}, col, 1.f);
        dl.line({xc - hw * 0.5f, yu}, {xc + hw * 0.5f, yu}, col, 1.f);
        dl.roundedRect({xc - hw, std::min(y1, y3), hw * 2, std::abs(y1 - y3)}, col.withAlpha(0.35f * col.a), {2, 2, 2, 2}, col, 1.f);
        dl.line({xc - hw, ym}, {xc + hw, ym}, c.theme.text, 2.f);
        for (f32 o : b.outliers) marker(dl, {xc, x.ap(s.yAxis, o)}, 2.5f, {}, col);
    }
    if (x.stats) x.stats->inputPoints += s.boxes.size(), x.stats->drawnPoints += s.boxes.size();
}
void drawHistogram(Ctx& x, u32 si) {
    const Chart& c = x.c;
    const Series& s = c.series[si];
    const Color col = seriesColor(c, si);
    const HistBins h = histogram(s);
    for (std::size_t i = 0; i < h.counts.size(); ++i) {
        const f32 x0 = x.ap(s.xAxis, h.lo + h.w * f32(i)), x1 = x.ap(s.xAxis, h.lo + h.w * f32(i + 1));
        const f32 y0 = x.ap(s.yAxis, 0.f), y1 = x.ap(s.yAxis, h.counts[i] * c.progress);
        const bool hov = c.hoverSeries == i32(si) && c.hoverIndex == i32(i);
        x.dl->rect({x0 + 0.5f, std::min(y0, y1), std::max(1.f, x1 - x0 - 1.f), std::abs(y1 - y0)}, hov ? mix(col, Color::rgb(0xFFFFFF), 0.25f) : col);
    }
    if (x.stats) x.stats->inputPoints += s.count(), x.stats->drawnPoints += h.counts.size();
}
void drawHeatmap(Ctx& x, u32 si) {
    const Chart& c = x.c;
    const Series& s = c.series[si];
    const std::size_t n = std::size_t(s.rows) * s.cols;
    if (!n || s.y.size() < n) return;
    f32 lo, hi;
    minMax(std::span<const f32>(s.y.data(), n), lo, hi);
    const f32 inv = hi > lo ? 1.f / (hi - lo) : 0.f;
    for (u32 r = 0; r < s.rows; ++r)
        for (u32 k = 0; k < s.cols; ++k) {
            const f32 x0 = std::round(x.ap(s.xAxis, f32(k) - 0.5f)), x1 = std::round(x.ap(s.xAxis, f32(k) + 0.5f));
            const f32 y0 = std::round(x.ap(s.yAxis, f32(r) - 0.5f)), y1 = std::round(x.ap(s.yAxis, f32(r) + 0.5f));
            const Color col = sampleMap(s.colormap, (s.y[std::size_t(r) * s.cols + k] - lo) * inv);
            x.dl->rect({std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0), std::abs(y1 - y0)}, col.withAlpha(col.a * c.progress));
        }
    if (x.stats) x.stats->inputPoints += n, x.stats->drawnPoints += n;
}
void drawLegend(Ctx& x) {
    const Chart& c = x.c;
    const f32 fs = fontPx(c), sw = fs * 0.9f;
    for (u32 i = 0; i < c.series.size(); ++i) {
        const Rect& r = x.L.legendItems[i];
        if (r.empty()) continue;
        const Series& s = c.series[i];
        const Color col = seriesColor(c, i);
        const Color tc = s.visible ? c.theme.text : c.theme.text.withAlpha(c.theme.text.a * 0.4f);
        x.dl->rect({r.x, std::round(r.y + (r.h - sw) * 0.5f), sw, sw}, s.visible ? col : col.withAlpha(0.3f * col.a), 2.f);
        text(x.tf, *x.dl, s.name.empty() ? "Series " + std::to_string(i + 1) : s.name, {r.x + sw + 4.f, r.y, r.w - sw - 4.f, r.h}, tc, fs, TextAlignment::Start);
    }
}
std::vector<Rect> pieAreas(const Chart& c, const Layout& L) {
    u32 n = 0;
    for (const Series& s : c.series) n += s.visible ? 1 : 0;
    std::vector<Rect> out(c.series.size());
    u32 k = 0;
    const f32 w = L.plot.w / f32(std::max(1u, n));
    for (u32 i = 0; i < c.series.size(); ++i) if (c.series[i].visible) out[i] = {L.plot.x + w * f32(k++), L.plot.y, w, L.plot.h};
    return out;
}
} // namespace

void render(const Chart& c, DrawList& dl, Rect b, const TextFns* tf, RenderStats* stats) {
    const std::size_t cmd0 = dl.size();
    Ctx x{c, &dl, tf, layout(c, b, tf), axisRanges(c), stats};
    dl.rect(b, c.theme.background, 4.f);
    if (!c.title.empty()) text(tf, dl, c.title, x.L.title, c.theme.text, fontPx(c) * 1.25f, TextAlignment::Center);
    const bool pies = onlyPies(c);
    if (!pies) {
        drawAxes(x);
        dl.pushClip(c.polar ? Rect{x.L.plot.x - 6, x.L.plot.y - 6, x.L.plot.w + 12, x.L.plot.h + 12} : x.L.plot);
        std::vector<f32> stackA, stackP;
        std::vector<Rect> rects;
        for (u32 i = 0; i < c.series.size(); ++i) {
            const Series& s = c.series[i];
            if (!s.visible) continue;
            if (s.type == SeriesType::Heatmap) drawHeatmap(x, i);
            else if (s.type == SeriesType::Histogram) drawHistogram(x, i);
            else if (s.type == SeriesType::Candlestick || s.type == SeriesType::Ohlc) drawFinancial(x, i);
            else if (s.type == SeriesType::BoxPlot) drawBoxes(x, i);
            else if (s.type == SeriesType::StackedArea) drawStackedArea(x, i, stackA);
            else if (s.type == SeriesType::PercentArea) drawStackedArea(x, i, stackP);
            else if (isBar(s.type)) {
                barRects(x, i, rects);
                const Color col = seriesColor(c, i);
                const bool rounded = s.type == SeriesType::Bar || s.type == SeriesType::HorizontalBar;
                for (std::size_t k = 0; k < rects.size(); ++k) {
                    const bool hov = c.hoverSeries == i32(i) && c.hoverIndex == i32(k);
                    dl.rect(rects[k], hov ? mix(col, Color::rgb(0xFFFFFF), 0.25f) : col, rounded ? 2.f : 0.f);
                }
                if (stats) stats->inputPoints += s.count(), stats->drawnPoints += rects.size();
            } else if (isLineLike(s.type)) drawLineLike(x, i);
        }
        auto ring = [&](u32 si, u32 idx, Color col, f32 r) {
            const Series& s = c.series[si];
            if (!isPointSeries(s.type) || idx >= s.count()) return;
            marker(dl, x.map(s, xAt(s, idx), s.ys()[idx]), r, seriesColor(c, si), col);
        };
        for (const Selected& sel : c.selection) if (sel.series < c.series.size() && c.series[sel.series].visible) ring(sel.series, sel.index, c.theme.selection, 5.5f);
        if (c.hoverSeries >= 0 && u32(c.hoverSeries) < c.series.size() && c.hoverIndex >= 0) ring(u32(c.hoverSeries), u32(c.hoverIndex), c.theme.text, 4.5f);
        dl.popClip();
    } else {
        const auto areas = pieAreas(c, x.L);
        for (u32 i = 0; i < c.series.size(); ++i) if (c.series[i].visible) drawPie(x, i, areas[i]);
    }
    drawLegend(x);
    if (stats) stats->commands += dl.size() - cmd0;
}

// ---------------- hit testing ----------------
Hit hitTest(const Chart& c, Rect b, Vec2 pos, f32 radius, const TextFns* tf) {
    Hit h;
    Ctx x{c, nullptr, tf, layout(c, b, tf), axisRanges(c), nullptr};
    for (u32 i = 0; i < x.L.legendItems.size(); ++i) if (x.L.legendItems[i].contains(pos)) { h.legendItem = i32(i); return h; }
    if (onlyPies(c)) {
        const auto areas = pieAreas(c, x.L);
        const f32 lh = fontPx(c) * 1.4f;
        for (u32 si = 0; si < c.series.size(); ++si) {
            const Series& s = c.series[si];
            if (!s.visible) continue;
            const Rect& area = areas[si];
            const Vec2 ctr{area.x + area.w * 0.5f, area.y + area.h * 0.5f};
            const f32 R = std::min(area.w, area.h) * 0.5f - lh * 1.2f, hole = s.type == SeriesType::Donut ? R * s.holeSize : 0.f;
            const f32 dx = pos.x - ctr.x, dy = pos.y - ctr.y, d = std::sqrt(dx * dx + dy * dy);
            if (d > R * 1.07f || d < hole) continue;
            f32 ang = std::atan2(dy, dx) + 1.5707963f;
            if (ang < 0) ang += 6.2831853f;
            f32 total = 0, a = 0;
            for (f32 v : s.ys()) total += std::max(0.f, v);
            for (std::size_t i = 0; i < s.count() && total > 0; ++i) {
                a += std::max(0.f, s.ys()[i]) / total * 6.2831853f;
                if (ang <= a) { h.series = i32(si), h.index = i32(i), h.y = s.ys()[i], h.screen = pos; return h; }
            }
        }
        return h;
    }
    if (!c.polar && !x.L.plot.contains(pos)) return h;
    f32 best = radius * radius;
    std::vector<Rect> rects;
    for (u32 si = 0; si < c.series.size(); ++si) {
        const Series& s = c.series[si];
        if (!s.visible) continue;
        if (isPointSeries(s.type)) {
            const auto ys = s.ys(), xs = s.xs();
            auto test = [&](std::size_t i) {
                if (!std::isfinite(ys[i])) return;
                const Vec2 p = x.map(s, xAt(s, i), ys[i]);
                const f32 d = (p.x - pos.x) * (p.x - pos.x) + (p.y - pos.y) * (p.y - pos.y);
                if (d <= best) best = d, h.series = i32(si), h.index = i32(i), h.x = xAt(s, i), h.y = ys[i], h.screen = p;
            };
            if (!c.polar && xs.size() == ys.size() && horizontalEdge(c.axes[s.xAxis].edge) && std::is_sorted(xs.begin(), xs.end())) {
                const f32 lo = x.pa(s.xAxis, pos.x - radius), hi = x.pa(s.xAxis, pos.x + radius);
                const auto [a, e] = visibleRange(xs, std::min(lo, hi), std::max(lo, hi));
                for (std::size_t i = a; i < e; ++i) test(i);
            } else for (std::size_t i = 0; i < ys.size(); ++i) test(i);
            continue;
        }
        if (isBar(s.type)) {
            barRects(x, si, rects);
            for (std::size_t i = 0; i < rects.size(); ++i)
                if (Rect{rects[i].x - 1, rects[i].y - 1, rects[i].w + 2, rects[i].h + 2}.contains(pos)) h.series = i32(si), h.index = i32(i), h.x = xAt(s, i), h.y = s.ys()[i], h.screen = pos;
            continue;
        }
        const f32 dx = x.pa(s.xAxis, pos.x);
        if (s.type == SeriesType::Histogram) {
            const HistBins hb = histogram(s);
            const i32 k = i32(std::floor((dx - hb.lo) / hb.w));
            if (k >= 0 && std::size_t(k) < hb.counts.size() && x.pa(s.yAxis, pos.y) <= hb.counts[std::size_t(k)])
                h.series = i32(si), h.index = k, h.x = hb.lo + hb.w * (f32(k) + 0.5f), h.y = hb.counts[std::size_t(k)], h.screen = pos;
        } else if (s.type == SeriesType::Heatmap) {
            const long col = std::lround(dx), row = std::lround(x.pa(s.yAxis, pos.y));
            if (col >= 0 && row >= 0 && u32(col) < s.cols && u32(row) < s.rows)
                h.series = i32(si), h.index = i32(row * long(s.cols) + col), h.x = f32(col), h.y = s.y[std::size_t(h.index)], h.screen = pos;
        } else if (s.type == SeriesType::Candlestick || s.type == SeriesType::Ohlc) {
            for (std::size_t i = 0; i < s.candles.size(); ++i)
                if (std::abs(s.candles[i].x - dx) <= s.barWidth * 0.5f) h.series = i32(si), h.index = i32(i), h.x = s.candles[i].x, h.y = s.candles[i].close, h.screen = pos;
        } else if (s.type == SeriesType::BoxPlot) {
            for (std::size_t i = 0; i < s.boxes.size(); ++i)
                if (std::abs(s.boxes[i].x - dx) <= s.barWidth * 0.5f) h.series = i32(si), h.index = i32(i), h.x = s.boxes[i].x, h.y = s.boxes[i].median, h.screen = pos;
        }
    }
    return h;
}

std::string tooltip(const Chart& c, u32 si, u32 i) {
    if (si >= c.series.size()) return {};
    const Series& s = c.series[si];
    char b[200];
    const std::string name = s.name.empty() ? "Series " + std::to_string(si + 1) : s.name;
    auto fx = [&](f32 v) { return s.xAxis < c.axes.size() ? formatTick(c.axes[s.xAxis], v) : std::to_string(v); };
    switch (s.type) {
    case SeriesType::Candlestick:
    case SeriesType::Ohlc: {
        if (i >= s.candles.size()) return {};
        const Candle& k = s.candles[i];
        std::snprintf(b, sizeof b, "%s %s: O %g H %g L %g C %g", name.c_str(), fx(k.x).c_str(), double(k.open), double(k.high), double(k.low), double(k.close));
        return b;
    }
    case SeriesType::BoxPlot: {
        if (i >= s.boxes.size()) return {};
        const BoxItem& k = s.boxes[i];
        std::snprintf(b, sizeof b, "%s %s: min %g Q1 %g median %g Q3 %g max %g", name.c_str(), fx(k.x).c_str(), double(k.lower), double(k.q1), double(k.median), double(k.q3), double(k.upper));
        return b;
    }
    case SeriesType::Histogram: {
        const HistBins h = histogram(s);
        if (i >= h.counts.size()) return {};
        std::snprintf(b, sizeof b, "%s [%g, %g): %g", name.c_str(), double(h.lo + h.w * f32(i)), double(h.lo + h.w * f32(i + 1)), double(h.counts[i]));
        return b;
    }
    case SeriesType::Heatmap:
        if (!s.cols || i >= s.y.size()) return {};
        std::snprintf(b, sizeof b, "%s row %u col %u: %g", name.c_str(), unsigned(i / s.cols), unsigned(i % s.cols), double(s.y[i]));
        return b;
    case SeriesType::Pie:
    case SeriesType::Donut: {
        if (i >= s.count()) return {};
        f32 tot = 0;
        for (f32 v : s.ys()) tot += std::max(0.f, v);
        std::snprintf(b, sizeof b, "%s: %g (%.1f%%)", i < s.labels.size() ? s.labels[i].c_str() : name.c_str(), double(s.ys()[i]), double(tot > 0 ? s.ys()[i] / tot * 100.f : 0.f));
        return b;
    }
    default: {
        if (i >= s.count()) return {};
        const bool horiz = isHBar(s.type);
        const std::string xs = horiz ? formatTick(c.axes[s.yAxis], xAt(s, i)) : fx(xAt(s, i));
        if (s.type == SeriesType::Bubble && i + s.m_head < s.size.size())
            std::snprintf(b, sizeof b, "%s: (%s, %g) size %g", name.c_str(), xs.c_str(), double(s.ys()[i]), double(s.size[i + s.m_head]));
        else if (isBar(s.type)) std::snprintf(b, sizeof b, "%s %s: %g", name.c_str(), xs.c_str(), double(s.ys()[i]));
        else std::snprintf(b, sizeof b, "%s: (%s, %g)", name.c_str(), xs.c_str(), double(s.ys()[i]));
        return b;
    }
    }
}

std::string describe(const Chart& c) {
    static const char* names[] = {"line", "spline", "step line", "scatter", "area", "stacked area", "percent area", "bar", "stacked bar", "percent bar",
                                  "horizontal bar", "horizontal stacked bar", "horizontal percent bar", "pie", "donut", "candlestick", "OHLC",
                                  "box plot", "histogram", "heatmap", "bubble"};
    std::string d = c.title.empty() ? std::string("Chart") : c.title;
    d += c.polar ? " (polar), " : ", ";
    d += std::to_string(c.series.size()) + " series";
    for (std::size_t i = 0; i < c.series.size(); ++i) {
        const Series& s = c.series[i];
        d += i ? "; " : ": ";
        d += (s.name.empty() ? "Series " + std::to_string(i + 1) : s.name) + " (" + names[u32(s.type)];
        const std::size_t n = s.type == SeriesType::Candlestick || s.type == SeriesType::Ohlc ? s.candles.size() : s.type == SeriesType::BoxPlot ? s.boxes.size() : s.count();
        d += ", " + std::to_string(n) + " points";
        if (s.count() && s.candles.empty() && s.boxes.empty()) {
            f32 lo, hi;
            minMax(s.ys(), lo, hi);
            char b[64];
            std::snprintf(b, sizeof b, ", %g to %g", double(lo), double(hi));
            d += b;
        }
        d += s.visible ? ")" : ", hidden)";
    }
    return d;
}

// ---------------- export ----------------
namespace {
u32 crc32(const u8* p, std::size_t n) {
    static const auto table = [] {
        std::array<u32, 256> t{};
        for (u32 i = 0; i < 256; ++i) { u32 k = i; for (int j = 0; j < 8; ++j) k = k & 1 ? 0xEDB88320u ^ (k >> 1) : k >> 1; t[i] = k; }
        return t;
    }();
    u32 c = ~0u;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
void be32(std::vector<u8>& v, u32 x) { for (int s = 24; s >= 0; s -= 8) v.push_back(u8(x >> s)); }
} // namespace
bool writePng(const Image& img, const std::string& path) {
    if (img.width <= 0 || img.height <= 0 || img.pixels.size() < std::size_t(img.width) * std::size_t(img.height)) return false;
    std::vector<u8> raw;
    raw.reserve(std::size_t(img.height) * (std::size_t(img.width) * 4 + 1));
    for (int y = 0; y < img.height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < img.width; ++x) {
            const u32 p = img.pixels[std::size_t(y) * std::size_t(img.width) + std::size_t(x)];
            raw.push_back(u8(p)), raw.push_back(u8(p >> 8)), raw.push_back(u8(p >> 16)), raw.push_back(u8(p >> 24));
        }
    }
    std::vector<u8> z = {0x78, 0x01};
    u32 a = 1, b = 0;
    for (u8 c : raw) a = (a + c) % 65521u, b = (b + a) % 65521u;
    for (std::size_t i = 0; i < raw.size(); i += 65535) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - i);
        z.push_back(i + n == raw.size() ? 1 : 0);
        z.push_back(u8(n)), z.push_back(u8(n >> 8)), z.push_back(u8(~n)), z.push_back(u8(~n >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(i), raw.begin() + std::ptrdiff_t(i + n));
    }
    be32(z, (b << 16) | a);
    std::vector<u8> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    auto chunk = [&](const char* type, const std::vector<u8>& d) {
        be32(out, u32(d.size()));
        const std::size_t s = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), d.begin(), d.end());
        be32(out, crc32(out.data() + s, d.size() + 4));
    };
    std::vector<u8> ihdr;
    be32(ihdr, u32(img.width)), be32(ihdr, u32(img.height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && ok;
}
void renderImage(UiContext& ui, const Chart& c, int w, int h, Image& out) {
    DrawList dl;
    TextFns tf;
    tf.measure = [&](std::string_view s, f32 px) { return ui.measureString(s, px); };
    tf.draw = [&](DrawList& d, std::string_view s, Rect r, Color col, f32 px, TextAlignment a) { ui.paintString(d, s, r, col, px, a, 0); };
    render(c, dl, {0, 0, f32(w), f32(h)}, &tf);
    out.resize(w, h, 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    r.render(dl, out);
}

// ---------------- widget ----------------
namespace {
struct ViewState {
    Chart chart;
    ModelMapping mapping;
    Vec2 pointer{}, dragFrom{};
    std::vector<std::pair<f32, f32>> dragRanges;
    Rect band{};
    bool inside = false, dragging = false, banding = false, moved = false, styled = false;
    Hit hover;
    i32 kbSeries = 0, kbIndex = -1;
    TimerHandle anim{};
    f32 animT = 1.f;
};
struct ViewSlot { NodeHandle node; std::unique_ptr<ViewState> s; };
std::vector<ViewSlot>& viewSlots() { static std::vector<ViewSlot> v; return v; }
ViewState* viewOf(const UiContext& ui, NodeHandle n) {
    if (!ui.alive(n)) return nullptr;
    for (auto& s : viewSlots()) if (s.node == n) return s.s.get();
    return nullptr;
}
TextFns textFns(UiContext& ui, u32 node) {
    TextFns t;
    t.measure = [&ui](std::string_view s, f32 px) { return ui.measureString(s, px); };
    t.draw = [&ui, node](DrawList& d, std::string_view s, Rect r, Color c, f32 px, TextAlignment a) { ui.paintString(d, s, r, c, px, a, node); };
    return t;
}
void setHover(UiContext& ui, NodeHandle n, ViewState& st, const Hit& h) {
    const bool changed = h.series != st.hover.series || h.index != st.hover.index;
    st.hover = h;
    st.chart.hoverSeries = h.series, st.chart.hoverIndex = h.index;
    if (changed) {
        ui.setValue(n, f32(h.index), true);
        ui.invalidate(n);
    }
}
void startAnim(UiContext& ui, NodeHandle n, ViewState& st) {
    if (!st.chart.animate) { st.chart.progress = 1.f; return; }
    if (st.anim.valid()) ui.cancelTimer(st.anim);
    st.animT = 0.f;
    st.chart.progress = 0.f;
    st.anim = ui.startTimer(1.f / 60.f, true, [n](UiContext& u) {
        ViewState* s = viewOf(u, n);
        if (!s) return;
        s->animT = std::min(1.f, s->animT + (1.f / 60.f) / 0.45f);
        const f32 t = 1.f - s->animT;
        s->chart.progress = 1.f - t * t * t; // ease-out cubic
        u.invalidate(n);
        if (s->animT >= 1.f) { u.cancelTimer(s->anim); s->anim = {}; }
    });
}
struct ChartW {
    static constexpr std::string_view kName = "fuse.chart";
    static constexpr A11yRole kRole = A11yRole::Image;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {480.f, 320.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        ViewState* st = viewOf(pc.ui, pc.node);
        if (!st) return;
        UiContext& ui = pc.ui;
        if (!st->styled) { // first paint: adopt the computed style (CSS colour / background / font size)
            Theme t = Theme::fromStyle(pc.style);
            st->chart.theme.text = t.text, st->chart.theme.axis = t.axis, st->chart.theme.fontSize = t.fontSize;
            if (pc.style.background.a > 0.f) st->chart.theme.background = t.background, st->chart.theme.plot = t.plot;
            st->styled = true;
        }
        const Theme& th = st->chart.theme;
        const TextFns tf = textFns(ui, pc.node.index);
        render(st->chart, dl, pc.content, &tf);
        if (st->banding) dl.roundedRect(st->band, th.selection.withAlpha(0.12f), {0, 0, 0, 0}, th.selection.withAlpha(0.6f), 1.f);
        if (st->inside && st->hover.series >= 0) {
            const std::string t = tooltip(st->chart, u32(st->hover.series), u32(std::max(0, st->hover.index)));
            if (!t.empty()) {
                const f32 fs = th.fontSize, lh = fs * 1.4f, w = ui.measureString(t, fs) + 16.f, h = lh + 8.f;
                const Rect c = pc.content;
                f32 x = st->pointer.x + 14.f, y = st->pointer.y + 14.f;
                if (x + w > c.right()) x = st->pointer.x - 14.f - w;
                if (y + h > c.bottom()) y = st->pointer.y - 14.f - h;
                x = std::max(c.x, x), y = std::max(c.y, y);
                dl.rect({x, y, w, h}, pc.tint(th.tooltipBg), 4.f);
                ui.paintString(dl, t, {x + 8.f, y + 4.f, w - 16.f, lh}, pc.tint(th.tooltipText), fs, TextAlignment::Start, pc.node.index);
            }
        }
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        ViewState* st = viewOf(ui, ec.node);
        if (!st) return false;
        Chart& c = st->chart;
        const Rect box = ui.rect(ec.node);
        const TextFns tf = textFns(ui, ec.node.index);
        switch (e.type) {
        case EventType::PointerDown:
            st->dragging = true, st->moved = false;
            st->banding = (e.mods & kModShift) != 0;
            st->dragFrom = e.pos;
            st->band = {e.pos.x, e.pos.y, 0, 0};
            st->dragRanges = axisRanges(c);
            return false;
        case EventType::PointerUp: {
            const bool click = !st->moved;
            st->dragging = false;
            if (st->banding) {
                st->banding = false;
                const Layout L = layout(c, box, &tf);
                const Rect b = st->band.intersect(L.plot);
                if (b.w > 4 && b.h > 4) {
                    const auto rg = axisRanges(c);
                    for (u32 a = 0; a < c.axes.size(); ++a) {
                        const Axis& ax = c.axes[a];
                        const bool h = horizontalEdge(ax.edge);
                        const f32 v0 = h ? fromPixel(ax, rg[a].first, rg[a].second, b.x, L.plot.x, L.plot.right()) : fromPixel(ax, rg[a].first, rg[a].second, b.bottom(), L.plot.bottom(), L.plot.y);
                        const f32 v1 = h ? fromPixel(ax, rg[a].first, rg[a].second, b.right(), L.plot.x, L.plot.right()) : fromPixel(ax, rg[a].first, rg[a].second, b.y, L.plot.bottom(), L.plot.y);
                        zoomTo(c, a, std::min(v0, v1), std::max(v0, v1));
                    }
                }
                ui.invalidate(ec.node);
                return true;
            }
            if (!click) return false;
            const Hit h = hitTest(c, box, e.pos, 10.f, &tf);
            if (h.legendItem >= 0) {
                Series& s = c.series[u32(h.legendItem)];
                s.visible = !s.visible;
                ui.emit(ec.node, SignalKind::Toggled, {.value = f32(h.legendItem), .flag = s.visible, .text = "legend"});
                chartChanged(ui, ec.node);
                return true;
            }
            if (h.series >= 0) {
                auto it = std::find_if(c.selection.begin(), c.selection.end(), [&](const Selected& s) { return i32(s.series) == h.series && i32(s.index) == h.index; });
                const bool on = it == c.selection.end();
                if (!(e.mods & kModCtrl)) c.selection.clear();
                else if (!on) c.selection.erase(it);
                if (on) c.selection.push_back({u32(h.series), u32(h.index)});
                ui.emit(ec.node, SignalKind::Toggled, {.value = f32(h.index), .flag = on, .text = c.series[u32(h.series)].name});
                ui.invalidate(ec.node);
            }
            return false;
        }
        case EventType::PointerMove: {
            st->pointer = e.pos;
            st->inside = true;
            if (st->dragging && ui.pressed() == ec.node) {
                const f32 dx = e.pos.x - st->dragFrom.x, dy = e.pos.y - st->dragFrom.y;
                if (std::abs(dx) + std::abs(dy) > 3.f) st->moved = true;
                if (st->banding) st->band = {std::min(st->dragFrom.x, e.pos.x), std::min(st->dragFrom.y, e.pos.y), std::abs(dx), std::abs(dy)};
                else if (st->moved && !c.polar && !onlyPies(c)) {
                    const Layout L = layout(c, box, &tf);
                    for (u32 a = 0; a < c.axes.size() && a < st->dragRanges.size(); ++a) {
                        const Axis& ax = c.axes[a];
                        if (ax.type == AxisType::Log) continue;
                        const auto [lo, hi] = st->dragRanges[a];
                        const f32 d = horizontalEdge(ax.edge) ? -dx / L.plot.w * (hi - lo) : dy / L.plot.h * (hi - lo);
                        zoomTo(c, a, lo + (ax.reversed ? -d : d), hi + (ax.reversed ? -d : d));
                    }
                }
                ui.invalidate(ec.node);
                return false;
            }
            setHover(ui, ec.node, *st, hitTest(c, box, e.pos, 10.f, &tf));
            ui.invalidate(ec.node);
            return false;
        }
        case EventType::PointerLeave:
            st->inside = false;
            setHover(ui, ec.node, *st, {});
            return false;
        case EventType::Wheel: {
            if (e.wheel.y == 0.f || c.polar || onlyPies(c)) return false;
            const Layout L = layout(c, box, &tf);
            const auto rg = axisRanges(c);
            const f32 f = std::pow(0.85f, e.wheel.y);
            for (u32 a = 0; a < c.axes.size(); ++a) {
                const Axis& ax = c.axes[a];
                const bool h = horizontalEdge(ax.edge);
                if (!h && !(e.mods & kModCtrl)) continue;
                const f32 ctr = h ? fromPixel(ax, rg[a].first, rg[a].second, e.pos.x, L.plot.x, L.plot.right()) : fromPixel(ax, rg[a].first, rg[a].second, e.pos.y, L.plot.bottom(), L.plot.y);
                zoom(c, a, f, ctr);
            }
            ui.invalidate(ec.node);
            return true;
        }
        case EventType::KeyDown: {
            if (e.key == Key::Escape) { resetZoom(c); ui.invalidate(ec.node); return true; }
            if (c.series.empty()) return false;
            st->kbSeries = std::clamp(st->kbSeries, 0, i32(c.series.size()) - 1);
            const Series& s = c.series[u32(st->kbSeries)];
            const i32 n = i32(s.type == SeriesType::Candlestick || s.type == SeriesType::Ohlc ? s.candles.size() : s.type == SeriesType::BoxPlot ? s.boxes.size() : s.count());
            if (e.key == Key::Right) st->kbIndex = std::min(n - 1, st->kbIndex + 1);
            else if (e.key == Key::Left) st->kbIndex = std::max(0, st->kbIndex - 1);
            else if (e.key == Key::Home) st->kbIndex = 0;
            else if (e.key == Key::End) st->kbIndex = n - 1;
            else if (e.key == Key::Down || e.key == Key::Up) {
                st->kbSeries = (st->kbSeries + (e.key == Key::Down ? 1 : i32(c.series.size()) - 1)) % i32(c.series.size());
                st->kbIndex = std::max(0, st->kbIndex);
            } else return false;
            Hit h;
            h.series = st->kbSeries, h.index = st->kbIndex;
            setHover(ui, ec.node, *st, h);
            ui.setA11y(ec.node, A11yRole::Image, tooltip(c, u32(h.series), u32(std::max(0, h.index)))); // screen readers hear the point
            return true;
        }
        default: return false;
        }
    }
};
} // namespace

NodeHandle chartView(UiContext& ui, NodeHandle parent, std::string_view id) {
    WidgetRegistry& r = ui.registry();
    if (r.find(ChartW::kName) == 0xFFFFFFFFu) r.registerWidget<ChartW>();
    auto h = ui.create(ChartW::kName, parent, id);
    if (!h) return {};
    auto& slots = viewSlots();
    std::erase_if(slots, [&](const ViewSlot& s) { return !ui.alive(s.node); });
    slots.push_back({*h, std::make_unique<ViewState>()});
    ui.setRange(*h, -1.f, 1e9f);
    ui.setValue(*h, -1.f, false);
    ui.setA11y(*h, A11yRole::Image, describe(slots.back().s->chart));
    return *h;
}
Chart* chartOf(UiContext& ui, NodeHandle v) { ViewState* s = viewOf(ui, v); return s ? &s->chart : nullptr; }
void chartChanged(UiContext& ui, NodeHandle v, bool animateChange) {
    ViewState* s = viewOf(ui, v);
    if (!s) return;
    ui.setA11y(v, A11yRole::Image, describe(s->chart));
    if (animateChange) startAnim(ui, v, *s);
    ui.invalidate(v);
}
Hit chartHovered(const UiContext& ui, NodeHandle v) { const ViewState* s = viewOf(ui, v); return s ? s->hover : Hit{}; }
std::string chartTooltip(const UiContext& ui, NodeHandle v) {
    const ViewState* s = viewOf(ui, v);
    return s && s->hover.series >= 0 ? tooltip(s->chart, u32(s->hover.series), u32(std::max(0, s->hover.index))) : std::string{};
}
void bindModel(UiContext& ui, NodeHandle v, ModelMapping m) {
    if (ViewState* s = viewOf(ui, v)) { s->mapping = std::move(m); chartModelChanged(ui, v); }
}
void chartModelChanged(UiContext& ui, NodeHandle v) {
    ViewState* s = viewOf(ui, v);
    if (!s || !s->mapping.model || !s->mapping.model->rows || !s->mapping.model->data) return;
    const widgets::ItemModel& m = *s->mapping.model;
    const u32 rows = m.rows(m.user);
    std::string scratch, tmp;
    auto num = [&](u32 r, u32 c) {
        tmp.assign(m.data(m.user, r, c, scratch));
        char* end = nullptr;
        const f32 val = std::strtof(tmp.c_str(), &end);
        return end != tmp.c_str() ? val : NAN;
    };
    Chart& c = s->chart;
    c.series.resize(s->mapping.yColumns.size());
    for (std::size_t k = 0; k < s->mapping.yColumns.size(); ++k) {
        Series& se = c.series[k];
        se.type = s->mapping.type;
        const u32 col = s->mapping.yColumns[k];
        se.name = m.header ? std::string(m.header(m.user, col)) : "Series " + std::to_string(k + 1);
        se.clear();
        for (u32 r = 0; r < rows; ++r) {
            se.x.push_back(s->mapping.xColumn < 0 ? f32(r) : num(r, u32(s->mapping.xColumn)));
            se.y.push_back(num(r, col));
        }
    }
    chartChanged(ui, v, c.animate);
}
bool exportImage(UiContext& ui, NodeHandle v, const std::string& path, int w, int h) {
    ViewState* s = viewOf(ui, v);
    if (!s) return false;
    const Rect r = ui.rect(v);
    if (w <= 0) w = std::max(1, int(r.w));
    if (h <= 0) h = std::max(1, int(r.h));
    Image img;
    renderImage(ui, s->chart, w, h, img);
    return writePng(img, path);
}

} // namespace fuse::ui::charts

// ---------------- 3D ----------------
namespace fuse::ui::charts {
void applyTheme(Chart3D& c, const Theme& t) {
    c.background = t.plot;
    c.gridColor = t.text;
    c.colormap.clear();
    const std::size_t n = std::min<std::size_t>(t.palette.size(), 5);
    for (std::size_t i = 0; i < n; ++i) c.colormap.push_back({n > 1 ? f32(i) / f32(n - 1) : 0.f, t.palette[i]});
    c.seriesColors = t.palette;
}
std::string describe(const Chart3D& c) {
    static const char* names[] = {"3D surface", "3D scatter", "3D bars", "3D line", "3D area"};
    std::string d = names[u32(c.kind)];
    char b[96];
    if (c.kind == Kind3D::Scatter || c.kind == Kind3D::Line) std::snprintf(b, sizeof b, ", %zu points", c.points.size());
    else std::snprintf(b, sizeof b, ", %u x %u values", c.rows, c.cols);
    d += b;
    f32 lo[3], hi[3];
    dv3d::ranges(c, lo, hi);
    std::snprintf(b, sizeof b, ", %s %g to %g", c.axis[1].title.empty() ? "values" : c.axis[1].title.c_str(), double(lo[1]), double(hi[1]));
    d += b;
    if (!c.selection.empty()) d += ", " + std::to_string(c.selection.size()) + " selected";
    return d;
}
void render(const Chart3D& c, DrawList& dl, Rect b, const Theme& t, std::vector<dv3d::ProjectedItem>* items, Stats3D* stats) {
    dv3d::render(c, dl, b, t.text, items, stats);
}
void renderImage(UiContext& ui, const Chart3D& c, const Theme& t, int w, int h, Image& out) {
    DrawList dl;
    dl.rect({0, 0, f32(w), f32(h)}, t.background);
    render(c, dl, {0, 0, f32(w), f32(h)}, t);
    out.resize(w, h, 0xFF000000u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    r.render(dl, out);
}
NodeHandle chart3dView(UiContext& ui, NodeHandle parent, Kind3D kind, std::string_view id) {
    const NodeHandle v = widgets::dataViz3d(ui, parent, kind, id);
    if (Chart3D* c = widgets::dataViz3dScene(ui, v)) ui.setA11y(v, A11yRole::Image, describe(*c));
    return v;
}
Chart3D* chart3dOf(UiContext& ui, NodeHandle v) { return widgets::dataViz3dScene(ui, v); }
void chart3dChanged(UiContext& ui, NodeHandle v) {
    if (Chart3D* c = chart3dOf(ui, v)) { ui.setA11y(v, A11yRole::Image, describe(*c)); ui.invalidate(v); }
}
i32 chart3dHovered(const UiContext& ui, NodeHandle v) { return widgets::dataViz3dHovered(ui, v); }
std::vector<std::string> chart3dTooltip(UiContext& ui, NodeHandle v) { return widgets::dataViz3dTooltip(ui, v); }
const std::vector<u32>& chart3dSelection(const UiContext& ui, NodeHandle v) { return widgets::dataViz3dSelection(ui, v); }
bool exportImage3d(UiContext& ui, NodeHandle v, const std::string& path, int w, int h) {
    Chart3D* c = chart3dOf(ui, v);
    if (!c) return false;
    const Rect r = ui.rect(v);
    if (w <= 0) w = std::max(1, int(r.w));
    if (h <= 0) h = std::max(1, int(r.h));
    Image img;
    renderImage(ui, *c, Theme::dark(), w, h, img); // the scene's own background / grid colours win
    return writePng(img, path);
}
} // namespace fuse::ui::charts
