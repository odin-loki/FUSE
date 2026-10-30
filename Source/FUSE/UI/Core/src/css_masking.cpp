#include "fuse/ui/css_masking.hpp"

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>

namespace fuse::ui {

namespace {
std::string_view trimv(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}
/// Top-level split (parentheses and quotes nest); empty items dropped for ' '.
std::vector<std::string_view> split(std::string_view t, char sep) {
    std::vector<std::string_view> out;
    int d = 0;
    char q = 0;
    std::size_t s = 0;
    for (std::size_t i = 0; i <= t.size(); ++i) {
        const char c = i < t.size() ? t[i] : sep;
        if (q) { if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == '(') ++d;
        else if (c == ')') --d;
        const bool cut = i == t.size() || (d == 0 && (sep == ' ' ? std::isspace(static_cast<unsigned char>(c)) != 0 : c == sep));
        if (cut) {
            const std::string_view item = trimv(t.substr(s, i - s));
            if (!item.empty() || sep != ' ') out.push_back(item);
            s = i + 1;
        }
    }
    return out;
}
bool len(std::string_view s, Length& l) { return parseCssLength(s, l) && (l.unit == Unit::Px || l.unit == Unit::Percent); }
f32 res(const Length& l, f32 ref) { return l.unit == Unit::Percent ? l.value * 0.01f * ref : l.value; }
bool fnCall(std::string_view t, std::string_view& fn, std::string_view& args) {
    const auto open = t.find('(');
    if (open == std::string_view::npos || t.empty() || t.back() != ')') return false;
    fn = trimv(t.substr(0, open));
    args = trimv(t.substr(open + 1, t.size() - open - 2));
    return true;
}
std::string unquote(std::string_view s) {
    s = trimv(s);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) s = s.substr(1, s.size() - 2);
    return std::string(s);
}
bool boxWord(std::string_view w, CssBox& b, bool allowNoClip) {
    if (w == "border-box" || w == "fill-box" || w == "stroke-box" || w == "view-box") b = CssBox::Border;
    else if (w == "padding-box") b = CssBox::Padding;
    else if (w == "content-box") b = CssBox::Content;
    else if (w == "margin-box") b = CssBox::Margin;
    else if (allowNoClip && w == "no-clip") b = CssBox::NoClip;
    else return false;
    return true;
}
/// "at x y" position (keywords or lengths) from words[from..]; default centre.
bool position(const std::vector<std::string_view>& w, std::size_t from, Length& x, Length& y) {
    x = y = Length::pct(50.f);
    if (from >= w.size()) return true;
    if (w[from] != "at" || w.size() - from > 3 || w.size() - from < 2) return false;
    auto kw = [](std::string_view s, Length& l, bool horiz) {
        if (s == "center") l = Length::pct(50.f);
        else if (s == (horiz ? "left" : "top")) l = Length::pct(0.f);
        else if (s == (horiz ? "right" : "bottom")) l = Length::pct(100.f);
        else return len(s, l);
        return true;
    };
    if (w.size() - from == 2) return kw(w[from + 1], x, true) || (kw(w[from + 1], y, false) && (x = Length::pct(50.f), true));
    if (w[from + 1] == "top" || w[from + 1] == "bottom") return kw(w[from + 1], y, false) && kw(w[from + 2], x, true);
    return kw(w[from + 1], x, true) && kw(w[from + 2], y, false);
}
bool shape(std::string_view fn, std::string_view args, ClipPathSpec& o) {
    using K = ClipPathSpec::Kind;
    const std::vector<std::string_view> w = split(args, ' ');
    auto radius = [&](std::string_view s, Length& l) {
        if (s == "closest-side") { l = Length{}; return true; }
        if (s == "farthest-side") { l = Length{-1.f, Unit::Auto}; return true; }
        return len(s, l) && l.value >= 0.f;
    };
    if (fn == "inset") {
        o.kind = K::Inset;
        std::size_t n = 0;
        while (n < w.size() && w[n] != "round") ++n;
        if (n == 0 || n > 4) return false;
        Length v[4];
        for (std::size_t k = 0; k < n; ++k) if (!len(w[k], v[k])) return false;
        const Length t = v[0], r = n > 1 ? v[1] : t, b = n > 2 ? v[2] : t, l = n > 3 ? v[3] : r;
        o.args = {t, r, b, l, Length::px(0.f)};
        if (n < w.size() && (w.size() != n + 2 || !len(w[n + 1], o.args[4]))) return false;
        return true;
    }
    if (fn == "circle" || fn == "ellipse") {
        const bool c = fn == "circle";
        o.kind = c ? K::Circle : K::Ellipse;
        o.args.assign(c ? 3 : 4, Length{});
        std::size_t k = 0;
        if (!w.empty() && w[0] != "at") {
            if (c) { if (!radius(w[0], o.args[0])) return false; k = 1; }
            else { if (w.size() < 2 || !radius(w[0], o.args[0]) || !radius(w[1], o.args[1])) return false; k = 2; }
        }
        return position(w, k, o.args[c ? 1 : 2], o.args[c ? 2 : 3]);
    }
    if (fn == "polygon") {
        o.kind = K::Polygon;
        std::vector<std::string_view> pts = split(args, ',');
        if (!pts.empty() && (pts[0] == "evenodd" || pts[0] == "nonzero")) {
            o.evenOdd = pts[0] == "evenodd";
            pts.erase(pts.begin());
        }
        if (pts.size() < 3) return false;
        for (auto p : pts) {
            const auto xy = split(p, ' ');
            Length x, y;
            if (xy.size() != 2 || !len(xy[0], x) || !len(xy[1], y)) return false;
            o.args.push_back(x), o.args.push_back(y);
        }
        return true;
    }
    if (fn == "path") {
        o.kind = K::Path;
        std::vector<std::string_view> parts = split(args, ',');
        if (parts.size() == 2) {
            if (parts[0] != "evenodd" && parts[0] != "nonzero") return false;
            o.evenOdd = parts[0] == "evenodd";
            parts.erase(parts.begin());
        }
        if (parts.size() != 1) return false;
        o.path = unquote(parts[0]);
        return !o.path.empty() && !Path::fromSvg(o.path).edges().empty();
    }
    return false;
}
} // namespace

