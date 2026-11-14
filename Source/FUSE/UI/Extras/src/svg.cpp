// SVG document parsing + rendering onto DrawList paths. See svg.hpp.
#include "fuse/ui/from_chars.hpp"
#include "fuse/ui/extras/svg.hpp"

#include "fuse/ui/os.hpp"
#include "fuse/ui/style.hpp"
#include "fuse/text/text.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <optional>

namespace fuse::ui::svg {
namespace {

// ---- minimal XML ------------------------------------------------------------------------------------------------
struct Element {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<std::unique_ptr<Element>> children;
    std::string text; ///< "#text" children (character data of <text> / <tspan>)
    std::string_view attr(std::string_view k) const {
        for (auto& [n, v] : attrs) if (n == k) return v;
        return {};
    }
    bool has(std::string_view k) const {
        for (auto& [n, v] : attrs) if (n == k) return true;
        return false;
    }
};

std::string decodeEntities(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const std::size_t e = s.find(';', i);
        if (e == std::string_view::npos) { out += s[i]; continue; }
        const std::string_view ent = s.substr(i + 1, e - i - 1);
        if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "amp") out += '&';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else { out.append(s.substr(i, e - i + 1)); }
        i = e;
    }
    return out;
}

class XmlParser {
public:
    explicit XmlParser(std::string_view s) : m_s(s) {}
    std::unique_ptr<Element> parse(std::string& err) {
        std::vector<Element*> stack;
        std::unique_ptr<Element> root;
        while (m_i < m_s.size()) {
            const std::size_t lt = m_s.find('<', m_i);
            if (!stack.empty() && lt != m_i) { // character data: kept for text content elements only
                const std::string_view pn = stack.back()->name;
                const std::string_view ln = pn.substr(pn.find(':') == std::string_view::npos ? 0 : pn.find(':') + 1);
                if (ln == "text" || ln == "tspan" || ln == "textPath") {
                    auto t = std::make_unique<Element>();
                    t->name = "#text";
                    t->text = decodeEntities(m_s.substr(m_i, (lt == std::string_view::npos ? m_s.size() : lt) - m_i));
                    stack.back()->children.push_back(std::move(t));
                }
            }
            if (lt == std::string_view::npos) break;
            m_i = lt;
            if (starts("<!--")) { if (!skipPast("-->")) return fail(err, "unterminated comment"); continue; }
            if (starts("<![CDATA[")) { if (!skipPast("]]>")) return fail(err, "unterminated CDATA"); continue; }
            if (starts("<?") ) { if (!skipPast("?>")) return fail(err, "unterminated processing instruction"); continue; }
            if (starts("<!")) { if (!skipDoctype()) return fail(err, "unterminated DOCTYPE"); continue; }
            if (starts("</")) {
                m_i += 2;
                const std::string name = readName();
                skipWs();
                if (m_i >= m_s.size() || m_s[m_i] != '>') return fail(err, "bad end tag");
                ++m_i;
                if (stack.empty() || stack.back()->name != name) return fail(err, "mismatched </" + name + ">");
                stack.pop_back();
                continue;
            }
            ++m_i;
            auto el = std::make_unique<Element>();
            el->name = readName();
            if (el->name.empty()) return fail(err, "bad tag");
            bool selfClose = false;
            while (true) {
                skipWs();
                if (m_i >= m_s.size()) return fail(err, "unterminated tag <" + el->name + ">");
                if (m_s[m_i] == '/') { selfClose = true; ++m_i; continue; }
                if (m_s[m_i] == '>') { ++m_i; break; }
                std::string an = readName();
                if (an.empty()) return fail(err, "bad attribute in <" + el->name + ">");
                skipWs();
                std::string av;
                if (m_i < m_s.size() && m_s[m_i] == '=') {
                    ++m_i;
                    skipWs();
                    if (m_i >= m_s.size() || (m_s[m_i] != '"' && m_s[m_i] != '\'')) return fail(err, "unquoted attribute");
                    const char q = m_s[m_i++];
                    const std::size_t e = m_s.find(q, m_i);
                    if (e == std::string_view::npos) return fail(err, "unterminated attribute");
                    av = decodeEntities(m_s.substr(m_i, e - m_i));
                    m_i = e + 1;
                }
                el->attrs.emplace_back(std::move(an), std::move(av));
            }
            Element* raw = el.get();
            if (stack.empty()) {
                if (root) return fail(err, "multiple root elements");
                root = std::move(el);
            } else {
                stack.back()->children.push_back(std::move(el));
            }
            if (!selfClose) stack.push_back(raw);
        }
        if (!stack.empty()) return fail(err, "unclosed <" + stack.back()->name + ">");
        if (!root) return fail(err, "no root element");
        return root;
    }

private:
    std::unique_ptr<Element> fail(std::string& err, std::string m) { err = std::move(m); return nullptr; }
    bool starts(std::string_view p) const { return m_s.substr(m_i, p.size()) == p; }
    bool skipPast(std::string_view p) {
        const std::size_t e = m_s.find(p, m_i);
        if (e == std::string_view::npos) return false;
        m_i = e + p.size();
        return true;
    }
    bool skipDoctype() {
        int depth = 0;
        for (; m_i < m_s.size(); ++m_i) {
            if (m_s[m_i] == '[') ++depth;
            else if (m_s[m_i] == ']') --depth;
            else if (m_s[m_i] == '>' && depth <= 0) { ++m_i; return true; }
        }
        return false;
    }
    void skipWs() { while (m_i < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_i]))) ++m_i; }
    std::string readName() {
        const std::size_t b = m_i;
        while (m_i < m_s.size()) {
            const char c = m_s[m_i];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == ':' || c == '-' || c == '_' || c == '.') ++m_i;
            else break;
        }
        return std::string(m_s.substr(b, m_i - b));
    }
    std::string_view m_s;
    std::size_t m_i = 0;
};

// ---- values -----------------------------------------------------------------------------------------------------
std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}
std::string_view localName(std::string_view n) { const auto c = n.find(':'); return c == std::string_view::npos ? n : n.substr(c + 1); }

