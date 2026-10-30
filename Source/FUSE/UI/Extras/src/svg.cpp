// SVG document parsing + rendering onto DrawList paths. See svg.hpp.
#include "fuse/ui/extras/svg.hpp"

#include "fuse/ui/os.hpp"
#include "fuse/ui/style.hpp"

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
        const auto r = std::from_chars(b, s.data() + s.size(), v);
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
struct Gradient {
    Color from, to;
    GradientKind kind = kGradientVertical;
};
struct StyleState {
    Paint fill{}, stroke{.none = true};
    f32 strokeWidth = 1.f, opacity = 1.f, fillOpacity = 1.f, strokeOpacity = 1.f;
    bool evenOdd = false, hidden = false;
    Color currentColor{0, 0, 0, 1};
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
    else if (k == "opacity") { if (auto l = length(v)) st.opacity *= std::clamp(*l, 0.f, 1.f); }
    else if (k == "fill-opacity") { if (auto l = length(v)) st.fillOpacity = std::clamp(*l, 0.f, 1.f); }
    else if (k == "stroke-opacity") { if (auto l = length(v)) st.strokeOpacity = std::clamp(*l, 0.f, 1.f); }
    else if (k == "display") st.hidden = st.hidden || v == "none";
    else if (k == "visibility") st.hidden = v == "hidden" || v == "collapse";
}

