// .fuseicons packs: IconSet (load, lookup, draw) and encodeIconPack (see fuse/ui/icon_set.hpp for the format).
#include "fuse/ui/icon_set.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fuse::ui {

// What the pack code may touch of an SvgImage (its parsed shapes; svg.hpp declares this struct a friend).
struct SvgCookAccess {
    using Paint = SvgImage::Paint;
    using Shape = SvgImage::Shape;
    using GradientDef = SvgImage::GradientDef;
    static Size& size(SvgImage& i) { return i.m_size; }
    static Rect& viewBox(SvgImage& i) { return i.m_viewBox; }
    static std::vector<Shape>& shapes(SvgImage& i) { return i.m_shapes; }
    static std::vector<GradientDef>& gradients(SvgImage& i) { return i.m_gradients; }
    static const std::vector<Shape>& shapes(const SvgImage& i) { return i.m_shapes; }
    static const std::vector<GradientDef>& gradients(const SvgImage& i) { return i.m_gradients; }
};

namespace {
using Paint = SvgCookAccess::Paint;
using Shape = SvgCookAccess::Shape;
using GradientDef = SvgCookAccess::GradientDef;

constexpr u32 kMaxIcons = 65536, kMaxShapes = 4096, kMaxGradients = 256, kMaxVerbs = 1u << 20, kMaxStops = 256;

struct Writer {
    std::vector<u8>& out;
    void bytes(const void* p, std::size_t n) {
        const u8* b = static_cast<const u8*>(p);
        out.insert(out.end(), b, b + n);
    }
    void u8_(u8 v) { out.push_back(v); }
    void u16_(u16 v) { out.push_back(u8(v)); out.push_back(u8(v >> 8)); }
    void u32_(u32 v) { for (int k = 0; k < 4; ++k) out.push_back(u8(v >> (8 * k))); }
    void f32_(f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, 4);
        u32_(bits);
    }
    void affine(const Affine& a) { f32_(a.a); f32_(a.b); f32_(a.c); f32_(a.d); f32_(a.tx); f32_(a.ty); }
    void color(Color c) { f32_(c.r); f32_(c.g); f32_(c.b); f32_(c.a); }
    void paint(const Paint& p) {
        u8_(u8(p.kind));
        color(p.color);
        u32_(p.gradient);
        f32_(p.opacity);
    }
};

struct Reader {
    std::span<const u8> in;
    std::size_t at = 0;
    bool ok = true;
    bool need(std::size_t n) {
        if (!ok || n > in.size() - at) ok = false;
        return ok;
    }
    u8 u8_() { return need(1) ? in[at++] : u8(0); }
    u16 u16_() {
        if (!need(2)) return 0;
        const u16 v = u16(in[at] | (in[at + 1] << 8));
        at += 2;
        return v;
    }
    u32 u32_() {
        if (!need(4)) return 0;
        u32 v = 0;
        for (int k = 0; k < 4; ++k) v |= u32(in[at + std::size_t(k)]) << (8 * k);
        at += 4;
        return v;
    }
    f32 f32_() { // non-finite numbers make the pack invalid
        const u32 bits = u32_();
        f32 v;
        std::memcpy(&v, &bits, 4);
        if (!std::isfinite(v)) ok = false;
        return ok ? v : 0.f;
    }
    Affine affine() {
        Affine a;
        a.a = f32_(); a.b = f32_(); a.c = f32_(); a.d = f32_(); a.tx = f32_(); a.ty = f32_();
        return a;
    }
    Color color() {
        Color c;
        c.r = f32_(); c.g = f32_(); c.b = f32_(); c.a = f32_();
        return c;
    }
    Paint paint(u32 gradientCount) {
        Paint p;
        const u8 k = u8_();
        if (k > u8(Paint::Kind::Gradient)) ok = false;
        p.kind = Paint::Kind(ok ? k : 0);
        p.color = color();
        p.gradient = u32_();
        p.opacity = f32_();
        if (p.kind == Paint::Kind::Gradient && p.gradient >= gradientCount) ok = false;
        return p;
    }
};

