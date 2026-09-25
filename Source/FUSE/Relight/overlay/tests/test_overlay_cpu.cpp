// FUSE Relight RL-6.1 CPU gates: the developer overlay's state machine, input rules, UI, rasteriser and menu actions
// (docs/plans/FUSE_REMIX_PORT_PLAN.md RL-6.1 "UI state-machine tests").
//
//   font         every printable glyph has ink (space excepted), unknown characters draw '?'.
//   ui           click = press and release over the same widget (in separate passes), press-drag-release elsewhere is
//                no click, clipped widgets are neither drawn nor hit, text-field typing / backspace, widget lookup.
//   input        the window-message rules: Alt+X toggles in both states (repeat ignored, the chord's WM_SYSCHAR /
//                WM_SYSKEYUP swallowed), hidden forwards everything else unchanged, shown consumes keyboard / mouse /
//                WM_INPUT and maps client to back-buffer pixels; queue overflow is counted, never allocates.
//   script       the script grammar (every command, errors with line numbers) and the messages of each command.
//   raster       compose rule identities (transparent layer = passthrough, opaque = the layer, BGRA round trip), the
//                rasteriser's text / fill coverage, composeRegionCpu touches only the panel rectangle.
//   debug_map    debug-view mapping (scale / bias / clamp, depth grey, motion rg0, alpha kept).
//   hidden       the state machine through scripted window messages: hidden -> every game message forwarded, none
//                consumed, no menu event; toggle -> shown consumes; toggle -> hidden forwards again.
//   menu_options scripted edit of a bool / int / float option (filter, click, +), Save: the rtx.conf layer holds the
//                values, the file has them, a fresh option system reads them back (persistence).
//   menu_tags    scripted click-to-tag of the listed textures into the RL-1.2 categories (sky / UI / ignore): the hash
//                lists resolve with the hashes, Save writes them to rtx.conf; a second click untags.
//   menu_debug   the Debug tab selects relight.overlay.debugView; unavailable views are marked.
//   layout       overlay_types.h OverlayPush == the push-constant block of shaders/overlay_compose.{comp,slang}.
//   zero_alloc   steady-state hidden frames (window messages through the hook core + update): no operator new.
#include <fuse/relight/overlay/dev_menu.hpp>
#include <fuse/relight/overlay/font5x7.hpp>
#include <fuse/relight/overlay/input.hpp>
#include <fuse/relight/overlay/overlay_options.hpp>
#include <fuse/relight/overlay/overlay_types.h>
#include <fuse/relight/overlay/raster.hpp>
#include <fuse/relight/overlay/script.hpp>
#include <fuse/relight/overlay/ui.hpp>
#include <fuse/relight/overlay/win32_hook.hpp>

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_layer.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/scene/classify/classify_options.hpp>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <sstream>
#include <string>
#include <vector>

// --- allocation counter (operator new) -----------------------------------------------------------
namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace fuse::relight::overlay;
namespace opt = fuse::relight::options;
namespace fs = std::filesystem;

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

/// Options only this test uses (edited through the Options tab).
struct TestOptions {
    FUSE_RELIGHT_OPTION("rtx", bool, overlayTestFlag, false, "RL-6.1 test option (bool).");
    FUSE_RELIGHT_OPTION("rtx", std::int32_t, overlayTestCount, 0, "RL-6.1 test option (int).");
    FUSE_RELIGHT_OPTION("rtx", float, overlayTestScale, 1.0f, "RL-6.1 test option (float).");
};

constexpr std::uint32_t kW = 640, kH = 480;

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

/// A fresh option system over `dir` (rtx.conf / user.conf there).
void initOptions(const fs::path& dir) {
    opt::OptionSystem::shutdown();
    opt::OptionSystemDesc d;
    d.baseDirectory = dir.string();
    d.loadEnvironmentVariables = false;
    opt::OptionSystem::initialize(d);
}