Rect cssBox(const ComputedStyle& s, const Rect& b, CssBox box) {
    switch (box) {
    case CssBox::Padding: {
        const f32 w = s.borderWidth;
        return {b.x + w, b.y + w, std::max(0.f, b.w - 2.f * w), std::max(0.f, b.h - 2.f * w)};
    }
    case CssBox::Content:
        return {b.x + s.padding.l, b.y + s.padding.t, std::max(0.f, b.w - s.padding.h()), std::max(0.f, b.h - s.padding.v())};
    case CssBox::Margin:
        return {b.x - s.margin.l, b.y - s.margin.t, b.w + s.margin.h(), b.h + s.margin.v()};
    default: return b;
    }
}

bool parseClipPathSpec(std::string_view text, ClipPathSpec& o) {
    o = {};
    text = trimv(text);
    if (text == "none") return true;
    if (text.starts_with("url(")) {
        std::string_view fn, args;
        if (!fnCall(text, fn, args)) return false;
        std::string u = unquote(args);
        if (u.starts_with('#')) u.erase(0, 1);
        if (u.empty()) return false;
        o.kind = ClipPathSpec::Kind::Url;
        o.url = std::move(u);
        return true;
    }
    bool haveBox = false, haveShape = false;
    for (std::string_view w : split(text, ' ')) {
        CssBox b;
        if (boxWord(w, b, false)) {
            if (haveBox) return false;
            o.box = b, haveBox = true;
            continue;
        }
        std::string_view fn, args;
        if (haveShape || !fnCall(w, fn, args) || !shape(fn, args, o)) return false;
        haveShape = true;
    }
    if (!haveShape) {
        if (!haveBox) return false;
        o.kind = ClipPathSpec::Kind::Box;
    }
    return true;
}

const ClipPathSpec& clipPathSpec(u32 id) {
    static std::mutex mu;
    static std::map<u32, ClipPathSpec> cache;
    static const ClipPathSpec none;
    if (!id) return none;
    std::lock_guard lock(mu);
    auto it = cache.find(id);
    if (it == cache.end()) {
        ClipPathSpec s;
        if (!parseClipPathSpec(animationName(id), s)) s = {};
        it = cache.emplace(id, std::move(s)).first;
    }
    return it->second;
}