bool validName(std::string_view n) {
    if (n.empty() || n.size() > 128) return false;
    for (const char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    return true;
}

bool isBlack(const Paint& p) {
    return p.kind == Paint::Kind::Solid && p.color.r == 0.f && p.color.g == 0.f && p.color.b == 0.f;
}
// An icon is tintable when every paint is currentColor or black (the SVG default fill).
bool monochrome(const SvgImage& img) {
    for (const Shape& s : SvgCookAccess::shapes(img))
        for (const Paint* p : {&s.fill, &s.stroke})
            if (p->kind == Paint::Kind::Gradient || (p->kind == Paint::Kind::Solid && !isBlack(*p))) return false;
    return true;
}
} // namespace

std::expected<std::vector<u8>, std::string> encodeIconPack(std::vector<IconPackSource> icons, IconPackStats* stats) {
    if (icons.size() > kMaxIcons) return std::unexpected("too many icons");
    std::sort(icons.begin(), icons.end(), [](const IconPackSource& a, const IconPackSource& b) { return a.name < b.name; });
    for (std::size_t i = 0; i < icons.size(); ++i) {
        if (!validName(icons[i].name)) return std::unexpected("icon name '" + icons[i].name + "': use [a-z0-9_.-]");
        if (i && icons[i].name == icons[i - 1].name) return std::unexpected("duplicate icon '" + icons[i].name + "'");
        if (icons[i].image.empty() || icons[i].image.viewBox().empty())
            return std::unexpected("icon '" + icons[i].name + "': no painted shapes or no size");
        if (icons[i].image.shapeCount() > kMaxShapes || icons[i].image.gradientCount() > kMaxGradients)
            return std::unexpected("icon '" + icons[i].name + "': too complex");
    }
    IconPackStats st;
    std::vector<u8> out;
    Writer w{out};
    w.bytes(kIconPackMagic, 4);
    w.u32_(kIconPackVersion);
    w.u32_(u32(icons.size()));
    for (IconPackSource& ic : icons) {
        const bool mono = monochrome(ic.image);
        if (mono) // black paints follow the text colour like currentColor ones (alpha kept through the opacity)
            for (Shape& s : SvgCookAccess::shapes(ic.image))
                for (Paint* p : {&s.fill, &s.stroke})
                    if (isBlack(*p)) {
                        p->opacity *= p->color.a;
                        p->color = {};
                        p->kind = Paint::Kind::Current;
                    }
        w.u32_(hashId(ic.name));
        w.u16_(u16(ic.name.size()));
        w.bytes(ic.name.data(), ic.name.size());
        w.u8_(mono ? 0 : 1);
        w.f32_(ic.image.size().w);
        w.f32_(ic.image.size().h);
        const Rect vb = ic.image.viewBox();
        w.f32_(vb.x); w.f32_(vb.y); w.f32_(vb.w); w.f32_(vb.h);
        const auto& grads = SvgCookAccess::gradients(ic.image);
        w.u32_(u32(grads.size()));
        for (const GradientDef& g : grads) {
            w.u8_(u8(g.kind));
            w.u8_(u8(g.spread));
            w.u8_(g.bboxUnits ? 1 : 0);
            w.affine(g.xf);
            w.f32_(g.x1); w.f32_(g.y1); w.f32_(g.x2); w.f32_(g.y2); w.f32_(g.cx); w.f32_(g.cy); w.f32_(g.r);
            const std::size_t stops = std::min<std::size_t>(g.stops.size(), kMaxStops);
            w.u16_(u16(stops));
            for (std::size_t k = 0; k < stops; ++k) {
                w.f32_(g.stops[k].offset);
                w.color(g.stops[k].color);
            }
        }
        const auto& shapes = SvgCookAccess::shapes(ic.image);
        w.u32_(u32(shapes.size()));
        for (const Shape& s : shapes) {
            w.affine(s.xf);
            w.paint(s.fill);
            w.paint(s.stroke);
            w.f32_(s.strokeStyle.width);
            w.u8_(u8(s.strokeStyle.cap));
            w.u8_(u8(s.strokeStyle.join));
            w.f32_(s.strokeStyle.miterLimit);
            w.f32_(s.strokeStyle.dashOffset);
            w.u8_(s.strokeStyle.dashCount);
            for (u8 k = 0; k < s.strokeStyle.dashCount; ++k) w.f32_(s.strokeStyle.dashes[k]);
            w.u8_(u8(s.rule));
            const auto verbs = s.path.verbs();
            const auto pts = s.path.points();
            w.u32_(u32(verbs.size()));
            for (const PathVerb v : verbs) w.u8_(u8(v));
            w.u32_(u32(pts.size()));
            for (const Vec2 p : pts) {
                w.f32_(p.x);
                w.f32_(p.y);
            }
        }
        ++st.icons;
        st.multiColor += mono ? 0u : 1u;
        st.shapes += u32(shapes.size());
    }
    st.bytes = out.size();
    if (stats) *stats = st;
    return out;
}

