#include "fuse/ui/svg.hpp"

#include "svg_scan.hpp"

#include <algorithm>
#include <cmath>

namespace fuse::ui {

namespace {

using detail::svgNumber;
using detail::svgSkipSep;
using detail::svgSpace;

constexpr f32 kPi = 3.14159265358979f;
constexpr std::size_t kMaxDepth = 64;
constexpr u32 kMaxElements = 200000;
constexpr std::size_t kMaxAttrs = 256;

std::string_view trim(std::string_view s) {
    while (!s.empty() && svgSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && svgSpace(s.back())) s.remove_suffix(1);
    return s;
}

// The colour keywords icons actually use (the CSS basic 16 plus the common extended names); the rest of the syntax
// (#hex, rgb()) goes through the style engine's parseColor.
struct NamedColor {
    std::string_view name;
    u32 hex;
};
constexpr NamedColor kColors[] = {
    {"black", 0x000000},   {"white", 0xFFFFFF},     {"red", 0xFF0000},      {"green", 0x008000},     {"blue", 0x0000FF},
    {"yellow", 0xFFFF00},  {"cyan", 0x00FFFF},      {"aqua", 0x00FFFF},     {"magenta", 0xFF00FF},   {"fuchsia", 0xFF00FF},
    {"gray", 0x808080},    {"grey", 0x808080},      {"silver", 0xC0C0C0},   {"maroon", 0x800000},    {"olive", 0x808000},
    {"lime", 0x00FF00},    {"teal", 0x008080},      {"navy", 0x000080},     {"purple", 0x800080},    {"orange", 0xFFA500},
    {"pink", 0xFFC0CB},    {"brown", 0xA52A2A},     {"gold", 0xFFD700},     {"indigo", 0x4B0082},    {"violet", 0xEE82EE},
    {"coral", 0xFF7F50},   {"salmon", 0xFA8072},    {"crimson", 0xDC143C},  {"tomato", 0xFF6347},    {"khaki", 0xF0E68C},
    {"orchid", 0xDA70D6},  {"plum", 0xDDA0DD},      {"tan", 0xD2B48C},      {"beige", 0xF5F5DC},     {"ivory", 0xFFFFF0},
    {"lightgray", 0xD3D3D3}, {"lightgrey", 0xD3D3D3}, {"darkgray", 0xA9A9A9}, {"darkgrey", 0xA9A9A9}, {"dimgray", 0x696969},
    {"dimgrey", 0x696969}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"whitesmoke", 0xF5F5F5}, {"gainsboro", 0xDCDCDC},
    {"darkred", 0x8B0000}, {"darkgreen", 0x006400}, {"darkblue", 0x00008B}, {"darkorange", 0xFF8C00}, {"lightblue", 0xADD8E6},
    {"lightgreen", 0x90EE90}, {"skyblue", 0x87CEEB}, {"steelblue", 0x4682B4}, {"royalblue", 0x4169E1}, {"dodgerblue", 0x1E90FF},
    {"forestgreen", 0x228B22}, {"seagreen", 0x2E8B57}, {"limegreen", 0x32CD32}, {"turquoise", 0x40E0D0}, {"firebrick", 0xB22222},
    {"hotpink", 0xFF69B4}, {"deeppink", 0xFF1493},  {"chocolate", 0xD2691E}, {"sienna", 0xA0522D},   {"goldenrod", 0xDAA520},
};

bool svgColor(std::string_view s, Color& out) {
    s = trim(s);
    char buf[64];
    if (s.empty() || s.size() >= sizeof buf) return false;
    for (std::size_t i = 0; i < s.size(); ++i) buf[i] = (s[i] >= 'A' && s[i] <= 'Z') ? char(s[i] + 32) : s[i];
    const std::string_view low(buf, s.size());
    for (const NamedColor& n : kColors)
        if (n.name == low) {
            out = Color::rgb(n.hex);
            return true;
        }
    Color c;
    if (!parseColor(low, c)) return false;
    if (!std::isfinite(c.r) || !std::isfinite(c.g) || !std::isfinite(c.b) || !std::isfinite(c.a)) return false;
    out = {std::clamp(c.r, 0.f, 1.f), std::clamp(c.g, 0.f, 1.f), std::clamp(c.b, 0.f, 1.f), std::clamp(c.a, 0.f, 1.f)};
    return true;
}

/// A number with an optional unit suffix (ignored) or '%' (reported).
bool svgLength(std::string_view s, f32& out, bool* percent = nullptr) {
    s = trim(s);
    const char* p = s.data();
    const char* end = p + s.size();
    f32 v = 0.f;
    if (!svgNumber(p, end, v)) return false;
    const bool pct = p < end && *p == '%';
    if (percent) *percent = pct;
    out = v;
    return true;
}

bool parseTransform(std::string_view s, Affine& out) {
    Affine m;
    const char* p = s.data();
    const char* end = p + s.size();
    for (;;) {
        svgSkipSep(p, end);
        if (p >= end) break;
        const char* n0 = p;
        while (p < end && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) ++p;
        const std::string_view name(n0, std::size_t(p - n0));
        while (p < end && svgSpace(*p)) ++p;
        if (name.empty() || p >= end || *p != '(') return false;
        ++p;
        f32 a[6]{};
        int n = 0;
        for (;;) {
            svgSkipSep(p, end);
            if (p < end && *p == ')') break;
            if (n >= 6 || !svgNumber(p, end, a[n])) return false;
            ++n;
        }
        ++p; // ')'
        Affine t;
        if (name == "matrix" && n == 6) t = {a[0], a[1], a[2], a[3], a[4], a[5]};
        else if (name == "translate" && (n == 1 || n == 2)) t = Affine::translation(a[0], n == 2 ? a[1] : 0.f);
        else if (name == "scale" && (n == 1 || n == 2)) t = Affine::scaling(a[0], n == 2 ? a[1] : a[0]);
        else if (name == "rotate" && (n == 1 || n == 3)) {
            t = Affine::rotation(a[0] * kPi / 180.f);
            if (n == 3) t = Affine::translation(a[1], a[2]) * t * Affine::translation(-a[1], -a[2]);
        } else if (name == "skewX" && n == 1) t = {1.f, 0.f, std::tan(a[0] * kPi / 180.f), 1.f, 0.f, 0.f};
        else if (name == "skewY" && n == 1) t = {1.f, std::tan(a[0] * kPi / 180.f), 0.f, 1.f, 0.f, 0.f};
        else return false;
        m = m * t;
    }
    const f32 v[6]{m.a, m.b, m.c, m.d, m.tx, m.ty};
    for (f32 x : v)
        if (!std::isfinite(x)) return false;
    out = m;
    return true;
}

// ---- XML subset scanner --------------------------------------------------------------------------------------------
struct Attr {
    std::string_view name, value;
};
enum class Tok : u8 { Open, Close, End, Error };

struct Scanner {
    const char* begin;
    const char* p;
    const char* end;
    std::string_view name;
    std::vector<Attr> attrs;
    bool selfClose = false;
    const char* message = "";

