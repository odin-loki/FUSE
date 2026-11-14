// SVG <filter> -> FilterProgram (see draw_list.hpp). A small tag scanner (attributes only, no entities beyond the
// basics) is enough for filter primitive markup.
#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/style.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace fuse::ui {
namespace {
struct Tag {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    bool close = false;     // </name>
    bool selfClose = false; // <name ... />
    std::string_view attr(std::string_view k) const {
        for (const auto& [n, v] : attrs)
            if (n == k) return v;
        return {};
    }
    bool has(std::string_view k) const {
        for (const auto& a : attrs)
            if (a.first == k) return true;
        return false;
    }
};
std::string_view trimSv(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}
std::string localName(std::string_view n) { // drop a namespace prefix (svg:feOffset)
    const std::size_t c = n.find(':');
    return std::string(c == std::string_view::npos ? n : n.substr(c + 1));
}
/// next tag at or after `i` (comments, <?...?>, <!...> skipped); false at the end
bool nextTag(std::string_view t, std::size_t& i, Tag& out) {
    for (;;) {
        const std::size_t lt = t.find('<', i);
        if (lt == std::string_view::npos) return false;
        if (t.compare(lt, 4, "<!--") == 0) {
            const std::size_t e = t.find("-->", lt + 4);
            if (e == std::string_view::npos) return false;
            i = e + 3;
            continue;
        }
        if (lt + 1 < t.size() && (t[lt + 1] == '?' || t[lt + 1] == '!')) {
            const std::size_t e = t.find('>', lt);
            if (e == std::string_view::npos) return false;
            i = e + 1;
            continue;
        }
        out = {};
        std::size_t j = lt + 1;
        if (j < t.size() && t[j] == '/') out.close = true, ++j;
        const std::size_t n0 = j;
        while (j < t.size() && !std::isspace(static_cast<unsigned char>(t[j])) && t[j] != '>' && t[j] != '/') ++j;
        out.name = localName(t.substr(n0, j - n0));
        for (;;) {
            while (j < t.size() && std::isspace(static_cast<unsigned char>(t[j]))) ++j;
            if (j >= t.size()) return false;
            if (t[j] == '>') { i = j + 1; return true; }
            if (t[j] == '/') { out.selfClose = true; ++j; continue; }
            const std::size_t a0 = j;
            while (j < t.size() && t[j] != '=' && !std::isspace(static_cast<unsigned char>(t[j])) && t[j] != '>' && t[j] != '/') ++j;
            std::string key = localName(t.substr(a0, j - a0));
            while (j < t.size() && std::isspace(static_cast<unsigned char>(t[j]))) ++j;
            std::string val;
            if (j < t.size() && t[j] == '=') {
                ++j;
                while (j < t.size() && std::isspace(static_cast<unsigned char>(t[j]))) ++j;
                if (j < t.size() && (t[j] == '"' || t[j] == '\'')) {
                    const char q = t[j];
                    const std::size_t e = t.find(q, j + 1);
                    if (e == std::string_view::npos) return false;
                    val = std::string(t.substr(j + 1, e - j - 1));
                    j = e + 1;
                } else {
                    const std::size_t v0 = j;
                    while (j < t.size() && !std::isspace(static_cast<unsigned char>(t[j])) && t[j] != '>') ++j;
                    val = std::string(t.substr(v0, j - v0));
                }
            }
            if (!key.empty()) out.attrs.emplace_back(std::move(key), std::move(val));
        }
    }
}
/// numbers in a list ("1 0, 0.5"); false on garbage
bool numbers(std::string_view s, std::vector<f32>& out) {
    out.clear();
    std::string buf(s);
    const char* p = buf.c_str();
    for (;;) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p;
        if (!*p) return true;
        char* e = nullptr;
        const f32 v = std::strtof(p, &e);
        if (e == p) return false;
        out.push_back(v);
        p = e;
    }
}
f32 number(std::string_view s, f32 def) {
    std::vector<f32> v;
    return numbers(s, v) && !v.empty() ? v[0] : def;
}
/// a presentation attribute or the same property inside style="a: b; ..."
std::string_view prop(const Tag& t, std::string_view name) {
    if (t.has(name)) return trimSv(t.attr(name));
    const std::string_view st = t.attr("style");
    std::size_t i = 0;
    while (i < st.size()) {
        std::size_t e = st.find(';', i);
        if (e == std::string_view::npos) e = st.size();
        const std::string_view decl = st.substr(i, e - i);
        const std::size_t c = decl.find(':');
        if (c != std::string_view::npos && trimSv(decl.substr(0, c)) == name) return trimSv(decl.substr(c + 1));
        i = e + 1;
    }
    return {};
}

/// a length / coordinate: number, optional '%'; false when absent or garbage
bool length(std::string_view s, f32& v, bool& pct) {
    s = trimSv(s);
    if (s.empty()) return false;
    pct = s.back() == '%';
    if (pct) s.remove_suffix(1);
    std::string buf(s);
    char* e = nullptr;
    v = std::strtof(buf.c_str(), &e);
    if (e == buf.c_str()) return false;
    while (*e == ' ') ++e;
    return *e == 0 || std::string_view(e) == "px";
}

struct Compiler {
    FilterProgram& prog;
    std::unordered_map<std::string, u8> named;
    u8 last = 0; // previous result (SourceGraphic before the first primitive)
    int alpha = -1;
    bool ok = true;
    u8 fresh() {
        if (prog.planes >= 32) { ok = false; return 0; }
        return prog.planes++;
    }
    FilterOpDesc& emit(FeOp op, u8 in1, u8 in2, u8 out) {
        FilterOpDesc d;
        d.op = op, d.in1 = in1, d.in2 = in2, d.out = out;
        prog.ops.push_back(d);
        return prog.ops.back();
    }
    u8 input(std::string_view ref) {
        ref = trimSv(ref);
        if (ref.empty()) return last;
        if (ref == "SourceGraphic") return 0;
        if (ref == "SourceAlpha") {
            if (alpha < 0) {
                alpha = fresh();
                emit(FeOp::Alpha, 0, 0, u8(alpha));
            }
            return u8(alpha);
        }
        const auto it = named.find(std::string(ref));
        if (it != named.end()) return it->second;
        ok = false; // BackgroundImage / FillPaint / unknown names
        return 0;
    }
    void result(const Tag& t, u8 plane) {
        last = plane;
        const std::string_view r = trimSv(t.attr("result"));
        if (!r.empty()) named[std::string(r)] = plane;
    }
    /// x / y / width / height of a primitive onto its ops [first, end)
    void subregion(const Tag& t, std::size_t first) {
        static const char* const k[4] = {"x", "y", "width", "height"};
        for (int j = 0; j < 4; ++j) {
            f32 v;
            bool pct;
            if (!length(t.attr(k[j]), v, pct)) continue;
            for (std::size_t o = first; o < prog.ops.size(); ++o) {
                prog.ops[o].sub[std::size_t(j)] = v;
                prog.ops[o].subSet |= u8(1u << j);
                if (pct) prog.ops[o].subPct |= u8(1u << j);
            }
        }
    }
    Color color(const Tag& t, std::string_view cprop = "flood-color", std::string_view oprop = "flood-opacity",
                Color def = {0.f, 0.f, 0.f, 1.f}) {
        Color c = def;
        const std::string_view cs = prop(t, cprop);
        if (!cs.empty() && !parseColor(cs, c)) ok = false;
        const std::string_view os = prop(t, oprop);
        if (!os.empty()) {
            f32 o = number(os, 1.f);
            if (os.back() == '%') o /= 100.f;
            c.a *= std::clamp(o, 0.f, 1.f);
        }
        return c;
    }
};

