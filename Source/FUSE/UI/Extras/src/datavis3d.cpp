// 3D data visualisation: CPU projection + BSP depth ordering (painter's sort for points / lines) onto DrawList paths. See datavis3d.hpp.
#include "fuse/ui/extras/datavis3d.hpp"

#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <unordered_map>

namespace fuse::ui::dv3d {
namespace {
constexpr f32 kDeg = 3.14159265f / 180.f;

struct View {
    f32 cy, sy, cp, sp; // yaw / pitch cos & sin
    f32 cx0, cy0, scale, dist;
    bool persp;
    f32 lo[3], span[3];
};
View makeView(const Scene& s, Rect t) {
    View v{};
    v.cy = std::cos(s.camera.yaw * kDeg), v.sy = std::sin(s.camera.yaw * kDeg);
    const f32 pitch = std::clamp(s.camera.pitch, -89.f, 89.f);
    v.cp = std::cos(pitch * kDeg), v.sp = std::sin(pitch * kDeg);
    v.cx0 = t.x + t.w * 0.5f, v.cy0 = t.y + t.h * 0.5f;
    v.scale = std::min(t.w, t.h) * 0.30f * std::clamp(s.camera.zoom, 0.1f, 20.f);
    v.dist = 5.f;
    v.persp = s.camera.perspective;
    f32 hi[3];
    ranges(s, v.lo, hi);
    for (int i = 0; i < 3; ++i) v.span[i] = std::max(hi[i] - v.lo[i], 1e-6f);
    return v;
}
/// Normalised cube coordinates [-1, 1]^3 -> camera space (x right, y up, z towards the viewer).
Vec3 toCam(const View& v, Vec3 n) {
    const f32 x = n.x * v.cy + n.z * v.sy, z0 = -n.x * v.sy + n.z * v.cy;
    return {x, n.y * v.cp - z0 * v.sp, n.y * v.sp + z0 * v.cp};
}
Vec2 toScreen(const View& v, Vec3 c) {
    const f32 f = v.persp ? v.dist / std::max(v.dist - c.z, 0.5f) : 1.f;
    return {v.cx0 + c.x * f * v.scale, v.cy0 - c.y * f * v.scale};
}
Vec3 norm(const View& v, Vec3 d) {
    return {(d.x - v.lo[0]) / v.span[0] * 2.f - 1.f, (d.y - v.lo[1]) / v.span[1] * 2.f - 1.f, (d.z - v.lo[2]) / v.span[2] * 2.f - 1.f};
}
Color shade(Color c, f32 k) { return {c.r * k, c.g * k, c.b * k, c.a}; }
Color colormapAt(const Scene& s, f32 t) {
    static const std::vector<GradientStop> kDefault = {{0.f, Color::rgb(0x2040FF)}, {0.25f, Color::rgb(0x20C0FF)}, {0.5f, Color::rgb(0x30E060)},
                                                       {0.75f, Color::rgb(0xFFE030)}, {1.f, Color::rgb(0xFF3020)}};
    fuse::ui::Gradient g;
    g.stops = s.colormap.empty() ? kDefault : s.colormap;
    return g.at(std::clamp(t, 0.f, 1.f));
}
/// Lambert term of a camera-space face normal against a fixed head light from upper left.
f32 lambert(Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 u{b.x - a.x, b.y - a.y, b.z - a.z}, w{c.x - a.x, c.y - a.y, c.z - a.z};
    Vec3 n{u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x};
    const f32 l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (l < 1e-12f) return 1.f;
    const Vec3 L{-0.35f, 0.6f, 0.72f};
    return 0.35f + 0.65f * std::abs((n.x * L.x + n.y * L.y + n.z * L.z) / l);
}

struct Prim {
    Path path;
    Color fill, stroke;
    f32 strokeW = 0.f, depth = 0.f;
    u32 order = 0; ///< stable tie-break
    std::vector<Vec3> cam3; ///< camera-space polygon (BSP-ordered when non-empty)
};

// ---- BSP ordering ----
struct Plane {
    Vec3 n;
    f32 d; ///< dot(n, p) - d: signed distance
};
f32 dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
/// Newell normal through the centroid: a stable plane for slightly non-planar quads (surface cells)
bool planeOf(const std::vector<Vec3>& p, Plane& out) {
    Vec3 n{}, c{};
    for (std::size_t i = 0; i < p.size(); ++i) {
        const Vec3& a = p[i];
        const Vec3& b = p[(i + 1) % p.size()];
        n.x += (a.y - b.y) * (a.z + b.z), n.y += (a.z - b.z) * (a.x + b.x), n.z += (a.x - b.x) * (a.y + b.y);
        c.x += a.x, c.y += a.y, c.z += a.z;
    }
    const f32 l = std::sqrt(dot3(n, n));
    if (l < 1e-9f || p.size() < 3) return false;
    n = {n.x / l, n.y / l, n.z / l};
    const f32 k = 1.f / f32(p.size());
    out = {n, dot3(n, {c.x * k, c.y * k, c.z * k})};
    return true;
}
struct BspBuild {
    const std::vector<std::vector<Vec3>>& src;
    bool persp;
    f32 eye;
    std::vector<BspFragment>& out;
    static constexpr f32 kEps = 1e-4f;
    /// split `f` by `pl` into back / front parts (vertices within kEps count as on the plane)
    static void split(const BspFragment& f, const Plane& pl, std::vector<BspFragment>& back, std::vector<BspFragment>& front,
                      std::vector<BspFragment>& on) {
        int nb = 0, nf = 0;
        std::vector<f32> d(f.pts.size());
        for (std::size_t i = 0; i < f.pts.size(); ++i) {
            d[i] = dot3(pl.n, f.pts[i]) - pl.d;
            if (d[i] > kEps) ++nf;
            else if (d[i] < -kEps) ++nb;
        }
        if (nb == 0 && nf == 0) { on.push_back(f); return; }
        if (nb == 0) { front.push_back(f); return; }
        if (nf == 0) { back.push_back(f); return; }
        BspFragment b{f.source, {}, {}}, fr{f.source, {}, {}};
        auto was = [&](std::size_t i) { return !f.cut.empty() && f.cut[i]; };
        for (std::size_t i = 0; i < f.pts.size(); ++i) {
            const std::size_t j = (i + 1) % f.pts.size();
            const Vec3 &a = f.pts[i], &c = f.pts[j];
            const f32 da = d[i], dc = d[j];
            const bool cross = (da > kEps && dc < -kEps) || (da < -kEps && dc > kEps);
            // the edge leaving a pushed vertex runs along source edge i when the next point of that piece lies on it
            if (da >= -kEps) { fr.pts.push_back(a); fr.cut.push_back(dc >= -kEps || cross ? was(i) : true); }
            if (da <= kEps) { b.pts.push_back(a); b.cut.push_back(dc <= kEps || cross ? was(i) : true); }
            if (cross) {
                const f32 t = da / (da - dc);
                const Vec3 x{a.x + (c.x - a.x) * t, a.y + (c.y - a.y) * t, a.z + (c.z - a.z) * t};
                fr.pts.push_back(x);
                fr.cut.push_back(dc > kEps ? was(i) : true);
                b.pts.push_back(x);
                b.cut.push_back(dc < -kEps ? was(i) : true);
            }
        }
        if (b.pts.size() >= 3) back.push_back(std::move(b));
        if (fr.pts.size() >= 3) front.push_back(std::move(fr));
    }
    /// emit `fs` back to front: (far side, polygons on the plane, near side) recursively. Iterative over an explicit
    /// work stack so degenerate inputs cannot overflow the call stack.
    void run(std::vector<BspFragment> fs) {
        struct Work {
            std::vector<BspFragment> list; ///< subtree to order, or (emit) fragments to output as they are
            bool emit;
        };
        std::vector<Work> stack;
        stack.push_back({std::move(fs), false});
        while (!stack.empty()) {
            Work w = std::move(stack.back());
            stack.pop_back();
            if (w.emit || w.list.size() <= 1) {
                for (auto& f : w.list) out.push_back(std::move(f));
                continue;
            }
            // splitter: of up to 16 candidates spread over the list, the plane that cuts the fewest others (splits
            // leave seams and grow the tree; bar tops would otherwise slice every taller neighbour)
            Plane pl{};
            std::size_t k = w.list.size();
            std::size_t bestCuts = ~std::size_t(0);
            const std::size_t stepK = std::max<std::size_t>(1, w.list.size() / 16);
            for (std::size_t c = 0; c < w.list.size() && bestCuts > 0; c += stepK) {
                Plane cp{};
                if (!planeOf(w.list[c].pts, cp)) continue;
                std::size_t cuts = 0;
                for (std::size_t i = 0; i < w.list.size() && cuts < bestCuts; ++i) {
                    if (i == c) continue;
                    bool pos = false, neg = false;
                    for (const Vec3& q : w.list[i].pts) {
                        const f32 d = dot3(cp.n, q) - cp.d;
                        pos |= d > kEps, neg |= d < -kEps;
                    }
                    cuts += pos && neg;
                }
                if (cuts < bestCuts) bestCuts = cuts, k = c, pl = cp;
            }
            if (k == w.list.size()) { // all degenerate: nothing to order
                for (auto& f : w.list) out.push_back(std::move(f));
                continue;
            }
            std::vector<BspFragment> back, front, on;
            on.push_back(std::move(w.list[k]));
            for (std::size_t i = 0; i < w.list.size(); ++i)
                if (i != k) split(w.list[i], pl, back, front, on);
            // which side is the viewer on
            const f32 side = persp ? dot3(pl.n, {0.f, 0.f, eye}) - pl.d : pl.n.z;
            std::vector<BspFragment>& nearS = side >= 0.f ? front : back;
            std::vector<BspFragment>& farS = side >= 0.f ? back : front;
            // stack: pushed in reverse of processing order
            stack.push_back({std::move(nearS), false});
            stack.push_back({std::move(on), true});
            stack.push_back({std::move(farS), false});
        }
    }
};
} // namespace

std::vector<BspFragment> bspOrder(const std::vector<std::vector<Vec3>>& polys, bool perspective, f32 eyeDist) {
    std::vector<BspFragment> out, in;
    in.reserve(polys.size());
    for (std::size_t i = 0; i < polys.size(); ++i)
        if (polys[i].size() >= 3) in.push_back({u32(i), polys[i], {}});
    out.reserve(in.size());
    BspBuild{polys, perspective, eyeDist, out}.run(std::move(in));
    return out;
}

void ranges(const Scene& s, f32 lo[3], f32 hi[3]) {
    f32 dl[3] = {0.f, 0.f, 0.f}, dh[3] = {1.f, 1.f, 1.f};
    bool any = false;
    auto add = [&](Vec3 p) {
        const f32 v[3] = {p.x, p.y, p.z};
        for (int i = 0; i < 3; ++i) {
            if (!any) dl[i] = dh[i] = v[i];
            else dl[i] = std::min(dl[i], v[i]), dh[i] = std::max(dh[i], v[i]);
        }
        any = true;
    };
    if (s.kind == Kind::Scatter || s.kind == Kind::Line) {
        for (const Vec3& p : s.points) if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) add(p);
    }
    else if (s.rows && s.cols) {
        for (u32 r = 0; r < s.rows; ++r)
            for (u32 c = 0; c < s.cols; ++c) {
                const std::size_t i = std::size_t(r) * s.cols + c;
                add({f32(c), i < s.values.size() ? s.values[i] : 0.f, f32(r)});
            }
        if (s.kind == Kind::Area) dl[1] = std::min(dl[1], 0.f), dh[1] = std::max(dh[1], 0.f); // ribbons stand on 0
        if (s.kind == Kind::Bars) { // bars stand on 0 and occupy cells [c - 0.5, c + 0.5]
            dl[1] = std::min(dl[1], 0.f), dh[1] = std::max(dh[1], 0.f);
            dl[0] -= 0.5f, dh[0] += 0.5f, dl[2] -= 0.5f, dh[2] += 0.5f;
        }
    }
    for (int i = 0; i < 3; ++i) {
        lo[i] = std::isnan(s.axis[i].min) ? dl[i] : s.axis[i].min;
        hi[i] = std::isnan(s.axis[i].max) ? dh[i] : s.axis[i].max;
        if (hi[i] <= lo[i]) hi[i] = lo[i] + 1.f;
    }
}

