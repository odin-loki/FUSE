// Gate fuse_ui_css_gradient: CSS multi-stop gradients on backgrounds (`background: linear-gradient(...)` /
// `background-image`).
//   * parsing: angles (deg / turn / rad), side and corner keywords, stop offsets, missing offsets spread evenly,
//     radial-gradient; the two-colour legacy forms keep their old meaning (the plain Rect gradient)
//   * paint: the background's pixels follow the gradient line over the border box (multi-stop, any angle, radial),
//     over the background colour, inside the border radius, with the node's opacity
// (Adapted from the gui-framework gate of the same name.)
#include "test_util.hpp"

#include <cmath>
#include <cstdio>

using namespace fuse::ui;
using namespace fuse::ui::test;

namespace {
bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool about(int got, f32 want, int tol = 3) { return std::abs(got - int(std::lround(want))) <= tol; }
} // namespace

int main() {
    // ---- parsing ----
    {
        auto s = StyleSheet::parse(".a { background: linear-gradient(vertical, #ff0000, #0000ff); } "
                                   ".b { background: linear-gradient(to right, red, blue); } "
                                   ".c { background: linear-gradient(90deg, red, #00ff00 25%, blue); } "
                                   ".d { background-image: linear-gradient(to bottom right, red, blue); } "
                                   ".e { background: radial-gradient(white, black 50%, transparent); } "
                                   ".f { background: linear-gradient(red, green, blue, white); } "
                                   ".g { background: #123456; }");
        UI_CHECK(s.has_value() && s->rules().size() == 7);
        if (s && s->rules().size() == 7) {
            auto style = [&](int i) {
                ComputedStyle cs;
                for (const auto& d : s->rules()[std::size_t(i)].decls) applyProp(cs, d.prop, d.value);
                return cs;
            };
            // the legacy two-colour forms: the Rect gradient, no background-image
            UI_CHECK(style(0).gradient == GradientDir::Vertical && style(0).backgroundImage.kind == GradientSpecKind::None);
            UI_CHECK(style(1).gradient == GradientDir::Horizontal && near(style(1).background2.b, 1.f));
            const ComputedStyle c = style(2);
            UI_CHECK(c.gradient == GradientDir::None && c.backgroundImage.kind == GradientSpecKind::Linear && c.backgroundImage.stopCount == 3);
            UI_CHECK(near(c.backgroundImage.angle, 1.5707963f) && near(c.backgroundImage.offset[0], 0.f) && near(c.backgroundImage.offset[1], 0.25f) &&
                     near(c.backgroundImage.offset[2], 1.f));
            UI_CHECK(near(style(3).backgroundImage.angle, 2.3561945f)); // 135 degrees
            UI_CHECK(style(4).backgroundImage.kind == GradientSpecKind::Radial && style(4).backgroundImage.stopCount == 3 &&
                     near(style(4).backgroundImage.color[2].a, 0.f));
            const ComputedStyle f = style(5); // to bottom by default; offsets spread evenly
            UI_CHECK(near(f.backgroundImage.angle, 3.14159265f) && near(f.backgroundImage.offset[1], 1.f / 3.f) && near(f.backgroundImage.offset[2], 2.f / 3.f));
            UI_CHECK(style(6).backgroundImage.kind == GradientSpecKind::None && near(style(6).background.r, 0x12 / 255.f));
        }
        PropValue v;
        UI_CHECK(parsePropValue(Prop::BackgroundImage, "linear-gradient(to left, red 20%, blue 10%)", v)); // offsets never go backwards
        ComputedStyle cs;
        applyProp(cs, Prop::BackgroundImage, v);
        UI_CHECK(near(cs.backgroundImage.angle, 4.712389f) && near(cs.backgroundImage.offset[1], 0.2f));
        UI_CHECK(!parsePropValue(Prop::BackgroundImage, "linear-gradient(red)", v) && !parsePropValue(Prop::BackgroundImage, "url(x.png)", v));
    }

    // ---- paint ----
    UiContext ui({.viewport = {240, 240}});
    UI_CHECK(ui.addStyleSheet(R"css(
panel { flex-shrink: 0; }
.stage { layout: absolute; width: 240px; height: 240px; }
.box { left: 20px; top: 20px; width: 200px; height: 100px; }
.h3 { background: linear-gradient(90deg, #ff0000, #00ff00 50%, #0000ff); }
.up { background: linear-gradient(to top, #000000, #ffffff); }
.diag { width: 100px; background: linear-gradient(45deg, #000000, #ffffff); }
.rad { background: radial-gradient(#ffffff, #000000); }
.over { background: #0000ff; background-image: linear-gradient(to right, #ff0000, transparent); }
.round { border-radius: 30px; }
.dim { opacity: 0.5; }
)css").has_value());
    const NodeHandle stage = ui.create(ui.registry().find("panel"), ui.root());
    ui.addClass(stage, "stage");
    const NodeHandle box = ui.create(ui.registry().find("panel"), stage);
    ui.addClass(box, "box");
    auto shot = [&](const char* cls, Image& img) {
        ui.addClass(box, cls);
        ui.update(0.f);
        img.resize(240, 240, 0xFF000000u);
        CpuRasterizer().render(ui.drawList(), img);
        ui.removeClass(box, cls);
    };
    auto ch = [](const Image& img, int x, int y, int c) { return int((img.at(x, y) >> (8 * c)) & 0xFF); };
    Image img;
    shot("h3", img);
    for (int x = 22; x < 218; x += 7) { // red -> green over the first half, green -> blue over the second
        const f32 t = (f32(x) + 0.5f - 20.f) / 200.f;
        const f32 r = t < 0.5f ? 1.f - t * 2.f : 0.f, g = t < 0.5f ? t * 2.f : 2.f - t * 2.f, b = t < 0.5f ? 0.f : t * 2.f - 1.f;
        UI_CHECK(about(ch(img, x, 70, 0), 255 * r) && about(ch(img, x, 70, 1), 255 * g) && about(ch(img, x, 70, 2), 255 * b));
    }
    UI_CHECK(ch(img, 120, 25, 1) == ch(img, 120, 115, 1) && ch(img, 10, 70, 0) == 0); // constant across; nothing outside the box
    shot("up", img);
    for (int y = 22; y < 118; y += 5) UI_CHECK(about(ch(img, 100, y, 0), 255 * (1.f - (f32(y) + 0.5f - 20.f) / 100.f)));
    shot("diag", img); // a 100 x 100 box: black at the bottom left corner, white at the top right, grey along the other diagonal
    UI_CHECK(ch(img, 21, 118, 0) < 8 && ch(img, 118, 21, 0) > 247 && about(ch(img, 40, 40, 0), 127.5f, 4) && about(ch(img, 90, 90, 0), 127.5f, 4));
    UI_CHECK(about(ch(img, 70, 70, 0), 127.5f, 4) && ch(img, 100, 40, 0) > ch(img, 70, 70, 0) && ch(img, 40, 100, 0) < ch(img, 70, 70, 0));
    shot("rad", img); // white in the middle, black at the corners, elliptical (the box is twice as wide as high)
    UI_CHECK(ch(img, 120, 70, 0) > 245 && ch(img, 21, 21, 0) < 8 && ch(img, 218, 118, 0) < 8);
    UI_CHECK(std::abs(ch(img, 120 + 60, 70, 0) - ch(img, 120, 70 + 30, 0)) <= 3 && ch(img, 180, 70, 0) < ch(img, 150, 70, 0));
    shot("over", img); // the image over the colour: red fading out over blue
    UI_CHECK(about(ch(img, 25, 70, 0), 255 * 0.9725f) && about(ch(img, 25, 70, 2), 255 * 0.0275f) && about(ch(img, 120, 70, 0), 127.5f) &&
             about(ch(img, 120, 70, 2), 127.5f) && about(ch(img, 215, 70, 2), 255 * 0.9775f));
    ui.addClass(box, "round");
    shot("h3", img);
    UI_CHECK(ch(img, 22, 22, 0) == 0 && ch(img, 22, 22, 1) == 0 && ch(img, 30, 70, 0) > 200); // the corner is cut
    ui.addClass(box, "dim");
    shot("up", img);
    UI_CHECK(about(ch(img, 100, 22, 0), 255 * 0.975f * 0.5f) && about(ch(img, 100, 70, 0), 255 * 0.495f * 0.5f));
    return finish("fuse_ui_css_gradient");
}