    bool starts(std::string_view s) const { return std::size_t(end - p) >= s.size() && std::string_view(p, s.size()) == s; }
    bool skipPast(std::string_view s) {
        for (; std::size_t(end - p) >= s.size(); ++p)
            if (std::string_view(p, s.size()) == s) {
                p += s.size();
                return true;
            }
        p = end;
        return false;
    }
    Tok fail(const char* msg) {
        message = msg;
        return Tok::Error;
    }
    static bool nameChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.' ||
               (static_cast<unsigned char>(c) >= 0x80);
    }
    Tok next() {
        for (;;) {
            while (p < end && *p != '<') ++p; // character data is skipped
            if (p >= end) return Tok::End;
            if (starts("<!--")) {
                if (!skipPast("-->")) return fail("unterminated comment");
                continue;
            }
            if (starts("<![CDATA[")) {
                if (!skipPast("]]>")) return fail("unterminated CDATA section");
                continue;
            }
            if (starts("<?")) {
                if (!skipPast("?>")) return fail("unterminated processing instruction");
                continue;
            }
            if (starts("<!")) { // DOCTYPE, with an optional [internal subset]
                int bracket = 0;
                for (; p < end; ++p) {
                    if (*p == '[') ++bracket;
                    else if (*p == ']') --bracket;
                    else if (*p == '>' && bracket <= 0) break;
                }
                if (p >= end) return fail("unterminated declaration");
                ++p;
                continue;
            }
            break;
        }
        ++p; // '<'
        if (p < end && *p == '/') {
            ++p;
            const char* n0 = p;
            while (p < end && nameChar(*p)) ++p;
            name = {n0, std::size_t(p - n0)};
            while (p < end && svgSpace(*p)) ++p;
            if (name.empty() || p >= end || *p != '>') return fail("malformed end tag");
            ++p;
            return Tok::Close;
        }
        const char* n0 = p;
        while (p < end && nameChar(*p)) ++p;
        name = {n0, std::size_t(p - n0)};
        if (name.empty()) return fail("malformed tag");
        attrs.clear();
        selfClose = false;
        for (;;) {
            while (p < end && svgSpace(*p)) ++p;
            if (p >= end) return fail("unterminated tag");
            if (*p == '>') {
                ++p;
                return Tok::Open;
            }
            if (*p == '/') {
                if (p + 1 >= end || p[1] != '>') return fail("malformed tag");
                p += 2;
                selfClose = true;
                return Tok::Open;
            }
            const char* a0 = p;
            while (p < end && nameChar(*p)) ++p;
            const std::string_view an(a0, std::size_t(p - a0));
            while (p < end && svgSpace(*p)) ++p;
            if (an.empty() || p >= end || *p != '=') return fail("malformed attribute");
            ++p;
            while (p < end && svgSpace(*p)) ++p;
            if (p >= end || (*p != '"' && *p != '\'')) return fail("attribute value is not quoted");
            const char quote = *p++;
            const char* v0 = p;
            while (p < end && *p != quote) ++p;
            if (p >= end) return fail("unterminated attribute value");
            if (attrs.size() >= kMaxAttrs) return fail("too many attributes");
            attrs.push_back({an, {v0, std::size_t(p - v0)}});
            ++p;
        }
    }
};