Vec2 project(const Scene& s, Rect target, Vec3 data, f32* depth) {
    const View v = makeView(s, target);
    const Vec3 c = toCam(v, norm(v, data));
    if (depth) *depth = c.z;
    return toScreen(v, c);
}

i32 pick(const std::vector<ProjectedItem>& items, Vec2 pos, f32 radius) {
    i32 best = -1;
    f32 bd = radius * radius, bz = -3e38f;
    for (const ProjectedItem& it : items) {
        const f32 dx = it.screen.x - pos.x, dy = it.screen.y - pos.y, d = dx * dx + dy * dy;
        if (d < bd - 1e-3f || (d <= bd + 1e-3f && it.depth > bz)) { bd = d, bz = it.depth, best = i32(it.index); }
    }
    return best;
}

std::vector<f32> axisTicks(const Scene& s, int a) {
    f32 lo[3], hi[3];
    ranges(s, lo, hi);
    const f32 span = hi[a] - lo[a];
    const u32 n = std::max(1u, s.axis[a].ticks);
    const f32 raw = span / f32(n), mag = std::pow(10.f, std::floor(std::log10(raw)));
    const f32 r = raw / mag, step = (r < 1.5f ? 1.f : r < 3.f ? 2.f : r < 7.f ? 5.f : 10.f) * mag;
    std::vector<f32> out;
    for (f32 t = std::ceil(lo[a] / step - 1e-4f) * step; t <= hi[a] + step * 1e-4f && out.size() < 64; t += step) out.push_back(std::abs(t) < step * 1e-5f ? 0.f : t);
    return out;
}