fs::path freshDir(const char* name) {
    const fs::path dir = fs::current_path() / "overlay_tmp" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::string runScriptText(OverlayCore& core, WindowHook& hook, const char* text, std::uint64_t frame,
                          const MenuFrame& mf) {
    Script s;
    if (!s.parse(text)) {
        return "parse: " + s.error();
    }
    std::string error;
    core.runScript(s, frame, hook, kW, kH, mf, &error);
    return error;
}

// ---- suites ------------------------------------------------------------------------------------------------------

void testFont() {
    for (int c = 33; c <= 126; ++c) {
        const std::uint8_t* g = glyph5x7(static_cast<char>(c));
        int ink = 0;
        for (int r = 0; r < kGlyphHeight; ++r) {
            ink += (g[r] & 0x1f) != 0 ? 1 : 0;
            check((g[r] & ~0x1fu) == 0, "glyph rows use 5 bits");
        }
        check(ink > 0, std::string("glyph '") + static_cast<char>(c) + "' has ink");
    }
    const std::uint8_t* space = glyph5x7(' ');
    for (int r = 0; r < kGlyphHeight; ++r) {
        check(space[r] == 0, "space is blank");
    }
    check(glyph5x7('\x07') == glyph5x7('?'), "control characters draw '?'");
}

void testUi() {
    Ui ui;
    const Rect panel{10, 10, 200, 2 * kLineHeight + 4}; // two lines fit
    bool clicked = false;
    auto build = [&](const InputEvent* e) {
        ui.begin(panel, 1, e);
        clicked = ui.button("b.one", "One");
        ui.sameLine();
        ui.button("b.two", "Two");
        ui.button("b.three", "Three");
        ui.button("b.four", "Four"); // third line: clipped
        ui.end();
    };
    build(nullptr);
    Rect one, two, four;
    check(ui.widgetRect("b.one", one) && ui.widgetRect("b.two", two), "laid out widgets are found");
    check(one.y == two.y && two.x > one.x + one.w - 1, "sameLine places the second button right of the first");
    check(!ui.widgetRect("b.four", four), "a clipped widget is not laid out");
    const InputEvent down{EventType::MouseDown, one.x + 2, one.y + 2, 0};
    const InputEvent up{EventType::MouseUp, one.x + 2, one.y + 2, 0};
    build(&down);
    check(!clicked, "press alone is no click");
    build(&up);
    check(clicked, "press + release over the widget clicks");
    build(nullptr);
    check(!clicked, "no click without an event");
    const InputEvent upElsewhere{EventType::MouseUp, two.x + 2, two.y + 2, 0};
    build(&down);
    build(&upElsewhere);
    check(!clicked, "press on one widget, release on another: no click");
    const InputEvent upOutside{EventType::MouseUp, 500, 400, 0};
    build(&down);
    build(&upOutside);
    check(!clicked, "release outside: no click");
    // Draw list: panel + buttons, all inside the panel.
    for (const DrawCmd& c : ui.draws().cmds()) {
        check(c.x >= panel.x && c.y >= panel.y && c.x + (c.kind == DrawCmd::Kind::Fill ? c.w : 0) <= panel.x + panel.w &&
                  c.y + c.h <= panel.y + panel.h,
              "draw commands stay inside the panel");
    }

    // Text field.
    std::string text = "x";
    auto field = [&](const InputEvent* e) {
        ui.begin(panel, 1, e);
        const bool changed = ui.textField("f", text, 10);
        ui.end();
        return changed;
    };
    field(nullptr);
    Rect f;
    check(ui.widgetRect("f", f), "text field laid out");
    const InputEvent fdown{EventType::MouseDown, f.x + 1, f.y + 1, 0};
    const InputEvent fup{EventType::MouseUp, f.x + 1, f.y + 1, 0};
    field(&fdown);
    field(&fup);
    check(ui.focused() == widgetId("f"), "clicking a text field focuses it");
    const InputEvent a{EventType::Char, 0, 0, 'a'}, b{EventType::Char, 0, 0, 'b'}, back{EventType::KeyDown, 0, 0, kVkBack};
    check(field(&a) && field(&b) && text == "xab", "typed characters append");
    check(field(&back) && text == "xa", "backspace removes the last character");
    const InputEvent elsewhere{EventType::MouseDown, 1, 1, 0};
    field(&elsewhere);
    check(ui.focused() == 0, "pressing elsewhere drops the focus");
    check(!field(&a) && text == "xa", "an unfocused field ignores characters");
}

void testInput() {
    const CoordMap identity{};
    using namespace win32;
    Translation t = translateMessage(kWmSysKeyDown, kVkX, kAltContextBit | 1, false, identity);
    check(t.consume && t.event.type == EventType::Toggle, "Alt+X toggles while hidden");
    t = translateMessage(kWmSysKeyDown, kVkX, kAltContextBit | 1, true, identity);
    check(t.consume && t.event.type == EventType::Toggle, "Alt+X toggles while shown");
    t = translateMessage(kWmSysKeyDown, kVkX, kAltContextBit | kRepeatBit | 1, false, identity);
    check(t.consume && t.event.type == EventType::None, "auto-repeat of Alt+X does not toggle again");
    t = translateMessage(kWmSysChar, 'x', kAltContextBit | 1, false, identity);
    check(t.consume && t.event.type == EventType::None, "the chord's WM_SYSCHAR is swallowed");
    t = translateMessage(kWmSysKeyUp, kVkX, kAltContextBit | 1, false, identity);
    check(t.consume, "the chord's WM_SYSKEYUP is swallowed");
    t = translateMessage(kWmKeyDown, kVkX, 1, false, identity);
    check(!t.consume && t.event.type == EventType::None, "X without Alt reaches the game");
    t = translateMessage(kWmSysKeyDown, 'F', kAltContextBit | 1, false, identity);
    check(!t.consume, "other Alt chords reach the game while hidden");
    for (std::uint32_t msg : {kWmKeyDown, kWmKeyUp, kWmChar, kWmMouseMove, kWmLButtonDown, kWmLButtonUp, kWmMouseWheel,
                              kWmInput, 0x0010u /*WM_CLOSE*/, 0x0005u /*WM_SIZE*/}) {
        t = translateMessage(msg, 'W', makeLParam(3, 4), false, identity);
        check(!t.consume && t.event.type == EventType::None, "hidden: message forwarded unchanged");
    }
    for (std::uint32_t msg : {kWmKeyDown, kWmKeyUp, kWmChar, kWmMouseMove, kWmLButtonDown, kWmLButtonUp, kWmMouseWheel,
                              kWmInput, kWmSysKeyDown}) {
        t = translateMessage(msg, 'W', makeLParam(3, 4), true, identity);
        check(t.consume, "shown: keyboard / mouse / raw input consumed");
    }
    for (std::uint32_t msg : {0x0010u, 0x0005u, 0x0006u /*WM_ACTIVATE*/, kWmKillFocus}) {
        t = translateMessage(msg, 0, 0, true, identity);
        check(!t.consume, "shown: window management still reaches the game");
    }
    const CoordMap half{256, 192, 128, 96};
    t = translateMessage(kWmLButtonDown, 1, makeLParam(100, 50), true, half);
    check(t.event.type == EventType::MouseDown && t.event.x == 50 && t.event.y == 25, "client -> back-buffer pixels");
    t = translateMessage(kWmMouseMove, 0, makeLParam(-3, 7), true, identity);
    check(t.event.x == -3 && t.event.y == 7, "signed client coordinates");
    t = translateMessage(kWmMouseWheel, static_cast<std::uint64_t>(static_cast<std::uint16_t>(-240)) << 16, 0, true,
                         identity);
    check(t.event.type == EventType::Wheel && t.event.value == -2, "wheel notches");
    t = translateMessage(kWmChar, 'q', 1, true, identity);
    check(t.event.type == EventType::Char && t.event.value == 'q', "WM_CHAR -> Char");

    InputHookCore core;
    core.setVisible(true);
    for (std::size_t i = 0; i < InputHookCore::kQueueCapacity + 10; ++i) {
        core.handle(kWmMouseMove, 0, makeLParam(static_cast<std::int32_t>(i), 0));
    }
    check(core.dropped() == 10 && core.consumed() == InputHookCore::kQueueCapacity + 10, "overflow counted");
    InputEvent out[InputHookCore::kQueueCapacity];
    check(core.drain(out, InputHookCore::kQueueCapacity) == InputHookCore::kQueueCapacity && out[0].x == 0 &&
              out[InputHookCore::kQueueCapacity - 1].x == static_cast<std::int32_t>(InputHookCore::kQueueCapacity - 1),
          "queue keeps the oldest events in order");
    check(core.drain(out, 4) == 0, "drained");
}

void testScript() {
    Script s;
    check(s.parse("# comment\n2 click 10 20\n0 toggle   # first\n1 clickw tab.opts\n1 type hello world\n1 key pgdn\n"
                  "1 key 65\n3 wheel -2\n3 game 5\n3 move 4 5\n"),
          "script parses: " + s.error());
    check(s.commands().size() == 9, "9 commands");
    check(s.commands()[0].kind == ScriptCommand::Kind::Toggle && s.commands()[0].frame == 0, "sorted by frame");
    std::size_t b = 0, e = 0;
    s.range(1, b, e);
    check(e - b == 4 && s.commands()[b].text == "tab.opts" && s.commands()[b + 1].text == "hello world" &&
              s.commands()[b + 2].value == kVkNext && s.commands()[b + 3].value == 65,
          "frame 1 commands in file order");
    s.range(7, b, e);
    check(b == e, "no commands for frame 7");
    Script bad;
    check(!bad.parse("0 toggle\n1 jump 3\n") && bad.error().find("line 2") != std::string::npos, "unknown command");
    check(!bad.parse("x toggle\n"), "bad frame");
    check(!bad.parse("0 click 3\n"), "click needs X Y");
    check(!bad.parse("0 toggle now\n"), "trailing text");
    std::vector<WindowMessage> m;
    Script::messages(s.commands()[0], 0, 0, m);
    check(m.size() == 3 && m[0].msg == win32::kWmSysKeyDown, "toggle = Alt+X key down / char / up");
    InputHookCore core;
    for (const WindowMessage& w : m) {
        core.handle(w.msg, w.wParam, w.lParam);
    }
    InputEvent ev[8];
    check(core.drain(ev, 8) == 1 && ev[0].type == EventType::Toggle && core.consumed() == 3,
          "the toggle messages give exactly one Toggle and are all consumed");
    m.clear();
    Script::messages(s.commands()[7], 0, 0, m);
    check(s.commands()[7].kind == ScriptCommand::Kind::Game && m.size() == 5, "game N = N messages");
}

void testRaster() {
    for (std::uint32_t raw : {0x00000000u, 0xffffffffu, 0x80402010u, 0x12345678u}) {
        check(composeTexel(raw, 0u, false) == raw && composeTexel(raw, 0u, true) == raw, "transparent layer: unchanged");
        const Color opaque = rgba(10, 20, 30, 255);
        const std::uint32_t outRgba = composeTexel(raw, opaque, false);
        check((outRgba & 0x00ffffffu) == 0x001e140au && (outRgba >> 24) == (raw >> 24), "opaque layer: its colour, alpha kept");
        const std::uint32_t outBgra = composeTexel(raw, opaque, true);
        check((outBgra & 0x00ffffffu) == 0x000a141eu, "opaque layer on BGRA: channels swapped");
        check(fromRgba(toRgba(raw, true), true) == raw, "BGRA round trip");
    }
    // Half-transparent black over white: 255 * (255 - 128) / 255 rounded.
    check((composeTexel(0xffffffffu, rgba(0, 0, 0, 128), false) & 0xffu) == 127u, "compose rounding");

    DrawList dl;
    const Rect panel{5, 5, 40, 20};
    dl.fill(panel, rgba(0, 0, 0, 255));
    dl.text(7, 7, "I", rgba(255, 255, 255, 255), 1, 30);
    std::vector<Color> layer;
    rasterize(dl, panel, layer);
    check(layer.size() >= 800u, "layer covers the panel");
    int white = 0;
    for (std::size_t i = 0; i < 800u; ++i) {
        white += layer[i] == rgba(255, 255, 255, 255) ? 1 : 0;
    }
    int ink = 0;
    for (int r = 0; r < kGlyphHeight; ++r) {
        for (int c = 0; c < kGlyphWidth; ++c) {
            ink += (glyph5x7('I')[r] >> c) & 1;
        }
    }
    check(white == ink, "text coverage == the glyph's ink (" + std::to_string(white) + " vs " + std::to_string(ink) + ")");

    // composeRegionCpu over the whole frame: only the panel rectangle changes.
    OverlayPush p;
    p.regionW = p.frameW = 64;
    p.regionH = p.frameH = 48;
    p.panelX = panel.x;
    p.panelY = panel.y;
    p.panelW = static_cast<std::uint32_t>(panel.w);
    p.panelH = static_cast<std::uint32_t>(panel.h);
    p.flags = kOverlayFlagBgra;
    std::vector<std::uint32_t> region(64u * 48u);
    for (std::size_t i = 0; i < region.size(); ++i) {
        region[i] = static_cast<std::uint32_t>(i * 2654435761u);
    }
    const std::vector<std::uint32_t> before = region;
    composeRegionCpu(p, DebugMapping{}, nullptr, layer, region);
    int changedOutside = 0, changedInside = 0;
    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * 64u + static_cast<std::size_t>(x);
            const bool inside = panel.contains(x, y);
            if (region[i] != before[i]) {
                (inside ? changedInside : changedOutside) += 1;
            }
        }
    }
    check(changedOutside == 0 && changedInside > 700, "compose changes only the panel rectangle");
}