std::string_view localName(std::string_view n) {
    const auto c = n.rfind(':');
    return c == std::string_view::npos ? n : n.substr(c + 1);
}

} // namespace

// ---- Parser --------------------------------------------------------------------------------------------------------
struct SvgParser {
    using Paint = SvgImage::Paint;
    struct Style {
        Paint fill{Paint::Kind::Solid, Color::rgb(0x000000), 0, 1.f};
        Paint stroke;
        std::string_view fillRef, strokeRef;
        StrokeStyle ss;
        FillRule rule = FillRule::NonZero;
        f32 opacity = 1.f, fillOpacity = 1.f, strokeOpacity = 1.f;
        bool hasColor = false, display = true;
        Color color;
    };
    struct Frame {
        std::string_view name;
        Style style;
        Affine xf;
        bool hidden = false;
        i32 gradient = -1;
    };

    SvgImage img;
    Scanner sc;
    std::vector<Frame> stack;
    std::vector<std::string_view> gradientIds, gradientHrefs;
    std::vector<std::string_view> fillRefs, strokeRefs;
    bool sawRoot = false, hasViewBox = false, hasW = false, hasH = false;
    u32 elements = 0;
    SvgError error;
    bool failed = false;

    bool fail(SvgErrorKind k, const char* msg, const char* at = nullptr) {
        failed = true;
        error = {k, u32((at ? at : sc.p) - sc.begin), msg};
        return false;
    }
    std::string_view attr(std::string_view name) const {
        for (const Attr& a : sc.attrs)
            if (a.name == name) return a.value;
        return {};
    }
    bool has(std::string_view name) const {
        for (const Attr& a : sc.attrs)
            if (a.name == name) return true;
        return false;
    }
    f32 num(std::string_view name, f32 def, bool* percent = nullptr) const {
        f32 v = def;
        if (has(name) && !svgLength(attr(name), v, percent)) v = def;
        return v;
    }