void render(const Scene& s, DrawList& dl, Rect target, Color gridColorIn, std::vector<ProjectedItem>* items, Stats* stats) {
    if (items) items->clear();
    if (target.w <= 0.f || target.h <= 0.f) return;
    const Color gridColor = s.gridColor.a > 0.f ? s.gridColor : gridColorIn;
    if (s.background.a > 0.f) dl.rect(target, s.background);
    const View v = makeView(s, target);
    std::size_t drawnItems = 0;
    auto item = [&](u32 i, Vec2 sp, f32 z) { ++drawnItems; if (items) items->push_back({i, sp, z}); };
    std::vector<u32> sel(s.selection);
    std::sort(sel.begin(), sel.end());
    auto lit = [&](std::size_t i) { return i32(i) == s.highlight || std::binary_search(sel.begin(), sel.end(), u32(i)); };
    std::vector<Prim> prims;
    u32 order = 0;
    auto cam = [&](Vec3 n) { return toCam(v, n); };
    auto poly = [&](std::initializer_list<Vec3> pts, Color fill, Color stroke, f32 sw, f32 depthBias = 0.f, bool bsp = false) {
        Prim p;
        f32 z = 0.f;
        bool first = true;
        for (const Vec3& q : pts) {
            const Vec2 sp = toScreen(v, q);
            if (first) p.path.moveTo(sp);
            else p.path.lineTo(sp);
            first = false;
            z += q.z;
        }
        p.path.close();
        if (bsp) p.cam3.assign(pts.begin(), pts.end());
        p.fill = fill, p.stroke = stroke, p.strokeW = sw;
        p.depth = z / f32(pts.size()) + depthBias;
        p.order = order++;
        prims.push_back(std::move(p));
    };

    // ---- axis box: floor + the two walls facing away from the camera, with grid lines ----
    if (s.grid) {
        const Color wall = gridColor.withAlpha(gridColor.a * 0.10f), line = gridColor.withAlpha(gridColor.a * 0.45f);
        const f32 wx = cam({1, 0, 0}).z > 0.f ? -1.f : 1.f;  // the x = ±1 wall farther from the viewer
        const f32 wz = cam({0, 0, 1}).z > 0.f ? -1.f : 1.f;
        const f32 fy = cam({0, 1, 0}).z > 0.f ? -1.f : 1.f;  // floor below (or ceiling when looking from below)
        auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) { poly({cam(a), cam(b), cam(c), cam(d)}, wall, {}, 0.f, -10.f); };
        quad({-1, fy, -1}, {1, fy, -1}, {1, fy, 1}, {-1, fy, 1});
        quad({wx, -1, -1}, {wx, 1, -1}, {wx, 1, 1}, {wx, -1, 1});
        quad({-1, -1, wz}, {1, -1, wz}, {1, 1, wz}, {-1, 1, wz});
        auto gl = [&](Vec3 a, Vec3 b) {
            Prim p;
            p.path.moveTo(toScreen(v, cam(a))).lineTo(toScreen(v, cam(b)));
            p.stroke = line, p.strokeW = 1.f, p.depth = -9.f, p.order = order++;
            prims.push_back(std::move(p));
        };
        for (u32 i = 0; i <= 4; ++i) {
            const f32 t = -1.f + 2.f * f32(i) / 4.f;
            gl({t, fy, -1}, {t, fy, 1});
            gl({-1, fy, t}, {1, fy, t});
            gl({wx, t, -1}, {wx, t, 1});
            gl({-1, t, wz}, {1, t, wz});
            gl({t, -1, wz}, {t, 1, wz});
            gl({wx, -1, t}, {wx, 1, t});
        }
    }

    const Color wire = Color::rgb(0x000000, 0.35f);
    // grid sample (large grids: strided rows / columns, always keeping the last)
    auto sample = [&](u32 n, u32 stride) {
        std::vector<u32> o;
        for (u32 k = 0; k < n; k += stride) o.push_back(k);
        if (!o.empty() && o.back() != n - 1) o.push_back(n - 1);
        return o;
    };
    const u32 gstride = s.rows && s.cols && std::size_t(s.rows) * s.cols > std::size_t(s.maxGrid) * s.maxGrid
                            ? u32(std::ceil(std::sqrt(double(s.rows) * double(s.cols) / (double(s.maxGrid) * double(s.maxGrid))))) : 1u;
    auto marker = [&](Vec2 sp, f32 r) {
        Prim p;
        p.path.circle(sp, r);
        p.fill = Color::rgb(0xFFFFFF), p.stroke = Color::rgb(0x000000), p.strokeW = 1.5f, p.depth = 1e9f, p.order = order++;
        prims.push_back(std::move(p));
    };
    if (s.kind == Kind::Surface && s.rows >= 2 && s.cols >= 2 && s.values.size() >= std::size_t(s.rows) * s.cols) {
        const std::vector<u32> R = sample(s.rows, gstride), C = sample(s.cols, gstride);
        // BSP depth splitting for modest grids; dense ones use the painter's order (quads are tiny, errors sub-pixel)
        const bool exactSort = R.size() * C.size() <= 32u * 32u;
        std::vector<Vec3> c(R.size() * C.size());
        for (std::size_t r = 0; r < R.size(); ++r)
            for (std::size_t k = 0; k < C.size(); ++k) {
                const std::size_t i = std::size_t(R[r]) * s.cols + C[k];
                c[r * C.size() + k] = cam(norm(v, {f32(C[k]), s.values[i], f32(R[r])}));
                item(u32(i), toScreen(v, c[r * C.size() + k]), c[r * C.size() + k].z);
            }
        for (std::size_t r = 0; r + 1 < R.size(); ++r)
            for (std::size_t k = 0; k + 1 < C.size(); ++k) {
                const std::size_t j00 = r * C.size() + k, j01 = j00 + 1, j10 = j00 + C.size(), j11 = j10 + 1;
                auto val = [&](std::size_t rr, std::size_t kk) { return s.values[std::size_t(R[rr]) * s.cols + C[kk]]; };
                const f32 avg = (val(r, k) + val(r, k + 1) + val(r + 1, k) + val(r + 1, k + 1)) * 0.25f;
                Color col = colormapAt(s, (avg - v.lo[1]) / v.span[1]);
                if (s.shading) col = shade(col, lambert(c[j00], c[j01], c[j10]));
                poly({c[j00], c[j01], c[j11], c[j10]}, col, s.wireframe ? wire : Color{}, s.wireframe ? 1.f : 0.f, 0.f, exactSort);
            }
        auto mark = [&](std::size_t i) {
            if (i >= std::size_t(s.rows) * s.cols) return;
            marker(toScreen(v, cam(norm(v, {f32(i % s.cols), s.values[i], f32(i / s.cols)}))), 4.f);
        };
        if (s.highlight >= 0) mark(std::size_t(s.highlight));
        for (u32 i : sel) if (i32(i) != s.highlight) mark(i);
    } else if (s.kind == Kind::Scatter) {
        // large clouds: one point per marker-sized screen cell (the nearest), so draw cost follows the pixels covered
        std::vector<u32> keep;
        if (s.points.size() > s.maxPoints) {
            const f32 cell = std::max(1.f, s.pointSize * 0.75f);
            std::unordered_map<u64, std::pair<u32, f32>> best;
            best.reserve(std::size_t(target.w * target.h / (cell * cell)) + 16);
            for (std::size_t i = 0; i < s.points.size(); ++i) {
                const Vec3 q = cam(norm(v, s.points[i]));
                const Vec2 sp = toScreen(v, q);
                const u64 key = (u64(u32(i32(std::floor(sp.x / cell)) + (1 << 20))) << 32) | u32(i32(std::floor(sp.y / cell)) + (1 << 20));
                auto [it, fresh] = best.try_emplace(key, u32(i), q.z);
                if (!fresh && q.z > it->second.second) it->second = {u32(i), q.z};
            }
            keep.reserve(best.size());
            for (const auto& [k, bz] : best) keep.push_back(bz.first);
            for (u32 i : sel) keep.push_back(i);
            if (s.highlight >= 0) keep.push_back(u32(s.highlight));
            std::sort(keep.begin(), keep.end());
            keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
            while (!keep.empty() && keep.back() >= s.points.size()) keep.pop_back();
        } else {
            keep.resize(s.points.size());
            for (std::size_t i = 0; i < keep.size(); ++i) keep[i] = u32(i);
        }
        for (u32 i : keep) {
            const Vec3 q = cam(norm(v, s.points[i]));
            const Vec2 sp = toScreen(v, q);
            const f32 f = v.persp ? v.dist / std::max(v.dist - q.z, 0.5f) : 1.f;
            Color col = i < s.pointColors.size() ? s.pointColors[i] : colormapAt(s, (s.points[i].y - v.lo[1]) / v.span[1]);
            if (s.shading) col = shade(col, 0.75f + 0.25f * std::clamp(q.z, -1.f, 1.f)); // depth cue
            Prim p;
            const bool hl = lit(i);
            const f32 r = s.pointSize * f * (hl ? 1.6f : 1.f);
            p.path.circle(sp, std::max(r, 1.f));
            p.fill = col, p.stroke = hl ? Color::rgb(0xFFFFFF) : Color::rgb(0x000000, 0.4f), p.strokeW = 1.f;
            p.depth = q.z, p.order = order++;
            prims.push_back(std::move(p));
            item(i, sp, q.z);
        }
    } else if (s.kind == Kind::Line) {
        // depth-sorted segments; large lines drop steps under ~0.75 px (the drawn polyline is unchanged at that scale)
        const bool thin = s.points.size() > s.maxPoints;
        const Color base = s.seriesColors.empty() ? Color{} : s.seriesColors[0];
        bool have = false;
        Vec3 pq{};
        Vec2 ps{};
        f32 py = 0.f;
        for (std::size_t i = 0; i < s.points.size(); ++i) {
            const Vec3 d = s.points[i];
            if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z)) { have = false; continue; }
            const Vec3 q = cam(norm(v, d));
            const Vec2 sp = toScreen(v, q);
            if (have && thin && i + 1 < s.points.size() && !lit(i) && std::abs(sp.x - ps.x) + std::abs(sp.y - ps.y) < 0.75f) continue;
            if (have) {
                Prim p;
                p.path.moveTo(ps).lineTo(sp);
                Color col = base.a > 0.f ? base : colormapAt(s, ((py + d.y) * 0.5f - v.lo[1]) / v.span[1]);
                if (s.shading) col = shade(col, 0.75f + 0.25f * std::clamp((pq.z + q.z) * 0.5f, -1.f, 1.f));
                p.stroke = col, p.strokeW = s.lineWidth;
                p.depth = (pq.z + q.z) * 0.5f, p.order = order++;
                prims.push_back(std::move(p));
            }
            item(u32(i), sp, q.z);
            if (lit(i)) marker(sp, 4.f);
            have = true, pq = q, ps = sp, py = d.y;
        }
    } else if (s.kind == Kind::Area && s.rows && s.cols >= 2 && s.values.size() >= std::size_t(s.rows) * s.cols) {
        // one ribbon per row: quads from the 0 line up to the values, BSP-ordered with the other faces
        const std::vector<u32> C = sample(s.cols, gstride);
        const f32 y0 = std::clamp(0.f, v.lo[1], v.lo[1] + v.span[1]);
        for (u32 r = 0; r < s.rows; ++r) {
            Color base = r < s.seriesColors.size() ? s.seriesColors[r] : colormapAt(s, s.rows > 1 ? f32(r) / f32(s.rows - 1) : 0.5f);
            for (std::size_t k = 0; k + 1 < C.size(); ++k) {
                const std::size_t i0 = std::size_t(r) * s.cols + C[k], i1 = std::size_t(r) * s.cols + C[k + 1];
                const Vec3 a = cam(norm(v, {f32(C[k]), y0, f32(r)})), b = cam(norm(v, {f32(C[k + 1]), y0, f32(r)}));
                const Vec3 c2 = cam(norm(v, {f32(C[k + 1]), s.values[i1], f32(r)})), d = cam(norm(v, {f32(C[k]), s.values[i0], f32(r)}));
                Color col = base;
                if (s.shading) col = shade(col, lambert(a, b, d));
                if (lit(i0) || lit(i1)) col = Color{std::min(col.r * 1.25f + 0.1f, 1.f), std::min(col.g * 1.25f + 0.1f, 1.f), std::min(col.b * 1.25f + 0.1f, 1.f), col.a};
                poly({a, b, c2, d}, col, s.wireframe ? wire : Color{}, s.wireframe ? 1.f : 0.f, 0.f, true);
            }
            for (u32 k : C) {
                const std::size_t i = std::size_t(r) * s.cols + k;
                const Vec3 q = cam(norm(v, {f32(k), s.values[i], f32(r)}));
                item(u32(i), toScreen(v, q), q.z);
                if (lit(i)) marker(toScreen(v, q), 4.f);
            }
        }
    } else if (s.kind == Kind::Bars && s.rows && s.cols) {
        const f32 hw = 0.38f; // bar half width in cells
        const f32 y0n = (0.f - v.lo[1]) / v.span[1] * 2.f - 1.f;
        for (u32 r = 0; r < s.rows; ++r)
            for (u32 k = 0; k < s.cols; ++k) {
                const std::size_t i = std::size_t(r) * s.cols + k;
                const f32 val = i < s.values.size() ? s.values[i] : 0.f;
                const Vec3 a = norm(v, {f32(k) - hw, 0.f, f32(r) - hw}), b = norm(v, {f32(k) + hw, val, f32(r) + hw});
                const f32 x0 = a.x, x1 = b.x, z0 = a.z, z1 = b.z, ylo = std::min(y0n, b.y), yhi = std::max(y0n, b.y);
                Color base = s.barColor.a > 0.f ? s.barColor : colormapAt(s, (val - v.lo[1]) / v.span[1]);
                if (lit(i)) base = Color{std::min(base.r * 1.3f + 0.15f, 1.f), std::min(base.g * 1.3f + 0.15f, 1.f), std::min(base.b * 1.3f + 0.15f, 1.f), base.a};
                const Vec3 P[8] = {cam({x0, ylo, z0}), cam({x1, ylo, z0}), cam({x1, ylo, z1}), cam({x0, ylo, z1}),
                                   cam({x0, yhi, z0}), cam({x1, yhi, z0}), cam({x1, yhi, z1}), cam({x0, yhi, z1})};
                const Vec3 ctr = cam({(x0 + x1) * 0.5f, (ylo + yhi) * 0.5f, (z0 + z1) * 0.5f});
                // faces (outward winding); drawn when facing the camera; all share the bar's centre depth
                const int F[6][4] = {{4, 5, 6, 7}, {0, 3, 2, 1}, {0, 1, 5, 4}, {2, 3, 7, 6}, {1, 2, 6, 5}, {3, 0, 4, 7}};
                const Vec3 N[6] = {cam({0, 1, 0}), cam({0, -1, 0}), cam({0, 0, -1}), cam({0, 0, 1}), cam({1, 0, 0}), cam({-1, 0, 0})};
                for (int f = 0; f < 6; ++f) {
                    const Vec3& n = N[f];
                    const Vec3& p0 = P[F[f][0]];
                    // facing test: perspective view ray from the eye (0, 0, dist) or orthographic (0, 0, 1)
                    const Vec3 eye = v.persp ? Vec3{-p0.x, -p0.y, v.dist - p0.z} : Vec3{0, 0, 1};
                    if (n.x * eye.x + n.y * eye.y + n.z * eye.z <= 0.f) continue;
                    const f32 lit = s.shading ? (f == 0 ? 1.f : f == 1 ? 0.45f : (f >= 4 ? 0.62f : 0.8f)) : 1.f;
                    Prim pr;
                    for (int j = 0; j < 4; ++j) {
                        const Vec2 sp = toScreen(v, P[F[f][j]]);
                        if (j == 0) pr.path.moveTo(sp);
                        else pr.path.lineTo(sp);
                    }
                    pr.path.close();
                    for (int j = 0; j < 4; ++j) pr.cam3.push_back(P[F[f][j]]);
                    pr.fill = shade(base, lit);
                    pr.stroke = s.wireframe ? wire : Color{};
                    pr.strokeW = s.wireframe ? 1.f : 0.f;
                    pr.depth = ctr.z + (f == 0 ? 1e-4f : 0.f);
                    pr.order = order++;
                    prims.push_back(std::move(pr));
                }
                item(u32(i), toScreen(v, cam({(x0 + x1) * 0.5f, b.y, (z0 + z1) * 0.5f})), ctr.z);
            }
    }

    // faces (surface cells, bar faces) are ordered by a BSP tree (exact for interpenetrating / cyclic overlaps);
    // the rest (axis walls and grid behind, points, the highlight marker) keep the painter's sort around them
    std::vector<std::vector<Vec3>> faces;
    std::vector<u32> faceOf;
    std::vector<Prim> rest;
    for (std::size_t i = 0; i < prims.size(); ++i)
        if (!prims[i].cam3.empty()) faces.push_back(prims[i].cam3), faceOf.push_back(u32(i));
        else rest.push_back(std::move(prims[i]));
    std::stable_sort(rest.begin(), rest.end(), [](const Prim& a, const Prim& b) { return a.depth < b.depth; });
    dl.pushClip(target);
    std::size_t ri = 0;
    if (!faces.empty())
        for (; ri < rest.size() && rest[ri].depth <= -9.f; ++ri) dl.path(rest[ri].path, rest[ri].fill, false, rest[ri].stroke, rest[ri].strokeW);
    for (const BspFragment& f : bspOrder(faces, v.persp, v.dist)) {
        const Prim& p = prims[faceOf[f.source]];
        const bool translucent = p.fill.a < 1.f && p.fill.a > 0.f;
        if (!translucent && f.pts.size() == faces[f.source].size() && std::equal(f.pts.begin(), f.pts.end(), faces[f.source].begin(),
                                                                [](Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; })) {
            dl.path(p.path, p.fill, false, p.stroke, p.strokeW); // unsplit: the original path
            continue;
        }
        // split part: fill the piece; where its edge is a BSP cut (not a real edge) the neighbouring pieces must meet
        // without an anti-aliasing seam. Opaque fills: a hairline in the fill colour over the cut. Translucent fills
        // (a line would double up; these take this path split or not, since a surface's or bar's neighbouring faces
        // share edges just the same): every edge moves outward by g (outer silhouettes grow by g < 0.21 px) so that, on the shared pixels, two coverages
        // c = 0.5 + g composite to the fill's own alpha: 1 - (1 - c a)^2 = a  ->  c = (1 - sqrt(1 - a)) / a.
        // Then the wire outline along the source polygon's own edges.
        std::vector<Vec2> sp(f.pts.size());
        for (std::size_t j = 0; j < f.pts.size(); ++j) sp[j] = toScreen(v, f.pts[j]);
        const std::size_t n = sp.size();
        std::vector<Vec2> fillPts = sp;
        const auto isCut = [&](std::size_t j) { return !f.cut.empty() && f.cut[j]; };
        if (translucent && n >= 3) {
            const f32 a = p.fill.a;
            const f32 g = (1.f - std::sqrt(1.f - a)) / a - 0.5f;
            f32 area = 0.f;
            for (std::size_t j = 0; j < n; ++j) area += sp[j].x * sp[(j + 1) % n].y - sp[(j + 1) % n].x * sp[j].y;
            const f32 side = area > 0.f ? 1.f : -1.f; // outward normal of edge d is side * (d.y, -d.x)
            std::vector<Vec2> nrm(n, Vec2{0.f, 0.f}); // unit outward edge normals
            for (std::size_t j = 0; j < n; ++j) {
                const Vec2 d{sp[(j + 1) % n].x - sp[j].x, sp[(j + 1) % n].y - sp[j].y};
                const f32 len = std::sqrt(d.x * d.x + d.y * d.y);
                if (len >= 1e-4f) nrm[j] = {side * d.y / len, -side * d.x / len};
            }
            for (std::size_t j = 0; j < n; ++j) { // vertex j: meet of the offset edges j-1 and j (miter, capped at 4g)
                const Vec2 a0 = nrm[(j + n - 1) % n], a1 = nrm[j];
                const f32 k = std::max(1.f + a0.x * a1.x + a0.y * a1.y, 0.125f);
                fillPts[j].x += g * (a0.x + a1.x) / k, fillPts[j].y += g * (a0.y + a1.y) / k;
            }
        }
        Path piece;
        for (std::size_t j = 0; j < n; ++j) {
            if (j == 0) piece.moveTo(fillPts[j]);
            else piece.lineTo(fillPts[j]);
        }
        piece.close();
        dl.path(piece, p.fill, false, {}, 0.f);
        if (p.fill.a >= 1.f)
            for (std::size_t j = 0; j < n; ++j)
                if (isCut(j)) dl.line(sp[j], sp[(j + 1) % n], p.fill, 1.f);
        if (p.strokeW > 0.f && p.stroke.a > 0.f)
            for (std::size_t j = 0; j < n; ++j)
                if (!isCut(j)) dl.line(sp[j], sp[(j + 1) % n], p.stroke, p.strokeW);
    }
    for (; ri < rest.size(); ++ri) dl.path(rest[ri].path, rest[ri].fill, false, rest[ri].stroke, rest[ri].strokeW);
    dl.popClip();
    if (stats) {
        stats->input = s.kind == Kind::Scatter || s.kind == Kind::Line ? s.points.size() : std::size_t(s.rows) * s.cols;
        stats->drawn = drawnItems;
    }
}

} // namespace fuse::ui::dv3d