void applyStyle(StyleState& st, const Element& e) {
    for (const char* k : {"color", "fill", "stroke", "stroke-width", "fill-rule", "opacity", "fill-opacity", "stroke-opacity", "display", "visibility"})
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

struct Builder {
    std::vector<Shape>& shapes;
    std::vector<std::pair<std::string, std::pair<std::size_t, std::size_t>>> ids;
    std::map<std::string, const Element*, std::less<>> byId;
    std::map<std::string, Gradient, std::less<>> gradients;
    int useDepth = 0;

    void index(const Element& e) {
        if (const auto id = e.attr("id"); !id.empty()) byId.emplace(std::string(id), &e);
        for (auto& c : e.children) index(*c);
    }
    void collectGradients() {
        for (auto& [id, e] : byId) {
            const std::string_view n = localName(e->name);
            if (n != "linearGradient" && n != "radialGradient") continue;
            const Element* stops = e;
            for (int hops = 0; hops < 4 && stops->children.empty(); ++hops) { // xlink:href inherits stops
                std::string_view h = stops->has("href") ? stops->attr("href") : stops->attr("xlink:href");
                if (h.starts_with("#")) h.remove_prefix(1);
                auto it = byId.find(h);
                if (it == byId.end()) break;
                stops = it->second;
            }
            std::vector<Color> cols;
            for (auto& s : stops->children) {
                if (localName(s->name) != "stop") continue;
                StyleState tmp;
                Color c{0, 0, 0, 1};
                f32 op = 1.f;
                std::string_view sc = s->attr("stop-color");
                std::string_view so = s->attr("stop-opacity");
                std::string_view css = s->attr("style");
                while (!css.empty()) {
                    const auto semi = css.find(';');
                    const std::string_view decl = css.substr(0, semi);
                    if (const auto colon = decl.find(':'); colon != std::string_view::npos) {
                        const auto k = trim(decl.substr(0, colon));
                        if (k == "stop-color") sc = trim(decl.substr(colon + 1));
                        else if (k == "stop-opacity") so = trim(decl.substr(colon + 1));
                    }
                    if (semi == std::string_view::npos) break;
                    css.remove_prefix(semi + 1);
                }
                if (!sc.empty()) parseColor(sc, c);
                if (auto o = length(so)) op = *o;
                cols.push_back(c.withAlpha(c.a * op));
            }
            if (cols.empty()) continue;
            Gradient g{cols.front(), cols.back(), kGradientVertical};
            if (n == "radialGradient") g.kind = kGradientRadial;
            else {
                const f32 x1 = length(e->attr("x1")).value_or(0.f), x2 = e->has("x2") ? length(e->attr("x2")).value_or(1.f) : 1.f;
                const f32 y1 = length(e->attr("y1")).value_or(0.f), y2 = length(e->attr("y2")).value_or(0.f);
                const bool pct = e->attr("x2").ends_with("%");
                const f32 dx = (pct ? (x2 - x1) / 100.f : x2 - x1), dy = (e->attr("y2").ends_with("%") ? (y2 - y1) / 100.f : y2 - y1);
                g.kind = std::abs(dx) >= std::abs(dy) ? kGradientHorizontal : kGradientVertical;
                if ((g.kind == kGradientHorizontal && dx < 0) || (g.kind == kGradientVertical && dy < 0)) std::swap(g.from, g.to);
            }
            gradients.emplace(id, g);
        }
    }

    void emit(Path p, const StyleState& st, const Affine& m, std::string_view id) {
        if (st.hidden || p.empty()) return;
        p.transform(m);
        Shape s;
        s.path = std::move(p);
        s.evenOdd = st.evenOdd;
        s.id = std::string(id);
        auto resolve = [&](const Paint& pt, f32 op, Color& c, bool fill) {
            if (pt.none) return;
            if (!pt.ref.empty()) {
                auto it = gradients.find(pt.ref);
                if (it == gradients.end()) return;
                if (fill) {
                    s.gradient = it->second.kind;
                    c = it->second.from.withAlpha(it->second.from.a * op * st.opacity);
                    s.fill2 = it->second.to.withAlpha(it->second.to.a * op * st.opacity);
                } else {
                    const Color mid{(it->second.from.r + it->second.to.r) * 0.5f, (it->second.from.g + it->second.to.g) * 0.5f,
                                    (it->second.from.b + it->second.to.b) * 0.5f, (it->second.from.a + it->second.to.a) * 0.5f};
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

    void walk(const Element& e, StyleState st, Affine m) {
        const std::string_view n = localName(e.name);
        if (n == "defs" || n == "linearGradient" || n == "radialGradient" || n == "clipPath" || n == "mask" || n == "symbol" ||
            n == "title" || n == "desc" || n == "metadata" || n == "style" || n == "pattern" || n == "marker")
            return;
        applyStyle(st, e);
        if (st.hidden) return;
        if (e.has("transform")) m = mul(m, parseTransform(e.attr("transform")));
        const std::size_t first = shapes.size();
        auto L = [&](std::string_view k, f32 d = 0.f) { return length(e.attr(k)).value_or(d); };
        const std::string_view id = e.attr("id");
        Path p;
        if (n == "svg" || n == "g" || n == "a" || n == "switch") {
            for (auto& c : e.children) walk(*c, st, m);
        } else if (n == "use" && useDepth < 8) {
            std::string_view h = e.has("href") ? e.attr("href") : e.attr("xlink:href");
            if (h.starts_with("#")) h.remove_prefix(1);
            if (auto it = byId.find(h); it != byId.end()) {
                ++useDepth;
                const Affine tm = mul(m, Affine{1, 0, 0, 1, L("x"), L("y")});
                const Element& ref = *it->second;
                if (localName(ref.name) == "symbol") for (auto& c : ref.children) walk(*c, st, tm);
                else walk(ref, st, tm);
                --useDepth;
            }
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
    Builder b{m_shapes, {}, {}, {}, 0};
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
    for (const Shape& s : m_shapes) {
        Path p = s.path;
        p.transform(Vec2{sx, sy}, Vec2{ox - m_viewBox.x * sx, oy - m_viewBox.y * sy});
        const Color fill = s.fill.withAlpha(s.fill.a * opacity), stroke = s.stroke.withAlpha(s.stroke.a * opacity);
        if (fill.a > 0.f) {
            if (s.gradient != kGradientNone) dl.gradientPath(p, fill, s.fill2.withAlpha(s.fill2.a * opacity), s.gradient, s.evenOdd);
            else dl.path(p, fill, s.evenOdd);
        }
        if (stroke.a > 0.f && s.strokeWidth > 0.f) dl.path(p, Color{}, s.evenOdd, stroke, s.strokeWidth * sw);
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