std::expected<IconSet, std::string> IconSet::fromBytes(std::span<const u8> bytes) {
    Reader r{bytes};
    if (bytes.size() < 12 || std::memcmp(bytes.data(), kIconPackMagic, 4) != 0) return std::unexpected("not a .fuseicons pack");
    r.at = 4;
    const u32 version = r.u32_();
    if (version != kIconPackVersion)
        return std::unexpected("icon pack version " + std::to_string(version) + " (this build reads " +
                               std::to_string(kIconPackVersion) + ": recook the SVG sources with fuse_ui_icon_cook)");
    const u32 count = r.u32_();
    if (count > kMaxIcons) return std::unexpected("icon pack: bad icon count");
    IconSet set;
    set.m_names.reserve(std::min<u32>(count, 4096));
    for (u32 i = 0; i < count && r.ok; ++i) {
        const u32 hash = r.u32_();
        const u16 nameLen = r.u16_();
        if (!r.need(nameLen)) break;
        std::string name(reinterpret_cast<const char*>(bytes.data() + r.at), nameLen);
        r.at += nameLen;
        if (!validName(name) || hash != hashId(name) || (i && !(set.m_names.back() < name))) {
            r.ok = false;
            break;
        }
        const u8 flags = r.u8_();
        SvgImage img;
        SvgCookAccess::size(img) = {r.f32_(), r.f32_()};
        Rect vb;
        vb.x = r.f32_(); vb.y = r.f32_(); vb.w = r.f32_(); vb.h = r.f32_();
        if (!(vb.w > 0.f) || !(vb.h > 0.f)) r.ok = false;
        SvgCookAccess::viewBox(img) = vb;
        const u32 gradCount = r.u32_();
        if (gradCount > kMaxGradients) r.ok = false;
        auto& grads = SvgCookAccess::gradients(img);
        for (u32 g = 0; g < gradCount && r.ok; ++g) {
            GradientDef d;
            const u8 kind = r.u8_(), spread = r.u8_();
            if (kind > u8(GradientKind::Radial) || spread > u8(GradientSpread::Reflect)) r.ok = false;
            d.kind = GradientKind(r.ok ? kind : 0);
            d.spread = GradientSpread(r.ok ? spread : 0);
            d.bboxUnits = r.u8_() != 0;
            d.xf = r.affine();
            d.x1 = r.f32_(); d.y1 = r.f32_(); d.x2 = r.f32_(); d.y2 = r.f32_(); d.cx = r.f32_(); d.cy = r.f32_(); d.r = r.f32_();
            const u16 stops = r.u16_();
            if (stops == 0 || stops > kMaxStops || !r.need(std::size_t(stops) * 20)) r.ok = false;
            for (u16 k = 0; k < stops && r.ok; ++k) {
                GradientStop s;
                s.offset = r.f32_();
                s.color = r.color();
                d.stops.push_back(s);
            }
            grads.push_back(std::move(d));
        }
        const u32 shapeCount = r.u32_();
        if (shapeCount == 0 || shapeCount > kMaxShapes) r.ok = false;
        auto& shapes = SvgCookAccess::shapes(img);
        for (u32 k = 0; k < shapeCount && r.ok; ++k) {
            Shape s;
            s.xf = r.affine();
            s.fill = r.paint(gradCount);
            s.stroke = r.paint(gradCount);
            s.strokeStyle.width = r.f32_();
            const u8 cap = r.u8_(), join = r.u8_();
            if (cap > u8(LineCap::Square) || join > u8(LineJoin::Bevel)) r.ok = false;
            s.strokeStyle.cap = LineCap(r.ok ? cap : 0);
            s.strokeStyle.join = LineJoin(r.ok ? join : 0);
            s.strokeStyle.miterLimit = r.f32_();
            s.strokeStyle.dashOffset = r.f32_();
            const u8 dashes = r.u8_();
            if (dashes > StrokeStyle::kMaxDashes) r.ok = false;
            s.strokeStyle.dashCount = r.ok ? dashes : u8(0);
            for (u8 d = 0; d < s.strokeStyle.dashCount; ++d) s.strokeStyle.dashes[d] = r.f32_();
            const u8 rule = r.u8_();
            if (rule > u8(FillRule::EvenOdd)) r.ok = false;
            s.rule = FillRule(r.ok ? rule : 0);
            const u32 verbCount = r.u32_();
            if (verbCount > kMaxVerbs || !r.need(verbCount)) r.ok = false;
            const std::size_t verbsAt = r.at;
            if (r.ok) r.at += verbCount;
            const u32 pointCount = r.u32_();
            if (!r.ok || pointCount > kMaxVerbs * 3 || !r.need(std::size_t(pointCount) * 8)) {
                r.ok = false;
                break;
            }
            // replay the verbs: the path comes out with the verbs and points the cooker saw
            u32 used = 0;
            auto pt = [&] {
                Vec2 p;
                p.x = r.f32_();
                p.y = r.f32_();
                ++used;
                return p;
            };
            for (u32 v = 0; v < verbCount && r.ok; ++v) {
                const u8 verb = bytes[verbsAt + v];
                const u32 needs = verb == u8(PathVerb::Move) || verb == u8(PathVerb::Line) ? 1u
                                  : verb == u8(PathVerb::Quad) ? 2u : verb == u8(PathVerb::Cubic) ? 3u : 0u;
                if (verb > u8(PathVerb::Close) || used + needs > pointCount || (v == 0 && verb != u8(PathVerb::Move))) {
                    r.ok = false;
                    break;
                }
                switch (PathVerb(verb)) {
                case PathVerb::Move: s.path.moveTo(pt()); break;
                case PathVerb::Line: s.path.lineTo(pt()); break;
                case PathVerb::Quad: { const Vec2 c = pt(); s.path.quadTo(c, pt()); break; }
                case PathVerb::Cubic: { const Vec2 c1 = pt(), c2 = pt(); s.path.cubicTo(c1, c2, pt()); break; }
                case PathVerb::Close: s.path.close(); break;
                }
            }
            if (used != pointCount || s.path.verbs().size() != verbCount) r.ok = false;
            shapes.push_back(std::move(s));
        }
        if (!r.ok) break;
        set.m_names.push_back(std::move(name));
        set.m_hashes.push_back(hash);
        set.m_multiColor.push_back(u8(flags & 1u));
        set.m_images.push_back(std::move(img));
    }
    if (!r.ok || set.m_names.size() != count || r.at != bytes.size()) return std::unexpected("icon pack: truncated or corrupt");
    set.m_byHash.resize(count);
    for (u32 i = 0; i < count; ++i) set.m_byHash[i] = i;
    std::sort(set.m_byHash.begin(), set.m_byHash.end(), [&](u32 a, u32 b) { return set.m_hashes[a] < set.m_hashes[b]; });
    return set;
}