namespace {
struct Geom {
    Rect r;
    f32 radius = 0.f;
    Vec2 c, rad;
    std::vector<Vec2> pts;
};
Geom geom(const ClipPathSpec& s, const Rect& b) {
    using K = ClipPathSpec::Kind;
    Geom g;
    const auto& a = s.args;
    if (s.kind == K::Inset) {
        const f32 t = res(a[0], b.h), r = res(a[1], b.w), bo = res(a[2], b.h), l = res(a[3], b.w);
        g.r = {b.x + l, b.y + t, std::max(0.f, b.w - l - r), std::max(0.f, b.h - t - bo)};
        g.radius = std::min(res(a[4], b.w), std::min(g.r.w, g.r.h) * 0.5f);
    } else if (s.kind == K::Circle || s.kind == K::Ellipse) {
        const bool circle = s.kind == K::Circle;
        const int pi = circle ? 1 : 2;
        g.c = {b.x + res(a[pi], b.w), b.y + res(a[pi + 1], b.h)};
        const f32 dl = g.c.x - b.x, dr = b.x + b.w - g.c.x, dt = g.c.y - b.y, db = b.y + b.h - g.c.y;
        auto rr = [&](const Length& l, bool horiz, f32 ref) {
            if (l.unit != Unit::Auto) return res(l, ref);
            const bool far = l.value < 0.f;
            auto pick = [far](f32 x, f32 y) { return far ? std::max(x, y) : std::min(x, y); };
            if (circle) return pick(pick(dl, dr), pick(dt, db));
            return horiz ? pick(dl, dr) : pick(dt, db);
        };
        if (circle) {
            const f32 r = rr(a[0], true, std::sqrt(b.w * b.w + b.h * b.h) / std::sqrt(2.f));
            g.rad = {r, r};
        } else g.rad = {rr(a[0], true, b.w), rr(a[1], false, b.h)};
    } else if (s.kind == K::Polygon) {
        for (std::size_t k = 0; k + 1 < a.size(); k += 2) g.pts.push_back({b.x + res(a[k], b.w), b.y + res(a[k + 1], b.h)});
    }
    return g;
}
} // namespace

Path clipPathShape(const ClipPathSpec& s, const Rect& b) {
    using K = ClipPathSpec::Kind;
    Path p;
    const Geom g = geom(s, b);
    switch (s.kind) {
    case K::Inset:
        if (g.radius > 0.f) {
            const f32 r = g.radius, x0 = g.r.x, y0 = g.r.y, x1 = g.r.x + g.r.w, y1 = g.r.y + g.r.h;
            p.moveTo({x0 + r, y0}).lineTo({x1 - r, y0}).arcTo({r, r}, 0.f, false, true, {x1, y0 + r}).lineTo({x1, y1 - r})
                .arcTo({r, r}, 0.f, false, true, {x1 - r, y1}).lineTo({x0 + r, y1}).arcTo({r, r}, 0.f, false, true, {x0, y1 - r})
                .lineTo({x0, y0 + r}).arcTo({r, r}, 0.f, false, true, {x0 + r, y0}).close();
        } else p.rect(g.r);
        break;
    case K::Circle: case K::Ellipse: p.ellipse(g.c, g.rad.x, g.rad.y); break;
    case K::Polygon:
        p.moveTo(g.pts[0]);
        for (std::size_t k = 1; k < g.pts.size(); ++k) p.lineTo(g.pts[k]);
        p.close();
        break;
    case K::Path: p = Path::fromSvg(s.path); p.transform(Vec2{1.f, 1.f}, Vec2{b.x, b.y}); break;
    case K::Box: p.rect(b); break;
    default: break;
    }
    return p;
}

