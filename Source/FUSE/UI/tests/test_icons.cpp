// Icon gates (fuse/ui/icon_set.hpp, fuse/ui/icons/icon_cook.hpp): one executable per gate, chosen by FUSE_UI_ICON_GATE.
//   cook    fuse_ui_icon_cook: the cooker is deterministic and equals the build-time pack; the pack index round trips;
//           a cooked icon draws exactly what the parsed SVG draws; monochrome / multi-colour classification; cooker and
//           loader errors (bad names, duplicates, unsupported elements, every truncation of a pack, flipped bytes);
//           the fuse_ui_icon_cook tool writes nothing on an error.
//   render  fuse_ui_icon_render: icons through the UI (the `icon` widget, `icon` / `icon-size` style properties,
//           widgets::setIcon by name, the "icon" document key, cooked style sheets), tinting (theme colour, a class,
//           :hover, :disabled), pack-over-built-in lookup and the built-in fallback, layout, render scale 2, and the
//           CPU raster goldens tests/golden/icons.png and icons_color.png (FUSE_UI_UPDATE_GOLDEN=1 rewrites them).
#include "fuse/ui/icons.hpp"
#include "fuse/ui/icons/icon_cook.hpp"
#include "fuse/ui/widgets/widgets.hpp"
#include "test_util.hpp"

#if FUSE_UI_ICON_HAVE_DOC
#include "fuse/ui/doc/cook.hpp"
#include "fuse/ui/doc/document.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

using namespace fuse::ui;
namespace w = fuse::ui::widgets;
namespace ic = fuse::ui::icons;
namespace fs = std::filesystem;
using fuse::ui::test::finish;

namespace {

int ch(u32 p, int k) { return int((p >> (8 * k)) & 0xFFu); }

std::vector<u8> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/// One image drawn into a size x size surface through a Painter (the path every icon takes), on opaque black.
Image draw(const SvgImage& img, int size, Color color) {
    DrawList dl;
    {
        Painter p(dl);
        img.render(p, {0.f, 0.f, f32(size), f32(size)}, color);
    }
    Image out;
    out.resize(size, size, 0xFF000000u);
    CpuRasterizer r;
    r.render(dl, out);
    return out;
}
int litPixels(const Image& img) {
    int n = 0;
    for (const u32 p : img.pixels) n += (p & 0xFFFFFFu) != 0;
    return n;
}

constexpr const char* kFeatureSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 48 48" width="48" height="48">
  <rect x="2" y="2" width="12" height="12" rx="3"/>
  <circle cx="24" cy="8" r="6" fill="#123456"/>
  <rect x="34" y="2" width="12" height="12" opacity="0.5"/>
  <line x1="2" y1="22" x2="14" y2="22" stroke="currentColor" stroke-width="4"/>
  <polyline points="18,26 21,18 24,26 27,18" fill="none" stroke="currentColor" stroke-width="2"/>
  <polygon points="34,26 40,16 46,26"/>
  <path d="M2 32h12v12H2z M6 36v4h4v-4z" fill-rule="evenodd"/>
  <g transform="translate(36 40)"><path d="M-6 0 A6 6 0 0 1 6 0 Z"/></g>
</svg>)svg";