/// Reads numbers separated by whitespace / commas.
std::vector<f32> numbers(std::string_view s) {
    std::vector<f32> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) ++i;
        if (i >= s.size()) break;
        const char* b = s.data() + i;
        if (*b == '+') ++b;
        f32 v = 0.f;
        const auto r = fuse::ui::fromChars(b, s.data() + s.size(), v);
        if (r.ec != std::errc{}) break;
        out.push_back(v);
        i = std::size_t(r.ptr - s.data());
        while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i; // units (px, pt, ...)
        if (i < s.size() && s[i] == '%') ++i;
    }
    return out;
}
std::optional<f32> length(std::string_view s) {
    s = trim(s);
    if (s.empty()) return std::nullopt;
    const auto v = numbers(s);
    if (v.empty()) return std::nullopt;
    f32 x = v[0];
    if (s.ends_with("pt")) x *= 96.f / 72.f;
    else if (s.ends_with("mm")) x *= 96.f / 25.4f;
    else if (s.ends_with("cm")) x *= 96.f / 2.54f;
    else if (s.ends_with("in")) x *= 96.f;
    return x;
}

Affine mul(const Affine& m, const Affine& n) { // m * n (apply n first)
    return {m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3], m[1] * n[2] + m[3] * n[3],
            m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5]};
}
constexpr Affine kIdentity{1, 0, 0, 1, 0, 0};

// ---- styling ----------------------------------------------------------------------------------------------------
struct Paint {
    bool none = false;
    Color color{0, 0, 0, 1};
    std::string ref; ///< url(#id)
};
/// A gradient definition: `unit` is the gradient in its own space (gradientTransform applied); bbox units are mapped
/// onto each shape's local bounding box when it is painted.
struct GradDef {
    fuse::ui::Gradient unit;
    bool bbox = true;
    Color from, to;
    GradientKind kind = kGradientVertical;
};
struct StyleState {
    Paint fill{}, stroke{.none = true};
    f32 strokeWidth = 1.f, opacity = 1.f, fillOpacity = 1.f, strokeOpacity = 1.f;
    bool evenOdd = false, clipEvenOdd = false, hidden = false;
    Color currentColor{0, 0, 0, 1};
    f32 fontSize = 16.f, letterSpacing = 0.f;
    int anchor = 0; ///< text-anchor: 0 start, 1 middle, 2 end
};

bool parsePaint(std::string_view v, Paint& out, const StyleState& st) {
    v = trim(v);
    if (v.empty() || v == "inherit") return false;
    if (v == "none" || v == "transparent") { out = {.none = true}; return true; }
    if (v == "currentColor") { out = {.color = st.currentColor}; return true; }
    if (v.starts_with("url(")) {
        const auto h = v.find('#'), e = v.find(')');
        if (h != std::string_view::npos && e != std::string_view::npos && e > h) {
            out = {.ref = std::string(v.substr(h + 1, e - h - 1))};
            return true;
        }
        return false;
    }
    Color c;
    if (!parseColor(v, c)) return false;
    out = {.color = c};
    return true;
}

void applyProperty(StyleState& st, std::string_view k, std::string_view v) {
    v = trim(v);
    if (k == "fill") parsePaint(v, st.fill, st);
    else if (k == "stroke") parsePaint(v, st.stroke, st);
    else if (k == "color") { Color c; if (parseColor(v, c)) st.currentColor = c; }
    else if (k == "stroke-width") { if (auto l = length(v)) st.strokeWidth = *l; }
    else if (k == "fill-rule") st.evenOdd = v == "evenodd";
    else if (k == "clip-rule") st.clipEvenOdd = v == "evenodd";
    else if (k == "opacity") { if (auto l = length(v)) st.opacity *= std::clamp(*l, 0.f, 1.f); }
    else if (k == "fill-opacity") { if (auto l = length(v)) st.fillOpacity = std::clamp(*l, 0.f, 1.f); }
    else if (k == "stroke-opacity") { if (auto l = length(v)) st.strokeOpacity = std::clamp(*l, 0.f, 1.f); }
    else if (k == "display") st.hidden = st.hidden || v == "none";
    else if (k == "visibility") st.hidden = v == "hidden" || v == "collapse";
    else if (k == "font-size") {
        if (v.ends_with("%")) { if (auto l = length(v)) st.fontSize *= *l / 100.f; }
        else if (v.ends_with("em")) { if (auto l = length(v)) st.fontSize *= *l; }
        else if (auto l = length(v); l && *l > 0.f) st.fontSize = *l;
    } else if (k == "letter-spacing") { if (auto l = length(v)) st.letterSpacing = *l; else st.letterSpacing = 0.f; }
    else if (k == "text-anchor") st.anchor = v == "middle" ? 1 : v == "end" ? 2 : 0;
}

/// A property from the attribute or from style="" (style wins).
std::string_view prop(const Element& e, std::string_view key) {
    std::string_view out = e.attr(key);
    std::string_view css = e.attr("style");
    while (!css.empty()) {
        const auto semi = css.find(';');
        const std::string_view decl = css.substr(0, semi);
        if (const auto colon = decl.find(':'); colon != std::string_view::npos && trim(decl.substr(0, colon)) == key)
            out = trim(decl.substr(colon + 1));
        if (semi == std::string_view::npos) break;
        css.remove_prefix(semi + 1);
    }
    return out;
}
std::string_view urlRef(std::string_view v) {
    v = trim(v);
    if (!v.starts_with("url(")) return {};
    const auto h = v.find('#'), e = v.find(')');
    if (h == std::string_view::npos || e == std::string_view::npos || e < h) return {};
    return v.substr(h + 1, e - h - 1);
}

void applyStyle(StyleState& st, const Element& e) {
    for (const char* k : {"color", "fill", "stroke", "stroke-width", "fill-rule", "clip-rule", "opacity", "fill-opacity", "stroke-opacity",
                          "display", "visibility", "font-size", "letter-spacing", "text-anchor"})
        if (e.has(k)) applyProperty(st, k, e.attr(k));
    std::string_view css = e.attr("style");
    while (!css.empty()) {
        const auto semi = css.find(';');
        const std::string_view decl = css.substr(0, semi);
        if (const auto colon = decl.find(':'); colon != std::string_view::npos) applyProperty(st, trim(decl.substr(0, colon)), decl.substr(colon + 1));
        if (semi == std::string_view::npos) break;
        css.remove_prefix(semi + 1);
    }
}

