// Gate fuse_ui_painter: the Painter (QPainter equivalent, plan §4.9) recording into a DrawList.
//   * state stack: save / restore bring back transform, opacity, brush, pen and fill rule and close the clips and
//     layers opened since; the destructor closes what is left; nesting beyond kMaxDepth is harmless
//   * transforms: translate / scale / rotate / general affine, applied in local coordinates, to paths, rects, lines
//   * native ops where they are exact (axis-aligned rects, round-capped lines), paths otherwise
//   * strokes under a non-uniform scale are outlined in user space (a circle's stroke becomes an ellipse band)
//   * clip to rect / rounded rect / path under transforms, opacity (per primitive) and layers (group)
//   * drawText through the context (translation and scale; upright under rotation), drawImage / drawNinePatch
//     (rejected under rotation), a widget painting with a Painter inside the paint pass at render scale 1 and 2
#include "fuse/ui/painter.hpp"
#include "test_util.hpp"

#include <cmath>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
constexpr f32 kPi = 3.14159265358979f;
u8 ch(u32 p, int c) { return u8((p >> (8 * c)) & 0xFF); }
const Color kWhite = Color::rgb(0xFFFFFF);

Image render(const DrawList& dl, int w, int h, u32 clear = 0, const Image* img0 = nullptr, UiContext* ui = nullptr) {
    Image img;
    img.resize(w, h, clear);
    CpuRasterizer r;
    if (img0) r.addImage(img0);
    if (ui) r.setGlyphAtlas(ui->atlas().pixels(), ui->atlas().size());
    r.render(dl, img);
    return img;
}
double area(const Image& img) {
    double a = 0.0;
    for (u32 p : img.pixels) a += double(p >> 24) / 255.0;
    return a;
}
bool nearRel(double v, double want, double rel) { return std::abs(v - want) <= rel * std::abs(want); }
int count(const DrawList& dl, DrawOp op) {
    int n = 0;
    for (const DrawCmd& c : dl.cmds()) n += c.op == op;
    return n;
}
int pushes(const DrawList& dl) { return count(dl, DrawOp::PushClip) + count(dl, DrawOp::PushClipPath) + count(dl, DrawOp::PushLayer); }

// A custom widget that paints with a Painter (what the plan's "used by custom widgets, charts" means).
struct Gauge {
    static constexpr std::string_view kName = "test-gauge";
    static Size measure(UiContext&, NodeHandle, Size) { return {60.f, 60.f}; }
    static void paint(const PaintContext& pc, DrawList& dl) {
        Painter p(pc, dl);
        p.translate(pc.content.x + pc.content.w * 0.5f, pc.content.y + pc.content.h * 0.5f);
        StrokeStyle st;
        st.width = 6.f;
        st.cap = LineCap::Round;
        p.setPen(Color::rgb(0x3D8BFD), st);
        p.drawArc({0, 0}, 24.f, 24.f, kPi * 0.75f, kPi * 1.5f * pc.ui.value(pc.node));
        p.setNoPen();
        p.setBrush(Color::rgb(0xFFFFFF));
        p.drawCircle({0, 0}, 5.f);
    }
};
} // namespace