// ---- cook -----------------------------------------------------------------------------------------------------------
int cookGate() {
    auto dir = ic::loadSvgDirectory(FUSE_UI_ICON_SVG_DIR);
    UI_CHECK(dir.has_value());
    if (!dir) { std::fprintf(stderr, "%s\n", dir.error().c_str()); return finish("fuse_ui_icon_cook"); }
    UI_CHECK(dir->size() >= 30);

    // deterministic: the same bytes for any input order, and the pack the build cooked with the tool
    ic::CookStats st;
    auto a = ic::cookIcons(*dir, &st);
    UI_CHECK(a.has_value());
    if (!a) { std::fprintf(stderr, "%s\n", a.error().c_str()); return finish("fuse_ui_icon_cook"); }
    auto reversed = *dir;
    std::reverse(reversed.begin(), reversed.end());
    auto b = ic::cookIcons(reversed);
    UI_CHECK(b && *b == *a);
    const std::vector<u8> built = readBytes(FUSE_UI_ICON_PACK);
    UI_CHECK(!built.empty() && built == *a);
    UI_CHECK_EQ(st.icons, u32(dir->size()));
    UI_CHECK(st.multiColor == 0u && st.shapes >= st.icons && st.bytes == a->size()); // Lucide: stroke="currentColor"
    std::printf("starter pack: %u icons, %u shapes, %zu bytes\n", st.icons, st.shapes, st.bytes);

    // index round trip
    auto set = IconSet::fromBytes(*a);
    UI_CHECK(set.has_value());
    if (!set) { std::fprintf(stderr, "%s\n", set.error().c_str()); return finish("fuse_ui_icon_cook"); }
    UI_CHECK_EQ(set->count(), u32(dir->size()));
    for (u32 i = 0; i < set->count(); ++i) {
        UI_CHECK(set->name(i) == (*dir)[i].name); // the directory listing is name-sorted too
        UI_CHECK_EQ(set->find((*dir)[i].name), i32(i));
        UI_CHECK_EQ(set->findHash(hashId((*dir)[i].name)), i32(i));
        UI_CHECK_EQ(set->nameHash(i), hashId((*dir)[i].name));
        UI_CHECK(!set->multiColor(i) && !set->image(i).empty());
    }
    UI_CHECK(set->find("save") >= 0 && set->find("no-such-icon") < 0 && set->findHash(hashId("nope")) < 0);

    // a cooked icon draws exactly what the parsed SVG draws (same shapes through the same painter), at any size
    for (u32 i = 0; i < set->count(); ++i) {
        auto parsed = SvgImage::parse((*dir)[i].svg);
        UI_CHECK(parsed.has_value());
        if (!parsed) continue;
        UI_CHECK_EQ(parsed->shapeCount(), set->image(i).shapeCount());
        UI_CHECK_EQ(parsed->segmentCount(), set->image(i).segmentCount());
        for (int size : {16, 24, 37}) {
            const Image want = draw(*parsed, size, Color::rgb(0x3D8BFD)), got = draw(set->image(i), size, Color::rgb(0x3D8BFD));
            UI_CHECK(want.pixels == got.pixels);
            UI_CHECK(litPixels(got) > size); // something was drawn
        }
    }

    // the SVG feature set survives cooking: shapes, opacity, strokes, fill rules, transforms, an own colour
    {
        auto pack = ic::cookIcons({{"features", kFeatureSvg}}, &st);
        UI_CHECK(pack.has_value() && st.multiColor == 1u); // the #123456 circle
        auto fset = pack ? IconSet::fromBytes(*pack) : std::unexpected(std::string("no pack"));
        UI_CHECK(fset.has_value());
        if (fset) {
            auto parsed = SvgImage::parse(kFeatureSvg);
            UI_CHECK(parsed && fset->multiColor(0));
            const Image got = draw(fset->image(0), 48, Color::rgb(0xFF0000));
            if (parsed) UI_CHECK(draw(*parsed, 48, Color::rgb(0xFF0000)).pixels == got.pixels); // not monochrome: untouched
            auto at = [&](int x, int y) { return got.at(x, y); };
            UI_CHECK(at(8, 8) == 0xFF000000u);                                  // black rect on black: its own colour kept
            UI_CHECK(ch(at(24, 8), 0) == 0x12 && ch(at(24, 8), 2) == 0x56);     // the circle's #123456
            UI_CHECK(ch(at(8, 22), 0) == 0xFF && ch(at(8, 22), 1) == 0);        // currentColor stroke: the tint
            UI_CHECK(at(8, 26) == 0xFF000000u);                                 // below the 4 px stroke
            UI_CHECK(at(41, 43) == 0xFF000000u && at(24, 30) == 0xFF000000u);   // outside the half disk / empty cell
        }
    }

    // colour classification on the multi-colour test set
    {
        auto cdir = ic::loadSvgDirectory(FUSE_UI_ICON_COLOR_DIR);
        UI_CHECK(cdir.has_value());
        ic::CookStats cst;
        auto cp = cdir ? ic::cookIcons(*cdir, &cst) : std::unexpected(std::string("no dir"));
        UI_CHECK(cp.has_value());
        if (cp) {
            UI_CHECK(readBytes(FUSE_UI_ICON_COLOR_PACK) == *cp); // the build-time pack (tool) is byte-identical
            UI_CHECK_EQ(cst.icons, 6u);
            UI_CHECK_EQ(cst.multiColor, 5u);
            auto cs = IconSet::fromBytes(*cp);
            UI_CHECK(cs.has_value());
            if (cs) {
                const std::pair<const char*, bool> want[] = {{"alert-color", true}, {"badge-check", true}, {"flag", true},
                                                             {"ink-square", false}, {"sunset", true}, {"traffic-light", true}};
                for (const auto& [name, multi] : want) {
                    const i32 i = cs->find(name);
                    UI_CHECK(i >= 0);
                    if (i >= 0) UI_CHECK_EQ(cs->multiColor(u32(i)), multi);
                }
                // black-only icon: fully tinted (black fills follow the colour like currentColor)
                const Image ink = draw(cs->image(u32(cs->find("ink-square"))), 24, Color::rgb(0x00FF00));
                UI_CHECK(ink.at(5, 5) == 0xFF00FF00u && ink.at(12, 12) == 0xFF00FF00u && ink.at(9, 9) == 0xFF000000u);
                // multi-colour: fixed colours stay, currentColor parts take the tint, a gradient stays a gradient
                const Image flag = draw(cs->image(u32(cs->find("flag"))), 24, Color::rgb(0x00FF00));
                UI_CHECK(ch(flag.at(15, 4), 0) == 0xD3 && ch(flag.at(15, 4), 1) == 0x2F); // red stripe #d32f2f
                UI_CHECK(flag.at(4, 18) == 0xFF00FF00u);                                  // the pole: currentColor
                const Image sun = draw(cs->image(u32(cs->find("sunset"))), 24, Color::rgb(0x00FF00));
                UI_CHECK(sun.at(12, 6) != sun.at(12, 16) && ch(sun.at(12, 6), 0) == 0xFF);  // top and bottom of the gradient
                UI_CHECK(sun.at(12, 19) == 0xFF00FF00u);
                const Image alert = draw(cs->image(u32(cs->find("alert-color"))), 24, Color::rgb(0x00FF00));
                UI_CHECK(alert.at(12, 11) == 0xFF000000u); // "#000000" in a multi-colour icon stays black
            }
        }
    }

    // cooker errors name the problem; nothing is produced
    UI_CHECK(!ic::cookIcons({{"a", "<svg width='8' height='8'><rect width='8' height='8'/></svg>"}, {"a", "<svg width='8' height='8'><rect width='8' height='8'/></svg>"}}));
    UI_CHECK(!ic::cookIcons({{"Bad Name", kFeatureSvg}}));
    UI_CHECK(!ic::cookIcons({{"empty", "<svg width='8' height='8'></svg>"}}));
    UI_CHECK(!ic::cookIcons({{"junk", "not an svg at all"}}));
    UI_CHECK(!ic::cookIcons({}));
    {
        const auto r = ic::cookIcons({{"txt", "<svg width='8' height='8'><rect width='8' height='8'/><text x='1' y='7'>a</text></svg>"}});
        UI_CHECK(!r && r.error().find("txt") != std::string::npos && r.error().find("unsupported") != std::string::npos);
    }
    UI_CHECK(ic::cookIcons({{"ok", kFeatureSvg}}).has_value());

    // loader: every truncation fails cleanly, flipped bytes never crash, other versions are refused with a message
    for (std::size_t n = 0; n < a->size(); n += (n < 64 ? 1 : 97)) UI_CHECK(!IconSet::fromBytes(std::span<const u8>(a->data(), n)));
    UI_CHECK(!IconSet::fromBytes(std::span<const u8>(a->data(), a->size() - 1)));
    {
        std::vector<u8> more = *a;
        more.push_back(0);
        UI_CHECK(!IconSet::fromBytes(more)); // trailing bytes
        std::vector<u8> v2 = *a;
        v2[4] = 2;
        const auto old = IconSet::fromBytes(v2);
        UI_CHECK(!old && old.error().find("version 2") != std::string::npos);
        u32 seed = 12345u, loaded = 0;
        for (int k = 0; k < 4000; ++k) {
            std::vector<u8> bad = *a;
            seed = seed * 1664525u + 1013904223u;
            bad[12 + (seed >> 8) % (bad.size() - 12)] ^= u8(1u << (seed & 7u));
            if (auto s = IconSet::fromBytes(bad)) { // a flipped coordinate still loads: it must stay drawable
                ++loaded;
                for (u32 i = 0; i < s->count(); i += 7) (void)draw(s->image(i), 16, Color::rgb(0xFFFFFF));
            }
        }
        std::printf("fuzz: %u of 4000 single-bit flips still load\n", loaded);
    }
    UI_CHECK(!IconSet::fromFile("no/such/pack.fuseicons"));

#ifdef FUSE_UI_ICON_COOK_EXE
    // the tool: an error leaves no pack behind (and keeps an existing one); a good run loads
    {
        const fs::path tmp = fs::temp_directory_path() / "fuse_ui_icon_cook_gate";
        std::error_code ec;
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp / "good");
        fs::create_directories(tmp / "bad");
        std::ofstream(tmp / "good" / "ok.svg") << "<svg width='8' height='8'><rect width='8' height='8'/></svg>";
        std::ofstream(tmp / "bad" / "ok.svg") << "<svg width='8' height='8'><rect width='8' height='8'/></svg>";
        std::ofstream(tmp / "bad" / "broken.svg") << "<svg width='8' height='8'><rect width='8'";
        auto cook = [&](const fs::path& out, const char* dirName) {
            const std::string cmd = std::string("\"") + FUSE_UI_ICON_COOK_EXE + "\" \"--out=" + out.string() + "\" \"" +
                                    (tmp / dirName).string() + "\" > \"" + (tmp / "log.txt").string() + "\" 2>&1";
            return std::system(cmd.c_str());
        };
        const fs::path fresh = tmp / "fresh.fuseicons", kept = tmp / "kept.fuseicons";
        UI_CHECK(cook(fresh, "bad") != 0);
        UI_CHECK(!fs::exists(fresh));
        std::ofstream(kept) << "previous pack";
        UI_CHECK(cook(kept, "bad") != 0);
        UI_CHECK(readBytes(kept.string()).size() == 13u);
        UI_CHECK_EQ(cook(kept, "good"), 0);
        auto good = IconSet::fromFile(kept.string());
        UI_CHECK(good.has_value() && good->count() == 1u && good->find("ok") == 0);
        fs::remove_all(tmp, ec);
    }