Rect unionRect(const Rect& a, const Rect& b, bool& any) {
    if (!any) { any = true; return b; }
    const f32 x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    return {x0, y0, std::max(a.right(), b.right()) - x0, std::max(a.bottom(), b.bottom()) - y0};
}
bool invert(const Affine& m, Affine& out) {
    const f32 det = m[0] * m[3] - m[1] * m[2];
    if (std::abs(det) < 1e-12f) return false;
    const f32 id = 1.f / det;
    out = {m[3] * id, -m[1] * id, -m[2] * id, m[0] * id, (m[2] * m[5] - m[3] * m[4]) * id, (m[1] * m[4] - m[0] * m[5]) * id};
    return true;
}

/// FUSE's bundled UI font (+ fallbacks) for <text>; loaded once.
struct TextFonts {
    fuse::text::Font main;
    std::vector<fuse::text::Font> fallbacks;
    bool ok = false;
};
const TextFonts& textFonts() {
    static const TextFonts* f = [] {
        auto* t = new TextFonts;
        if (auto b = fuse::text::Font::bundled()) {
            t->main = std::move(*b);
            t->fallbacks = fuse::text::Font::bundledFallbacks();
            t->ok = true;
        }
        return t;
    }();
    return *f;
}

struct Builder {
    std::vector<Shape>& shapes;
    std::vector<ClipRegion>& clips;
    std::vector<PatternFill>& patterns;
    Rect vb{};
    std::vector<std::pair<std::string, std::pair<std::size_t, std::size_t>>> ids{};
    std::map<std::string, const Element*, std::less<>> byId{};
    std::map<std::string, GradDef, std::less<>> gradients{};
    int useDepth = 0, patternDepth = 0;
    int curClip = -1;
    bool clipMode = false; ///< building clipPath / mask geometry: keep unpainted shapes

    void index(const Element& e) {
        if (const auto id = e.attr("id"); !id.empty()) byId.emplace(std::string(id), &e);
        for (auto& c : e.children) index(*c);
    }
    const Element* href(const Element& e) const {
        std::string_view h = e.has("href") ? e.attr("href") : e.attr("xlink:href");
        if (!h.starts_with("#")) return nullptr;
        h.remove_prefix(1);
        auto it = byId.find(h);
        return it == byId.end() ? nullptr : it->second;
    }
    /// Attribute through the href chain (gradients / patterns inherit unset attributes).
    std::string_view chainAttr(const Element& e, std::string_view k) const {
        const Element* c = &e;
        for (int hops = 0; c && hops < 5; ++hops, c = href(*c))
            if (c->has(k)) return c->attr(k);
        return {};
    }
    const Element* chainChildren(const Element& e) const {
        const Element* c = &e;
        for (int hops = 0; c && hops < 5; ++hops, c = href(*c))
            if (!c->children.empty()) return c;
        return &e;
    }
    Builder sub(std::vector<Shape>& out) const {
        Builder b{out, clips, patterns, vb};
        b.byId = byId;
        b.gradients = gradients;
        b.useDepth = useDepth;
        b.patternDepth = patternDepth;
        return b;
    }

    void collectGradients() {
        for (auto& [id, e] : byId) {
            const std::string_view n = localName(e->name);
            if (n != "linearGradient" && n != "radialGradient") continue;
            const Element* stops = chainChildren(*e);
            std::vector<GradientStop> st;
            for (auto& s : stops->children) {
                if (localName(s->name) != "stop") continue;
                Color c{0, 0, 0, 1};
                f32 op = 1.f;
                const std::string_view sc = prop(*s, "stop-color"), so = prop(*s, "stop-opacity");
                if (!sc.empty()) parseColor(sc, c);
                if (auto o = length(so)) op = std::clamp(*o, 0.f, 1.f);
                f32 off = 0.f;
                const std::string_view os = s->attr("offset");
                if (auto o = length(os)) off = os.ends_with("%") ? *o / 100.f : *o;
                off = std::clamp(off, 0.f, 1.f);
                if (!st.empty()) off = std::max(off, st.back().offset); // offsets never decrease
                st.push_back({off, c.withAlpha(c.a * op)});
            }
            if (st.empty()) continue;
            GradDef g;
            g.from = st.front().color;
            g.to = st.back().color;
            g.bbox = chainAttr(*e, "gradientUnits") != "userSpaceOnUse";
            const std::string_view sm = chainAttr(*e, "spreadMethod");
            const GradientSpread spread = sm == "reflect" ? GradientSpread::Reflect : sm == "repeat" ? GradientSpread::Repeat : GradientSpread::Pad;
            // percentages: fractions in bbox units, else of the viewBox (x: width, y: height, r: normalised diagonal)
            auto coord = [&](std::string_view k, std::string_view def, int axis) {
                std::string_view v = chainAttr(*e, k);
                if (v.empty()) v = def;
                f32 x = length(v).value_or(0.f);
                if (trim(v).ends_with("%")) {
                    x /= 100.f;
                    if (!g.bbox) x *= axis == 0 ? vb.w : axis == 1 ? vb.h : std::sqrt((vb.w * vb.w + vb.h * vb.h) * 0.5f);
                }
                return x;
            };
            if (n == "radialGradient") {
                g.kind = kGradientRadial;
                const f32 r = coord("r", "50%", 2);
                const Vec2 c{coord("cx", "50%", 0), coord("cy", "50%", 1)};
                const std::string_view fxs = chainAttr(*e, "fx"), fys = chainAttr(*e, "fy");
                const Vec2 f{fxs.empty() ? c.x : coord("fx", "50%", 0), fys.empty() ? c.y : coord("fy", "50%", 1)};
                g.unit = fuse::ui::Gradient::radialFocal(c, std::max(r, 1e-6f), std::max(r, 1e-6f), f, st, spread);
            } else {
                const f32 x1 = coord("x1", "0%", 0), y1 = coord("y1", "0%", 1), x2 = coord("x2", "100%", 0), y2 = coord("y2", "0%", 1);
                g.kind = std::abs(x2 - x1) >= std::abs(y2 - y1) ? kGradientHorizontal : kGradientVertical;
                if ((g.kind == kGradientHorizontal && x2 < x1) || (g.kind == kGradientVertical && y2 < y1)) std::swap(g.from, g.to);
                g.unit = fuse::ui::Gradient::linear({x1, y1}, {x2, y2}, st, spread);
            }
            if (const std::string_view t = chainAttr(*e, "gradientTransform"); !t.empty()) g.unit.transform(parseTransform(t));
            gradients.emplace(id, std::move(g));
        }
    }