// ---- feTurbulence: the SVG 1.1 reference implementation (lattice and gradients from the seed) -------------------
constexpr int kBSize = 0x100, kBM = 0xff, kPerlinN = 0x1000;
long turbRandom(long seed) {
    constexpr long m = 2147483647, a = 16807, q = 127773, r = 2836;
    seed = a * (seed % q) - r * (seed / q);
    if (seed <= 0) seed += m;
    return seed;
}
/// data: lattice[kBSize + kBSize + 2] then gradient[4][kBSize + kBSize + 2][2]
void turbInit(long seed, std::vector<f32>& data) {
    constexpr int n = kBSize + kBSize + 2;
    std::vector<int> lat(n);
    std::vector<f32> grad(std::size_t(4 * n * 2));
    const auto G = [&](int k, int i, int j) -> f32& { return grad[std::size_t((k * n + i) * 2 + j)]; };
    if (seed <= 0) seed = -(seed % (2147483647 - 1)) + 1;
    if (seed > 2147483647 - 1) seed = 2147483647 - 1;
    int i = 0;
    for (int k = 0; k < 4; ++k)
        for (i = 0; i < kBSize; ++i) {
            lat[std::size_t(i)] = i;
            for (int j = 0; j < 2; ++j) G(k, i, j) = f32(double(((seed = turbRandom(seed)) % (kBSize + kBSize)) - kBSize) / kBSize);
            const double s = std::sqrt(double(G(k, i, 0)) * G(k, i, 0) + double(G(k, i, 1)) * G(k, i, 1));
            if (s > 0.0) G(k, i, 0) = f32(G(k, i, 0) / s), G(k, i, 1) = f32(G(k, i, 1) / s);
        }
    while (--i) {
        const int k = lat[std::size_t(i)];
        const int j = int((seed = turbRandom(seed)) % kBSize);
        lat[std::size_t(i)] = lat[std::size_t(j)];
        lat[std::size_t(j)] = k;
    }
    for (i = 0; i < kBSize + 2; ++i) {
        lat[std::size_t(kBSize + i)] = lat[std::size_t(i)];
        for (int k = 0; k < 4; ++k)
            for (int j = 0; j < 2; ++j) G(k, kBSize + i, j) = G(k, i, j);
    }
    data.clear();
    for (int v : lat) data.push_back(f32(v));
    data.insert(data.end(), grad.begin(), grad.end());
}

f32& param(GradientStop* op, int k) {
    GradientStop& st = op[2 + k / 5];
    switch (k % 5) {
    case 0: return st.color.r;
    case 1: return st.color.g;
    case 2: return st.color.b;
    case 3: return st.color.a;
    default: return st.offset;
    }
}
f32 dataAt(const GradientStop* prog, int i) {
    const GradientStop& st = prog[2 + 7 * int(prog[0].offset) + i / 5];
    switch (i % 5) {
    case 0: return st.color.r;
    case 1: return st.color.g;
    case 2: return st.color.b;
    case 3: return st.color.a;
    default: return st.offset;
    }
}
constexpr f32 kInfMargin = 1048576.f;
int blurRadius(f32 sg) { return sg > 0.f ? std::max(1, int(std::floor(sg + 0.5f))) : 0; }

/// derived words of every op + the header margin
void deriveProgram(GradientStop* h) {
    const int K = int(h[0].offset);
    f32 margin = 1.f;
    for (int i = 0; i < K; ++i) {
        GradientStop* op = h + 2 + 7 * i;
        const auto P = [&](int k) -> f32& { return param(op, k); };
        switch (FeOp(int(op[0].offset))) {
        case FeOp::Blur:
            for (int a = 0; a < 2; ++a) {
                const int r = blurRadius(P(a));
                P(2 + a) = f32(r);
                P(4 + a) = 1.f / f32(2 * r + 1);
            }
            margin += 3.f * std::max(P(2), P(3));
            break;
        case FeOp::Offset:
            P(2) = f32(std::lround(P(0))), P(3) = f32(std::lround(P(1)));
            margin += std::max(std::abs(P(2)), std::abs(P(3)));
            break;
        case FeOp::Morphology:
            P(3) = f32(std::min(64l, std::lround(std::max(0.f, P(1))))), P(4) = f32(std::min(64l, std::lround(std::max(0.f, P(2)))));
            margin += std::max(P(3), P(4));
            break;
        case FeOp::Lighting: margin += 1.f; break;
        case FeOp::Tile: {
            const f32 x0 = std::floor(P(0) + 0.5f), y0 = std::floor(P(1) + 0.5f);
            P(4) = x0, P(5) = y0;
            P(6) = std::max(1.f, std::floor(P(0) + P(2) + 0.5f) - x0), P(7) = std::max(1.f, std::floor(P(1) + P(3) + 0.5f) - y0);
            margin = kInfMargin;
            break;
        }
        case FeOp::Image:
            P(6) = P(2) > 0.f ? P(4) / P(2) : 0.f, P(7) = P(3) > 0.f ? P(5) / P(3) : 0.f;
            break;
        case FeOp::Convolve:
            margin += P(6) == 1.f ? kInfMargin : std::max(P(0), P(1));
            break;
        case FeOp::Displace: margin += std::ceil(std::abs(P(0)) * 0.5f) + 1.f; break;
        case FeOp::Turbulence: {
            f32 fx = P(0), fy = P(1);
            const f32 tx = P(7), ty = P(8), tw = P(9), th = P(10);
            int nw = 0, nh = 0, wx = 0, wy = 0;
            if (P(4) != 0.f && tw > 0.f && th > 0.f) { // stitchTiles: frequencies that tile the subregion exactly
                if (fx != 0.f) {
                    const f32 lo = std::floor(tw * fx) / tw, hi = std::ceil(tw * fx) / tw;
                    fx = (lo > 0.f && fx / lo < hi / fx) ? lo : hi;
                }
                if (fy != 0.f) {
                    const f32 lo = std::floor(th * fy) / th, hi = std::ceil(th * fy) / th;
                    fy = (lo > 0.f && fy / lo < hi / fy) ? lo : hi;
                }
                nw = int(tw * fx + 0.5f), nh = int(th * fy + 0.5f);
                wx = int(tx * fx + f32(kPerlinN) + f32(nw)), wy = int(ty * fy + f32(kPerlinN) + f32(nh));
            }
            P(11) = fx, P(12) = fy, P(13) = f32(nw), P(14) = f32(nh), P(15) = f32(wx), P(16) = f32(wy);
            break;
        }
        default: break;
        }
        if (margin > kInfMargin) margin = kInfMargin;
    }
    h[0].color.g = margin;
}
} // namespace

int FilterProgram::reach() const {
    int r = 1;
    for (const FilterOpDesc& d : ops) {
        if (d.op == FeOp::Blur) {
            r += 3 * std::max(blurRadius(d.p[0]), blurRadius(d.p[1]));
        } else if (d.op == FeOp::Offset) {
            r += int(std::max(std::abs(std::lround(d.p[0])), std::abs(std::lround(d.p[1]))));
        }
    }
    return r;
}

