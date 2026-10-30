// SVG <filter> -> FilterProgram (see draw_list.hpp). A small tag scanner (attributes only, no entities beyond the
// basics) is enough for filter primitive markup.
#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/style.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
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
    Color color(const Tag& t) {
        Color c{0.f, 0.f, 0.f, 1.f};
        const std::string_view cs = prop(t, "flood-color");
        if (!cs.empty() && !parseColor(cs, c)) ok = false;
        const std::string_view os = prop(t, "flood-opacity");
        if (!os.empty()) {
            f32 o = number(os, 1.f);
            if (os.back() == '%') o /= 100.f;
            c.a *= std::clamp(o, 0.f, 1.f);
        }
        return c;
    }
};
} // namespace

int FilterProgram::reach() const {
    int r = 1;
    for (const FilterOpDesc& d : ops) {
        if (d.op == FeOp::Blur) {
            const auto rad = [](f32 s) { return s > 0.f ? std::max(1, int(std::floor(s + 0.5f))) : 0; };
            r += 3 * std::max(rad(d.p[0]), rad(d.p[1]));
        } else if (d.op == FeOp::Offset) {
            r += int(std::max(std::abs(std::lround(d.p[0])), std::abs(std::lround(d.p[1]))));
        }
    }
    return r;
}

std::vector<std::string> svgFilterIds(std::string_view text) {
    std::vector<std::string> ids;
    std::size_t i = 0;
    Tag t;
    while (nextTag(text, i, t))
        if (!t.close && t.name == "filter" && t.has("id")) ids.emplace_back(trimSv(t.attr("id")));
    return ids;
}

bool parseSvgFilter(std::string_view text, FilterProgram& out, std::string_view id) {
    out = {};
    std::size_t i = 0;
    Tag t;
    bool found = false;
    while (nextTag(text, i, t))
        if (!t.close && t.name == "filter" && (id.empty() || trimSv(t.attr("id")) == id)) { found = true; break; }
    if (!found || t.selfClose) return false;
    Compiler c{out, {}, 0, -1, true};
    std::vector<f32> nums;
    while (c.ok && nextTag(text, i, t)) {
        if (t.close) {
            if (t.name == "filter") break;
            continue;
        }
        const std::string& n = t.name;
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
            const Tag merge = t;
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
            c.result(merge, o);
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
        } else if (n.size() > 2 && n.compare(0, 2, "fe") == 0) {
            return false; // other primitives (feTurbulence, lighting, ...) are not supported
        }
    }
    if (!c.ok || out.ops.empty()) return false;
    c.emit(FeOp::Copy, c.last, 0, 0); // the result to plane 0
    return true;
}
} // namespace fuse::ui