    static void parsePaint(std::string_view v, Paint& paint, std::string_view& ref, const Style& st) {
        v = trim(v);
        if (v == "none" || v == "transparent") {
            paint.kind = Paint::Kind::None;
        } else if (v.size() == 12 && (v == "currentColor" || v == "currentcolor")) {
            if (st.hasColor) {
                paint.kind = Paint::Kind::Solid;
                paint.color = st.color;
            } else {
                paint.kind = Paint::Kind::Current;
            }
        } else if (v.starts_with("url(")) {
            const auto hash = v.find('#'), close = v.find(')');
            if (hash == std::string_view::npos || close == std::string_view::npos || close < hash) {
                paint.kind = Paint::Kind::None;
                return;
            }
            std::string_view id = trim(v.substr(hash + 1, close - hash - 1));
            if (!id.empty() && (id.back() == '"' || id.back() == '\'')) id.remove_suffix(1);
            ref = id;
            paint.kind = Paint::Kind::Gradient;
        } else {
            Color c;
            if (svgColor(v, c)) { // an invalid colour leaves the inherited paint
                paint.kind = Paint::Kind::Solid;
                paint.color = c;
            }
        }
    }

    static void applyProp(Style& st, std::string_view name, std::string_view value) {
        value = trim(value);
        f32 v = 0.f;
        if (name == "fill") parsePaint(value, st.fill, st.fillRef, st);
        else if (name == "stroke") parsePaint(value, st.stroke, st.strokeRef, st);
        else if (name == "color") {
            Color c;
            if (svgColor(value, c)) {
                st.color = c;
                st.hasColor = true;
            }
        } else if (name == "stroke-width") {
            if (svgLength(value, v) && v >= 0.f) st.ss.width = v;
        } else if (name == "fill-rule") {
            st.rule = value == "evenodd" ? FillRule::EvenOdd : FillRule::NonZero;
        } else if (name == "opacity") {
            if (svgLength(value, v)) st.opacity *= std::clamp(v, 0.f, 1.f);
        } else if (name == "fill-opacity") {
            if (svgLength(value, v)) st.fillOpacity = std::clamp(v, 0.f, 1.f);
        } else if (name == "stroke-opacity") {
            if (svgLength(value, v)) st.strokeOpacity = std::clamp(v, 0.f, 1.f);
        } else if (name == "stroke-linecap") {
            st.ss.cap = value == "round" ? LineCap::Round : value == "square" ? LineCap::Square : LineCap::Butt;
        } else if (name == "stroke-linejoin") {
            st.ss.join = value == "round" ? LineJoin::Round : value == "bevel" ? LineJoin::Bevel : LineJoin::Miter;
        } else if (name == "stroke-miterlimit") {
            if (svgLength(value, v) && v >= 1.f) st.ss.miterLimit = v;
        } else if (name == "stroke-dashoffset") {
            if (svgLength(value, v)) st.ss.dashOffset = v;
        } else if (name == "stroke-dasharray") {
            st.ss.dashCount = 0;
            if (value != "none") {
                const char* p = value.data();
                const char* end = p + value.size();
                bool ok = true;
                while (ok) {
                    svgSkipSep(p, end);
                    if (p >= end) break;
                    if (!svgNumber(p, end, v) || v < 0.f) { ok = false; break; }
                    while (p < end && !svgSpace(*p) && *p != ',') ++p; // unit suffix
                    if (st.ss.dashCount < StrokeStyle::kMaxDashes) st.ss.dashes[st.ss.dashCount++] = v;
                }
                if (!ok) st.ss.dashCount = 0;
            }
        } else if (name == "display") {
            if (value == "none") st.display = false;
        } else if (name == "visibility") {
            st.display = st.display && value != "hidden" && value != "collapse";
        }
    }

    void applyStyle(Style& st) const {
        for (const Attr& a : sc.attrs)
            if (a.name != "style") applyProp(st, a.name, a.value);
        std::string_view css = attr("style"); // declarations override presentation attributes
        while (!css.empty()) {
            const auto semi = css.find(';');
            const std::string_view decl = css.substr(0, semi);
            const auto colon = decl.find(':');
            if (colon != std::string_view::npos) applyProp(st, trim(decl.substr(0, colon)), decl.substr(colon + 1));
            if (semi == std::string_view::npos) break;
            css.remove_prefix(semi + 1);
        }
    }