    int buildPattern(const Element& pe, const Rect& bbox, const Affine& m) {
        if (patternDepth >= 2) return -1;
        const bool bboxUnits = chainAttr(pe, "patternUnits") != "userSpaceOnUse";
        const bool bboxContent = chainAttr(pe, "patternContentUnits") == "objectBoundingBox";
        auto val = [&](std::string_view k, int axis) {
            const std::string_view v = chainAttr(pe, k);
            f32 x = length(v).value_or(0.f);
            if (trim(v).ends_with("%")) x /= 100.f;
            if (bboxUnits) x = axis == 0 ? bbox.x + x * bbox.w : axis == 1 ? bbox.y + x * bbox.h : axis == 2 ? x * bbox.w : x * bbox.h;
            else if (trim(v).ends_with("%")) x *= (axis == 0 || axis == 2) ? vb.w : vb.h;
            return x;
        };
        PatternFill pf;
        pf.tile = {val("x", 0), val("y", 1), val("width", 2), val("height", 3)};
        if (pf.tile.w <= 0.f || pf.tile.h <= 0.f) return -1;
        pf.toDoc = m;
        if (const std::string_view t = chainAttr(pe, "patternTransform"); !t.empty()) pf.toDoc = mul(m, parseTransform(t));
        const auto pvb = numbers(chainAttr(pe, "viewBox"));
        if (pvb.size() == 4 && pvb[2] > 0.f && pvb[3] > 0.f) {
            const f32 sx = pf.tile.w / pvb[2], sy = pf.tile.h / pvb[3];
            pf.content = {sx, 0, 0, sy, -pvb[0] * sx, -pvb[1] * sy};
        } else if (bboxContent) {
            pf.content = {bbox.w, 0, 0, bbox.h, 0, 0};
        }
        Builder b = sub(pf.shapes);
        ++b.patternDepth;
        for (auto& c : chainChildren(pe)->children) b.walk(*c, StyleState{}, kIdentity);
        if (pf.shapes.empty()) return -1;
        patterns.push_back(std::move(pf));
        return int(patterns.size() - 1);
    }

    void emit(Path p, const StyleState& st, const Affine& m, std::string_view id) {
        if (st.hidden || p.empty()) return;
        const Rect localBox = p.bounds();
        p.transform(m);
        Shape s;
        s.path = std::move(p);
        s.evenOdd = clipMode ? st.clipEvenOdd : st.evenOdd;
        s.id = std::string(id);
        s.clip = curClip;
        if (clipMode) { // geometry only; the fill keeps the paint's colour for mask luminance
            s.fill = st.fill.none ? Color{} : st.fill.color.withAlpha(st.fill.color.a * st.fillOpacity * st.opacity);
            if (!st.fill.ref.empty()) {
                if (auto it = gradients.find(st.fill.ref); it != gradients.end()) {
                    const Color a = it->second.from, b = it->second.to;
                    s.fill = Color{(a.r + b.r) * 0.5f, (a.g + b.g) * 0.5f, (a.b + b.b) * 0.5f, (a.a + b.a) * 0.5f * st.fillOpacity * st.opacity};
                    const GradDef& g = it->second; // the full gradient too: masks evaluate it per pixel
                    if (!g.bbox || (localBox.w > 0.f && localBox.h > 0.f)) {
                        fuse::ui::Gradient gr = g.unit;
                        if (g.bbox) gr.transform({localBox.w, 0, 0, localBox.h, localBox.x, localBox.y});
                        gr.transform(m);
                        for (GradientStop& k : gr.stops) k.color = k.color.withAlpha(k.color.a * st.fillOpacity * st.opacity);
                        s.paint = std::move(gr);
                    }
                } else s.fill = Color{1, 1, 1, st.fillOpacity * st.opacity};
            }
            shapes.push_back(std::move(s));
            return;
        }
        auto resolve = [&](const Paint& pt, f32 op, Color& c, bool fill) {
            if (pt.none) return;
            if (!pt.ref.empty()) {
                auto it = gradients.find(pt.ref);
                if (it == gradients.end()) {
                    if (!fill) return;
                    auto pe = byId.find(pt.ref);
                    if (pe == byId.end() || localName(pe->second->name) != "pattern") return;
                    const int pi = buildPattern(*pe->second, localBox, m);
                    if (pi < 0) return;
                    s.pattern = pi;
                    c = Color{1, 1, 1, op * st.opacity};
                    return;
                }
                const GradDef& g = it->second;
                if (fill) {
                    s.gradient = g.kind;
                    c = g.from.withAlpha(g.from.a * op * st.opacity);
                    s.fill2 = g.to.withAlpha(g.to.a * op * st.opacity);
                    if (!g.bbox || (localBox.w > 0.f && localBox.h > 0.f)) {
                        fuse::ui::Gradient gr = g.unit;
                        if (g.bbox) gr.transform({localBox.w, 0, 0, localBox.h, localBox.x, localBox.y});
                        gr.transform(m);
                        for (GradientStop& k : gr.stops) k.color = k.color.withAlpha(k.color.a * op * st.opacity);
                        s.paint = std::move(gr);
                    }
                } else {
                    const Color mid{(g.from.r + g.to.r) * 0.5f, (g.from.g + g.to.g) * 0.5f, (g.from.b + g.to.b) * 0.5f, (g.from.a + g.to.a) * 0.5f};
                    c = mid.withAlpha(mid.a * op * st.opacity);
                }
                return;
            }
            c = pt.color.withAlpha(pt.color.a * op * st.opacity);
        };
        resolve(st.fill, st.fillOpacity, s.fill, true);
        resolve(st.stroke, st.strokeOpacity, s.stroke, false);
        if (s.stroke.a > 0.f) s.strokeWidth = st.strokeWidth * std::sqrt(std::abs(m[0] * m[3] - m[1] * m[2]));
        if (s.fill.a <= 0.f && s.stroke.a <= 0.f) return;
        shapes.push_back(std::move(s));
    }

