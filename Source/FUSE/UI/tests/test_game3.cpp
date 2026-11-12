// fuse_ui_game HUD widgets added with the rest of the catalogue (plan §4.15), one executable per gate
// (FUSE_UI_GAME_GATE, ctest fuse_ui_widget_<gate>):
//   minimap       — blip projection (range, north-up and rotating map), clamp to the circular / square frame,
//                   background image command, the frame takes no input
//   ammo_counter  — "magazine / reserve" text, one pip per round (bar for a large magazine), low-ammo tint, reload
//                   bar, ValueChanged when a count changes
// `--out=<dir>` writes hud_extras.png.
#include "test_util.hpp"

#include "fuse/ui/game/game_extras.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_write_impl.hpp"

#include <cmath>
#include <cstring>
#include <string>

using namespace fuse::ui;
using namespace fuse::ui::test;
namespace g = fuse::ui::game;
namespace w = fuse::ui::widgets;

namespace {
constexpr Size kVp{960, 540};
const char* g_out = nullptr;

void setup(UiContext& ui) {
    w::install(ui);
    g::install(ui);
    ui.setCss(ui.root(), "layout", "anchor");
    ui.setCss(ui.root(), "background", "#1B2230");
}
bool near(f32 a, f32 b, f32 eps = 0.05f) { return std::abs(a - b) <= eps; }
bool at(Vec2 p, f32 x, f32 y) { return near(p.x, x) && near(p.y, y); }
void place(UiContext& ui, NodeHandle n, f32 x, f32 y, f32 wpx, f32 hpx) {
    ui.set(n, Prop::Left, PropValue::ofLength(Length::px(x)));
    ui.set(n, Prop::Top, PropValue::ofLength(Length::px(y)));
    ui.set(n, Prop::Width, PropValue::ofLength(Length::px(wpx)));
    ui.set(n, Prop::Height, PropValue::ofLength(Length::px(hpx)));
}
void savePng(UiContext& ui, const char* name) {
    if (!g_out) return;
    ui.update(0.f);
    Image img;
    img.resize(int(ui.viewport().w), int(ui.viewport().h), 0u);
    CpuRasterizer r;
    r.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
    static Image map; // a stand-in map texture for image index 0
    map.resize(8, 8, 0xFF203020u);
    r.addImage(&map);
    r.render(ui.drawList(), img);
    for (u32& p : img.pixels) p |= 0xFF000000u;
    const std::string path = std::string(g_out) + "/" + name;
    stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
    std::printf("wrote %s\n", path.c_str());
}
/// The rect command filling exactly `r` (null when there is none). Rect commands a widget paints carry no node index,
/// so they are found by geometry; the scenes here have one widget where `r` is.
const DrawCmd* cmdAt(const UiContext& ui, Rect r) {
    for (const DrawCmd& d : ui.drawList().cmds())
        if (d.op == DrawOp::Rect && near(d.rect.x, r.x) && near(d.rect.y, r.y) && near(d.rect.w, r.w) && near(d.rect.h, r.h)) return &d;
    return nullptr;
}

int minimapGate() {
    UiContext ui({.viewport = kVp, .font = &font()});
    setup(ui);
    const NodeHandle map = g::minimap(ui, ui.root(), "map");
    place(ui, map, 100, 50, 200, 200);
    g::setMinimapRange(ui, map, 100.f);
    const Color red = Color::rgb(0xE5484D), gold = Color::rgb(0xF2C14E), blue = Color::rgb(0x5EA2FF);
    const g::MinimapBlip blips[] = {{{0.f, 50.f}, red, g::MinimapBlipKind::Dot},        // 50 north
                                    {{50.f, 0.f}, blue, g::MinimapBlipKind::Square},    // 50 east
                                    {{1000.f, 1000.f}, gold, g::MinimapBlipKind::Diamond}, // far north-east
                                    {{-300.f, 0.f}, gold, g::MinimapBlipKind::Triangle}};  // far west
    g::minimapSetBlips(ui, map, blips);
    ui.update(0.f);
    UI_CHECK_EQ(g::minimapBlipCount(ui, map), 4u);
    UI_CHECK_RECT(g::minimapFrame(ui, map), 100, 50, 200, 200);
    // range 100 world units = the frame radius less its 2 px border (98 px): 50 units -> 49 px
    const Vec2 c{200.f, 150.f};
    bool clamped = true;
    UI_CHECK(at(g::minimapBlipPos(ui, map, 0, &clamped), c.x, c.y - 49.f) && !clamped); // heading 0: north is up
    UI_CHECK(at(g::minimapBlipPos(ui, map, 1, &clamped), c.x + 49.f, c.y) && !clamped);
    // beyond the range: held on the frame edge (6 px inside the rim) in the blip's direction
    const f32 edge = 92.f, d45 = edge * std::sqrt(0.5f);
    UI_CHECK(at(g::minimapBlipPos(ui, map, 2, &clamped), c.x + d45, c.y - d45) && clamped);
    UI_CHECK(at(g::minimapBlipPos(ui, map, 3, &clamped), c.x - edge, c.y) && clamped);
    UI_CHECK(g::minimapBlipPos(ui, map, 9, &clamped) == Vec2{} && !clamped);
    // the blips are painted where the query says, in their colours; the edge blip is drawn smaller
    const Vec2 p0 = g::minimapBlipPos(ui, map, 0), p1 = g::minimapBlipPos(ui, map, 1);
    const DrawCmd* dot = cmdAt(ui, {p0.x - 3.5f, p0.y - 3.5f, 7.f, 7.f});
    UI_CHECK(dot && dot->fill == red && dot->radius[0] == 3.5f);
    const DrawCmd* sq = cmdAt(ui, {p1.x - 3.5f, p1.y - 3.5f, 7.f, 7.f});
    UI_CHECK(sq && sq->fill == blue);
    // no background image by default; with one: an Image command over the frame, inside the frame's clip
    auto imageCmd = [&]() -> const DrawCmd* {
        for (const DrawCmd& d : ui.drawList().cmds())
            if (d.op == DrawOp::Image && d.node == map.index) return &d;
        return nullptr;
    };
    UI_CHECK(!imageCmd());
    g::setMinimapImage(ui, map, 3);
    ui.update(0.f);
    UI_CHECK(imageCmd() && imageCmd()->image == 3u && imageCmd()->rect == g::minimapFrame(ui, map));
    UI_CHECK_EQ(g::minimapInfo(ui, map).imageIndex, 3);
    {
        int depth = 0, imageDepth = -1;
        f32 clipRadius = 0.f;
        for (const DrawCmd& d : ui.drawList().cmds()) {
            if (d.op == DrawOp::PushClip) { ++depth; if (d.rect == g::minimapFrame(ui, map)) clipRadius = d.radius[0]; }
            else if (d.op == DrawOp::PopClip) --depth;
            else if (d.op == DrawOp::Image && d.node == map.index) imageDepth = depth;
        }
        UI_CHECK(imageDepth >= 1 && clipRadius == 100.f); // clipped to the circle
    }
    g::setMinimapImage(ui, map, -1);

    // rotating map: the heading points up. Facing east, the east blip is ahead (up) and the north blip to the left
    g::setMinimapHeading(ui, map, 90.f);
    ui.update(0.f);
    UI_CHECK(g::minimapInfo(ui, map).rotateMap && g::minimapInfo(ui, map).headingDeg == 90.f);
    UI_CHECK(at(g::minimapBlipPos(ui, map, 1), c.x, c.y - 49.f));
    UI_CHECK(at(g::minimapBlipPos(ui, map, 0), c.x - 49.f, c.y));
    UI_CHECK(at(g::minimapBlipPos(ui, map, 3, &clamped), c.x, c.y + edge) && clamped); // west is behind
    g::setMinimapHeading(ui, map, 180.f);
    UI_CHECK(at(g::minimapBlipPos(ui, map, 0), c.x, c.y + 49.f));
    UI_CHECK(at(g::minimapBlipPos(ui, map, 1), c.x - 49.f, c.y));
    g::setMinimapHeading(ui, map, 45.f); // facing north-east: the far north-east blip is dead ahead on the edge
    UI_CHECK(at(g::minimapBlipPos(ui, map, 2, &clamped), c.x, c.y - edge) && clamped);
    // north-up: the heading only turns the player arrow, blips stay put
    g::setMinimapHeading(ui, map, 90.f, false);
    UI_CHECK(!g::minimapInfo(ui, map).rotateMap);
    UI_CHECK(at(g::minimapBlipPos(ui, map, 0), c.x, c.y - 49.f) && at(g::minimapBlipPos(ui, map, 1), c.x + 49.f, c.y));
    // range: a wider range pulls blips in, a tighter one pushes them to the edge
    g::setMinimapRange(ui, map, 200.f);
    UI_CHECK(at(g::minimapBlipPos(ui, map, 1, &clamped), c.x + 24.5f, c.y) && !clamped);
    g::setMinimapRange(ui, map, 40.f);
    UI_CHECK(at(g::minimapBlipPos(ui, map, 1, &clamped), c.x + edge, c.y) && clamped);
    g::setMinimapRange(ui, map, -5.f); // ignored
    UI_CHECK_EQ(g::minimapInfo(ui, map).range, 40.f);
    g::setMinimapRange(ui, map, 100.f);
    // square frame: clamps to the square, so a diagonal blip goes to the corner
    g::setMinimapShape(ui, map, g::MinimapShape::Square);
    ui.update(0.f);
    UI_CHECK(at(g::minimapBlipPos(ui, map, 2, &clamped), c.x + edge, c.y - edge) && clamped);
    UI_CHECK(at(g::minimapBlipPos(ui, map, 3, &clamped), c.x - edge, c.y) && clamped);
    UI_CHECK(at(g::minimapBlipPos(ui, map, 0, &clamped), c.x, c.y - 49.f) && !clamped);
    // a non-square widget: the frame is the centred square
    place(ui, map, 100, 50, 300, 200);
    ui.update(0.f);
    UI_CHECK_RECT(g::minimapFrame(ui, map), 150, 50, 200, 200);
    UI_CHECK(at(g::minimapBlipPos(ui, map, 0), 250.f, 150.f - 49.f));
    // replacing the blips
    g::minimapSetBlips(ui, map, std::span<const g::MinimapBlip>(blips, 1));
    UI_CHECK_EQ(g::minimapBlipCount(ui, map), 1u);
    g::minimapSetBlips(ui, map, {});
    UI_CHECK_EQ(g::minimapBlipCount(ui, map), 0u);
    // HUD furniture: the pointer goes through it
    ui.update(0.f);
    UI_CHECK(ui.hitTest({250.f, 150.f}) != map);
    std::string a11y;
    ui.dumpA11y(a11y);
    UI_CHECK(a11y.find("Minimap") != std::string::npos);
    // the API ignores nodes that are not minimaps
    const NodeHandle other = w::label(ui, ui.root(), "x");
    g::minimapSetBlips(ui, other, blips);
    UI_CHECK_EQ(g::minimapBlipCount(ui, other), 0u);
    UI_CHECK(g::minimapFrame(ui, other).empty());
    return finish("fuse_ui_widget_minimap");
}

int ammoGate() {
    UiContext ui({.viewport = kVp, .font = &font()});
    setup(ui);
    const NodeHandle ammo = g::ammoCounter(ui, ui.root(), "ammo");
    place(ui, ammo, 700, 440, 200, 60);
    int changes = 0;
    f32 last = -1.f;
    std::string text;
    ui.connect(ammo, SignalKind::ValueChanged, [&changes, &last, &text](UiContext&, NodeHandle, const SignalArgs& a) {
        ++changes;
        last = a.value;
        text = a.text;
    });
    g::setAmmo(ui, ammo, 12, 30, 90);
    ui.update(0.f);
    UI_CHECK(ui.text(ammo) == "12 / 90");
    UI_CHECK_EQ(ui.value(ammo), 12.f);
    UI_CHECK_EQ(changes, 1);
    UI_CHECK(last == 12.f && text == "12 / 90");
    g::AmmoInfo in = g::ammoInfo(ui, ammo);
    UI_CHECK(in.inMagazine == 12 && in.magazineSize == 30 && in.reserve == 90 && in.pips == 30 && !in.low && !in.reloading);
    // one pip per round of the magazine, left to right inside the widget, filled for the rounds left
    const Rect c = ui.contentRect(ammo);
    const Color white = Color::rgb(0xFFFFFF), danger = Color::rgb(0xE5484D);
    UI_CHECK(ui.computed(ammo).color == white);
    auto filledPips = [&](Color full) {
        u32 n = 0;
        for (u32 i = 0; i < g::ammoInfo(ui, ammo).pips; ++i) {
            const DrawCmd* d = cmdAt(ui, g::ammoPipRect(ui, ammo, i));
            UI_CHECK(d != nullptr);
            if (!d) continue;
            if (d->fill == full) { UI_CHECK_EQ(n, i); ++n; } // the filled pips are the first ones
            else UI_CHECK(d->fill.a < 0.5f);
        }
        return n;
    };
    for (u32 i = 0; i < 30; ++i) {
        const Rect p = g::ammoPipRect(ui, ammo, i);
        UI_CHECK(p.w >= 1.f && p.h >= 4.f && p.x >= c.x - 0.01f && p.right() <= c.right() + 0.01f && p.bottom() <= c.bottom());
        if (i) UI_CHECK(p.x >= g::ammoPipRect(ui, ammo, i - 1).right() && near(p.y, g::ammoPipRect(ui, ammo, 0).y));
    }
    UI_CHECK(g::ammoPipRect(ui, ammo, 30).empty());
    UI_CHECK_EQ(filledPips(white), 12u);
    // the same counts again: no signal
    g::setAmmo(ui, ammo, 12, 30, 90);
    UI_CHECK_EQ(changes, 1);
    // firing: one pip less, one signal per change
    g::setAmmo(ui, ammo, 11, 30, 90);
    ui.update(0.f);
    UI_CHECK_EQ(changes, 2);
    UI_CHECK(last == 11.f && ui.text(ammo) == "11 / 90");
    UI_CHECK_EQ(filledPips(white), 11u);
    // low ammo: at or below 25 % of the magazine the counter turns to the danger colour (text and pips)
    g::setAmmo(ui, ammo, 8, 30, 90);
    ui.update(0.f);
    UI_CHECK(!g::ammoInfo(ui, ammo).low && !ui.hasClass(ammo, "ammo-low"));
    g::setAmmo(ui, ammo, 7, 30, 90);
    ui.update(0.f);
    UI_CHECK(g::ammoInfo(ui, ammo).low && ui.hasClass(ammo, "ammo-low"));
    UI_CHECK(ui.computed(ammo).color == danger);
    UI_CHECK_EQ(filledPips(danger), 7u);
    {
        bool glyphs = false, allDanger = true;
        for (const DrawCmd& d : ui.drawList().cmds())
            if (d.node == ammo.index && d.op == DrawOp::Glyph) { glyphs = true; allDanger = allDanger && d.fill == danger; }
        UI_CHECK(glyphs && allDanger);
    }
    g::setAmmo(ui, ammo, 0, 30, 90);
    ui.update(0.f);
    UI_CHECK(g::ammoInfo(ui, ammo).low && ui.text(ammo) == "0 / 90");
    UI_CHECK_EQ(filledPips(danger), 0u);
    // reload: a progress bar across the bottom while 0 < progress < 1
    auto reloadBar = [&]() -> const DrawCmd* {
        for (const DrawCmd& d : ui.drawList().cmds())
            if (d.op == DrawOp::Rect && d.fill == Color::rgb(0xF2C14E) && ui.rect(ammo).contains({d.rect.x, d.rect.y})) return &d;
        return nullptr;
    };
    UI_CHECK(!reloadBar());
    g::setAmmoReload(ui, ammo, 0.4f);
    ui.update(0.f);
    UI_CHECK(g::ammoInfo(ui, ammo).reloading && near(g::ammoInfo(ui, ammo).reload, 0.4f));
    UI_CHECK(reloadBar() && near(reloadBar()->rect.w, c.w * 0.4f, 0.5f) && near(reloadBar()->rect.x, c.x));
    UI_CHECK(reloadBar() && reloadBar()->rect.y >= g::ammoPipRect(ui, ammo, 0).bottom());
    g::setAmmoReload(ui, ammo, 0.9f);
    ui.update(0.f);
    UI_CHECK(reloadBar() && near(reloadBar()->rect.w, c.w * 0.9f, 0.5f));
    UI_CHECK_EQ(changes, 5); // reload progress is not a count change
    g::setAmmoReload(ui, ammo, 7.f); // clamped: done
    g::setAmmo(ui, ammo, 30, 30, 60);
    ui.update(0.f);
    UI_CHECK(!g::ammoInfo(ui, ammo).reloading && g::ammoInfo(ui, ammo).reload == 1.f && !reloadBar());
    UI_CHECK(!g::ammoInfo(ui, ammo).low && !ui.hasClass(ammo, "ammo-low") && ui.computed(ammo).color == white);
    UI_CHECK_EQ(filledPips(white), 30u);
    UI_CHECK_EQ(changes, 6);
    g::setAmmoReload(ui, ammo, 0.f);
    // a reserve-only change is a change too
    g::setAmmo(ui, ammo, 30, 30, 59);
    UI_CHECK_EQ(changes, 7);
    UI_CHECK(text == "30 / 59" && last == 30.f);
    // the low threshold is configurable
    g::setAmmo(ui, ammo, 14, 30, 59);
    UI_CHECK(!g::ammoInfo(ui, ammo).low);
    g::setAmmoLowFraction(ui, ammo, 0.5f);
    UI_CHECK(g::ammoInfo(ui, ammo).low && ui.hasClass(ammo, "ammo-low"));
    g::setAmmoLowFraction(ui, ammo, 0.25f);
    UI_CHECK(!ui.hasClass(ammo, "ammo-low"));
    // the largest magazine that still gets pips, then one more: a bar
    g::setAmmo(ui, ammo, 40, g::kAmmoMaxPips, 0);
    ui.update(0.f);
    UI_CHECK_EQ(g::ammoInfo(ui, ammo).pips, g::kAmmoMaxPips);
    UI_CHECK_EQ(filledPips(white), 40u);
    UI_CHECK(g::ammoPipRect(ui, ammo, 0).x >= c.x - 0.01f && g::ammoPipRect(ui, ammo, 0).w >= 1.f);
    g::setAmmo(ui, ammo, 150, 200, 400);
    ui.update(0.f);
    UI_CHECK_EQ(g::ammoInfo(ui, ammo).pips, 0u);
    UI_CHECK(g::ammoPipRect(ui, ammo, 0).empty());
    UI_CHECK(ui.text(ammo) == "150 / 400");
    {
        const DrawCmd* track = nullptr;
        const DrawCmd* fill = nullptr;
        u32 rects = 0;
        for (const DrawCmd& d : ui.drawList().cmds()) {
            if (d.op != DrawOp::Rect || d.rect.h != 10.f || !ui.rect(ammo).contains({d.rect.x, d.rect.y})) continue;
            ++rects;
            if (d.fill == white) fill = &d;
            else track = &d;
        }
        UI_CHECK_EQ(rects, 2u); // track + fill, no pips
        UI_CHECK(track && fill && near(track->rect.w, c.w) && near(fill->rect.w, c.w * 0.75f, 0.5f) && near(fill->rect.x, track->rect.x));
    }
    g::setAmmo(ui, ammo, 50, 200, 400); // low applies to the bar as well
    ui.update(0.f);
    UI_CHECK(g::ammoInfo(ui, ammo).low && ui.computed(ammo).color == danger);
    // accessible as text; takes no input
    std::string a11y;
    ui.dumpA11y(a11y);
    UI_CHECK(a11y.find("50 / 400") != std::string::npos);
    UI_CHECK(ui.hitTest({800.f, 470.f}) != ammo);
    // the API ignores other nodes
    const NodeHandle other = w::label(ui, ui.root(), "x");
    g::setAmmo(ui, other, 1, 2, 3);
    UI_CHECK(ui.text(other) == "x" && g::ammoInfo(ui, other).magazineSize == 0u);

    if (g_out) { // demo shot: both HUD widgets
        const NodeHandle map = g::minimap(ui, ui.root(), "map");
        place(ui, map, 730, 30, 190, 190);
        g::setMinimapRange(ui, map, 80.f);
        g::setMinimapHeading(ui, map, 30.f);
        g::setMinimapImage(ui, map, 0);
        const g::MinimapBlip blips[] = {{{10.f, 40.f}, Color::rgb(0xE5484D), g::MinimapBlipKind::Triangle},
                                        {{-35.f, 12.f}, Color::rgb(0x5EA2FF), g::MinimapBlipKind::Dot},
                                        {{30.f, -20.f}, Color::rgb(0x4CC26A), g::MinimapBlipKind::Square},
                                        {{400.f, 100.f}, Color::rgb(0xF2C14E), g::MinimapBlipKind::Diamond}};
        g::minimapSetBlips(ui, map, blips);
        g::setAmmo(ui, ammo, 7, 30, 90);
        g::setAmmoReload(ui, ammo, 0.6f);
        const NodeHandle big = g::ammoCounter(ui, ui.root(), "lmg");
        place(ui, big, 700, 370, 200, 60);
        g::setAmmo(ui, big, 150, 200, 400);
        savePng(ui, "hud_extras.png");
    }
    return finish("fuse_ui_widget_ammo_counter");
}
} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) g_out = argv[i] + 6;
    const std::string gate = FUSE_UI_GAME_GATE;
    if (gate == "minimap") return minimapGate();
    if (gate == "ammo_counter") return ammoGate();
    std::fprintf(stderr, "unknown gate %s\n", gate.c_str());
    return 2;
}