void testDebugMap() {
    const float t[4] = {0.25f, 2.0f, -1.0f, 0.5f};
    DebugMapping m;
    std::uint32_t out = debugTexel(t, m, false, 0xAB000000u);
    check(out == 0xAB00FF40u, "rgb clamp and round, alpha kept (" + std::to_string(out) + ")");
    m.swizzle = kOverlaySwizzleRrr;
    out = debugTexel(t, m, false, 0);
    check((out & 0xffffffu) == 0x404040u, "depth: grey from red");
    m.swizzle = kOverlaySwizzleRg0;
    m.scale[0] = m.scale[1] = 0.5f;
    m.bias[0] = m.bias[1] = 0.5f;
    out = debugTexel(t, m, true, 0);
    // r = 0.625 -> 159, g = 1.5 -> 255, b = 0; BGRA order.
    check((out & 0xffffffu) == (159u << 16 | 255u << 8), "motion: rg * 0.5 + 0.5, blue 0, BGRA");
}

MenuFrame emptyFrame() { return MenuFrame{}; }

void testHidden() {
    OverlayConfig cfg;
    OverlayCore core(cfg);
    WindowHook hook; // no window: messages go straight to the core
    const MenuFrame mf = emptyFrame();
    check(runScriptText(core, hook, "0 game 40\n", 0, mf).empty(), "script");
    check(!core.visible() && core.input().forwarded() == 40 && core.input().consumed() == 0 && core.events() == 0,
          "hidden: 40 game messages forwarded, none consumed, no menu event");
    check(runScriptText(core, hook, "1 toggle\n", 1, mf).empty() && core.visible(), "toggle shows");
    check(core.input().consumed() == 3 && core.input().forwarded() == 40, "the chord is consumed");
    check(runScriptText(core, hook, "2 game 8\n", 2, mf).empty(), "script");
    check(core.input().consumed() == 11 && core.input().forwarded() == 40, "shown: game input consumed");
    check(runScriptText(core, hook, "3 toggle\n4 game 4\n", 3, mf).empty() && !core.visible(), "toggle hides");
    check(runScriptText(core, hook, "4 game 4\n", 4, mf).empty(), "script");
    check(core.input().forwarded() == 44 && core.input().consumed() == 14, "hidden again: forwarded");
    check(core.togglesApplied() == 2, "two toggles applied");

    OverlayConfig visibleCfg;
    visibleCfg.startVisible = true;
    OverlayCore shown(visibleCfg);
    check(shown.visible() && shown.input().visible(), "relight.overlay.startVisible");
}