#endif
    return finish("fuse_ui_icon_cook");
}

// ---- render ---------------------------------------------------------------------------------------------------------
constexpr const char* kNames[] = {"save", "folder-open", "search", "settings", "triangle-alert", "star"};
constexpr int kSizes[] = {16, 24, 32, 40, 48};

void buildScene(UiContext& ui) {
    (void)ui.addStyleSheet(R"css(
        .rows { padding: 12px; gap: 8px; }
        .row { flex-direction: row; gap: 6px; align-items: center; }
        .accent { color: #3d8bfd; }
        .tb { color: #e6e6eb; }
        .tb:hover { color: #ffcc00; }
        .tb:disabled { color: #9a9cab; }
        icon { color: #e6e6eb; }
        icon.accent { color: #3d8bfd; }
    )css");
    const NodeHandle rows = w::panel(ui, ui.root(), "rows");
    ui.addClass(rows, "rows");
    for (int size : kSizes) {
        const NodeHandle row = w::panel(ui, rows);
        ui.addClass(row, "row");
        for (const char* n : kNames) w::icon(ui, row, n, f32(size));
        for (const char* n : kNames) ui.addClass(w::icon(ui, row, n, f32(size)), "accent");
        const std::string id = "tb" + std::to_string(size);
        const NodeHandle tb = w::toolButton(ui, row, "", id);
        ui.addClass(tb, "tb");
        UI_CHECK(w::setIcon(ui, tb, "pencil", f32(size)));
        const NodeHandle off = w::toolButton(ui, row, "", id + "off");
        ui.addClass(off, "tb");
        UI_CHECK(w::setIcon(ui, off, "pencil", f32(size)));
        ui.setDisabled(off, true);
    }
}

// Brightest pixel inside a rect (an icon's fully covered core carries the exact tint).
u32 brightest(const Image& img, Rect r) {
    u32 best = 0;
    int bestSum = -1;
    for (int y = std::max(0, int(r.y)); y < std::min(img.height, int(r.bottom())); ++y)
        for (int x = std::max(0, int(r.x)); x < std::min(img.width, int(r.right())); ++x) {
            const u32 p = img.at(x, y);
            const int s = ch(p, 0) + ch(p, 1) + ch(p, 2);
            if (s > bestSum) { bestSum = s; best = p; }
        }
    return best;
}
bool near(u32 p, u32 rgb, int tol = 3) {
    return std::abs(ch(p, 0) - int((rgb >> 16) & 0xFF)) <= tol && std::abs(ch(p, 1) - int((rgb >> 8) & 0xFF)) <= tol &&
           std::abs(ch(p, 2) - int(rgb & 0xFF)) <= tol;
}
int litIn(const Image& img, Rect r, f32 scale = 1.f) {
    int n = 0;
    for (int y = int(r.y * scale); y < int(r.bottom() * scale); ++y)
        for (int x = int(r.x * scale); x < int(r.right() * scale); ++x) n += (img.at(x, y) & 0xFFFFFFu) != 0;
    return n;
}

/// Compares against (or, with FUSE_UI_UPDATE_GOLDEN=1, rewrites) a golden PNG: at most 0.1 % of the pixels may differ by
/// more than 3 per channel.
void golden(const Image& img, const std::string& path) {
    const char* update = std::getenv("FUSE_UI_UPDATE_GOLDEN");
    if (update && update[0] == '1') {
        stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        std::printf("golden written: %s\n", path.c_str());
        return;
    }
    int gw = 0, gh = 0, n = 0;
    unsigned char* ref = stbi_load(path.c_str(), &gw, &gh, &n, 4);
    UI_CHECK(ref != nullptr);
    if (!ref) {
        std::fprintf(stderr, "missing golden %s (run from the tests directory; FUSE_UI_UPDATE_GOLDEN=1 writes it)\n", path.c_str());
        return;
    }
    UI_CHECK(gw == img.width && gh == img.height);
    if (gw == img.width && gh == img.height) {
        int bad = 0, maxd = 0;
        for (int i = 0; i < gw * gh; ++i)
            for (int k = 0; k < 4; ++k) {
                const int d = std::abs(int(ref[i * 4 + k]) - ch(img.pixels[std::size_t(i)], k));
                maxd = std::max(maxd, d);
                if (d > 3) { ++bad; break; }
            }
        std::printf("%s diff: %d pixels > 3, max channel diff %d\n", path.c_str(), bad, maxd);
        UI_CHECK(bad <= gw * gh / 1000);
    }
    stbi_image_free(ref);
}

int renderGate(int argc, char** argv) {
    auto set = IconSet::fromFile(FUSE_UI_ICON_PACK);
    UI_CHECK(set.has_value());
    if (!set) { std::fprintf(stderr, "%s\n", set.error().c_str()); return finish("fuse_ui_icon_render"); }
    std::string out;
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--out=", 6) == 0) out = argv[i] + 6;

    // ---- the scene: icon nodes at five sizes, a tinted class, tool buttons with an icon by name ----
    {
        UiContext ui{{.viewport = {700, 260}, .font = &fuse::ui::test::font()}};
        w::install(ui);
        ui.setIcons(&*set);
        UI_CHECK(ui.icons() == &*set);
        buildScene(ui);
        ui.update();
        const NodeHandle rows = ui.find("rows");
        const NodeHandle row0 = ui.firstChild(rows);
        const NodeHandle first = ui.firstChild(row0);
        UI_CHECK_RECT(ui.rect(first), 12, 16, 16, 16); // 16 px cell, centred in the row (the tool button is taller)
        UI_CHECK_EQ(ui.computed(first).icon, hashId("save"));
        UI_CHECK_EQ(ui.computed(first).iconSize, 16.f);
        Image img;
        fuse::ui::test::render(ui, img);
        if (!out.empty()) stbi_write_png((out + "/icons.png").c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4);
        // tint: the theme colour, a class, a disabled button
        NodeHandle accent = first;
        for (int k = 0; k < 6; ++k) accent = ui.nextSibling(accent);
        UI_CHECK(near(brightest(img, ui.rect(first)), 0xE6E6EB));
        UI_CHECK(near(brightest(img, ui.rect(accent)), 0x3D8BFD));
        UI_CHECK(near(brightest(img, ui.rect(ui.find("tb24"))), 0xE6E6EB));
        { // a disabled button: its own (dimmer) colour, whatever the theme makes of :disabled
            const NodeHandle off = ui.find("tb24off");
            const Color oc = ui.computed(off).color;
            const u32 got = brightest(img, ui.rect(off));
            std::printf("disabled icon: computed colour %.2f %.2f %.2f a %.2f, opacity %.2f, drawn %06x\n", double(oc.r), double(oc.g),
                        double(oc.b), double(oc.a), double(ui.computed(off).opacity), got & 0xFFFFFFu);
            const int sumOff = ch(got, 0) + ch(got, 1) + ch(got, 2);
            const u32 on = brightest(img, ui.rect(ui.find("tb24")));
            UI_CHECK(sumOff > 60 && sumOff < ch(on, 0) + ch(on, 1) + ch(on, 2) - 60);
        }
        // every icon cell has ink, bigger cells have more
        for (NodeHandle row = row0; row.valid(); row = ui.nextSibling(row))
            for (NodeHandle n = ui.firstChild(row); n.valid(); n = ui.nextSibling(n)) UI_CHECK(litIn(img, ui.rect(n)) > 12);
        UI_CHECK(litIn(img, ui.rect(ui.find("tb48"))) > 3 * litIn(img, ui.rect(ui.find("tb16"))));
        golden(img, "golden/icons.png");

        // :hover re-tints the icon (it follows `color` like text)
        fuse::ui::test::UiTestDriver drive(ui);
        drive.hover("tb24");
        Image hov;
        fuse::ui::test::render(ui, hov);
        UI_CHECK(near(brightest(hov, ui.rect(ui.find("tb24"))), 0xFFCC00));
        UI_CHECK(near(brightest(hov, ui.rect(ui.find("tb32"))), 0xE6E6EB));

        // render scale 2: vector icons are drawn at device resolution (about 4x the pixels, not a scaled bitmap)
        const Rect cell = ui.rect(ui.find("tb48"));
        const int lit1 = litIn(img, cell);
        ui.setViewport({1400, 520});
        ui.setRenderScale(2.f);
        ui.update();
        Image img2;
        img2.resize(1400, 520, 0xFF000000u);
        CpuRasterizer r2;
        r2.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
        r2.render(ui.drawList(), img2);
        const int lit2 = litIn(img2, ui.rect(ui.find("tb48")), 2.f);
        std::printf("48 px icon: %d lit pixels at scale 1, %d at scale 2\n", lit1, lit2);
        UI_CHECK(lit2 > lit1 * 3 && lit2 < lit1 * 5);
    }

    // ---- the `icon` style property, setIcon by name, lookup order, layout ----
    {
        UiContext u2{{.viewport = {600, 200}, .font = &fuse::ui::test::font()}};
        w::install(u2);
        u2.setCss(u2.root(), "flex-direction", "row");
        u2.setCss(u2.root(), "align-items", "flex-start");
        const NodeHandle plain = w::button(u2, u2.root(), "Save", "plain");
        const NodeHandle withIcon = w::button(u2, u2.root(), "Save", "with");
        const NodeHandle builtinOnly = w::button(u2, u2.root(), "Save", "builtin");
        const NodeHandle unknown = w::button(u2, u2.root(), "Save", "unknown"); // last: it grows when the pack arrives
        const NodeHandle sheetBtn = w::button(u2, u2.root(), "Save", "sheet");
        // no pack attached: built-in names work, pack-only names are unknown (and take no room)
        UI_CHECK(w::setIcon(u2, withIcon, "save"));
        UI_CHECK(!w::setIcon(u2, unknown, "pencil"));      // Lucide only: unknown until the pack is attached
        UI_CHECK(w::setIcon(u2, builtinOnly, "close"));    // built-in only (Lucide calls it "x")
        UI_CHECK(!w::setIcon(u2, plain, "bad name!"));
        u2.update();
        UI_CHECK_EQ(u2.computed(withIcon).icon, hashId("save"));
        UI_CHECK_EQ(u2.rect(withIcon).w, u2.rect(plain).w + 16.f + 6.f);
        UI_CHECK_EQ(u2.rect(unknown).w, u2.rect(plain).w);
        UI_CHECK_EQ(u2.rect(builtinOnly).w, u2.rect(withIcon).w);
        Image before;
        fuse::ui::test::render(u2, before);
        // attach the pack: the pack-only name appears, the built-in-only name stays, "save" now comes from the pack
        u2.setIcons(&*set);
        u2.update();
        UI_CHECK_EQ(u2.rect(unknown).w, u2.rect(withIcon).w);
        UI_CHECK_EQ(u2.rect(builtinOnly).w, u2.rect(withIcon).w);
        Image after;
        fuse::ui::test::render(u2, after);
        auto same = [&](Rect r) {
            for (int y = int(r.y); y < int(r.bottom()); ++y)
                for (int x = int(r.x); x < int(r.right()); ++x)
                    if (before.at(x, y) != after.at(x, y)) return false;
            return true;
        };
        UI_CHECK(!same(u2.rect(withIcon)));   // Lucide's save is drawn instead of the built-in one
        UI_CHECK(same(u2.rect(builtinOnly))); // not in the pack: still the built-in icon, same pixels
        // the icon sits left of the text
        const Rect wr = u2.rect(withIcon);
        int iconX = -1, textX = -1;
        for (int x = int(wr.x); x < int(wr.right()) && textX < 0; ++x)
            for (int y = int(wr.y); y < int(wr.bottom()); ++y)
                if ((after.at(x, y) & 0xFFFFFFu) != (after.at(int(wr.x) + 1, int(wr.y) + 1) & 0xFFFFFFu)) {
                    if (iconX < 0) iconX = x;
                    else if (x > iconX + 20) textX = x;
                    break;
                }
        UI_CHECK(iconX >= int(wr.x) && textX > iconX);
        // CSS: quoted names, icon-size, none, bad values
        UI_CHECK(u2.setCss(plain, "icon", "\"search\""));
        UI_CHECK(u2.setCss(plain, "icon-size", "24px"));
        u2.update();
        UI_CHECK(u2.computed(plain).icon == hashId("search") && u2.computed(plain).iconSize == 24.f);
        UI_CHECK_EQ(u2.rect(plain).w, u2.rect(withIcon).w + 8.f);
        UI_CHECK(u2.rect(plain).h >= 24.f);
        UI_CHECK(u2.setCss(plain, "icon", "none"));
        UI_CHECK(!u2.setCss(plain, "icon", "bad name!"));
        u2.update();
        UI_CHECK_EQ(u2.computed(plain).icon, 0u);
        // a style sheet rule gives every tool button of a class its icon; clearIcon removes an inline one
        UI_CHECK(u2.addStyleSheet(".starred { icon: star; icon-size: 20px; }").has_value());
        u2.addClass(sheetBtn, "starred");
        u2.update();
        UI_CHECK(u2.computed(sheetBtn).icon == hashId("star") && u2.computed(sheetBtn).iconSize == 20.f);
        UI_CHECK_EQ(u2.rect(sheetBtn).w, u2.rect(builtinOnly).w + 4.f);
        w::clearIcon(u2, withIcon);
        u2.update();
        UI_CHECK_EQ(u2.computed(withIcon).icon, 0u);
        // detaching the pack: pack-only icons go away again
        u2.setIcons(nullptr);
        u2.update();
        UI_CHECK(u2.rect(unknown).w < u2.rect(builtinOnly).w);
    }

    // ---- multi-colour pack through the UI ----
    {
        auto cs = IconSet::fromFile(FUSE_UI_ICON_COLOR_PACK);
        UI_CHECK(cs.has_value());
        if (cs) {
            UiContext uc{{.viewport = {360, 120}, .font = &fuse::ui::test::font()}};
            w::install(uc);
            uc.setIcons(&*cs);
            (void)uc.addStyleSheet(".rows { padding: 12px; gap: 8px; } .row { flex-direction: row; gap: 8px; } icon { color: #e6e6eb; } .accent icon { color: #3d8bfd; }");
            const NodeHandle rows = w::panel(uc, uc.root(), "rows");
            uc.addClass(rows, "rows");
            for (int pass = 0; pass < 2; ++pass) {
                const NodeHandle row = w::panel(uc, rows, pass ? "row1" : "row0");
                uc.addClass(row, "row");
                if (pass) uc.addClass(row, "accent");
                for (u32 i = 0; i < cs->count(); ++i) w::icon(uc, row, cs->name(i), 40.f);
            }
            uc.update();
            Image ci;
            fuse::ui::test::render(uc, ci);
            if (!out.empty()) stbi_write_png((out + "/icons_color.png").c_str(), ci.width, ci.height, 4, ci.pixels.data(), ci.width * 4);
            // per icon: the tintable ones change with the row's colour, fixed colours do not
            NodeHandle a = uc.firstChild(uc.find("row0")), b = uc.firstChild(uc.find("row1"));
            for (u32 i = 0; i < cs->count(); ++i, a = uc.nextSibling(a), b = uc.nextSibling(b)) {
                const Rect ra = uc.rect(a), rb = uc.rect(b);
                int differ = 0, inked = 0;
                for (int y = 0; y < 40; ++y)
                    for (int x = 0; x < 40; ++x) {
                        const u32 pa = ci.at(int(ra.x) + x, int(ra.y) + y), pb = ci.at(int(rb.x) + x, int(rb.y) + y);
                        inked += (pa & 0xFFFFFFu) != 0;
                        differ += pa != pb;
                    }
                const std::string_view name = cs->name(i);
                if (name == "ink-square") UI_CHECK(differ > 100);                  // monochrome: all of it follows the tint
                else if (name == "badge-check") UI_CHECK(differ == 0 && inked > 100); // no currentColor paint at all
                else if (name == "alert-color") UI_CHECK(differ == 0); // default-black dot in a colour icon stays black
                else UI_CHECK(differ > 0 && differ < inked);          // some tintable parts, some fixed
            }
            golden(ci, "golden/icons_color.png");
        }
    }

#if FUSE_UI_ICON_HAVE_DOC
    // ---- documents and cooked style sheets carry icon names ----
    {
        UiContext u3{{.viewport = {300, 200}, .font = &fuse::ui::test::font()}};
        w::install(u3);
        const char* json = R"({"fuseui": 1, "root": {"class": "panel", "children": [
            {"class": "button", "id": "b", "text": "Edit", "icon": "pencil"},
            {"class": "toolbutton", "id": "t", "icon": "close"},
            {"class": "svgimage", "id": "s", "icon": "star", "style": {"width": "24px", "height": "24px"}}]}})";
        // without a pack "pencil" and "star" are unknown names: the load fails and says so
        UI_CHECK(!fuse::ui::doc::Document::load(u3, json));
        u3.setIcons(&*set);
        auto doc = fuse::ui::doc::Document::load(u3, json);
        UI_CHECK(doc.has_value());
        if (doc) {
            u3.update();
            UI_CHECK_EQ(u3.computed(u3.find("b")).icon, hashId("pencil"));
            UI_CHECK(w::iconOf(u3, u3.find("t")).valid()); // not in the pack: the built-in icon, set as before
            Image di;
            fuse::ui::test::render(u3, di);
            UI_CHECK(litIn(di, u3.rect(u3.find("s"))) > 30 && litIn(di, u3.rect(u3.find("t"))) > 10);
        }
        UI_CHECK(!fuse::ui::doc::Document::load(u3, R"({"fuseui": 1, "root": {"class": "button", "icon": "a b"}})"));
        UI_CHECK(!fuse::ui::doc::Document::load(u3, R"({"fuseui": 1, "root": {"class": "label", "icon": "save"}})")); // buttons only
        // the document cooker lists the icon names a document uses (what its pack must hold); a cooked document loads
        {
            fuse::ui::doc::CookInfo info;
            auto cookedDoc = fuse::ui::doc::cookDocument(json, &info);
            UI_CHECK(cookedDoc.has_value());
            UI_CHECK(info.icons.size() == 3 && info.icons[0] == "close" && info.icons[1] == "pencil" && info.icons[2] == "star");
            if (cookedDoc) {
                UiContext u5{{.viewport = {300, 200}, .font = &fuse::ui::test::font()}};
                w::install(u5);
                u5.setIcons(&*set);
                auto d5 = fuse::ui::doc::Document::loadCooked(u5, *cookedDoc);
                UI_CHECK(d5.has_value());
                if (d5) {
                    u5.update();
                    UI_CHECK_EQ(u5.computed(u5.find("b")).icon, hashId("pencil"));
                }
            }
        }
        // a cooked sheet keeps `icon` (a name hash) and `icon-size`
        auto cooked = fuse::ui::doc::cookStyle("#i { icon: star; icon-size: 32px; }");
        UI_CHECK(cooked.has_value());
        if (cooked) {
            auto sheet = fuse::ui::doc::loadCookedStyle(*cooked);
            UI_CHECK(sheet.has_value());
            if (sheet) {
                UiContext u4{{.viewport = {200, 100}, .font = &fuse::ui::test::font()}};
                w::install(u4);
                u4.setIcons(&*set);
                u4.addStyleSheet(std::move(*sheet));
                const NodeHandle i = w::icon(u4, u4.root(), "none", 0.f, "i");
                u4.update();
                UI_CHECK(u4.computed(i).icon == hashId("star") && u4.computed(i).iconSize == 32.f && u4.rect(i).h == 32.f);
            }
        }
    }
#endif
    return finish("fuse_ui_icon_render");
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view gate = FUSE_UI_ICON_GATE;
    if (gate == "cook") return cookGate();
    if (gate == "render") return renderGate(argc, argv);
    std::fprintf(stderr, "unknown gate\n");
    return 2;
}