    // ---- <text> ----
    struct TextCursor {
        f32 x = 0.f, y = 0.f;
        bool atStart = true;      ///< collapse leading white space
        bool pendingSpace = false;
        struct Run { Path path; StyleState st; f32 x0, x1; };
        std::vector<Run> chunk;   ///< runs of the current text chunk (text-anchor shifts them together)
        int anchor = 0;
    };
    void flushChunk(TextCursor& tc, const Affine& m, std::string_view id) {
        if (tc.chunk.empty()) return;
        f32 x0 = tc.chunk.front().x0, x1 = tc.chunk.front().x1;
        for (auto& r : tc.chunk) { x0 = std::min(x0, r.x0); x1 = std::max(x1, r.x1); }
        const f32 shift = tc.anchor == 1 ? -(x1 - x0) * 0.5f : tc.anchor == 2 ? -(x1 - x0) : 0.f;
        for (auto& r : tc.chunk) {
            if (shift != 0.f) r.path.transform(Affine{1, 0, 0, 1, shift, 0});
            emit(std::move(r.path), r.st, m, id);
        }
        tc.chunk.clear();
    }
    void textRun(TextCursor& tc, std::string_view raw, const StyleState& st) {
        const TextFonts& tf = textFonts();
        if (!tf.ok) return;
        // xml:space="default": newlines dropped, tabs -> spaces, runs collapsed, leading space at the text start dropped
        std::string t;
        for (char c : raw) {
            if (c == '\n' || c == '\r') continue;
            if (c == '\t') c = ' ';
            if (c == ' ') { if (!tc.atStart) tc.pendingSpace = true; continue; }
            if (tc.pendingSpace) { t += ' '; tc.pendingSpace = false; }
            t += c;
            tc.atStart = false;
        }
        if (t.empty()) return;
        TextCursor::Run run{Path{}, st, tc.x, tc.x};
        std::vector<fuse::text::GlyphOutlineCmd> cmds;
        std::uint32_t prev = 0;
        const fuse::text::Font* prevFont = nullptr;
        for (std::size_t i = 0; i < t.size();) {
            // UTF-8 decode
            char32_t cp = static_cast<unsigned char>(t[i]);
            int len = 1;
            if (cp >= 0xF0 && i + 3 < t.size() + 0) { cp = ((cp & 7u) << 18) | ((t[i + 1] & 63u) << 12) | ((t[i + 2] & 63u) << 6) | (t[i + 3] & 63u); len = 4; }
            else if (cp >= 0xE0 && i + 2 < t.size()) { cp = ((cp & 15u) << 12) | ((t[i + 1] & 63u) << 6) | (t[i + 2] & 63u); len = 3; }
            else if (cp >= 0xC0 && i + 1 < t.size()) { cp = ((cp & 31u) << 6) | (t[i + 1] & 63u); len = 2; }
            i += std::size_t(len);
            const fuse::text::Font* f = &tf.main;
            std::uint32_t g = f->glyphIndex(cp);
            if (g == 0)
                for (const auto& fb : tf.fallbacks)
                    if (const std::uint32_t k = fb.glyphIndex(cp)) { f = &fb; g = k; break; }
            const f32 px = st.fontSize;
            if (prevFont == f && prev) tc.x += f->kerning(prev, g, px);
            if (f->glyphOutline(g, px, cmds)) {
                for (const auto& c : cmds) {
                    const Vec2 p{tc.x + c.x, tc.y + c.y};
                    switch (c.kind) {
                    case fuse::text::GlyphOutlineCmd::Move: run.path.moveTo(p); break;
                    case fuse::text::GlyphOutlineCmd::Line: run.path.lineTo(p); break;
                    case fuse::text::GlyphOutlineCmd::Quad: run.path.quadTo({tc.x + c.cx, tc.y + c.cy}, p); break;
                    case fuse::text::GlyphOutlineCmd::Cubic: run.path.cubicTo({tc.x + c.cx, tc.y + c.cy}, {tc.x + c.cx1, tc.y + c.cy1}, p); break;
                    }
                }
                run.path.close();
            }
            tc.x += f->advance(g, px) + st.letterSpacing;
            prev = g;
            prevFont = f;
        }
        run.x1 = tc.x;
        if (!run.path.empty()) tc.chunk.push_back(std::move(run));
        else if (!tc.chunk.empty()) tc.chunk.back().x1 = std::max(tc.chunk.back().x1, tc.x);
        else tc.chunk.push_back({Path{}, st, run.x0, tc.x});
    }
    void textElement(const Element& e, StyleState st, TextCursor& tc, const Affine& m, std::string_view id, bool root) {
        if (!root) applyStyle(st, e);
        if (st.hidden) return;
        const auto xs = numbers(e.attr("x")), ys = numbers(e.attr("y")), dxs = numbers(e.attr("dx")), dys = numbers(e.attr("dy"));
        if (!xs.empty() || !ys.empty()) { // absolute position: a new text chunk
            flushChunk(tc, m, id);
            tc.anchor = st.anchor;
            if (!xs.empty()) tc.x = xs[0];
            if (!ys.empty()) tc.y = ys[0];
        }
        if (root) tc.anchor = st.anchor;
        if (!dxs.empty()) tc.x += dxs[0];
        if (!dys.empty()) tc.y += dys[0];
        for (auto& c : e.children) {
            const std::string_view n = localName(c->name);
            if (c->name == "#text") textRun(tc, c->text, st);
            else if (n == "tspan" || n == "a") textElement(*c, st, tc, m, id, false);
        }
    }