void testMenuOptions() {
    const fs::path dir = freshDir("options");
    initOptions(dir);
    check(!TestOptions::overlayTestFlag() && TestOptions::overlayTestCount() == 0, "defaults");
    OverlayConfig cfg;
    OverlayCore core(cfg);
    WindowHook hook;
    const MenuFrame mf = emptyFrame();
    const std::string err = runScriptText(core, hook,
                                          "0 toggle\n0 clickw tab.opts\n0 clickw opt.filter\n0 type overlayTest\n"
                                          "0 clickw opt.rtx.overlayTestFlag\n0 clickw opt.rtx.overlayTestCount.inc\n"
                                          "0 clickw opt.rtx.overlayTestCount.inc\n0 clickw opt.rtx.overlayTestScale.inc\n"
                                          "0 clickw save\n",
                                          0, mf);
    check(err.empty(), "scripted option edit: " + err);
    check(core.menu().tab() == Tab::Options, "Options tab");
    check(TestOptions::overlayTestFlag(), "bool toggled");
    check(TestOptions::overlayTestCount() == 2, "int stepped twice (" + std::to_string(TestOptions::overlayTestCount()) + ")");
    check(std::fabs(TestOptions::overlayTestScale() - 1.1f) < 1e-6f, "float stepped by 10%");
    check(core.menu().edits() == 4 && core.menu().saves() == 1, "4 edits, 1 save (" + core.menu().status() + ")");
    const opt::OptionLayer* rtx = opt::OptionLayer::getRtxConfLayer();
    check(rtx && TestOptions::overlayTestFlag.hasValueInLayer(rtx) && TestOptions::overlayTestCount.hasValueInLayer(rtx),
          "edits land in the rtx.conf layer");
    check(rtx && !rtx->hasUnsavedChanges(), "saved: no unsaved changes");
    const std::string conf = readFile(dir / "rtx.conf");
    check(conf.find("rtx.overlayTestFlag = True") != std::string::npos &&
              conf.find("rtx.overlayTestCount = 2") != std::string::npos &&
              conf.find("rtx.overlayTestScale = 1.1") != std::string::npos,
          "rtx.conf has the edits:\n" + conf);
    // Persistence: a fresh option system reads them back.
    opt::OptionSystem::shutdown();
    check(!TestOptions::overlayTestFlag() && TestOptions::overlayTestCount() == 0, "shutdown restores defaults");
    initOptions(dir);
    check(TestOptions::overlayTestFlag() && TestOptions::overlayTestCount() == 2, "reloaded from rtx.conf");
    // Filtering: the list holds the three test options; a scroll key keeps the first row.
    Rect r;
    check(core.menu().ui().widgetRect("opt.rtx.overlayTestFlag", r), "filtered row laid out");
    opt::OptionSystem::shutdown();
}