bool clipPathContains(const ClipPathSpec& s, const Rect& b, Vec2 q) {
    using K = ClipPathSpec::Kind;
    const Geom g = geom(s, b);
    switch (s.kind) {
    case K::Inset: {
        if (!g.r.contains(q)) return false;
        const f32 r = g.radius;
        const f32 cx = std::clamp(q.x, g.r.x + r, g.r.x + g.r.w - r), cy = std::clamp(q.y, g.r.y + r, g.r.y + g.r.h - r);
        return (q.x - cx) * (q.x - cx) + (q.y - cy) * (q.y - cy) <= r * r + 1e-3f;
    }
    case K::Circle: case K::Ellipse: {
        if (g.rad.x <= 0.f || g.rad.y <= 0.f) return false;
        const f32 dx = (q.x - g.c.x) / g.rad.x, dy = (q.y - g.c.y) / g.rad.y;
        return dx * dx + dy * dy <= 1.f;
    }
    case K::Polygon: {
        int wind = 0, cross = 0;
        for (std::size_t k = 0, j = g.pts.size() - 1; k < g.pts.size(); j = k++) {
            const Vec2 a = g.pts[j], c = g.pts[k];
            if ((a.y <= q.y) != (c.y <= q.y)) {
                const f32 x = a.x + (q.y - a.y) / (c.y - a.y) * (c.x - a.x);
                if (q.x < x) { ++cross; wind += c.y > a.y ? 1 : -1; }
            }
        }
        return s.evenOdd ? (cross & 1) != 0 : wind != 0;
    }
    case K::Path: {
        const Path p = clipPathShape(s, b);
        return pathFillCoverage(p.edges().data(), p.edges().size(), q.x, q.y, s.evenOdd) >= 0.5f;
    }
    case K::Box: return b.contains(q);
    default: return true;
    }
}