// ---- widget ---------------------------------------------------------------------------------------------------------
namespace fuse::ui::widgets {
namespace {
struct Dv3dState {
    dv3d::Scene scene;
    std::vector<dv3d::ProjectedItem> items;
    Vec2 pointer{}, dragFrom{};
    f32 dragYaw = 0.f, dragPitch = 0.f;
    bool inside = false, dragging = false, moved = false;
};
struct Dv3dSlot {
    NodeHandle node;
    std::unique_ptr<Dv3dState> s;
};
std::vector<Dv3dSlot>& dvSlots() { static std::vector<Dv3dSlot> v; return v; }
Dv3dState* stateOf(const UiContext& ui, NodeHandle n) {
    if (!ui.alive(n)) return nullptr;
    for (auto& s : dvSlots()) if (s.node == n) return s.s.get();
    return nullptr;
}
std::string fmt(f32 v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3g", double(v));
    return b;
}
std::vector<std::string> tipLines(const Dv3dState& st, i32 i) {
    const dv3d::Scene& s = st.scene;
    if (i < 0) return {};
    auto title = [&](int a, const char* d) { return s.axis[a].title.empty() ? std::string(d) : s.axis[a].title; };
    if (s.kind == dv3d::Kind::Scatter || s.kind == dv3d::Kind::Line) {
        if (std::size_t(i) >= s.points.size()) return {};
        const dv3d::Vec3 p = s.points[std::size_t(i)];
        return {title(0, "x") + ": " + fmt(p.x), title(1, "y") + ": " + fmt(p.y), title(2, "z") + ": " + fmt(p.z)};
    }
    if (s.cols == 0 || std::size_t(i) >= s.values.size()) return {};
    return {title(2, "row") + ": " + std::to_string(u32(i) / s.cols), title(0, "column") + ": " + std::to_string(u32(i) % s.cols),
            title(1, "value") + ": " + fmt(s.values[std::size_t(i)])};
}
struct Dv3dW {
    static constexpr std::string_view kName = "datavis3d";
    static constexpr A11yRole kRole = A11yRole::Image;
    static constexpr bool kFocusable = true;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext&, NodeHandle, Size) { return {400.f, 300.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        Dv3dState* st = stateOf(pc.ui, pc.node);
        if (!st) return;
        UiContext& ui = pc.ui;
        const Color text = pc.tint(pc.style.color);
        st->scene.highlight = st->inside ? i32(ui.value(pc.node)) : -1;
        dv3d::render(st->scene, dl, pc.content, text, &st->items);
        // axis titles at the far ends of the box edges
        const f32 fs = ui.computed(pc.node).fontSize * 0.8f;
        f32 lo[3], hi[3];
        dv3d::ranges(st->scene, lo, hi);
        const dv3d::Vec3 ends[3] = {{hi[0], lo[1], lo[2]}, {lo[0], hi[1], lo[2]}, {lo[0], lo[1], hi[2]}};
        const char* def[3] = {"X", "Y", "Z"};
        for (int a = 0; a < 3; ++a) {
            const Vec2 p = dv3d::project(st->scene, pc.content, ends[a]);
            const std::string t = st->scene.axis[a].title.empty() ? def[a] : st->scene.axis[a].title;
            ui.paintString(dl, t, {p.x + 4.f, p.y - fs, 160.f, fs * 1.4f}, text, fs, TextAlignment::Start, pc.node.index);
        }
        // tick labels along the three box edges that meet at (lo, lo, lo)
        const f32 ts = fs * 0.85f;
        const Color tc = text.withAlpha(text.a * 0.75f);
        for (int a = 0; a < 3; ++a) {
            const std::vector<f32> tk = dv3d::axisTicks(st->scene, a);
            for (f32 t : tk) {
                dv3d::Vec3 q{lo[0], lo[1], lo[2]};
                (a == 0 ? q.x : a == 1 ? q.y : q.z) = t;
                const Vec2 p = dv3d::project(st->scene, pc.content, q);
                char b[32];
                std::snprintf(b, sizeof b, "%g", double(t));
                const f32 w = ui.measureString(b, ts) + 2.f;
                ui.paintString(dl, b, {a == 1 ? p.x - w - 4.f : p.x - w * 0.5f, a == 1 ? p.y - ts * 0.7f : p.y + 2.f, w, ts * 1.4f}, tc, ts,
                               TextAlignment::Start, pc.node.index);
            }
        }
        if (st->inside && ui.value(pc.node) >= 0.f) {
            const auto lines = tipLines(*st, i32(ui.value(pc.node)));
            if (!lines.empty()) {
                const f32 lh = fs * 1.35f;
                f32 w = 0.f;
                for (const auto& l : lines) w = std::max(w, ui.measureString(l, fs));
                w += 16.f;
                const f32 h = lh * f32(lines.size()) + 8.f;
                const Rect c = pc.content;
                f32 x = st->pointer.x + 14.f, y = st->pointer.y + 14.f;
                if (x + w > c.right()) x = st->pointer.x - 14.f - w;
                if (y + h > c.bottom()) y = st->pointer.y - 14.f - h;
                x = std::max(c.x, x), y = std::max(c.y, y);
                dl.rect({x, y, w, h}, pc.tint(Color::rgb(0x1B1D24, 0.94f)), 4.f);
                for (u32 i = 0; i < lines.size(); ++i)
                    ui.paintString(dl, lines[i], {x + 8.f, y + 4.f + f32(i) * lh, w - 16.f, lh}, pc.tint(Color::rgb(0xF2F3F7)), fs,
                                   TextAlignment::Start, pc.node.index);
            }
        }
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target) return false;
        UiContext& ui = ec.ui;
        Dv3dState* st = stateOf(ui, ec.node);
        if (!st) return false;
        dv3d::Camera& cam = st->scene.camera;
        switch (e.type) {
        case EventType::PointerDown:
            st->dragging = true, st->moved = false;
            st->dragFrom = e.pos, st->dragYaw = cam.yaw, st->dragPitch = cam.pitch;
            return false;
        case EventType::PointerUp: {
            st->dragging = false;
            if (st->moved) return false;
            // click: toggle the item under the pointer (Ctrl adds to the selection)
            const i32 h = dv3d::pick(st->items, e.pos, std::max(10.f, st->scene.pointSize + 4.f));
            if (h < 0) return false;
            auto& sel = st->scene.selection;
            auto it = std::find(sel.begin(), sel.end(), u32(h));
            const bool on = it == sel.end();
            if (!(e.mods & kModCtrl)) sel.clear();
            else if (!on) sel.erase(it);
            if (on) sel.push_back(u32(h));
            ui.emit(ec.node, SignalKind::Toggled, {.value = f32(h), .flag = on});
            ui.invalidate(ec.node);
            return false;
        }
        case EventType::PointerMove: {
            st->pointer = e.pos;
            st->inside = true;
            if (st->dragging && ui.pressed() == ec.node) {
                if (std::abs(e.pos.x - st->dragFrom.x) + std::abs(e.pos.y - st->dragFrom.y) > 3.f) st->moved = true;
                cam.yaw = st->dragYaw + (e.pos.x - st->dragFrom.x) * 0.5f;
                cam.pitch = std::clamp(st->dragPitch + (e.pos.y - st->dragFrom.y) * 0.5f, -89.f, 89.f);
            }
            const i32 h = st->dragging ? -1 : dv3d::pick(st->items, e.pos, std::max(10.f, st->scene.pointSize + 4.f));
            if (f32(h) != ui.value(ec.node)) ui.setValue(ec.node, f32(h), true);
            ui.invalidate(ec.node);
            return false;
        }
        case EventType::PointerLeave:
            st->inside = false;
            if (ui.value(ec.node) != -1.f) ui.setValue(ec.node, -1.f, true);
            ui.invalidate(ec.node);
            return false;
        case EventType::Wheel:
            if (e.wheel.y == 0.f) return false;
            cam.zoom = std::clamp(cam.zoom * std::pow(1.1f, e.wheel.y), 0.3f, 5.f);
            ui.invalidate(ec.node);
            return true;
        case EventType::KeyDown:
            if (e.key == Key::Left || e.key == Key::Right) cam.yaw += e.key == Key::Left ? -5.f : 5.f;
            else if (e.key == Key::Up || e.key == Key::Down) cam.pitch = std::clamp(cam.pitch + (e.key == Key::Up ? 5.f : -5.f), -89.f, 89.f);
            else if (e.key == Key::Home) cam = dv3d::Camera{};
            else if (e.key == Key::Escape && !st->scene.selection.empty()) st->scene.selection.clear();
            else return false;
            ui.invalidate(ec.node);
            return true;
        default: return false;
        }
    }
};
} // namespace