void testMenuTags() {
    const fs::path dir = freshDir("tags");
    initOptions(dir);
    namespace sc = fuse::relight::scene;
    const std::uint64_t a = 0x1111222233334444ull, b = 0xA5A5A5A500C0FFEEull;
    std::vector<MenuTexture> textures = {{a, 64, 64, 21, 3}, {b, 32, 16, 21, 1}};
    MenuFrame mf;
    mf.textures = &textures;
    OverlayConfig cfg;
    OverlayCore core(cfg);
    WindowHook hook;
    std::string err = runScriptText(core, hook,
                                    "0 toggle\n0 clickw tab.tex\n0 clickw tex.1.ui\n0 clickw tex.0.sky\n"
                                    "0 clickw tex.0.ignore\n0 clickw save\n",
                                    0, mf);
    check(err.empty(), "scripted tagging: " + err);
    check(sc::ClassifyOptions::uiTextures.containsHash(b), "texture 1 in rtx.uiTextures");
    check(sc::ClassifyOptions::skyBoxTextures.containsHash(a) && sc::ClassifyOptions::ignoreTextures.containsHash(a),
          "texture 0 in rtx.skyBoxTextures and rtx.ignoreTextures");
    check(!sc::ClassifyOptions::uiTextures.containsHash(a) && !sc::ClassifyOptions::decalTextures.containsHash(a),
          "only the clicked categories");
    check(core.menu().tags() == 3, "three tags");
    std::string conf = readFile(dir / "rtx.conf");
    check(conf.find("rtx.uiTextures = 0xA5A5A5A500C0FFEE") != std::string::npos &&
              conf.find("rtx.skyBoxTextures = 0x1111222233334444") != std::string::npos &&
              conf.find("rtx.ignoreTextures = 0x1111222233334444") != std::string::npos,
          "rtx.conf lists the hashes:\n" + conf);
    // Untag.
    err = runScriptText(core, hook, "1 clickw tex.0.ignore\n1 clickw save\n", 1, mf);
    check(err.empty(), "scripted untag: " + err);
    check(!sc::ClassifyOptions::ignoreTextures.containsHash(a), "untagged");
    conf = readFile(dir / "rtx.conf");
    check(conf.find("rtx.ignoreTextures") == std::string::npos && conf.find("rtx.uiTextures") != std::string::npos,
          "rtx.conf drops the untagged list:\n" + conf);
    // A hash a weaker layer lists: untagging writes an explicit removal.
    opt::OptionSystem::shutdown();
    {
        std::ofstream f(dir / "rtx.conf", std::ios::binary);
        f << "rtx.decalTextures = 0x1111222233334444\n";
    }
    opt::OptionSystemDesc d;
    d.baseDirectory = dir.string();
    d.loadEnvironmentVariables = false;
    d.appConfig.set("rtx.particleTextures", "0xA5A5A5A500C0FFEE");
    opt::OptionSystem::initialize(d);
    check(sc::ClassifyOptions::particleTextures.containsHash(b), "the app layer lists texture 1 as particle");
    check(DevMenu::toggleTextureCategory(kTextureCategories[4], b) &&
              !sc::ClassifyOptions::particleTextures.containsHash(b),
          "untagging a weaker layer's hash removes it (negative entry in rtx.conf)");
    check(DevMenu::saveLayers(), "save");
    conf = readFile(dir / "rtx.conf");
    check(conf.find("rtx.particleTextures = -0xA5A5A5A500C0FFEE") != std::string::npos, "negative entry saved:\n" + conf);
    opt::OptionSystem::shutdown();
}