// ---- mask-* -------------------------------------------------------------------------------------------------------
namespace {
bool parseImage(std::string_view t, MaskLayer& L) {
    using K = MaskLayer::Kind;
    t = trimv(t);
    if (t == "none") { L.kind = K::None; return true; }
    std::string_view fn, args;
    if (!fnCall(t, fn, args)) return false;
    if (fn == "url") {
        L.kind = K::Url;
        L.url = unquote(args);
        if (L.url.starts_with('#')) L.url.erase(0, 1);
        return !L.url.empty();
    }
    L.repeating = fn.starts_with("repeating-");
    if (L.repeating) fn.remove_prefix(10);
    if (fn != "linear-gradient" && fn != "radial-gradient") return false;
    std::vector<std::string_view> parts = split(args, ',');
    if (parts.empty()) return false;
    std::size_t first = 0;
    if (fn == "linear-gradient") {
        L.kind = K::Linear;
        const std::string_view p0 = parts[0];
        if (p0.starts_with("to ")) {
            const bool tp = p0.find("top") != std::string_view::npos, b = p0.find("bottom") != std::string_view::npos;
            const bool l = p0.find("left") != std::string_view::npos, r = p0.find("right") != std::string_view::npos;
            if (tp && r) L.angle = 45.f; else if (b && r) L.angle = 135.f; else if (b && l) L.angle = 225.f; else if (tp && l) L.angle = 315.f;
            else if (tp) L.angle = 0.f; else if (r) L.angle = 90.f; else if (b) L.angle = 180.f; else if (l) L.angle = 270.f;
            else return false;
            first = 1;
        } else {
            char* end = nullptr;
            const std::string tmp(p0);
            const f32 v = std::strtof(tmp.c_str(), &end);
            const std::string_view u = trimv(std::string_view(end));
            if (end != tmp.c_str() && (u == "deg" || u == "rad" || u == "turn" || u == "grad")) {
                L.angle = u == "deg" ? v : u == "rad" ? v * 57.29577951f : u == "turn" ? v * 360.f : v * 0.9f;
                first = 1;
            }
        }
    } else {
        L.kind = K::Radial;
        // [<shape> || <size>] [at <position>]
        const std::vector<std::string_view> w = split(parts[0], ' ');
        Color probe;
        if (!w.empty() && !parseColor(w[0], probe)) {
            std::size_t k = 0;
            int lens = 0;
            for (; k < w.size() && w[k] != "at"; ++k) {
                const std::string_view x = w[k];
                Length l;
                if (x == "circle") L.circle = true;
                else if (x == "ellipse") L.circle = false;
                else if (x == "closest-side") L.extent = 0;
                else if (x == "closest-corner") L.extent = 1;
                else if (x == "farthest-side") L.extent = 2;
                else if (x == "farthest-corner") L.extent = 3;
                else if (len(x, l) && lens < 2) (lens++ ? L.ry : L.rx) = l;
                else return false;
            }
            if (lens == 1) { L.circle = true; L.ry = L.rx; }
            if (!position(w, k, L.cx, L.cy)) return false;
            first = 1;
        }
    }
    for (std::size_t k = first; k < parts.size(); ++k) {
        const std::vector<std::string_view> w = split(parts[k], ' ');
        if (w.empty() || w.size() > 3) return false;
        Color c;
        if (!parseColor(w[0], c)) return false;
        Length p{};
        if (w.size() >= 2 && !len(w[1], p)) return false;
        L.stopColors.push_back(c), L.stopPos.push_back(p);
        if (w.size() == 3) { // two positions: a hard stop pair
            Length p2;
            if (!len(w[2], p2)) return false;
            L.stopColors.push_back(c), L.stopPos.push_back(p2);
        }
    }
    return L.stopColors.size() >= 2;
}
bool parseMode(std::string_view t, u8& m) {
    if (t == "match-source") m = 0; else if (t == "alpha") m = 1; else if (t == "luminance") m = 2; else return false;
    return true;
}
bool parseComposite(std::string_view t, u8& m) {
    if (t == "add") m = 0; else if (t == "subtract") m = 1; else if (t == "intersect") m = 2; else if (t == "exclude") m = 3; else return false;
    return true;
}
bool parseSize(std::string_view t, MaskLayer& L) {
    if (t == "cover") { L.sizeKind = 1; return true; }
    if (t == "contain") { L.sizeKind = 2; return true; }
    const auto w = split(t, ' ');
    if (w.empty() || w.size() > 2) return false;
    L.sizeKind = 0;
    auto one = [](std::string_view s, Length& l) { if (s == "auto") { l = {}; return true; } return len(s, l) && l.value >= 0.f; };
    if (!one(w[0], L.sw)) return false;
    L.sh = Length{};
    return w.size() == 1 || one(w[1], L.sh);
}
bool parsePos(std::string_view t, MaskLayer& L) {
    const auto w = split(t, ' ');
    if (w.empty() || w.size() > 2) return false;
    std::vector<std::string_view> at{"at"};
    at.insert(at.end(), w.begin(), w.end());
    return position(at, 0, L.px, L.py);
}
bool parseRepeat(std::string_view t, MaskLayer& L) {
    auto one = [](std::string_view s, u8& r) {
        if (s == "repeat") r = 0; else if (s == "no-repeat") r = 1; else if (s == "space") r = 2; else if (s == "round") r = 3; else return false;
        return true;
    };
    if (t == "repeat-x") { L.repX = 0, L.repY = 1; return true; }
    if (t == "repeat-y") { L.repX = 1, L.repY = 0; return true; }
    const auto w = split(t, ' ');
    if (w.empty() || w.size() > 2 || !one(w[0], L.repX)) return false;
    L.repY = L.repX;
    return w.size() == 1 || one(w[1], L.repY);
}
/// Applies longhand item text to layer L (false: invalid).
bool applyItem(Prop p, std::string_view t, MaskLayer& L) {
    switch (p) {
    case Prop::MaskImage: return parseImage(t, L);
    case Prop::MaskMode: return parseMode(t, L.mode);
    case Prop::MaskComposite: return parseComposite(t, L.composite);
    case Prop::MaskSize: return parseSize(t, L);
    case Prop::MaskPosition: return parsePos(t, L);
    case Prop::MaskRepeat: return parseRepeat(t, L);
    case Prop::MaskOrigin: return boxWord(t, L.origin, false);
    case Prop::MaskClip: return boxWord(t, L.clip, true);
    default: return false;
    }
}
} // namespace

bool validMaskLonghand(Prop p, std::string_view text) {
    const auto items = split(trimv(text), ',');
    if (items.empty()) return false;
    for (auto it : items) {
        MaskLayer L;
        if (it.empty() || !applyItem(p, it, L)) return false;
    }
    return true;
}