    void addShape(const Frame& f, Path&& path) {
        if (f.hidden || !f.style.display || path.empty()) return;
        const Style& st = f.style;
        SvgImage::Shape s;
        s.xf = f.xf;
        s.rule = st.rule;
        s.strokeStyle = st.ss;
        s.fill = st.fill;
        s.fill.opacity = st.opacity * st.fillOpacity;
        s.stroke = st.stroke;
        s.stroke.opacity = st.opacity * st.strokeOpacity;
        if (!(st.ss.width > 0.f) || !std::isfinite(st.ss.width)) s.stroke.kind = Paint::Kind::None;
        if (s.fill.opacity <= 0.f) s.fill.kind = Paint::Kind::None;
        if (s.stroke.opacity <= 0.f) s.stroke.kind = Paint::Kind::None;
        if (s.fill.kind == Paint::Kind::None && s.stroke.kind == Paint::Kind::None) return;
        for (const Vec2& p : path.points())
            if (!std::isfinite(p.x) || !std::isfinite(p.y)) return;
        s.path = std::move(path);
        img.m_shapes.push_back(std::move(s));
        fillRefs.push_back(st.fillRef);
        strokeRefs.push_back(st.strokeRef);
    }

    bool shape(std::string_view name, const Frame& f, const char* at) {
        Path path;
        if (name == "path") {
            if (!has("d")) return true;
            auto r = Path::fromSvg(attr("d"));
            if (!r) return fail(SvgErrorKind::BadValue, "malformed path data", attr("d").data() + std::min<std::size_t>(r.error().offset, attr("d").size()));
            path = std::move(*r);
        } else if (name == "rect") {
            const f32 x = num("x", 0.f), y = num("y", 0.f), w = num("width", 0.f), h = num("height", 0.f);
            if (!(w > 0.f) || !(h > 0.f)) return true;
            f32 rx = num("rx", -1.f), ry = num("ry", -1.f);
            if (rx < 0.f) rx = std::max(ry, 0.f);
            if (ry < 0.f) ry = rx;
            rx = std::min(rx, w * 0.5f);
            ry = std::min(ry, h * 0.5f);
            if (rx <= 0.f || ry <= 0.f) {
                path.addRect({x, y, w, h});
            } else {
                path.moveTo({x + rx, y}).lineTo({x + w - rx, y}).arcTo(rx, ry, 0.f, false, true, {x + w, y + ry});
                path.lineTo({x + w, y + h - ry}).arcTo(rx, ry, 0.f, false, true, {x + w - rx, y + h});
                path.lineTo({x + rx, y + h}).arcTo(rx, ry, 0.f, false, true, {x, y + h - ry});
                path.lineTo({x, y + ry}).arcTo(rx, ry, 0.f, false, true, {x + rx, y}).close();
            }
        } else if (name == "circle") {
            const f32 r = num("r", 0.f);
            if (!(r > 0.f)) return true;
            path.addCircle({num("cx", 0.f), num("cy", 0.f)}, r);
        } else if (name == "ellipse") {
            const f32 rx = num("rx", 0.f), ry = num("ry", 0.f);
            if (!(rx > 0.f) || !(ry > 0.f)) return true;
            path.addEllipse({num("cx", 0.f), num("cy", 0.f)}, rx, ry);
        } else if (name == "line") {
            path.moveTo({num("x1", 0.f), num("y1", 0.f)}).lineTo({num("x2", 0.f), num("y2", 0.f)});
        } else { // polyline / polygon
            const std::string_view pts = attr("points");
            const char* p = pts.data();
            const char* end = p + pts.size();
            bool first = true;
            for (;;) {
                f32 x = 0.f, y = 0.f;
                svgSkipSep(p, end);
                if (p >= end) break;
                if (!svgNumber(p, end, x)) return fail(SvgErrorKind::BadValue, "malformed points list", p);
                svgSkipSep(p, end);
                if (p >= end) break; // an odd coordinate is dropped
                if (!svgNumber(p, end, y)) return fail(SvgErrorKind::BadValue, "malformed points list", p);
                if (first) path.moveTo({x, y});
                else path.lineTo({x, y});
                first = false;
            }
            if (name == "polygon") path.close();
        }
        (void)at;
        addShape(f, std::move(path));
        return true;
    }