int main() {
    // ---- state stack ----
    {
        DrawList dl;
        {
            Painter p(dl);
            UI_CHECK_EQ(p.depth(), 0);
            p.translate(10, 20);
            p.setOpacity(0.5f);
            p.setBrush(Color::rgb(0xFF0000));
            p.setPen(Color::rgb(0x00FF00), 3.f);
            p.save();
            p.scale(2.f);
            p.rotate(0.5f);
            p.setOpacity(0.25f);
            p.setBrush(Color::rgb(0x0000FF));
            p.setNoPen();
            p.clipRect({0, 0, 5, 5});
            Path tri;
            tri.moveTo({0, 0}).lineTo({4, 0}).lineTo({0, 4}).close();
            p.clipPath(tri);
            p.beginLayer(0.5f);
            UI_CHECK_EQ(p.depth(), 1);
            UI_CHECK_EQ(pushes(dl), 3);
            UI_CHECK_EQ(count(dl, DrawOp::PopClip), 0);
            p.restore(); // closes the three groups, restores the state
            UI_CHECK_EQ(count(dl, DrawOp::PopClip), 3);
            UI_CHECK_EQ(p.depth(), 0);
            UI_CHECK(p.worldTransform() == Affine::translation(10, 20));
            UI_CHECK(p.opacity() == 0.5f);
            p.drawRect({0, 0, 4, 4}); // red at half opacity + green pen: the saved brush / pen are back
            p.restore();              // unbalanced restore: ignored
            UI_CHECK_EQ(p.depth(), 0);
            p.clipRect({0, 0, 100, 100}); // left open: the destructor closes it
            p.save();
            p.clipRect({0, 0, 50, 50}); // and this one, inside an unrestored save
        }
        UI_CHECK_EQ(pushes(dl), count(dl, DrawOp::PopClip));
        const DrawCmd* rect = nullptr;
        for (const DrawCmd& c : dl.cmds())
            if (c.op == DrawOp::Rect) rect = &c;
        UI_CHECK(rect && rect->fill == Color::rgb(0xFF0000, 0.5f));
        if (rect) UI_CHECK_RECT(rect->rect, 10, 20, 4, 4);
        UI_CHECK_EQ(count(dl, DrawOp::Path), 1); // the pen's outline
        // deep nesting: saves beyond kMaxDepth do not corrupt anything and their restores are no-ops
        DrawList deep;
        Painter p(deep);
        for (int i = 0; i < 40; ++i) {
            p.save();
            p.translate(1, 0);
        }
        UI_CHECK_EQ(p.depth(), Painter::kMaxDepth - 1);
        for (int i = 0; i < 40; ++i) p.restore();
        UI_CHECK_EQ(p.depth(), 0);
        UI_CHECK(p.worldTransform().isIdentity());
    }
    // ---- transforms ----
    {
        DrawList dl;
        Painter p(dl);
        p.setBrush(kWhite);
        p.translate(20, 10);
        p.scale(2.f, 3.f);
        p.drawRect({1, 1, 4, 2}); // local coordinates: translate first, then scale
        UI_CHECK_EQ(dl.size(), std::size_t(1));
        UI_CHECK(dl.cmds()[0].op == DrawOp::Rect); // axis-aligned: the native op
        UI_CHECK_RECT(dl.cmds()[0].rect, 22, 13, 8, 6);
        p.setTransform({});
        p.translate(50, 50);
        p.rotate(kPi / 2.f);
        p.drawRect({10, -2, 20, 4}); // a horizontal bar turned to point down
        UI_CHECK(dl.cmds()[1].op == DrawOp::Path);
        const Image img = render(dl, 100, 100);
        UI_CHECK_EQ(img.at(50, 70), 0xFFFFFFFFu);
        UI_CHECK_EQ(img.at(51, 61), 0xFFFFFFFFu);
        UI_CHECK_EQ(img.at(70, 50), 0u);
        UI_CHECK_EQ(img.at(50, 59), 0u);
        UI_CHECK(nearRel(area(img), 48.0 + 80.0, 0.001));
        // a general affine (skew): the unit square becomes a parallelogram of the same area x det
        DrawList sk;
        Painter q(sk);
        q.setBrush(kWhite);
        q.transform({20.f, 0.f, 10.f, 20.f, 10.f, 10.f});
        q.drawRect({0, 0, 1, 1});
        const Image si = render(sk, 64, 48);
        UI_CHECK(nearRel(area(si), 400.0, 0.002));
        UI_CHECK_EQ(si.at(12, 12), 0xFFFFFFFFu);
        UI_CHECK_EQ(si.at(36, 28), 0xFFFFFFFFu);
        UI_CHECK_EQ(si.at(12, 28), 0u);
        // ellipse and polygon follow the transform too
        DrawList el;
        Painter e(el);
        e.setBrush(kWhite);
        e.scale(2.f, 1.f);
        e.drawEllipse({20, 20}, 10.f, 10.f);
        UI_CHECK(nearRel(area(render(el, 80, 40)), kPi * 20.0 * 10.0, 0.004));
        const Vec2 tri[3]{{0, 0}, {10, 0}, {0, 10}};
        DrawList pg;
        Painter g(pg);
        g.setBrush(kWhite);
        g.translate(5, 5);
        g.drawPolygon(tri);
        UI_CHECK(nearRel(area(render(pg, 32, 32)), 50.0, 0.002));
    }
    // ---- pens: native lines, path strokes, non-uniform scale ----
    {
        DrawList dl;
        Painter p(dl);
        StrokeStyle round;
        round.width = 4.f;
        round.cap = LineCap::Round;
        p.setPen(kWhite, round);
        p.scale(2.f);
        p.drawLine({5, 5}, {25, 5});
        UI_CHECK(dl.cmds()[0].op == DrawOp::Line && dl.cmds()[0].borderWidth == 8.f); // the capsule op, width scaled
        UI_CHECK(dl.cmds()[0].rect.x == 10.f && dl.cmds()[0].rect.w == 50.f);
        p.setPen(kWhite, 4.f); // butt caps: a path
        p.drawLine({5, 15}, {25, 15});
        UI_CHECK(dl.cmds()[1].op == DrawOp::Path);
        const Image img = render(dl, 64, 48);
        double butt = 0.0;
        for (int y = 20; y < 40; ++y)
            for (int x = 0; x < 64; ++x) butt += double(img.at(x, y) >> 24) / 255.0;
        UI_CHECK(nearRel(butt, 40.0 * 8.0, 0.001));
        UI_CHECK_EQ(img.at(9, 30), 0u);
        const Vec2 zig[4]{{2, 2}, {10, 2}, {10, 10}, {18, 10}};
        DrawList pl;
        Painter q(pl);
        q.setBrush(Color::rgb(0xFF0000)); // a polyline is never filled
        q.setPen(kWhite, 2.f);
        q.drawPolyline(zig);
        UI_CHECK_EQ(pl.size(), std::size_t(1));
        UI_CHECK(nearRel(area(render(pl, 24, 16)), 24.0 * 2.0, 0.001)); // miter joins: length x width
        // non-uniform scale: the pen is stretched with the shape (8 px wide where the outline runs vertically, 2 px
        // where it runs horizontally), as QPainter does with a non-cosmetic pen
        DrawList nu;
        Painter n(nu);
        n.setPen(kWhite, 2.f);
        n.scale(4.f, 1.f);
        n.drawRect({5, 10, 20, 40});
        UI_CHECK(nu.cmds()[0].op == DrawOp::Path && !nu.paths()[0].stroked); // outlined in user space, then filled
        const Image ni = render(nu, 128, 64);
        double across = 0.0, down = 0.0;
        for (int x = 0; x < 50; ++x) across += double(ni.at(x, 30) >> 24) / 255.0; // through the left edge
        for (int y = 0; y < 30; ++y) down += double(ni.at(60, y) >> 24) / 255.0;    // through the top edge
        UI_CHECK(nearRel(across, 8.0, 0.01));
        UI_CHECK(nearRel(down, 2.0, 0.01));
        // similarity: the stroke stays a stroke (flattened in device space by the rasteriser)
        DrawList sm;
        Painter s(sm);
        s.setPen(kWhite, 2.f);
        s.rotate(0.3f);
        s.scale(3.f);
        s.drawEllipse({10, 5}, 4.f, 4.f);
        UI_CHECK(sm.paths()[0].stroked && std::abs(sm.paths()[0].stroke.width - 6.f) < 1e-4f);
        // arcs and dashes
        DrawList ar;
        Painter a(ar);
        StrokeStyle st;
        st.width = 4.f;
        a.setPen(kWhite, st);
        a.drawArc({32, 32}, 20.f, 20.f, 0.f, kPi); // lower half (clockwise from +x on screen)
        const Image ai = render(ar, 64, 64);
        UI_CHECK(nearRel(area(ai), kPi * 20.0 * 4.0, 0.004));
        UI_CHECK(ch(ai.at(32, 52), 3) == 255);
        UI_CHECK_EQ(ai.at(32, 12), 0u);
    }
    // ---- brushes: gradients in local coordinates, fill rule, opacity ----
    {
        const GradientStop bw[2]{{0.f, Color::rgb(0x000000)}, {1.f, Color::rgb(0xFFFFFF)}};
        DrawList dl;
        Painter p(dl);
        p.translate(10, 10);
        p.scale(2.f);
        p.setBrush(Gradient::linear({0, 0}, {20, 0}, bw));
        p.drawRect({0, 0, 20, 10});
        p.setOpacity(0.5f);
        p.translate(0, 12);
        p.drawRoundedRect({0, 0, 20, 10}, 3.f);
        const Image img = render(dl, 64, 64);
        UI_CHECK(ch(img.at(11, 15), 0) <= 12 && ch(img.at(49, 15), 0) >= 247); // spans the scaled rect
        UI_CHECK(std::abs(int(ch(img.at(30, 15), 0)) - 130) <= 3);
        UI_CHECK(std::abs(int(ch(img.at(48, 45), 3)) - 128) <= 1); // opacity multiplies the gradient
        UI_CHECK_EQ(img.at(10, 34), 0u);                           // rounded corner
        DrawList fr;
        Painter q(fr);
        q.setBrush(kWhite);
        q.setFillRule(FillRule::EvenOdd);
        Path two;
        two.addRect({0, 0, 20, 20});
        two.addRect({5, 5, 10, 10}); // same direction: a hole only under even-odd
        q.drawPath(two);
        q.setFillRule(FillRule::NonZero);
        q.translate(30, 0);
        q.drawPath(two);
        const Image fi = render(fr, 64, 24);
        UI_CHECK_EQ(fi.at(10, 10), 0u);
        UI_CHECK_EQ(fi.at(40, 10), 0xFFFFFFFFu);
        // explicit paint ignores the state
        DrawList ex;
        Painter e(ex);
        e.setOpacity(0.5f);
        e.fillRect({0, 0, 4, 4}, Color::rgb(0xFF0000));
        UI_CHECK(ex.cmds()[0].fill == Color::rgb(0xFF0000, 0.5f));
        e.setBrush(Color{}); // a transparent brush records nothing
        e.drawRect({0, 0, 4, 4});
        UI_CHECK_EQ(ex.size(), std::size_t(1));
    }
    // ---- clips and layers ----
    {
        DrawList dl;
        {
            Painter p(dl);
            p.setBrush(kWhite);
            p.save();
            p.translate(10, 10);
            p.scale(2.f);
            p.clipRect({0, 0, 10, 10}); // axis-aligned: a rect clip
            UI_CHECK(dl.cmds()[0].op == DrawOp::PushClip);
            UI_CHECK_RECT(dl.cmds()[0].rect, 10, 10, 20, 20);
            p.drawRect({-50, -50, 100, 100});
            p.restore();
            p.save();
            p.translate(70, 20);
            p.rotate(kPi / 4.f);
            p.clipRect({-10, -10, 20, 20}); // rotated: a path clip (a diamond)
            p.drawRect({-50, -50, 100, 100});
            p.restore();
            p.save();
            p.clipRoundedRect({100, 5, 30, 30}, {15, 15, 15, 15});
            p.drawRect({0, 0, 200, 200});
            p.restore();
            p.save();
            Path circle;
            circle.addCircle({160, 20}, 12.f);
            p.clipPath(circle);
            p.drawRect({0, 0, 200, 200});
            p.restore();
        }
        UI_CHECK_EQ(count(dl, DrawOp::PushClip), 2);
        UI_CHECK_EQ(count(dl, DrawOp::PushClipPath), 2);
        UI_CHECK_EQ(count(dl, DrawOp::PopClip), 4);
        const Image img = render(dl, 180, 44);
        UI_CHECK_EQ(img.at(10, 10), 0xFFFFFFFFu);
        UI_CHECK_EQ(img.at(29, 29), 0xFFFFFFFFu);
        UI_CHECK_EQ(img.at(30, 29), 0u);
        UI_CHECK_EQ(img.at(70, 8), 0xFFFFFFFFu); // the diamond's top (radius 14.1 along the axes)
        UI_CHECK_EQ(img.at(61, 11), 0u);         // a corner of its bounding box
        UI_CHECK_EQ(img.at(115, 20), 0xFFFFFFFFu);
        UI_CHECK_EQ(img.at(101, 6), 0u);
        UI_CHECK_EQ(img.at(160, 20), 0xFFFFFFFFu);
        UI_CHECK_EQ(img.at(150, 10), 0u);
        double a = 0.0;
        for (int y = 0; y < 44; ++y)
            for (int x = 40; x < 100; ++x) a += double(img.at(x, y) >> 24) / 255.0;
        UI_CHECK(nearRel(a, 400.0, 0.003));
        // group opacity vs per-primitive opacity
        DrawList ly;
        {
            Painter p(ly);
            p.fillRect({0, 0, 40, 40}, kWhite);
            p.beginLayer(0.5f, {0, 0, 40, 20});
            p.fillRect({0, 0, 30, 20}, Color::rgb(0xFF0000));
            p.fillRect({10, 0, 30, 20}, Color::rgb(0x0000FF));
            p.endLayer();
            p.setOpacity(0.5f);
            p.fillRect({0, 20, 30, 20}, Color::rgb(0xFF0000));
            p.fillRect({10, 20, 30, 20}, Color::rgb(0x0000FF));
        }
        const Image li = render(ly, 40, 40);
        UI_CHECK(std::abs(int(ch(li.at(20, 10), 1)) - 128) <= 1); // layer: only blue over white (0.5, 0.5, 1)
        UI_CHECK(std::abs(int(ch(li.at(20, 30), 1)) - 64) <= 1);  // per primitive: blue over (red over white)
    }
    // ---- text and images ----
    {
        UiContext ui({.viewport = {200, 100}, .font = &font()});
        DrawList dl;
        Painter p(dl, &ui, 7u);
        p.drawText("Hi", {10, 10, 80, 20}, kWhite, 14.f);
        const std::size_t n1 = dl.size();
        UI_CHECK(n1 >= 2);
        f32 h1 = 0.f, x1 = 1e9f;
        for (const DrawCmd& c : dl.cmds()) {
            UI_CHECK(c.op == DrawOp::Glyph && c.node == 7u);
            h1 = std::max(h1, c.rect.h);
            x1 = std::min(x1, c.rect.x);
        }
        UI_CHECK(x1 >= 10.f && x1 < 14.f);
        p.save();
        p.translate(100, 0);
        p.scale(2.f);
        p.setOpacity(0.5f);
        p.drawText("Hi", {10, 10, 80, 20}, kWhite, 14.f); // scale = a bigger font at the scaled position
        p.restore();
        f32 h2 = 0.f, x2 = 1e9f;
        for (std::size_t i = n1; i < dl.size(); ++i) {
            h2 = std::max(h2, dl.cmds()[i].rect.h);
            x2 = std::min(x2, dl.cmds()[i].rect.x);
            UI_CHECK(dl.cmds()[i].fill.a == 0.5f);
        }
        UI_CHECK(h2 > h1 * 1.7f && h2 < h1 * 2.3f);
        UI_CHECK(x2 >= 120.f && x2 < 128.f);
        const std::size_t n2 = dl.size();
        p.save();
        p.translate(50, 50);
        p.rotate(kPi / 2.f);
        p.drawText("Hi", {0, 0, 80, 20}, kWhite, 14.f); // documented limit: upright, at the transformed origin
        p.restore();
        UI_CHECK(dl.size() == n2 + n1);
        f32 x3 = 1e9f, h3 = 0.f;
        for (std::size_t i = n2; i < dl.size(); ++i) {
            x3 = std::min(x3, dl.cmds()[i].rect.x);
            h3 = std::max(h3, dl.cmds()[i].rect.h);
        }
        UI_CHECK(x3 >= 50.f && x3 < 54.f && h3 == h1);
        Painter none(dl); // no context: text is a no-op
        none.drawText("Hi", {0, 0, 80, 20}, kWhite, 14.f);
        UI_CHECK(dl.size() == n2 + n1);

        Image src;
        src.resize(8, 8, 0xFF0000FFu);
        for (int y = 0; y < 8; ++y)
            for (int x = 4; x < 8; ++x) src.pixels[std::size_t(y) * 8 + std::size_t(x)] = 0xFFFF0000u;
        DrawList im;
        Painter q(im);
        q.translate(4, 4);
        q.scale(2.f);
        UI_CHECK(q.drawImage(0, {0, 0, 8, 8}));
        UI_CHECK(im.cmds()[0].op == DrawOp::Image);
        UI_CHECK_RECT(im.cmds()[0].rect, 4, 4, 16, 16);
        UI_CHECK(q.drawImage(0, {10, 0, 4, 8}, {4, 0, 4, 8})); // the blue half only
        UI_CHECK(im.cmds()[1].op == DrawOp::NinePatch);
        UI_CHECK(q.drawNinePatch(0, {16, 0, 10, 8}, Edges{2, 2, 2, 2}));
        UI_CHECK(im.patches()[1].dstInsets.l == 4.f && im.patches()[1].srcInsets.l == 2.f); // borders scale with the painter
        const Image out = render(im, 64, 24, 0, &src);
        UI_CHECK_EQ(out.at(5, 5), 0xFF0000FFu);
        UI_CHECK_EQ(out.at(18, 5), 0xFFFF0000u);
        UI_CHECK_EQ(out.at(25, 10), 0xFFFF0000u);
        UI_CHECK_EQ(out.at(31, 10), 0xFFFF0000u);
        UI_CHECK_EQ(out.at(32, 10), 0u);
        UI_CHECK_EQ(out.at(37, 10), 0xFF0000FFu);
        const std::size_t before = im.size();
        q.rotate(0.2f); // documented limit: images are rejected under rotation
        UI_CHECK(!q.drawImage(0, {0, 0, 8, 8}));
        UI_CHECK(!q.drawImage(0, {0, 0, 8, 8}, {0, 0, 4, 4}));
        UI_CHECK(!q.drawNinePatch(0, {0, 0, 8, 8}, Edges{1, 1, 1, 1}));
        q.setTransform(Affine::scaling(-1.f, 1.f));
        UI_CHECK(!q.drawImage(0, {0, 0, 8, 8}));
        UI_CHECK_EQ(im.size(), before);
    }
    // ---- a widget painting with a Painter, in the paint pass, at render scale 1 and 2 ----
    {
        auto frame = [&](f32 scale, Image& img, std::size_t* paths = nullptr) {
            UiContext ui({.viewport = {80, 80}, .font = &font()});
            ui.registry().registerWidget<Gauge>();
            ui.setRenderScale(scale);
            const NodeHandle g = *ui.create("test-gauge", ui.root(), "gauge");
            ui.setCss(g, "width", "60px");
            ui.setCss(g, "height", "60px");
            ui.setCss(g, "margin", "10px");
            ui.setCss(g, "opacity", "0.5");
            ui.setRange(g, 0.f, 1.f);
            ui.setValue(g, 1.f);
            ui.update();
            if (paths) *paths = ui.drawList().paths().size();
            for (const DrawCmd& c : ui.drawList().cmds())
                if (c.op == DrawOp::Path) UI_CHECK(c.node == g.index); // tagged with the node by the paint pass
            img.resize(int(80 * scale), int(80 * scale), 0);
            CpuRasterizer r;
            r.render(ui.drawList(), img);
        };
        Image i1, i2;
        std::size_t n = 0;
        frame(1.f, i1, &n);
        frame(2.f, i2);
        UI_CHECK_EQ(n, std::size_t(2));
        UI_CHECK(ch(i1.at(40, 40), 3) >= 127 && ch(i1.at(40, 40), 3) <= 128); // the hub, at the node's opacity
        UI_CHECK_EQ(i1.at(40, 60), 0u);                                       // the arc's gap at the bottom
        UI_CHECK(ch(i1.at(40, 16), 3) >= 127);                                // the arc at the top
        UI_CHECK(nearRel(area(i2), area(i1) * 4.0, 0.01));                    // 2x: four times the pixels...
        int upscaleDiff = 0;
        for (int y = 0; y < 160; ++y)
            for (int x = 0; x < 160; ++x) upscaleDiff += i2.at(x, y) != i1.at(x / 2, y / 2);
        UI_CHECK(upscaleDiff > 300); // ...and re-rasterised, not an upscale
    }
    return finish("fuse_ui_painter");
}