    /// clip-path / mask on the element that produced shapes [first, end): a new clip region inside `outer`.
    void applyClip(std::string_view ref, std::size_t first, int outer, const Affine& m, bool mask) {
        auto it = byId.find(ref);
        if (it == byId.end() || shapes.size() <= first) return;
        const Element& ce = *it->second;
        if (localName(ce.name) != (mask ? "mask" : "clipPath")) return;
        Affine cm = m;
        const bool bboxUnits = mask ? ce.attr("maskContentUnits") == "objectBoundingBox" : ce.attr("clipPathUnits") == "objectBoundingBox";
        if (bboxUnits) {
            bool any = false;
            Rect b{};
            for (std::size_t i = first; i < shapes.size(); ++i) b = unionRect(b, shapes[i].path.bounds(), any);
            cm = {b.w, 0, 0, b.h, b.x, b.y};
        }
        if (ce.has("transform")) cm = mul(cm, parseTransform(ce.attr("transform")));
        std::vector<Shape> geo;
        Builder b = sub(geo);
        b.clipMode = true;
        for (auto& c : ce.children) b.walk(*c, StyleState{}, cm);
        ClipRegion r;
        r.parent = outer;
        auto lumOf = [](Color c) { return (0.2125f * c.r + 0.7154f * c.g + 0.0721f * c.b) * c.a; };
        for (const Shape& s : geo) {
            r.path.append(s.path);
            r.evenOdd = s.evenOdd;
            ClipLayer l;
            l.path = s.path;
            l.evenOdd = s.evenOdd;
            if (mask) {
                if (!s.paint.stops.empty()) { // ramp between the end stops along the gradient's own geometry
                    const Color c0 = s.paint.stops.front().color, c1 = s.paint.stops.back().color;
                    l.gradient = s.paint.radial ? 2 : 1;
                    l.toUnit = s.paint.toUnit;
                    l.value0 = lumOf(c0), l.alpha0 = c0.a; // stop alphas carry the opacities
                    l.value1 = lumOf(c1), l.alpha1 = c1.a;
                } else if (s.gradient != kGradientNone) {
                    l.value0 = 0.5f * (lumOf(s.fill) + lumOf(s.fill2)), l.alpha0 = 0.5f * (s.fill.a + s.fill2.a);
                } else l.value0 = lumOf(s.fill), l.alpha0 = s.fill.a;
            }
            r.layers.push_back(std::move(l));
        }
        if (geo.size() > 1) r.evenOdd = false;
        if (!mask && geo.size() <= 1) r.layers.clear(); // one plain path: the classic clip entry
        const int idx = int(clips.size());
        clips.push_back(std::move(r)); // empty geometry clips everything (SVG: an empty clipPath hides the element)
        for (std::size_t i = first; i < shapes.size(); ++i) {
            Shape& s = shapes[i];
            if (s.clip == outer) s.clip = idx;
            else { // an inner element's own region chain: insert this region below `outer`
                int k = s.clip;
                while (k >= 0 && clips[std::size_t(k)].parent != outer && clips[std::size_t(k)].parent != idx) k = clips[std::size_t(k)].parent;
                if (k >= 0 && clips[std::size_t(k)].parent == outer) clips[std::size_t(k)].parent = idx;
            }
        }
    }

    void walk(const Element& e, StyleState st, Affine m) {
        const std::string_view n = localName(e.name);
        if (n == "defs" || n == "linearGradient" || n == "radialGradient" || n == "clipPath" || n == "mask" || n == "symbol" ||
            n == "title" || n == "desc" || n == "metadata" || n == "style" || n == "pattern" || n == "marker" || e.name == "#text")
            return;
        applyStyle(st, e);
        if (st.hidden) return;
        if (e.has("transform")) m = mul(m, parseTransform(e.attr("transform")));
        const std::size_t first = shapes.size();
        const int outer = curClip;
        auto L = [&](std::string_view k, f32 d = 0.f) { return length(e.attr(k)).value_or(d); };
        const std::string_view id = e.attr("id");
        Path p;
        if (n == "svg" || n == "g" || n == "a" || n == "switch") {
            for (auto& c : e.children) walk(*c, st, m);
        } else if (n == "use" && useDepth < 8) {
            if (const Element* ref = href(e)) {
                ++useDepth;
                const Affine tm = mul(m, Affine{1, 0, 0, 1, L("x"), L("y")});
                if (localName(ref->name) == "symbol") for (auto& c : ref->children) walk(*c, st, tm);
                else walk(*ref, st, tm);
                --useDepth;
            }
        } else if (n == "text") {
            TextCursor tc;
            textElement(e, st, tc, m, id, true);
            flushChunk(tc, m, id);
        } else if (n == "rect") {
            const f32 x = L("x"), y = L("y"), w = L("width"), h = L("height");
            if (w > 0.f && h > 0.f) {
                f32 rx = L("rx", -1.f), ry = L("ry", -1.f);
                if (rx < 0.f) rx = ry;
                if (ry < 0.f) ry = rx;
                rx = std::clamp(rx, 0.f, w * 0.5f);
                ry = std::clamp(ry, 0.f, h * 0.5f);
                if (rx <= 0.f || ry <= 0.f) p.rect({x, y, w, h});
                else {
                    p.moveTo({x + rx, y}).lineTo({x + w - rx, y}).arcTo({rx, ry}, 0, false, true, {x + w, y + ry});
                    p.lineTo({x + w, y + h - ry}).arcTo({rx, ry}, 0, false, true, {x + w - rx, y + h});
                    p.lineTo({x + rx, y + h}).arcTo({rx, ry}, 0, false, true, {x, y + h - ry});
                    p.lineTo({x, y + ry}).arcTo({rx, ry}, 0, false, true, {x + rx, y}).close();
                }
            }
        } else if (n == "circle") {
            if (const f32 r = L("r"); r > 0.f) p.ellipse({L("cx"), L("cy")}, r, r);
        } else if (n == "ellipse") {
            const f32 rx = L("rx"), ry = L("ry");
            if (rx > 0.f && ry > 0.f) p.ellipse({L("cx"), L("cy")}, rx, ry);
        } else if (n == "line") {
            p.moveTo({L("x1"), L("y1")}).lineTo({L("x2"), L("y2")});
            StyleState ls = st;
            ls.fill = {.none = true}; // lines are never filled
            emit(std::move(p), ls, m, id);
            p = {};
        } else if (n == "polyline" || n == "polygon") {
            const auto v = numbers(e.attr("points"));
            for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
                if (i == 0) p.moveTo({v[0], v[1]});
                else p.lineTo({v[i], v[i + 1]});
            }
            if (n == "polygon" && v.size() >= 4) p.close();
        } else if (n == "path") {
            p.appendSvg(e.attr("d"));
        }
        if (!p.empty()) emit(std::move(p), st, m, id);
        if (!clipMode) {
            if (const std::string_view c = urlRef(prop(e, "clip-path")); !c.empty()) applyClip(c, first, outer, m, false);
            if (const std::string_view k = urlRef(prop(e, "mask")); !k.empty()) applyClip(k, first, outer, m, true);
        }
        if (!id.empty() && shapes.size() > first) ids.push_back({std::string(id), {first, shapes.size()}});
    }
};

} // namespace