void testMenuDebug() {
    const fs::path dir = freshDir("debug");
    initOptions(dir);
    std::array<bool, kDebugViewCount> available{};
    available[static_cast<std::size_t>(DebugView::Albedo)] = true;
    MenuFrame mf;
    mf.debugAvailable = &available;
    OverlayConfig cfg;
    OverlayCore core(cfg);
    WindowHook hook;
    const std::string err = runScriptText(core, hook, "0 toggle\n0 clickw tab.dbg\n0 clickw dbg.albedo\n", 0, mf);
    check(err.empty(), "scripted debug view: " + err);
    check(OverlayOptions::debugView() == static_cast<std::int32_t>(DebugView::Albedo), "debug view = albedo");
    check(runScriptText(core, hook, "1 clickw dbg.off\n", 1, mf).empty() && OverlayOptions::debugView() == 0,
          "debug view off");
    // Unavailable views carry an "n/a" label next to them.
    int na = 0;
    for (const DrawCmd& c : core.menu().ui().draws().cmds()) {
        na += c.kind == DrawCmd::Kind::Text && core.menu().ui().draws().text(c) == "n/a" ? 1 : 0;
    }
    check(na == static_cast<int>(kDebugViewCount) - 2, "n/a on the unavailable views (" + std::to_string(na) + ")");
    opt::OptionSystem::shutdown();
}