FilterProgram FilterProgram::resolve(const Rect& bb) const {
    FilterProgram r = *this;
    r.resolved = true;
    const f32 bw = bb.w, bh = bb.h, diag = std::sqrt((bw * bw + bh * bh) * 0.5f);
    const auto ext = [&](int axis) { return axis == 0 ? bw : axis == 1 ? bh : diag; };
    // a coordinate / length in filterUnits (prim false) or primitiveUnits (prim true); % of the bbox
    const auto coord = [&](f32 v, bool pct, int axis, bool prim) {
        const bool box = prim ? primitiveUnitsBox : !filterUnitsUser;
        const f32 org = axis == 0 ? bb.x : bb.y;
        return org + (pct ? v * 0.01f * ext(axis) : box ? v * ext(axis) : v);
    };
    const auto len = [&](f32 v, bool pct, int axis, bool prim) {
        const bool box = prim ? primitiveUnitsBox : !filterUnitsUser;
        return pct ? v * 0.01f * ext(axis) : box ? v * ext(axis) : v;
    };
    r.regionPx = {coord(region[0], regionPct & 1, 0, false), coord(region[1], regionPct & 2, 1, false),
                  len(region[2], regionPct & 4, 0, false), len(region[3], regionPct & 8, 1, false)};
    std::vector<Rect> planeSub(std::size_t(std::max<u8>(planes, 1)), r.regionPx);
    for (FilterOpDesc& d : r.ops) {
        Rect def{};
        switch (d.op) {
        case FeOp::Flood: case FeOp::Turbulence: case FeOp::Image: case FeOp::Tile: def = r.regionPx; break;
        case FeOp::Blend: case FeOp::Composite: case FeOp::Displace:
            def = planeSub[d.in1 % planeSub.size()].unite(planeSub[d.in2 % planeSub.size()]);
            break;
        default: def = planeSub[d.in1 % planeSub.size()]; break;
        }
        f32 v[4] = {def.x, def.y, def.w, def.h};
        for (int j = 0; j < 4; ++j)
            if (d.subSet >> j & 1u) {
                const bool pct = d.subPct >> j & 1u;
                v[j] = j < 2 ? coord(d.sub[std::size_t(j)], pct, j, true) : len(d.sub[std::size_t(j)], pct, j - 2, true);
            }
        const Rect s = Rect{v[0], v[1], std::max(0.f, v[2]), std::max(0.f, v[3])}.intersect(r.regionPx);
        const Rect inSub = planeSub[d.in1 % planeSub.size()];
        d.sub = {s.x, s.y, s.w, s.h};
        d.subSet = 0xF, d.subPct = 0;
        planeSub[d.out % planeSub.size()] = s;
        auto& p = d.p;
        switch (d.op) {
        case FeOp::Blur: case FeOp::Offset:
            p[0] = len(p[0], false, 0, true), p[1] = len(p[1], false, 1, true);
            break;
        case FeOp::Morphology: p[1] = len(p[1], false, 0, true), p[2] = len(p[2], false, 1, true); break;
        case FeOp::Displace: p[0] = len(p[0], false, 2, true); break;
        case FeOp::Lighting:
            if (p[7] != 0.f) {
                p[8] = coord(p[8], false, 0, true), p[9] = coord(p[9], false, 1, true), p[10] = len(p[10], false, 2, true);
                if (p[7] == 2.f) {
                    const f32 ax = coord(p[11], false, 0, true) - p[8], ay = coord(p[12], false, 1, true) - p[9];
                    const f32 az = len(p[13], false, 2, true) - p[10];
                    const f32 l = std::sqrt(ax * ax + ay * ay + az * az);
                    p[11] = l > 0.f ? ax / l : 0.f, p[12] = l > 0.f ? ay / l : 0.f, p[13] = l > 0.f ? az / l : 0.f;
                }
            }
            break;
        case FeOp::Turbulence:
            p[5] = bb.x, p[6] = bb.y;
            p[7] = s.x - bb.x, p[8] = s.y - bb.y, p[9] = s.w, p[10] = s.h;
            break;
        case FeOp::Image: p[0] = s.x, p[1] = s.y, p[2] = s.w, p[3] = s.h; break;
        case FeOp::Tile: p[0] = inSub.x, p[1] = inSub.y, p[2] = inSub.w, p[3] = inSub.h; break;
        default: break;
        }
    }
    return r;
}

bool encodeFilterProgram(const FilterProgram& pr, std::vector<GradientStop>& stops, u16& count) {
    count = 0;
    if (pr.empty()) return false;
    std::size_t dataN = 0;
    for (const FilterOpDesc& d : pr.ops) dataN += d.data.size();
    const std::size_t total = 2 + 7 * pr.ops.size() + (dataN + 4) / 5;
    if (total > 65535) return false;
    const std::size_t at = stops.size();
    const Rect& g = pr.regionPx;
    stops.push_back({f32(pr.ops.size()), Color{f32(pr.planes), 0.f, f32((dataN + 4) / 5), 0.f}});
    stops.push_back({0.f, Color{g.x, g.y, g.w, g.h}});
    std::vector<f32> data;
    data.reserve(dataN);
    for (const FilterOpDesc& d : pr.ops) {
        stops.push_back({f32(int(d.op)), Color{f32(d.in1), f32(d.in2), f32(d.out), f32(data.size())}});
        stops.push_back({0.f, Color{d.sub[0], d.sub[1], d.sub[2], d.sub[3]}});
        for (int j = 0; j < 5; ++j)
            stops.push_back({d.p[std::size_t(j * 5 + 4)], Color{d.p[std::size_t(j * 5)], d.p[std::size_t(j * 5 + 1)],
                                                              d.p[std::size_t(j * 5 + 2)], d.p[std::size_t(j * 5 + 3)]}});
        data.insert(data.end(), d.data.begin(), d.data.end());
    }
    data.resize((dataN + 4) / 5 * 5, 0.f);
    for (std::size_t j = 0; j < data.size(); j += 5) stops.push_back({data[j + 4], Color{data[j], data[j + 1], data[j + 2], data[j + 3]}});
    deriveProgram(stops.data() + at);
    count = u16(total);
    return true;
}

void scaleFilterProgram(GradientStop* h, f32 s) {
    const int K = int(h[0].offset);
    h[1].color.r *= s, h[1].color.g *= s, h[1].color.b *= s, h[1].color.a *= s;
    for (int i = 0; i < K; ++i) {
        GradientStop* op = h + 2 + 7 * i;
        op[1].color.r *= s, op[1].color.g *= s, op[1].color.b *= s, op[1].color.a *= s;
        const auto P = [&](int k) -> f32& { return param(op, k); };
        switch (FeOp(int(op[0].offset))) {
        case FeOp::Blur: case FeOp::Offset: P(0) *= s, P(1) *= s; break;
        case FeOp::Morphology: P(1) *= s, P(2) *= s; break;
        case FeOp::Displace: P(0) *= s; break;
        case FeOp::Lighting:
            P(1) *= s;
            if (P(7) != 0.f) P(8) *= s, P(9) *= s, P(10) *= s;
            break;
        case FeOp::Turbulence:
            if (s > 0.f) P(0) /= s, P(1) /= s;
            for (int k = 5; k <= 10; ++k) P(k) *= s;
            break;
        case FeOp::Image: case FeOp::Tile: for (int k = 0; k < 4; ++k) P(k) *= s; break;
        default: break;
        }
    }
    deriveProgram(h);
}

FeOp filterProgramOp(const GradientStop* prog, u32 i) { return FeOp(int(prog[2 + 7 * i].offset)); }