Affine parseTransform(std::string_view t) {
    Affine m = kIdentity;
    std::size_t i = 0;
    while (i < t.size()) {
        while (i < t.size() && (std::isspace(static_cast<unsigned char>(t[i])) || t[i] == ',')) ++i;
        const std::size_t nb = i;
        while (i < t.size() && std::isalpha(static_cast<unsigned char>(t[i]))) ++i;
        const std::string_view name = t.substr(nb, i - nb);
        if (name.empty()) break;
        const std::size_t open = t.find('(', i), close = t.find(')', i);
        if (open == std::string_view::npos || close == std::string_view::npos || close < open) return kIdentity;
        const auto a = numbers(t.substr(open + 1, close - open - 1));
        i = close + 1;
        Affine n = kIdentity;
        constexpr f32 kDeg = 3.14159265f / 180.f;
        if (name == "matrix" && a.size() == 6) n = {a[0], a[1], a[2], a[3], a[4], a[5]};
        else if (name == "translate" && !a.empty()) n = {1, 0, 0, 1, a[0], a.size() > 1 ? a[1] : 0.f};
        else if (name == "scale" && !a.empty()) n = {a[0], 0, 0, a.size() > 1 ? a[1] : a[0], 0, 0};
        else if (name == "rotate" && !a.empty()) {
            const f32 c = std::cos(a[0] * kDeg), s = std::sin(a[0] * kDeg);
            n = {c, s, -s, c, 0, 0};
            if (a.size() == 3) n = mul(mul(Affine{1, 0, 0, 1, a[1], a[2]}, n), Affine{1, 0, 0, 1, -a[1], -a[2]});
        } else if (name == "skewX" && !a.empty()) n = {1, 0, std::tan(a[0] * kDeg), 1, 0, 0};
        else if (name == "skewY" && !a.empty()) n = {1, std::tan(a[0] * kDeg), 0, 1, 0, 0};
        else return kIdentity;
        m = mul(m, n);
    }
    return m;
}

bool Document::load(std::string_view text) {
    m_shapes.clear();
    m_clips.clear();
    m_patterns.clear();
    m_ids.clear();
    m_valid = false;
    m_error.clear();
    XmlParser xp(text);
    const auto root = xp.parse(m_error);
    if (!root) return false;
    if (localName(root->name) != "svg") { m_error = "root element is <" + root->name + ">, not <svg>"; return false; }
    const auto vb = numbers(root->attr("viewBox"));
    const auto w = length(root->attr("width")), h = length(root->attr("height"));
    const bool pctW = root->attr("width").ends_with("%"), pctH = root->attr("height").ends_with("%");
    if (vb.size() == 4 && vb[2] > 0.f && vb[3] > 0.f) m_viewBox = {vb[0], vb[1], vb[2], vb[3]};
    else m_viewBox = {0, 0, (w && !pctW) ? *w : 300.f, (h && !pctH) ? *h : 150.f};
    m_size = {(w && !pctW) ? *w : m_viewBox.w, (h && !pctH) ? *h : m_viewBox.h};
    m_stretch = trim(root->attr("preserveAspectRatio")).starts_with("none");
    Builder b{m_shapes, m_clips, m_patterns, m_viewBox};
    b.index(*root);
    b.collectGradients();
    b.walk(*root, StyleState{}, kIdentity);
    for (auto& [id, r] : b.ids) m_ids.push_back({id, r.first, r.second});
    m_valid = true;
    return true;
}

bool Document::loadFile(const std::string& path) {
    auto text = fuse::ui::os::readFile(path); // OS access through the fuse::ui::os seam (FORGE)
    if (!text) { m_error = text.error(); m_valid = false; return false; }
    return load(*text);
}

Rect Document::boundsOf(std::string_view id) const {
    Rect out{};
    bool any = false;
    auto add = [&](const Shape& s) {
        Rect b = s.path.bounds();
        if (s.strokeWidth > 0.f) { const f32 h = s.strokeWidth * 0.5f; b = {b.x - h, b.y - h, b.w + 2 * h, b.h + 2 * h}; }
        if (!any) { out = b; any = true; return; }
        const f32 x0 = std::min(out.x, b.x), y0 = std::min(out.y, b.y);
        out = {x0, y0, std::max(out.right(), b.right()) - x0, std::max(out.bottom(), b.bottom()) - y0};
    };
    for (const IdRange& r : m_ids)
        if (r.id == id) for (std::size_t i = r.first; i < r.last; ++i) add(m_shapes[i]);
    return out;
}

namespace {
void drawShape(DrawList& dl, const Shape& s, const Affine& M, f32 sw, f32 opacity) {
    Path p = s.path;
    p.transform(M);
    const Color fill = s.fill.withAlpha(s.fill.a * opacity), stroke = s.stroke.withAlpha(s.stroke.a * opacity);
    if (fill.a > 0.f) {
        if (!s.paint.stops.empty()) {
            fuse::ui::Gradient g = s.paint;
            g.transform(M);
            if (opacity < 1.f) for (GradientStop& k : g.stops) k.color = k.color.withAlpha(k.color.a * opacity);
            dl.gradientPath(p, g, s.evenOdd);
        } else if (s.gradient != kGradientNone) {
            dl.gradientPath(p, fill, s.fill2.withAlpha(s.fill2.a * opacity), s.gradient, s.evenOdd);
        } else {
            dl.path(p, fill, s.evenOdd);
        }
    }
    if (stroke.a > 0.f && s.strokeWidth > 0.f) dl.path(p, Color{}, s.evenOdd, stroke, s.strokeWidth * sw);
}
} // namespace