    void gradient(std::string_view name, Frame& f) {
        SvgImage::GradientDef g;
        g.kind = name == "radialGradient" ? GradientKind::Radial : GradientKind::Linear;
        g.bboxUnits = attr("gradientUnits") != "userSpaceOnUse";
        const std::string_view spread = attr("spreadMethod");
        g.spread = spread == "repeat" ? GradientSpread::Repeat : spread == "reflect" ? GradientSpread::Reflect : GradientSpread::Pad;
        if (has("gradientTransform") && !parseTransform(attr("gradientTransform"), g.xf)) g.xf = {};
        auto coord = [&](std::string_view n, f32 def) {
            bool pct = false;
            f32 v = num(n, def, &pct);
            return pct ? v / 100.f : v;
        };
        g.x1 = coord("x1", 0.f);
        g.y1 = coord("y1", 0.f);
        g.x2 = coord("x2", 1.f);
        g.y2 = coord("y2", 0.f);
        g.cx = coord("cx", 0.5f);
        g.cy = coord("cy", 0.5f);
        g.r = coord("r", 0.5f);
        std::string_view href = has("href") ? attr("href") : attr("xlink:href");
        href = trim(href);
        if (!href.empty() && href.front() == '#') href.remove_prefix(1);
        f.gradient = i32(img.m_gradients.size());
        img.m_gradients.push_back(std::move(g));
        gradientIds.push_back(attr("id"));
        gradientHrefs.push_back(href);
    }

    void stop(const Frame& parent) {
        if (parent.gradient < 0) return;
        bool pct = false;
        f32 off = num("offset", 0.f, &pct);
        if (pct) off /= 100.f;
        Color c = Color::rgb(0x000000);
        f32 op = 1.f;
        auto prop = [&](std::string_view n, std::string_view v) {
            f32 x = 0.f;
            if (n == "stop-color") (void)svgColor(v, c);
            else if (n == "stop-opacity" && svgLength(v, x)) op = std::clamp(x, 0.f, 1.f);
        };
        for (const Attr& a : sc.attrs) prop(a.name, a.value);
        std::string_view css = attr("style");
        while (!css.empty()) {
            const auto semi = css.find(';');
            const std::string_view decl = css.substr(0, semi);
            const auto colon = decl.find(':');
            if (colon != std::string_view::npos) prop(trim(decl.substr(0, colon)), decl.substr(colon + 1));
            if (semi == std::string_view::npos) break;
            css.remove_prefix(semi + 1);
        }
        c.a *= op;
        img.m_gradients[std::size_t(parent.gradient)].stops.push_back({std::clamp(off, 0.f, 1.f), c});
    }