const std::vector<MaskLayer>& maskLayers(const ComputedStyle& s) {
    static std::mutex mu;
    using Key = std::array<u32, 8>;
    static std::map<Key, std::vector<MaskLayer>> cache;
    static const std::vector<MaskLayer> empty;
    if (!s.maskImage) return empty;
    const Key key{s.maskImage, s.maskMode, s.maskComposite, s.maskSize, s.maskPosition, s.maskRepeat, s.maskOrigin, s.maskClip};
    std::lock_guard lock(mu);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::vector<MaskLayer> layers;
    for (auto t : split(animationName(s.maskImage), ',')) {
        MaskLayer L;
        if (!parseImage(t, L)) { layers.clear(); break; }
        layers.push_back(std::move(L));
    }
    bool any = false;
    for (const auto& L : layers) any = any || L.kind != MaskLayer::Kind::None;
    if (!any) layers.clear();
    const std::pair<Prop, u32> lists[] = {{Prop::MaskMode, s.maskMode}, {Prop::MaskComposite, s.maskComposite}, {Prop::MaskSize, s.maskSize},
                                          {Prop::MaskPosition, s.maskPosition}, {Prop::MaskRepeat, s.maskRepeat},
                                          {Prop::MaskOrigin, s.maskOrigin}, {Prop::MaskClip, s.maskClip}};
    for (const auto& [p, id] : lists) {
        if (!id || layers.empty()) continue;
        const auto items = split(animationName(id), ',');
        for (std::size_t k = 0; k < layers.size() && !items.empty(); ++k) applyItem(p, items[k % items.size()], layers[k]);
    }
    return cache.emplace(key, std::move(layers)).first->second;
}