void Document::render(DrawList& dl, Rect target, f32 opacity) const {
    if (!m_valid || m_viewBox.w <= 0.f || m_viewBox.h <= 0.f || target.w <= 0.f || target.h <= 0.f) return;
    f32 sx = target.w / m_viewBox.w, sy = target.h / m_viewBox.h;
    f32 ox = target.x, oy = target.y;
    if (!m_stretch) {
        const f32 s = std::min(sx, sy);
        ox += (target.w - m_viewBox.w * s) * 0.5f;
        oy += (target.h - m_viewBox.h * s) * 0.5f;
        sx = sy = s;
    }
    const f32 sw = std::sqrt(sx * sy);
    const Affine T{sx, 0, 0, sy, ox - m_viewBox.x * sx, oy - m_viewBox.y * sy};
    int chain[32];
    for (const Shape& s : m_shapes) {
        int depth = 0; // clip chain, outermost first
        for (int k = s.clip; k >= 0 && depth < 32; k = m_clips[std::size_t(k)].parent) chain[depth++] = k;
        for (int i = depth - 1; i >= 0; --i) {
            const ClipRegion& r = m_clips[std::size_t(chain[i])];
            if (!r.layers.empty()) {
                std::vector<ClipLayer> ls = r.layers;
                for (ClipLayer& l : ls) {
                    l.path.transform(T);
                    fuse::ui::Gradient g;
                    g.toUnit = l.toUnit;
                    g.transform(T);
                    l.toUnit = g.toUnit;
                }
                dl.pushClipLayers(ls);
                continue;
            }
            Path cp = r.path;
            cp.transform(T);
            dl.pushClipPath(cp, r.evenOdd);
        }
        if (s.pattern >= 0 && std::size_t(s.pattern) < m_patterns.size()) {
            const PatternFill& pf = m_patterns[std::size_t(s.pattern)];
            Path sp = s.path;
            sp.transform(T);
            dl.pushClipPath(sp, s.evenOdd);
            // tiles covering the shape: its document bounds mapped into pattern space
            Affine inv;
            const Rect b = s.path.bounds();
            if (invert(pf.toDoc, inv)) {
                f32 x0 = 3e38f, y0 = 3e38f, x1 = -3e38f, y1 = -3e38f;
                for (const Vec2 c : {Vec2{b.x, b.y}, Vec2{b.right(), b.y}, Vec2{b.x, b.bottom()}, Vec2{b.right(), b.bottom()}}) {
                    const f32 u = inv[0] * c.x + inv[2] * c.y + inv[4], v = inv[1] * c.x + inv[3] * c.y + inv[5];
                    x0 = std::min(x0, u), y0 = std::min(y0, v), x1 = std::max(x1, u), y1 = std::max(y1, v);
                }
                const int i0 = int(std::floor((x0 - pf.tile.x) / pf.tile.w)), i1 = int(std::floor((x1 - pf.tile.x) / pf.tile.w));
                const int j0 = int(std::floor((y0 - pf.tile.y) / pf.tile.h)), j1 = int(std::floor((y1 - pf.tile.y) / pf.tile.h));
                if (i1 >= i0 && j1 >= j0 && i64(i1 - i0 + 1) * i64(j1 - j0 + 1) <= 4096) {
                    const Affine TD = mul(T, pf.toDoc);
                    const f32 op = s.fill.a * opacity;
                    for (int j = j0; j <= j1; ++j)
                        for (int i = i0; i <= i1; ++i) {
                            const f32 tx = pf.tile.x + f32(i) * pf.tile.w, ty = pf.tile.y + f32(j) * pf.tile.h;
                            Path tile;
                            tile.rect({tx, ty, pf.tile.w, pf.tile.h});
                            tile.transform(TD);
                            dl.pushClipPath(tile); // overflow: hidden
                            const Affine M = mul(TD, mul(Affine{1, 0, 0, 1, tx, ty}, pf.content));
                            const f32 csw = std::sqrt(std::abs(M[0] * M[3] - M[1] * M[2]));
                            for (const Shape& c : pf.shapes) drawShape(dl, c, M, csw, op);
                            dl.popClip();
                        }
                }
            }
            dl.popClip();
            if (s.stroke.a > 0.f && s.strokeWidth > 0.f) {
                Shape st = s;
                st.fill = {};
                drawShape(dl, st, T, sw, opacity);
            }
        } else {
            drawShape(dl, s, T, sw, opacity);
        }
        for (int i = 0; i < depth; ++i) dl.popClip();
    }
}

} // namespace fuse::ui::svg

// ---- QSvgWidget -----------------------------------------------------------------------------------------------------
namespace fuse::ui::widgets {
namespace {
struct SvgSlot {
    NodeHandle node;
    std::unique_ptr<svg::Document> doc;
};
std::vector<SvgSlot>& svgSlots() { static std::vector<SvgSlot> v; return v; }
svg::Document* docOf(const UiContext& ui, NodeHandle n) {
    if (!ui.alive(n)) return nullptr;
    for (auto& s : svgSlots()) if (s.node == n) return s.doc.get();
    return nullptr;
}
struct SvgW {
    static constexpr std::string_view kName = "svgwidget";
    static constexpr A11yRole kRole = A11yRole::Image;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const svg::Document* d = docOf(ui, n);
        return d && d->valid() ? d->size() : Size{0.f, 0.f};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        if (const svg::Document* d = docOf(pc.ui, pc.node)) d->render(dl, pc.content, pc.opacity);
    }
};
} // namespace

NodeHandle svgWidget(UiContext& ui, NodeHandle parent, std::string_view svgText, std::string_view id) {
    WidgetRegistry& r = ui.registry();
    if (r.find("svgwidget") == 0xFFFFFFFFu) r.registerWidget<SvgW>();
    auto h = ui.create("svgwidget", parent, id);
    if (!h) return {};
    auto& slots = svgSlots();
    std::erase_if(slots, [&](const SvgSlot& s) { return !ui.alive(s.node); });
    slots.push_back({*h, std::make_unique<svg::Document>()});
    svgWidgetLoad(ui, *h, svgText);
    return *h;
}

bool svgWidgetLoad(UiContext& ui, NodeHandle w, std::string_view svgText) {
    svg::Document* d = docOf(ui, w);
    if (!d) return false;
    const bool ok = d->load(svgText);
    ui.invalidate(w);
    ui.setA11y(w, A11yRole::Image, "SVG image");
    return ok;
}

const svg::Document* svgWidgetDocument(const UiContext& ui, NodeHandle w) { return docOf(ui, w); }

} // namespace fuse::ui::widgets