// ---- layout gate ---------------------------------------------------------------------------------------------------

struct Member {
    std::string name;
    std::size_t offset;
};

std::vector<Member> shaderPushMembers(const std::string& path, std::string* error) {
    const std::string text = readFile(path);
    std::vector<Member> out;
    const std::size_t at = text.find("OverlayPush {");
    if (at == std::string::npos) {
        *error = "no 'OverlayPush {' in " + path;
        return out;
    }
    const std::size_t end = text.find('}', at);
    std::istringstream body(text.substr(at + 13, end - at - 13));
    std::string type, name;
    std::size_t offset = 0;
    while (body >> type >> name) {
        if (!name.empty() && name.back() == ';') {
            name.pop_back();
        }
        const bool vec4 = type == "vec4" || type == "float4";
        const std::size_t size = vec4 ? 16 : 4;
        if (!vec4 && type != "int" && type != "uint" && type != "float") {
            *error = "unknown member type '" + type + "' in " + path;
            return {};
        }
        offset = (offset + size - 1) / size * size;
        out.push_back({name, offset});
        offset += size;
    }
    return out;
}

void testLayout() {
    const std::vector<Member> cpp = {
        {"regionX", offsetof(OverlayPush, regionX)},   {"regionY", offsetof(OverlayPush, regionY)},
        {"regionW", offsetof(OverlayPush, regionW)},   {"regionH", offsetof(OverlayPush, regionH)},
        {"panelX", offsetof(OverlayPush, panelX)},     {"panelY", offsetof(OverlayPush, panelY)},
        {"panelW", offsetof(OverlayPush, panelW)},     {"panelH", offsetof(OverlayPush, panelH)},
        {"frameW", offsetof(OverlayPush, frameW)},     {"frameH", offsetof(OverlayPush, frameH)},
        {"flags", offsetof(OverlayPush, flags)},       {"debugSwizzle", offsetof(OverlayPush, debugSwizzle)},
        {"debugW", offsetof(OverlayPush, debugW)},     {"debugH", offsetof(OverlayPush, debugH)},
        {"pad0", offsetof(OverlayPush, pad0)},         {"pad1", offsetof(OverlayPush, pad1)},
        {"debugScale", offsetof(OverlayPush, debugScale)}, {"debugBias", offsetof(OverlayPush, debugBias)},
    };
    for (const char* file : {"overlay_compose.comp", "overlay_compose.slang"}) {
        std::string error;
        const std::vector<Member> s = shaderPushMembers(std::string(RL_OVERLAY_SHADER_DIR) + "/" + file, &error);
        check(error.empty(), error);
        check(s.size() == cpp.size(), std::string(file) + ": member count");
        for (std::size_t i = 0; i < s.size() && i < cpp.size(); ++i) {
            check(s[i].name == cpp[i].name && s[i].offset == cpp[i].offset,
                  std::string(file) + ": " + s[i].name + " @" + std::to_string(s[i].offset) + " vs " + cpp[i].name + " @" +
                      std::to_string(cpp[i].offset));
        }
        check(!s.empty() && s.back().offset + 16 == sizeof(OverlayPush), std::string(file) + ": size");
    }
    check(kOverlayGroupSize == 8, "group size matches local_size / numthreads (8)");
    const std::string comp = readFile(std::string(RL_OVERLAY_SHADER_DIR) + "/overlay_compose.comp");
    const std::string slang = readFile(std::string(RL_OVERLAY_SHADER_DIR) + "/overlay_compose.slang");
    check(comp.find("local_size_x = 8, local_size_y = 8") != std::string::npos &&
              slang.find("[numthreads(8, 8, 1)]") != std::string::npos,
          "both shaders use 8x8 groups");
}