namespace {
/// Premultiplied RGBA (0..1) of layer L's image at a point local to its tile (tile size tw x th).
Color layerColor(const MaskLayer& L, const Image* img, f32 lx, f32 ly, f32 tw, f32 th) {
    using K = MaskLayer::Kind;
    if (L.kind == K::Url) {
        if (!img || img->width <= 0 || img->height <= 0 || img->pixels.empty()) return {};
        const int x = std::clamp(int(lx / tw * f32(img->width)), 0, img->width - 1);
        const int y = std::clamp(int(ly / th * f32(img->height)), 0, img->height - 1);
        const u32 p = img->pixels[std::size_t(y) * std::size_t(img->width) + std::size_t(x)];
        return {f32(p & 0xFF) / 255.f, f32((p >> 8) & 0xFF) / 255.f, f32((p >> 16) & 0xFF) / 255.f, f32(p >> 24) / 255.f};
    }
    if (L.kind != K::Linear && L.kind != K::Radial) return {};
    // gradient line length and t
    f32 t = 0.f, glen = 1.f;
    if (L.kind == K::Linear) {
        const f32 a = L.angle * 3.14159265358979f / 180.f, sx = std::sin(a), sy = -std::cos(a);
        glen = std::max(std::abs(tw * sx) + std::abs(th * sy), 1e-6f);
        t = ((lx - tw * 0.5f) * sx + (ly - th * 0.5f) * sy) / glen + 0.5f;
    } else {
        const f32 cx = res(L.cx, tw), cy = res(L.cy, th);
        const f32 dl = cx, dr = tw - cx, dt = cy, db = th - cy;
        f32 rx, ry;
        if (L.rx.unit != Unit::Auto) {
            rx = res(L.rx, tw);
            ry = L.circle ? rx : res(L.ry, th);
        } else {
            const f32 sxs = L.extent >= 2 ? std::max(dl, dr) : std::min(dl, dr), sys = L.extent >= 2 ? std::max(dt, db) : std::min(dt, db);
            if (L.circle) {
                if (L.extent == 0) rx = std::min(sxs, sys);
                else if (L.extent == 2) rx = std::max(sxs, sys);
                else rx = std::sqrt(sxs * sxs + sys * sys);
                ry = rx;
            } else if (L.extent == 0 || L.extent == 2) rx = sxs, ry = sys;
            else rx = sxs * std::sqrt(2.f), ry = sys * std::sqrt(2.f); // corners: keep the side ratio
        }
        rx = std::max(rx, 1e-6f), ry = std::max(ry, 1e-6f);
        const f32 dx = (lx - cx) / rx, dy = (ly - cy) / ry;
        t = std::sqrt(dx * dx + dy * dy);
        glen = rx;
    }
    // stop offsets along the line (px / %), auto distributed, non-decreasing
    const std::size_t n = L.stopColors.size();
    std::vector<f32> off(n, -1.f);
    for (std::size_t k = 0; k < n; ++k)
        if (L.stopPos[k].unit != Unit::Auto) off[k] = L.stopPos[k].unit == Unit::Percent ? L.stopPos[k].value * 0.01f : L.stopPos[k].value / glen;
    if (off[0] < 0.f) off[0] = 0.f;
    if (off[n - 1] < 0.f) off[n - 1] = std::max(1.f, off[0]);
    for (std::size_t k = 1; k < n; ++k) off[k] = off[k] < 0.f ? off[k] : std::max(off[k], off[k - 1 < k ? k - 1 : 0] < 0.f ? 0.f : off[k - 1]);
    for (std::size_t k = 1; k + 1 < n; ++k)
        if (off[k] < 0.f) {
            std::size_t j = k + 1;
            while (off[j] < 0.f) ++j;
            for (std::size_t m = k; m < j; ++m) off[m] = off[k - 1] + (off[j] - off[k - 1]) * f32(m - k + 1) / f32(j - k + 1);
        }
    for (std::size_t k = 1; k < n; ++k) off[k] = std::max(off[k], off[k - 1]);
    if (L.repeating) {
        const f32 span = off[n - 1] - off[0];
        if (span > 1e-6f) t = off[0] + (t - off[0]) - span * std::floor((t - off[0]) / span);
    }
    Color c;
    if (t <= off[0]) c = L.stopColors[0];
    else if (t >= off[n - 1]) c = L.stopColors[n - 1];
    else
        for (std::size_t k = 1; k < n; ++k)
            if (t <= off[k]) {
                const f32 span = off[k] - off[k - 1];
                const f32 f = span > 1e-6f ? (t - off[k - 1]) / span : 1.f;
                const Color a = L.stopColors[k - 1], b = L.stopColors[k];
                // premultiplied interpolation (CSS)
                const f32 al = a.a + (b.a - a.a) * f;
                c = {a.r * a.a + (b.r * b.a - a.r * a.a) * f, a.g * a.a + (b.g * b.a - a.g * a.a) * f,
                     a.b * a.a + (b.b * b.a - a.b * a.a) * f, al};
                return c;
            }
    return {c.r * c.a, c.g * c.a, c.b * c.a, c.a};
}
/// Tile placement along one axis: returns local coordinate in the tile or < 0 when p is outside every tile.
f32 tileAxis(f32 p, f32 area0, f32 area, f32& tile, f32 pos, u8 rep) {
    if (tile <= 0.f || area <= 0.f) return -1.f;
    const f32 rel = p - area0;
    if (rep == 3) { // round: whole number of tiles
        const f32 n = std::max(1.f, std::round(area / tile));
        tile = area / n;
        pos = 0.f;
    }
    if (rep == 2) { // space
        const f32 n = std::floor(area / tile);
        if (n >= 2.f) {
            const f32 gap = (area - n * tile) / (n - 1.f), period = tile + gap;
            const f32 k = std::floor(rel / period), local = rel - k * period;
            return local < tile ? local : -1.f;
        }
        rep = 1;
        pos = (area - tile) * 0.5f;
    }
    if (rep == 1) {
        const f32 local = rel - pos;
        return local >= 0.f && local < tile ? local : -1.f;
    }
    const f32 local = rel - pos;
    return local - tile * std::floor(local / tile);
}
} // namespace