NodeHandle dataViz3d(UiContext& ui, NodeHandle parent, dv3d::Kind kind, std::string_view id) {
    WidgetRegistry& r = ui.registry();
    if (r.find("datavis3d") == 0xFFFFFFFFu) r.registerWidget<Dv3dW>();
    auto h = ui.create("datavis3d", parent, id);
    if (!h) return {};
    auto& slots = dvSlots();
    std::erase_if(slots, [&](const Dv3dSlot& s) { return !ui.alive(s.node); });
    slots.push_back({*h, std::make_unique<Dv3dState>()});
    slots.back().s->scene.kind = kind;
    ui.setRange(*h, 0.f, 0.f);
    ui.setValue(*h, -1.f, false);
    static const char* names[] = {"3D surface", "3D scatter", "3D bars", "3D line", "3D area"};
    ui.setA11y(*h, A11yRole::Image, names[u32(kind)]);
    return *h;
}
void dataViz3dSetGrid(UiContext& ui, NodeHandle w, u32 rows, u32 cols, std::span<const f32> values) {
    if (Dv3dState* s = stateOf(ui, w)) {
        s->scene.rows = rows, s->scene.cols = cols;
        s->scene.values.assign(values.begin(), values.end());
        s->scene.values.resize(std::size_t(rows) * cols, 0.f);
        ui.invalidate(w);
    }
}
void dataViz3dSetPoints(UiContext& ui, NodeHandle w, std::span<const dv3d::Vec3> points, std::span<const Color> colors) {
    if (Dv3dState* s = stateOf(ui, w)) {
        s->scene.points.assign(points.begin(), points.end());
        s->scene.pointColors.assign(colors.begin(), colors.end());
        ui.invalidate(w);
    }
}
dv3d::Scene* dataViz3dScene(UiContext& ui, NodeHandle w) {
    Dv3dState* s = stateOf(ui, w);
    return s ? &s->scene : nullptr;
}
void dataViz3dSetCamera(UiContext& ui, NodeHandle w, const dv3d::Camera& c) {
    if (Dv3dState* s = stateOf(ui, w)) { s->scene.camera = c; ui.invalidate(w); }
}
dv3d::Camera dataViz3dCamera(const UiContext& ui, NodeHandle w) {
    const Dv3dState* s = stateOf(ui, w);
    return s ? s->scene.camera : dv3d::Camera{};
}
const std::vector<u32>& dataViz3dSelection(const UiContext& ui, NodeHandle w) {
    static const std::vector<u32> none;
    const Dv3dState* s = stateOf(ui, w);
    return s ? s->scene.selection : none;
}
i32 dataViz3dHovered(const UiContext& ui, NodeHandle w) {
    const Dv3dState* s = stateOf(ui, w);
    return s && s->inside ? i32(ui.value(w)) : -1;
}
std::vector<std::string> dataViz3dTooltip(UiContext& ui, NodeHandle w) {
    const Dv3dState* s = stateOf(ui, w);
    return s && s->inside ? tipLines(*s, i32(ui.value(w))) : std::vector<std::string>{};
}
} // namespace fuse::ui::widgets