    bool open() {
        const char* at = sc.p;
        if (++elements > kMaxElements) return fail(SvgErrorKind::Limit, "too many elements");
        if (stack.size() >= kMaxDepth) return fail(SvgErrorKind::Limit, "elements nested too deeply");
        const std::string_view name = localName(sc.name);
        Frame f;
        if (!stack.empty()) {
            f = stack.back();
            f.gradient = -1;
        }
        f.name = sc.name;
        if (!sawRoot) {
            if (name != "svg") return fail(SvgErrorKind::NoSvgRoot, "the root element is not <svg>");
            sawRoot = true;
            f32 w = 0.f, h = 0.f;
            bool pw = false, ph = false; // a percentage is relative to the host: no intrinsic size
            hasW = has("width") && svgLength(attr("width"), w, &pw) && !pw && w > 0.f;
            hasH = has("height") && svgLength(attr("height"), h, &ph) && !ph && h > 0.f;
            img.m_size = {hasW ? w : 0.f, hasH ? h : 0.f};
            if (has("viewBox")) {
                const std::string_view vb = attr("viewBox");
                const char* p = vb.data();
                const char* end = p + vb.size();
                f32 v[4]{};
                bool ok = true;
                for (f32& x : v) {
                    svgSkipSep(p, end);
                    ok = ok && svgNumber(p, end, x);
                }
                if (ok && v[2] > 0.f && v[3] > 0.f) {
                    img.m_viewBox = {v[0], v[1], v[2], v[3]};
                    hasViewBox = true;
                }
            }
            applyStyle(f.style);
        } else if (name == "g" || name == "a" || name == "switch" || name == "svg") {
            applyStyle(f.style);
            Affine t;
            if (has("transform") && parseTransform(attr("transform"), t)) f.xf = f.xf * t;
        } else if (name == "defs" || name == "title" || name == "desc" || name == "metadata") {
            f.hidden = true;
        } else if (name == "linearGradient" || name == "radialGradient") {
            f.hidden = true;
            gradient(name, f);
        } else if (name == "stop") {
            if (!stack.empty()) stop(stack.back());
        } else if (name == "path" || name == "rect" || name == "circle" || name == "ellipse" || name == "line" ||
                   name == "polyline" || name == "polygon") {
            applyStyle(f.style);
            Affine t;
            if (has("transform") && parseTransform(attr("transform"), t)) f.xf = f.xf * t;
            if (!shape(name, f, at)) return false;
            f.hidden = true; // children of a shape (animation, title) draw nothing
        } else if (name == "text" || name == "tspan" || name == "use" || name == "image" || name == "style" || name == "filter" ||
                   name == "clipPath" || name == "mask" || name == "pattern" || name == "marker" || name == "symbol" ||
                   name == "foreignObject" || name == "script") {
            if (!f.hidden) ++img.m_unsupported;
            f.hidden = true;
        }
        if (!sc.selfClose) stack.push_back(f);
        return true;
    }

    bool run(std::string_view xml) {
        sc.begin = sc.p = xml.data();
        sc.end = xml.data() + xml.size();
        for (;;) {
            const Tok t = sc.next();
            if (t == Tok::Error) return fail(SvgErrorKind::Syntax, sc.message);
            if (t == Tok::End) break;
            if (t == Tok::Open) {
                if (!open()) return false;
            } else {
                if (stack.empty() || stack.back().name != sc.name) return fail(SvgErrorKind::Syntax, "mismatched end tag");
                stack.pop_back();
            }
        }
        if (!sawRoot) return fail(SvgErrorKind::NoSvgRoot, "no <svg> element");
        if (!stack.empty()) return fail(SvgErrorKind::Syntax, "unclosed element");
        finish();
        return true;
    }

    void finish() {
        // gradient href: inherit the stops of the referenced gradient
        for (std::size_t i = 0; i < img.m_gradients.size(); ++i) {
            std::size_t cur = i;
            for (int hop = 0; hop < 8 && img.m_gradients[i].stops.empty() && !gradientHrefs[cur].empty(); ++hop) {
                std::size_t next = img.m_gradients.size();
                for (std::size_t k = 0; k < gradientIds.size(); ++k)
                    if (gradientIds[k] == gradientHrefs[cur]) { next = k; break; }
                if (next == img.m_gradients.size() || next == i) break;
                img.m_gradients[i].stops = img.m_gradients[next].stops;
                cur = next;
            }
            auto& st = img.m_gradients[i].stops; // offsets must not decrease
            for (std::size_t k = 1; k < st.size(); ++k) st[k].offset = std::max(st[k].offset, st[k - 1].offset);
        }
        auto resolve = [&](Paint& p, std::string_view ref) {
            if (p.kind != Paint::Kind::Gradient) return;
            for (std::size_t k = 0; k < gradientIds.size(); ++k)
                if (!ref.empty() && gradientIds[k] == ref && !img.m_gradients[k].stops.empty()) {
                    p.gradient = u32(k);
                    return;
                }
            p.kind = Paint::Kind::None;
        };
        for (std::size_t i = 0; i < img.m_shapes.size(); ++i) {
            resolve(img.m_shapes[i].fill, fillRefs[i]);
            resolve(img.m_shapes[i].stroke, strokeRefs[i]);
        }
        if (!hasViewBox) {
            if (hasW && hasH) {
                img.m_viewBox = {0.f, 0.f, img.m_size.w, img.m_size.h};
            } else { // neither: the bounds of what is drawn
                Rect b;
                bool any = false;
                for (const SvgImage::Shape& s : img.m_shapes)
                    for (const Vec2& p : s.path.points()) {
                        const Vec2 q = s.xf.apply(p);
                        if (!std::isfinite(q.x) || !std::isfinite(q.y)) continue;
                        if (!any) { b = {q.x, q.y, 0.f, 0.f}; any = true; }
                        const f32 x0 = std::min(b.x, q.x), y0 = std::min(b.y, q.y);
                        b = {x0, y0, std::max(b.right(), q.x) - x0, std::max(b.bottom(), q.y) - y0};
                    }
                img.m_viewBox = b;
            }
        }
        if (!hasW) img.m_size.w = img.m_viewBox.w;
        if (!hasH) img.m_size.h = img.m_viewBox.h;
    }
};