f32 maskValueAt(const std::vector<MaskLayer>& layers, const ComputedStyle& s, const Rect& border, const MaskImageLookup& images, Vec2 p) {
    f32 acc = 0.f;
    for (std::size_t li = layers.size(); li-- > 0;) {
        const MaskLayer& L = layers[li];
        f32 v = 0.f;
        const Rect clip = cssBox(s, border, L.clip);
        const bool inClip = L.clip == CssBox::NoClip || clip.contains(p);
        if (L.kind != MaskLayer::Kind::None && inClip) {
            const Rect area = cssBox(s, border, L.origin);
            const Image* img = L.kind == MaskLayer::Kind::Url && images ? images(L.url) : nullptr;
            const f32 iw = img ? f32(img->width) : 0.f, ih = img ? f32(img->height) : 0.f;
            f32 tw = area.w, th = area.h;
            if (L.sizeKind != 0) {
                if (iw > 0.f && ih > 0.f) {
                    const f32 sc = L.sizeKind == 1 ? std::max(area.w / iw, area.h / ih) : std::min(area.w / iw, area.h / ih);
                    tw = iw * sc, th = ih * sc;
                }
            } else {
                const bool aw = L.sw.unit == Unit::Auto, ah = L.sh.unit == Unit::Auto;
                if (!aw) tw = res(L.sw, area.w);
                if (!ah) th = res(L.sh, area.h);
                if (iw > 0.f && ih > 0.f) {
                    if (aw && ah) tw = iw, th = ih;
                    else if (aw) tw = th * iw / ih;
                    else if (ah) th = tw * ih / iw;
                }
            }
            const f32 ox = (area.w - tw) * (L.px.unit == Unit::Percent ? L.px.value * 0.01f : 0.f) + (L.px.unit == Unit::Px ? L.px.value : 0.f);
            const f32 oy = (area.h - th) * (L.py.unit == Unit::Percent ? L.py.value * 0.01f : 0.f) + (L.py.unit == Unit::Px ? L.py.value : 0.f);
            const f32 lx = tileAxis(p.x, area.x, area.w, tw, ox, L.repX), ly = tileAxis(p.y, area.y, area.h, th, oy, L.repY);
            if (lx >= 0.f && ly >= 0.f) {
                const Color c = layerColor(L, img, lx, ly, tw, th);
                const bool lum = L.mode == 2;
                v = lum ? 0.2125f * c.r + 0.7154f * c.g + 0.0721f * c.b : c.a; // premultiplied: luminance x alpha
                v = std::clamp(v, 0.f, 1.f);
            }
        }
        if (li + 1 == layers.size()) { acc = v; continue; }
        switch (L.composite) {
        case 1: acc = v * (1.f - acc); break;
        case 2: acc = v * acc; break;
        case 3: acc = v * (1.f - acc) + acc * (1.f - v); break;
        default: acc = v + acc * (1.f - v); break;
        }
    }
    return acc;
}

bool buildMaskClip(const ComputedStyle& s, const Rect& border, const MaskImageLookup& images, ClipLayer& out) {
    const std::vector<MaskLayer>& layers = maskLayers(s);
    if (layers.empty()) return false;
    // texture area: union of the layers' clip boxes (no-clip: border box)
    Rect box{};
    bool first = true;
    for (const auto& L : layers) {
        const Rect r = cssBox(s, border, L.clip);
        box = first ? r : box.unite(r);
        first = false;
    }
    out = {};
    out.gradient = 3;
    out.path.rect(box);
    const f32 area = std::max(1.f, std::ceil(box.w) * std::ceil(box.h));
    const f32 k = std::min(1.f, std::sqrt(f32(kMaxMaskTexelArea) / area));
    const int tw = std::clamp(int(std::ceil(box.w * k)), 1, kMaxMaskTexels), th = std::clamp(int(std::ceil(box.h * k)), 1, kMaxMaskTexels);
    out.texW = u16(tw), out.texH = u16(th);
    out.texels.resize(std::size_t(tw) * std::size_t(th));
    const f32 sx = box.w > 0.f ? f32(tw) / box.w : 1.f, sy = box.h > 0.f ? f32(th) / box.h : 1.f;
    out.toUnit = {sx, 0.f, 0.f, sy, -box.x * sx, -box.y * sy};
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x)
            out.texels[std::size_t(y) * std::size_t(tw) + std::size_t(x)] =
                maskValueAt(layers, s, border, images, {box.x + (f32(x) + 0.5f) / sx, box.y + (f32(y) + 0.5f) / sy});
    return true;
}

} // namespace fuse::ui