// ---- zero allocations while hidden ---------------------------------------------------------------------------------

void testZeroAlloc() {
    OverlayConfig cfg;
    OverlayCore core(cfg);
    WindowHook hook;
    MenuStats stats;
    std::vector<MenuTexture> textures(4);
    MenuFrame mf;
    mf.stats = &stats;
    mf.textures = &textures;
    const WindowMessage game[] = {{win32::kWmKeyDown, 'W', 1},
                                  {win32::kWmKeyUp, 'W', win32::kRepeatBit | 1},
                                  {win32::kWmMouseMove, 0, win32::makeLParam(5, 6)},
                                  {win32::kWmLButtonDown, 1, win32::makeLParam(5, 6)},
                                  {win32::kWmLButtonUp, 0, win32::makeLParam(5, 6)},
                                  {win32::kWmChar, 'w', 1}};
    auto frame = [&] {
        for (const WindowMessage& m : game) {
            hook.send(core.input(), m.msg, m.wParam, m.lParam);
        }
        core.update(kW, kH, mf, true);
    };
    for (int i = 0; i < 16; ++i) {
        frame();
    }
    t_allocations = 0;
    t_count = true;
    for (int i = 0; i < 1000; ++i) {
        frame();
    }
    t_count = false;
    check(t_allocations == 0, "hidden steady state: " + std::to_string(t_allocations) + " operator new call(s)");
    check(core.input().forwarded() == 1016u * 6u && !core.visible(), "every message forwarded");

    // Shown, then hidden again: back to zero.
    core.input().handle(win32::kWmSysKeyDown, win32::kVkX, win32::kAltContextBit | 1);
    core.update(kW, kH, mf, true);
    core.draw();
    core.input().handle(win32::kWmSysKeyDown, win32::kVkX, win32::kAltContextBit | 1);
    core.update(kW, kH, mf, true);
    check(!core.visible(), "hidden again");
    t_allocations = 0;
    t_count = true;
    for (int i = 0; i < 200; ++i) {
        frame();
    }
    t_count = false;
    check(t_allocations == 0, "hidden after a shown frame: " + std::to_string(t_allocations) + " operator new call(s)");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    registerOverlayOptions();
    (void)fuse::relight::scene::ClassifyOptions::uiTextures.getFullName();
    struct Suite {
        const char* name;
        void (*fn)();
    };
    const Suite suites[] = {{"font", testFont},           {"ui", testUi},
                            {"input", testInput},         {"script", testScript},
                            {"raster", testRaster},       {"debug_map", testDebugMap},
                            {"hidden", testHidden},       {"menu_options", testMenuOptions},
                            {"menu_tags", testMenuTags},  {"menu_debug", testMenuDebug},
                            {"layout", testLayout},       {"zero_alloc", testZeroAlloc}};
    bool ran = false;
    for (const Suite& s : suites) {
        if (suite == "all" || suite == s.name) {
            s.fn();
            ran = true;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite '%s'\n", suite.c_str());
        return 2;
    }
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS: rl_overlay %s\n", suite.c_str());
    return 0;
}