std::expected<IconSet, std::string> IconSet::fromFile(const std::string& path) {
    namespace fs = std::filesystem;
    std::ifstream f(fs::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())),
                    std::ios::binary | std::ios::ate);
    if (!f) return std::unexpected(path + ": cannot open");
    const std::streamoff n = f.tellg();
    if (n <= 0 || n > (std::streamoff(1) << 30)) return std::unexpected(path + ": empty or too large");
    std::vector<u8> bytes(static_cast<std::size_t>(n));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), n)) return std::unexpected(path + ": read failed");
    auto set = fromBytes(bytes);
    if (!set) return std::unexpected(path + ": " + set.error());
    return set;
}

i32 IconSet::find(std::string_view name) const {
    const auto it = std::lower_bound(m_names.begin(), m_names.end(), name, [](const std::string& a, std::string_view b) { return a < b; });
    return it != m_names.end() && *it == name ? i32(it - m_names.begin()) : -1;
}

i32 IconSet::findHash(u32 nameHash) const {
    const auto it = std::lower_bound(m_byHash.begin(), m_byHash.end(), nameHash, [&](u32 i, u32 h) { return m_hashes[i] < h; });
    return it != m_byHash.end() && m_hashes[*it] == nameHash ? i32(*it) : -1;
}

void IconSet::draw(Painter& p, u32 icon, const Rect& box, Color color) const {
    if (icon < m_images.size()) m_images[icon].render(p, box, color);
}

} // namespace fuse::ui