// ---- CPU executor (ui_backdrop.glsl runOp / modes 8, 9: same arithmetic, same order) -----------------------------
namespace {
constexpr f32 kInv255 = 0.003921568859368563f;
void boxPassH(const f32* in, f32* out, int w, int h, int r, f32 inv) {
    for (int y = 0; y < h; ++y)
        for (int k = 0; k < 4; ++k) {
            const std::size_t row = std::size_t(y) * std::size_t(w) * 4 + std::size_t(k);
            f32 acc = 0.f;
            for (int i = -r; i <= r; ++i) acc = acc + in[row + std::size_t(std::clamp(i, 0, w - 1)) * 4];
            for (int i = 0; i < w; ++i) {
                out[row + std::size_t(i) * 4] = acc * inv;
                const f32 d = in[row + std::size_t(std::clamp(i + r + 1, 0, w - 1)) * 4] - in[row + std::size_t(std::clamp(i - r, 0, w - 1)) * 4];
                acc = acc + d;
            }
        }
}
void boxPassV(const f32* in, f32* out, int w, int h, int r, f32 inv) {
    const std::size_t stride = std::size_t(w) * 4;
    for (std::size_t col = 0; col < stride; ++col) {
        f32 acc = 0.f;
        for (int i = -r; i <= r; ++i) acc = acc + in[std::size_t(std::clamp(i, 0, h - 1)) * stride + col];
        for (int i = 0; i < h; ++i) {
            out[std::size_t(i) * stride + col] = acc * inv;
            const f32 d = in[std::size_t(std::clamp(i + r + 1, 0, h - 1)) * stride + col] - in[std::size_t(std::clamp(i - r, 0, h - 1)) * stride + col];
            acc = acc + d;
        }
    }
}
f32 ppow(f32 x, f32 y) { return x <= 0.f ? (y == 0.f ? 1.f : 0.f) : std::pow(x, y); }
f32 sCurve(f32 t) { return t * t * (3.f - 2.f * t); }
f32 lerpT(f32 t, f32 a, f32 b) { return a + t * (b - a); }
int posMod(int a, int m) { return a >= 0 ? a % m : (m - (-a) % m) % m; }

struct Exec {
    const GradientStop* prog;
    const GradientStop* op;
    int w, h, x0, y0, dataBase;
    bool hdr;
    f32 P(int k) const { return param(const_cast<GradientStop*>(op), k); }
    f32 D(int i) const { return dataAt(prog, dataBase + i); }
    static const f32* px(const f32* pl, int w, int x, int y) { return pl + (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4; }
    f32 noise2(int c, f32 vx, f32 vy, bool stitch, int nw, int nh, int wx, int wy) const {
        const f32 tx = vx + f32(kPerlinN);
        int bx0 = int(tx), bx1 = bx0 + 1;
        const f32 rx0 = tx - f32(int(tx)), rx1 = rx0 - 1.f;
        const f32 ty = vy + f32(kPerlinN);
        int by0 = int(ty), by1 = by0 + 1;
        const f32 ry0 = ty - f32(int(ty)), ry1 = ry0 - 1.f;
        if (stitch) {
            if (bx0 >= wx) bx0 -= nw;
            if (bx1 >= wx) bx1 -= nw;
            if (by0 >= wy) by0 -= nh;
            if (by1 >= wy) by1 -= nh;
        }
        bx0 &= kBM, bx1 &= kBM, by0 &= kBM, by1 &= kBM;
        const int i = int(D(bx0)), j = int(D(bx1));
        const int b00 = int(D(i + by0)), b10 = int(D(j + by0)), b01 = int(D(i + by1)), b11 = int(D(j + by1));
        const f32 sx = sCurve(rx0), sy = sCurve(ry0);
        constexpr int n = kBSize + kBSize + 2;
        const auto g = [&](int b, int k) { return D(n + (c * n + b) * 2 + k); };
        f32 u = rx0 * g(b00, 0) + ry0 * g(b00, 1);
        f32 v = rx1 * g(b10, 0) + ry0 * g(b10, 1);
        const f32 a = lerpT(sx, u, v);
        u = rx0 * g(b01, 0) + ry1 * g(b01, 1);
        v = rx1 * g(b11, 0) + ry1 * g(b11, 1);
        const f32 b = lerpT(sx, u, v);
        return lerpT(sy, a, b);
    }
    f32 alphaAt(const f32* A, int x, int y) const { return px(A, w, x, y)[3] * kInv255; }
    void pixel(FeOp kind, const f32* A, const f32* B, int x, int y, f32* r) const {
        const f32* a = px(A, w, x, y);
        const f32* b = px(B, w, x, y);
        const int gx = x0 + x, gy = y0 + y;
        switch (kind) {
        case FeOp::Offset: {
            const int qx = x - int(P(2)), qy = y - int(P(3));
            if (qx >= 0 && qy >= 0 && qx < w && qy < h) std::memcpy(r, px(A, w, qx, qy), 16);
            else r[0] = r[1] = r[2] = r[3] = 0.f;
            break;
        }
        case FeOp::Matrix: {
            const f32 al = a[3] / 255.f;
            f32 st[4] = {0.f, 0.f, 0.f, al};
            if (al > 0.f)
                for (int k = 0; k < 3; ++k) st[k] = std::clamp(a[k] / 255.f / al, 0.f, 1.f);
            f32 res[4];
            for (int k = 0; k < 4; ++k) {
                const f32 v = P(k * 5) * st[0] + P(k * 5 + 1) * st[1] + P(k * 5 + 2) * st[2] + P(k * 5 + 3) * st[3] + P(k * 5 + 4);
                res[k] = hdr && k < 3 ? std::max(v, 0.f) : std::clamp(v, 0.f, 1.f); // float target keeps rgb above 1
            }
            for (int k = 0; k < 3; ++k) r[k] = res[k] * res[3] * 255.f;
            r[3] = res[3] * 255.f;
            break;
        }
        case FeOp::Flood: {
            const f32 al = P(3);
            r[0] = P(0) * al * 255.f, r[1] = P(1) * al * 255.f, r[2] = P(2) * al * 255.f, r[3] = al * 255.f;
            break;
        }
        case FeOp::Alpha: r[0] = r[1] = r[2] = 0.f, r[3] = a[3]; break;
        case FeOp::Blend: {
            const int mode = int(P(0));
            const f32 qa = a[3] * kInv255, qb = b[3] * kInv255;
            for (int k = 0; k < 3; ++k) {
                const f32 ia = a[k] * (1.f - qb), ib = b[k] * (1.f - qa);
                const f32 ab = a[k] * b[k] * kInv255;
                f32 v;
                if (mode == 1) v = ia + ib + ab;
                else if (mode == 2) v = a[k] + b[k] - ab;
                else if (mode == 3) v = std::min(ia + b[k], ib + a[k]);
                else if (mode == 4) v = std::max(ia + b[k], ib + a[k]);
                else v = a[k] + ib;
                r[k] = v;
            }
            r[3] = a[3] + b[3] - a[3] * b[3] * kInv255;
            break;
        }
        case FeOp::Composite: {
            const int opr = int(P(0));
            const f32 qa = a[3] * kInv255, qb = b[3] * kInv255;
            if (opr == 5) {
                const f32 k1 = P(1), k2 = P(2), k3 = P(3), k4 = P(4);
                for (int k = 0; k < 4; ++k) {
                    const f32 v = k1 * a[k] * b[k] * kInv255 + k2 * a[k] + k3 * b[k] + k4 * 255.f;
                    r[k] = std::clamp(v, 0.f, 255.f);
                }
                for (int k = 0; k < 3; ++k) r[k] = std::min(r[k], r[3]);
            } else {
                for (int k = 0; k < 4; ++k) {
                    f32 v;
                    if (opr == 1) v = a[k] * qb;
                    else if (opr == 2) v = a[k] * (1.f - qb);
                    else if (opr == 3) v = a[k] * qb + b[k] * (1.f - qa);
                    else if (opr == 4) v = a[k] * (1.f - qb) + b[k] * (1.f - qa);
                    else v = a[k] + b[k] * (1.f - qa);
                    r[k] = v;
                }
            }
            break;
        }
        case FeOp::Turbulence: {
            const f32 fx = P(11), fy = P(12);
            const int oct = std::clamp(int(P(2)), 0, 10);
            const bool fractal = P(3) != 0.f, stitch = P(4) != 0.f;
            const f32 bx = (f32(gx) - P(5)) * fx, by = (f32(gy) - P(6)) * fy;
            f32 res[4];
            for (int c = 0; c < 4; ++c) {
                int nw = int(P(13)), nh = int(P(14)), wx = int(P(15)), wy = int(P(16));
                f32 sum = 0.f, vx = bx, vy = by, ratio = 1.f;
                for (int o = 0; o < oct; ++o) {
                    const f32 nz = noise2(c, vx, vy, stitch, nw, nh, wx, wy);
                    sum = sum + (fractal ? nz : std::abs(nz)) / ratio;
                    vx = vx * 2.f, vy = vy * 2.f, ratio = ratio * 2.f;
                    if (stitch) nw *= 2, wx = 2 * wx - kPerlinN, nh *= 2, wy = 2 * wy - kPerlinN;
                }
                const f32 v = fractal ? (sum * 255.f + 255.f) * 0.5f : sum * 255.f;
                res[c] = std::clamp(v, 0.f, 255.f);
            }
            const f32 al = res[3] * kInv255;
            r[0] = res[0] * al, r[1] = res[1] * al, r[2] = res[2] * al, r[3] = res[3];
            break;
        }
        case FeOp::Lighting: {
            const bool l = x > 0, rr = x < w - 1, u = y > 0, dn = y < h - 1;
            const int xl = l ? x - 1 : x, xr = rr ? x + 1 : x, yu = u ? y - 1 : y, yd = dn ? y + 1 : y;
            const f32 wu = u ? 1.f : 0.f, wd = dn ? 1.f : 0.f, wl = l ? 1.f : 0.f, wr = rr ? 1.f : 0.f;
            const f32 fxx = (l && rr) ? 0.5f : 1.f, fyy = (u && dn) ? 0.5f : 1.f;
            const int sw = int(wu + wd), sh = int(wl + wr);
            const f32 facx = sw == 2 ? 0.5f : sw == 1 ? 0.6666666865348816f : 1.f;
            const f32 facy = sh == 2 ? 0.5f : sh == 1 ? 0.6666666865348816f : 1.f;
            const f32 sxv = wu * (alphaAt(A, xr, yu) - alphaAt(A, xl, yu)) + 2.f * (alphaAt(A, xr, y) - alphaAt(A, xl, y)) +
                            wd * (alphaAt(A, xr, yd) - alphaAt(A, xl, yd));
            const f32 syv = wl * (alphaAt(A, xl, yd) - alphaAt(A, xl, yu)) + 2.f * (alphaAt(A, x, yd) - alphaAt(A, x, yu)) +
                            wr * (alphaAt(A, xr, yd) - alphaAt(A, xr, yu));
            const f32 ss = P(1);
            const f32 nx = -ss * facx * fxx * sxv, ny = -ss * facy * fyy * syv;
            const f32 nl = std::sqrt(nx * nx + ny * ny + 1.f);
            const f32 Nx = nx / nl, Ny = ny / nl, Nz = 1.f / nl;
            f32 Lx = P(8), Ly = P(9), Lz = P(10);
            const int lt = int(P(7));
            if (lt != 0) {
                Lx = P(8) - f32(gx), Ly = P(9) - f32(gy), Lz = P(10) - ss * alphaAt(A, x, y);
                const f32 ll = std::sqrt(Lx * Lx + Ly * Ly + Lz * Lz);
                if (ll > 0.f) Lx = Lx / ll, Ly = Ly / ll, Lz = Lz / ll;
            }
            f32 lc[3] = {P(4), P(5), P(6)};
            if (lt == 2) {
                const f32 d = -(Lx * P(11) + Ly * P(12) + Lz * P(13));
                const f32 f = d < P(15) ? 0.f : ppow(d, P(14));
                lc[0] = lc[0] * f, lc[1] = lc[1] * f, lc[2] = lc[2] * f;
            }
            if (P(0) == 0.f) {
                const f32 f = P(2) * std::max(Nx * Lx + Ny * Ly + Nz * Lz, 0.f);
                for (int k = 0; k < 3; ++k) r[k] = std::clamp(f * lc[k], 0.f, 1.f) * 255.f;
                r[3] = 255.f;
            } else {
                f32 Hx = Lx, Hy = Ly, Hz = Lz + 1.f;
                const f32 hl = std::sqrt(Hx * Hx + Hy * Hy + Hz * Hz);
                if (hl > 0.f) Hx = Hx / hl, Hy = Hy / hl, Hz = Hz / hl;
                const f32 f = P(2) * ppow(std::max(Nx * Hx + Ny * Hy + Nz * Hz, 0.f), P(3));
                f32 c[3];
                for (int k = 0; k < 3; ++k) c[k] = std::clamp(f * lc[k], 0.f, 1.f);
                const f32 al = std::max(c[0], std::max(c[1], c[2]));
                r[0] = c[0] * 255.f, r[1] = c[1] * 255.f, r[2] = c[2] * 255.f, r[3] = al * 255.f;
            }
            break;
        }
        case FeOp::Morphology: {
            const int rx = int(P(3)), ry = int(P(4));
            const bool dil = P(0) != 0.f;
            if (rx == 0 && ry == 0) { std::memcpy(r, a, 16); break; }
            for (int k = 0; k < 4; ++k) r[k] = dil ? 0.f : 255.f;
            for (int j = -ry; j <= ry; ++j)
                for (int i = -rx; i <= rx; ++i) {
                    const int qx = x + i, qy = y + j;
                    const bool in = qx >= 0 && qy >= 0 && qx < w && qy < h;
                    for (int k = 0; k < 4; ++k) {
                        const f32 v = in ? px(A, w, qx, qy)[k] : 0.f;
                        r[k] = dil ? std::max(r[k], v) : std::min(r[k], v);
                    }
                }
            break;
        }
        case FeOp::Image: {
            const f32 fx = (f32(gx) + 0.5f - P(0)) * P(6), fy = (f32(gy) + 0.5f - P(1)) * P(7);
            const int iw = int(P(4)), ih = int(P(5));
            const int u = std::clamp(int(std::floor(fx)), 0, std::max(0, iw - 1)), v = std::clamp(int(std::floor(fy)), 0, std::max(0, ih - 1));
            if (iw <= 0 || ih <= 0 || fx < 0.f || fy < 0.f || fx >= f32(iw) || fy >= f32(ih)) { r[0] = r[1] = r[2] = r[3] = 0.f; break; }
            for (int k = 0; k < 4; ++k) r[k] = D((v * iw + u) * 4 + k);
            break;
        }
        case FeOp::Tile: {
            const int tx = int(P(4)), ty = int(P(5)), tw = int(P(6)), th = int(P(7));
            const int qx = tx + posMod(gx - tx, tw) - x0, qy = ty + posMod(gy - ty, th) - y0;
            if (qx >= 0 && qy >= 0 && qx < w && qy < h) std::memcpy(r, px(A, w, qx, qy), 16);
            else r[0] = r[1] = r[2] = r[3] = 0.f;
            break;
        }
        case FeOp::Transfer: {
            const int ch = std::clamp(int(P(0)), 0, 3), type = int(P(1)), n = int(P(5));
            const f32 al = a[3];
            f32 C = ch == 3 ? al * kInv255 : (al > 0.f ? std::clamp(a[ch] / al, 0.f, 1.f) : 0.f);
            f32 f = C;
            if (type == 1 && n > 0) {
                const f32 t = C * f32(n - 1);
                const int k = std::clamp(int(std::floor(t)), 0, n - 1);
                f = k >= n - 1 ? D(n - 1) : D(k) + (t - f32(k)) * (D(k + 1) - D(k));
            } else if (type == 2 && n > 0) {
                f = D(std::min(int(std::floor(C * f32(n))), n - 1));
            } else if (type == 3) {
                f = P(2) * C + P(3);
            } else if (type == 4) {
                f = P(2) * ppow(C, P(3)) + P(4);
            }
            f = std::clamp(f, 0.f, 1.f);
            std::memcpy(r, a, 16);
            if (ch == 3) {
                const f32 na = f * 255.f;
                for (int k = 0; k < 3; ++k) r[k] = al > 0.f ? std::clamp(a[k] / al, 0.f, 1.f) * na : 0.f;
                r[3] = na;
            } else r[ch] = f * al;
            break;
        }
        case FeOp::Convolve: {
            const int ox = int(P(0)), oy = int(P(1)), tx = int(P(4)), ty = int(P(5)), edge = int(P(6));
            const bool pa = P(7) != 0.f;
            f32 sum[4] = {0.f, 0.f, 0.f, 0.f};
            for (int j = 0; j < oy; ++j)
                for (int i = 0; i < ox; ++i) {
                    int qx = x - tx + i, qy = y - ty + j;
                    bool in = true;
                    if (edge == 0) qx = std::clamp(qx, 0, w - 1), qy = std::clamp(qy, 0, h - 1);
                    else if (edge == 1) qx = posMod(qx, w), qy = posMod(qy, h);
                    else in = qx >= 0 && qy >= 0 && qx < w && qy < h;
                    if (!in) continue;
                    const f32* s = px(A, w, qx, qy);
                    const f32 kv = D(ox * oy - 1 - (j * ox + i));
                    for (int k = 0; k < 4; ++k) {
                        f32 v = s[k];
                        if (pa && k < 3) v = s[3] > 0.f ? s[k] / s[3] * 255.f : 0.f;
                        sum[k] = sum[k] + kv * v;
                    }
                }
            for (int k = 0; k < 4; ++k) r[k] = std::clamp(sum[k] * P(2) + P(3) * 255.f, 0.f, 255.f);
            if (pa) {
                for (int k = 0; k < 3; ++k) r[k] = r[k] * (a[3] * kInv255);
                r[3] = a[3];
            } else
                for (int k = 0; k < 3; ++k) r[k] = std::min(r[k], r[3]);
            break;
        }
        case FeOp::Displace: {
            const int cx = std::clamp(int(P(1)), 0, 3), cy = std::clamp(int(P(2)), 0, 3);
            const auto chan = [&](int c) { return c == 3 ? b[3] * kInv255 : (b[3] > 0.f ? std::clamp(b[c] / b[3], 0.f, 1.f) : 0.f); };
            const f32 dx = P(0) * (chan(cx) - 0.5f), dy = P(0) * (chan(cy) - 0.5f);
            const int qx = int(std::floor(f32(x) + dx + 0.5f)), qy = int(std::floor(f32(y) + dy + 0.5f));
            if (qx >= 0 && qy >= 0 && qx < w && qy < h) std::memcpy(r, px(A, w, qx, qy), 16);
            else r[0] = r[1] = r[2] = r[3] = 0.f;
            break;
        }
        default: std::memcpy(r, a, 16); break;
        }
    }
};
} // namespace

void runFilterProgram(const GradientStop* prog, std::vector<f32>& plane0, int w, int h, int x0, int y0, bool hdr) {
    const int K = int(prog[0].offset), P = std::clamp(int(prog[0].color.r), 2, 32);
    const std::size_t n = std::size_t(w) * std::size_t(h) * 4;
    std::vector<std::vector<f32>> pl;
    pl.resize(std::size_t(P));
    pl[0] = std::move(plane0);
    for (int i = 1; i < P; ++i) pl[std::size_t(i)].assign(n, 0.f);
    std::vector<f32> R(n);
    for (int i = 0; i < K; ++i) {
        const GradientStop* op = prog + 2 + 7 * i;
        const FeOp kind = FeOp(int(op[0].offset));
        const int i1 = std::clamp(int(op[0].color.r), 0, P - 1), i2 = std::clamp(int(op[0].color.g), 0, P - 1);
        const int io = std::clamp(int(op[0].color.b), 0, P - 1);
        Exec ex{prog, op, w, h, x0, y0, int(op[0].color.a), hdr};
        const f32* A = pl[std::size_t(i1)].data();
        const f32* B = pl[std::size_t(i2)].data();
        f32* O = pl[std::size_t(io)].data();
        const f32 sx = op[1].color.r, sy = op[1].color.g, sx1 = sx + op[1].color.b, sy1 = sy + op[1].color.a;
        if (kind == FeOp::Blur) {
            if (O != A) std::memcpy(O, A, n * 4);
            f32* T = pl[1].data();
            for (int it = 0; it < 3; ++it) {
                boxPassH(O, T, w, h, int(ex.P(2)), ex.P(4));
                boxPassV(T, O, w, h, int(ex.P(3)), ex.P(5));
            }
            std::memcpy(R.data(), O, n * 4);
        } else {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) ex.pixel(kind, A, B, x, y, R.data() + (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4);
        }
        for (int y = 0; y < h; ++y) {
            const f32 py = f32(y0 + y) + 0.5f;
            for (int x = 0; x < w; ++x) {
                const f32 pxc = f32(x0 + x) + 0.5f;
                const bool inside = pxc >= sx && pxc < sx1 && py >= sy && py < sy1;
                const std::size_t q = (std::size_t(y) * std::size_t(w) + std::size_t(x)) * 4;
                for (int k = 0; k < 4; ++k) O[q + std::size_t(k)] = inside ? R[q + std::size_t(k)] : 0.f;
            }
        }
    }
    plane0 = std::move(pl[0]);
}

std::vector<std::string> svgFilterIds(std::string_view text) {
    std::vector<std::string> ids;
    std::size_t i = 0;
    Tag t;
    while (nextTag(text, i, t))
        if (!t.close && t.name == "filter" && t.has("id")) ids.emplace_back(trimSv(t.attr("id")));
    return ids;
}

namespace {
/// feFuncX attributes -> one Transfer op (false: identity / nothing to do)
bool transferFunc(const Tag& t, int ch, FilterOpDesc& d, std::vector<f32>& nums) {
    const std::string_view type = trimSv(t.attr("type"));
    d.p[0] = f32(ch);
    if (type == "table" || type == "discrete") {
        if (!numbers(t.attr("tableValues"), nums) || nums.empty()) return false;
        d.p[1] = type == "table" ? 1.f : 2.f;
        d.p[5] = f32(nums.size());
        d.data = nums;
    } else if (type == "linear") {
        d.p[1] = 3.f, d.p[2] = number(t.attr("slope"), 1.f), d.p[3] = number(t.attr("intercept"), 0.f);
    } else if (type == "gamma") {
        d.p[1] = 4.f, d.p[2] = number(t.attr("amplitude"), 1.f), d.p[3] = number(t.attr("exponent"), 1.f);
        d.p[4] = number(t.attr("offset"), 0.f);
    } else return false;
    return true;
}
int channelIndex(std::string_view s) {
    s = trimSv(s);
    return s == "R" ? 0 : s == "G" ? 1 : s == "B" ? 2 : 3;
}
} // namespace

bool parseSvgFilter(std::string_view text, FilterProgram& out, std::string_view id, const FilterImageResolver& images) {
    out = {};
    std::size_t i = 0;
    Tag t;
    bool found = false;
    while (nextTag(text, i, t))
        if (!t.close && t.name == "filter" && (id.empty() || trimSv(t.attr("id")) == id)) { found = true; break; }
    if (!found || t.selfClose) return false;
    out.filterUnitsUser = trimSv(t.attr("filterUnits")) == "userSpaceOnUse";
    out.primitiveUnitsBox = trimSv(t.attr("primitiveUnits")) == "objectBoundingBox";
    {
        static const char* const k[4] = {"x", "y", "width", "height"};
        for (int j = 0; j < 4; ++j) {
            f32 v;
            bool pct;
            if (!length(t.attr(k[j]), v, pct)) continue;
            out.region[std::size_t(j)] = v;
            out.regionSet |= u8(1u << j);
            if (pct) out.regionPct |= u8(1u << j);
            else out.regionPct &= u8(~(1u << j));
        }
    }
    Compiler c{out, {}, 0, -1, true};
    std::vector<f32> nums;
    while (c.ok && nextTag(text, i, t)) {
        if (t.close) {
            if (t.name == "filter") break;
            continue;
        }
        const std::string& n = t.name;
        const std::size_t first = out.ops.size();
        const Tag prim = t;
        if (n == "feGaussianBlur") {
            const u8 in = c.input(t.attr("in"));
            if (!numbers(t.attr("stdDeviation"), nums)) return false;
            const f32 sx = nums.empty() ? 0.f : nums[0], sy = nums.size() > 1 ? nums[1] : sx;
            const u8 o = c.fresh();
            FilterOpDesc& d = c.emit(FeOp::Blur, in, 0, o);
            d.p[0] = std::max(0.f, sx), d.p[1] = std::max(0.f, sy);
            c.result(t, o);
        } else if (n == "feColorMatrix") {
            const u8 in = c.input(t.attr("in"));
            const std::string_view type = t.has("type") ? trimSv(t.attr("type")) : std::string_view("matrix");
            if (!numbers(t.attr("values"), nums)) return false;
            ColorMatrix m;
            if (type == "matrix") {
                if (nums.size() == 20) std::copy(nums.begin(), nums.end(), m.m.begin());
                else if (!nums.empty()) return false;
            } else if (type == "saturate") {
                m = ColorMatrix::saturate(nums.empty() ? 1.f : nums[0]);
            } else if (type == "hueRotate") {
                m = ColorMatrix::hueRotate(nums.empty() ? 0.f : nums[0]);
            } else if (type == "luminanceToAlpha") {
                m.m = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.2125f, 0.7154f, 0.0721f, 0, 0};
            } else return false;
            const u8 o = c.fresh();
            FilterOpDesc& d = c.emit(FeOp::Matrix, in, 0, o);
            std::copy(m.m.begin(), m.m.end(), d.p.begin());
            c.result(t, o);
        } else if (n == "feOffset") {
            const u8 in = c.input(t.attr("in"));
            const u8 o = c.fresh();
            FilterOpDesc& d = c.emit(FeOp::Offset, in, 0, o);
            d.p[0] = number(t.attr("dx"), 0.f), d.p[1] = number(t.attr("dy"), 0.f);
            c.result(t, o);
        } else if (n == "feFlood") {
            const Color col = c.color(t);
            const u8 o = c.fresh();
            FilterOpDesc& d = c.emit(FeOp::Flood, 0, 0, o);
            d.p[0] = col.r, d.p[1] = col.g, d.p[2] = col.b, d.p[3] = col.a;
            c.result(t, o);
        } else if (n == "feBlend") {
            const u8 a = c.input(t.attr("in")), b = c.input(t.attr("in2"));
            const std::string_view m = trimSv(t.attr("mode"));
            f32 mode = 0.f;
            if (m.empty() || m == "normal") mode = 0.f;
            else if (m == "multiply") mode = 1.f;
            else if (m == "screen") mode = 2.f;
            else if (m == "darken") mode = 3.f;
            else if (m == "lighten") mode = 4.f;
            else return false;
            const u8 o = c.fresh();
            c.emit(FeOp::Blend, a, b, o).p[0] = mode;
            c.result(t, o);
        } else if (n == "feComposite") {
            const u8 a = c.input(t.attr("in")), b = c.input(t.attr("in2"));
            const std::string_view op = trimSv(t.attr("operator"));
            f32 k = 0.f;
            if (op.empty() || op == "over") k = 0.f;
            else if (op == "in") k = 1.f;
            else if (op == "out") k = 2.f;
            else if (op == "atop") k = 3.f;
            else if (op == "xor") k = 4.f;
            else if (op == "arithmetic") k = 5.f;
            else return false;
            const u8 o = c.fresh();
            FilterOpDesc& d = c.emit(FeOp::Composite, a, b, o);
            d.p[0] = k;
            d.p[1] = number(t.attr("k1"), 0.f), d.p[2] = number(t.attr("k2"), 0.f);
            d.p[3] = number(t.attr("k3"), 0.f), d.p[4] = number(t.attr("k4"), 0.f);
            c.result(t, o);
        } else if (n == "feMerge") {
            std::vector<u8> nodes;
            if (!t.selfClose) {
                while (nextTag(text, i, t)) {
                    if (t.close && t.name == "feMerge") break;
                    if (!t.close && t.name == "feMergeNode") nodes.push_back(c.input(t.attr("in")));
                }
            }
            const u8 o = c.fresh();
            if (nodes.empty()) c.emit(FeOp::Flood, 0, 0, o); // transparent
            else {
                c.emit(FeOp::Copy, nodes[0], 0, o);
                for (std::size_t k = 1; k < nodes.size(); ++k) c.emit(FeOp::Composite, nodes[k], o, o); // node over result
            }
            c.result(prim, o);
        } else if (n == "feDropShadow") {
            const u8 in = c.input(t.attr("in"));
            if (!numbers(t.has("stdDeviation") ? t.attr("stdDeviation") : std::string_view("2"), nums)) return false;
            const f32 sx = nums.empty() ? 0.f : nums[0], sy = nums.size() > 1 ? nums[1] : sx;
            const Color col = c.color(t);
            const u8 al = c.fresh(), bl = c.fresh(), of = c.fresh(), fl = c.fresh(), sh = c.fresh(), o = c.fresh();
            c.emit(FeOp::Alpha, in, 0, al);
            FilterOpDesc& b = c.emit(FeOp::Blur, al, 0, bl);
            b.p[0] = std::max(0.f, sx), b.p[1] = std::max(0.f, sy);
            FilterOpDesc& d = c.emit(FeOp::Offset, bl, 0, of);
            d.p[0] = number(t.has("dx") ? t.attr("dx") : std::string_view("2"), 2.f);
            d.p[1] = number(t.has("dy") ? t.attr("dy") : std::string_view("2"), 2.f);
            FilterOpDesc& f = c.emit(FeOp::Flood, 0, 0, fl);
            f.p[0] = col.r, f.p[1] = col.g, f.p[2] = col.b, f.p[3] = col.a;
            c.emit(FeOp::Composite, fl, of, sh).p[0] = 1.f; // flood in shadow alpha
            c.emit(FeOp::Composite, in, sh, o);              // source over shadow
            c.result(t, o);
        } else if (n == "feTurbulence") {
            if (!numbers(t.attr("baseFrequency"), nums)) return false;
            const f32 fx = nums.empty() ? 0.f : nums[0], fy = nums.size() > 1 ? nums[1] : fx;
            if (fx < 0.f || fy < 0.f) return false;
            const u8 o = c.fresh();
            FilterOpDesc& d = c.emit(FeOp::Turbulence, 0, 0, o);
            d.p[0] = fx, d.p[1] = fy;
            d.p[2] = std::clamp(number(t.attr("numOctaves"), 1.f), 0.f, 10.f);
            d.p[3] = trimSv(t.attr("type")) == "fractalNoise" ? 1.f : 0.f;
            d.p[4] = trimSv(t.attr("stitchTiles")) == "stitch" ? 1.f : 0.f;
            turbInit(long(std::lround(number(t.attr("seed"), 0.f))), d.data);
            c.result(t, o);
        } else if (n == "feDiffuseLighting" || n == "feSpecularLighting") {
            const bool spec = n == "feSpecularLighting";
            const u8 in = c.input(t.attr("in"));
            const Color lc = c.color(t, "lighting-color", "lighting-opacity", Color{1.f, 1.f, 1.f, 1.f});
            FilterOpDesc d;
            d.op = FeOp::Lighting;
            d.in1 = in;
            d.p[0] = spec ? 1.f : 0.f;
            d.p[1] = number(t.attr("surfaceScale"), 1.f);
            d.p[2] = std::max(0.f, number(t.attr(spec ? "specularConstant" : "diffuseConstant"), 1.f));
            d.p[3] = spec ? std::clamp(number(t.attr("specularExponent"), 1.f), 1.f, 128.f) : 1.f;
            d.p[4] = lc.r, d.p[5] = lc.g, d.p[6] = lc.b;
            bool light = false;
            if (!t.selfClose) {
                while (nextTag(text, i, t)) {
                    if (t.close && t.name == n) break;
                    if (t.close || light) continue;
                    if (t.name == "feDistantLight") {
                        const f32 az = number(t.attr("azimuth"), 0.f) * 0.017453292519943295f;
                        const f32 el = number(t.attr("elevation"), 0.f) * 0.017453292519943295f;
                        d.p[7] = 0.f;
                        d.p[8] = std::cos(az) * std::cos(el), d.p[9] = std::sin(az) * std::cos(el), d.p[10] = std::sin(el);
                        light = true;
                    } else if (t.name == "fePointLight" || t.name == "feSpotLight") {
                        const bool spot = t.name == "feSpotLight";
                        d.p[7] = spot ? 2.f : 1.f;
                        d.p[8] = number(t.attr("x"), 0.f), d.p[9] = number(t.attr("y"), 0.f), d.p[10] = number(t.attr("z"), 0.f);
                        if (spot) {
                            d.p[11] = number(t.attr("pointsAtX"), 0.f), d.p[12] = number(t.attr("pointsAtY"), 0.f);
                            d.p[13] = number(t.attr("pointsAtZ"), 0.f);
                            d.p[14] = std::max(0.f, number(t.attr("specularExponent"), 1.f));
                            d.p[15] = t.has("limitingConeAngle")
                                          ? std::cos(std::min(90.f, std::abs(number(t.attr("limitingConeAngle"), 90.f))) * 0.017453292519943295f)
                                          : -2.f;
                        }
                        light = true;
                    }
                }
            }
            if (!light) return false;
            d.out = c.fresh();
            out.ops.push_back(d);
            c.result(prim, d.out);
        } else if (n == "feMorphology") {
            const u8 in = c.input(t.attr("in"));
            if (!numbers(t.has("radius") ? t.attr("radius") : std::string_view("0"), nums)) return false;
            const f32 rx = nums.empty() ? 0.f : nums[0], ry = nums.size() > 1 ? nums[1] : rx;
            const u8 o = c.fresh();
            FilterOpDesc& d = c.emit(FeOp::Morphology, in, 0, o);
            d.p[0] = trimSv(t.attr("operator")) == "dilate" ? 1.f : 0.f;
            d.p[1] = rx < 0.f || ry < 0.f ? 0.f : rx, d.p[2] = rx < 0.f || ry < 0.f ? 0.f : ry;
            c.result(t, o);
        } else if (n == "feImage") {
            std::string_view href = trimSv(t.attr("href"));
            if (!href.empty() && href.front() == '#') href.remove_prefix(1);
            const Image* img = images ? images(href) : nullptr;
            const u8 o = c.fresh();
            FilterOpDesc& d = c.emit(FeOp::Image, 0, 0, o);
            if (img && img->width > 0 && img->height > 0 && img->pixels.size() == std::size_t(img->width) * std::size_t(img->height)) {
                // at most 128 x 128 texels ride in the stop table (nearest-downsampled)
                const int iw = std::min(img->width, 128), ih = std::min(img->height, 128);
                d.p[4] = f32(iw), d.p[5] = f32(ih);
                d.data.reserve(std::size_t(iw * ih * 4));
                for (int y = 0; y < ih; ++y)
                    for (int x = 0; x < iw; ++x) {
                        const u32 p = img->at(x * img->width / iw, y * img->height / ih);
                        for (int k = 0; k < 4; ++k) d.data.push_back(f32((p >> (8 * k)) & 0xFFu));
                    }
            } else if (!href.empty() && images) return false;
            c.result(t, o);
        } else if (n == "feTile") {
            const u8 in = c.input(t.attr("in"));
            const u8 o = c.fresh();
            c.emit(FeOp::Tile, in, 0, o);
            c.result(t, o);
        } else if (n == "feComponentTransfer") {
            const u8 in = c.input(t.attr("in"));
            std::vector<FilterOpDesc> funcs;
            if (!t.selfClose) {
                while (nextTag(text, i, t)) {
                    if (t.close && t.name == n) break;
                    if (t.close) continue;
                    const int ch = t.name == "feFuncR" ? 0 : t.name == "feFuncG" ? 1 : t.name == "feFuncB" ? 2 : t.name == "feFuncA" ? 3 : -1;
                    if (ch < 0) continue;
                    FilterOpDesc d;
                    d.op = FeOp::Transfer;
                    if (transferFunc(t, ch, d, nums)) funcs.push_back(std::move(d));
                }
            }
            const u8 o = c.fresh();
            if (funcs.empty()) c.emit(FeOp::Copy, in, 0, o);
            for (std::size_t k = 0; k < funcs.size(); ++k) {
                funcs[k].in1 = k == 0 ? in : o;
                funcs[k].out = o;
                out.ops.push_back(std::move(funcs[k]));
            }
            c.result(prim, o);
        } else if (n == "feConvolveMatrix") {
            const u8 in = c.input(t.attr("in"));
            if (!numbers(t.has("order") ? t.attr("order") : std::string_view("3"), nums) || nums.empty()) return false;
            const int ox = int(nums[0]), oy = nums.size() > 1 ? int(nums[1]) : ox;
            if (ox < 1 || oy < 1 || ox > 9 || oy > 9) return false;
            std::vector<f32> kern;
            if (!numbers(t.attr("kernelMatrix"), kern) || kern.size() != std::size_t(ox * oy)) return false;
            f32 div = 0.f;
            for (f32 v : kern) div += v;
            if (t.has("divisor")) div = number(t.attr("divisor"), 0.f);
            if (div == 0.f) div = 1.f;
            const int tx = t.has("targetX") ? int(number(t.attr("targetX"), 0.f)) : ox / 2;
            const int ty = t.has("targetY") ? int(number(t.attr("targetY"), 0.f)) : oy / 2;
            if (tx < 0 || tx >= ox || ty < 0 || ty >= oy) return false;
            const std::string_view em = trimSv(t.attr("edgeMode"));
            const u8 o = c.fresh();
            FilterOpDesc& d = c.emit(FeOp::Convolve, in, 0, o);
            d.p[0] = f32(ox), d.p[1] = f32(oy), d.p[2] = 1.f / div, d.p[3] = number(t.attr("bias"), 0.f);
            d.p[4] = f32(tx), d.p[5] = f32(ty), d.p[6] = em == "wrap" ? 1.f : em == "none" ? 2.f : 0.f;
            d.p[7] = trimSv(t.attr("preserveAlpha")) == "true" ? 1.f : 0.f;
            d.data = std::move(kern);
            c.result(t, o);
        } else if (n == "feDisplacementMap") {
            const u8 a = c.input(t.attr("in")), b = c.input(t.attr("in2"));
            const u8 o = c.fresh();
            FilterOpDesc& d = c.emit(FeOp::Displace, a, b, o);
            d.p[0] = number(t.attr("scale"), 0.f);
            d.p[1] = f32(channelIndex(t.has("xChannelSelector") ? t.attr("xChannelSelector") : std::string_view("A")));
            d.p[2] = f32(channelIndex(t.has("yChannelSelector") ? t.attr("yChannelSelector") : std::string_view("A")));
            c.result(t, o);
        } else if (n.size() > 2 && n.compare(0, 2, "fe") == 0) {
            return false; // unknown primitive
        } else continue;
        c.subregion(prim, first);
    }
    if (!c.ok || out.ops.empty()) return false;
    c.emit(FeOp::Copy, c.last, 0, 0); // the result to plane 0
    return out.ops.size() <= 32 && out.planes <= 32; // GPU: one blur-mask bit per op
}
} // namespace fuse::ui