std::expected<SvgImage, SvgError> SvgImage::parse(std::string_view xml) {
    SvgParser p;
    if (!p.run(xml)) return std::unexpected(std::move(p.error));
    return std::move(p.img);
}

u32 SvgImage::segmentCount() const {
    u32 n = 0;
    for (const Shape& s : m_shapes)
        for (const PathVerb v : s.path.verbs()) n += v != PathVerb::Move;
    return n;
}

Brush SvgImage::brushFor(const Paint& paint, const Shape& s, Color currentColor, Gradient& storage) const {
    switch (paint.kind) {
    case Paint::Kind::None: return Brush(Color{});
    case Paint::Kind::Solid: return Brush(paint.color.withAlpha(paint.color.a * paint.opacity));
    case Paint::Kind::Current: return Brush(currentColor.withAlpha(currentColor.a * paint.opacity));
    case Paint::Kind::Gradient: break;
    }
    const GradientDef& g = m_gradients[paint.gradient];
    if (g.stops.size() == 1) return Brush(g.stops[0].color.withAlpha(g.stops[0].color.a * paint.opacity));
    storage.kind = g.kind;
    storage.spread = g.spread;
    storage.stops = g.stops;
    storage.transform = g.xf;
    if (g.bboxUnits) {
        const Rect bb = s.path.bounds();
        if (bb.empty()) return Brush(Color{}); // SVG: no bounding box, no paint
        storage.transform = Affine{bb.w, 0.f, 0.f, bb.h, bb.x, bb.y} * g.xf;
    }
    if (g.kind == GradientKind::Linear) {
        storage.p0 = {g.x1, g.y1};
        storage.p1 = {g.x2, g.y2};
    } else {
        storage.p0 = {g.cx, g.cy};
        storage.p1 = {g.r, 0.f};
    }
    Brush b(storage);
    b.opacity = paint.opacity;
    return b;
}

void SvgImage::render(Painter& p, const Rect& dst, Color currentColor, bool stretch) const {
    if (m_shapes.empty() || dst.empty() || m_viewBox.empty()) return;
    f32 sx = dst.w / m_viewBox.w, sy = dst.h / m_viewBox.h;
    if (!stretch) sx = sy = std::min(sx, sy);
    const f32 tx = dst.x + (dst.w - m_viewBox.w * sx) * 0.5f - m_viewBox.x * sx;
    const f32 ty = dst.y + (dst.h - m_viewBox.h * sy) * 0.5f - m_viewBox.y * sy;
    const Affine saved = p.worldTransform();
    const Affine base = saved * Affine{sx, 0.f, 0.f, sy, tx, ty};
    for (const Shape& s : m_shapes) {
        p.setTransform(base * s.xf);
        Gradient g;
        if (s.fill.kind != Paint::Kind::None) {
            const Brush b = brushFor(s.fill, s, currentColor, g);
            if (b.hasGradient || b.color.a > 0.f) p.fillPath(s.path, b, s.rule);
        }
        if (s.stroke.kind != Paint::Kind::None) {
            const Brush b = brushFor(s.stroke, s, currentColor, g);
            if (b.hasGradient || b.color.a > 0.f) p.strokePath(s.path, s.strokeStyle, b);
        }
    }
    p.setTransform(saved);
}

} // namespace fuse::ui
