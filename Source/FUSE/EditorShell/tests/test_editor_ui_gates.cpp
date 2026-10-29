// fuse_editor_ui_* twin gates (UI9): the FUSE UI editor shell checked the way the fuse_editor_qt_* gates check the Qt
// shell (same names, same panel ids / actions / title format), headless on the CPU rasteriser. One executable:
//   fuse_editor_ui_gates --gate=<name> [--out=shot.png]
#include "fuse/editor/fui/editor_shell.hpp"
#include "scripted_services.hpp"

#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/editor_theme_tokens.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>

using namespace fuse::editor;
using fuse::u32;
using fuse::i32;
using fuse::ui::Key;
using fuse::ui::NodeHandle;
using fuse::ui::Rect;
using fuse::ui::Vec2;
namespace w = fuse::ui::widgets;

namespace {
int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

struct Rig {
    fuse::app::Application app;
    fuse::app::AppWindow& win;
    EditorHost host;
    test::ScriptedServices sv;
    EditorController c;
    fui::EditorShellWindow shell;
    explicit Rig(float fontScale = 1.f)
        : app({.name = "fuse_editor_ui_gates", .loadSettings = false}),
          win(app.createWindow({.title = "FUSE Editor", .width = 1600, .height = 900})), c(host, sv),
          shell(win, c, {.fontScale = fontScale}) {
        frames(2);
    }
    void frames(int n = 1) {
        for (int i = 0; i < n; ++i) {
            c.tick();
            shell.sync();
            app.step(0.f);
        }
    }
    fuse::ui::UiContext& ui() { return win.ui(); }
    Rect rect(std::string_view id) { return ui().rect(ui().find(id)); }
    Vec2 centre(NodeHandle n) {
        const Rect r = ui().rect(n);
        return {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    }
    void click(NodeHandle n, fuse::ui::u8 mods = 0) {
        ui().setModifiers(mods);
        win.click(centre(n));
        ui().setModifiers(0);
        frames();
    }
    void clickId(std::string_view id) { click(ui().find(id)); }
    const EditorViewState& v() { return c.viewState(); }
};

bool near8(fuse::ui::u32 px, const char* role, int tol = 3) {
    fuse::ui::Color col{1, 0, 1, 1};
    for (const ThemeToken& t : kEditorDarkTheme)
        if (std::string_view(role) == t.role) col = {t.r, t.g, t.b, 1.f};
    const fuse::ui::u32 want = fuse::ui::packPremultiplied(col);
    for (int s = 0; s < 32; s += 8)
        if (std::abs(int((px >> s) & 255) - int((want >> s) & 255)) > tol) return false;
    return true;
}

// ---- gates ----------------------------------------------------------------------------------------------------------
void gateStartup(Rig& r) {
    CHECK(r.win.title() == "EditorHostScene - FUSE Editor");
    for (const fui::ShellPanel& p : fui::shellPanels()) CHECK(r.ui().alive(r.ui().find(p.id)));
    for (const char* m : editorMenus()) CHECK(r.ui().alive(r.shell.menuNode(m)));
    CHECK(r.ui().alive(r.ui().find("viewport")));
    CHECK(r.shell.viewportRenders() >= 1);
    CHECK(r.ui().alive(r.ui().find("tool.file.new_scene")));
}

void gateLayout(Rig& r) {
    const Rect left = r.rect("dock.left"), right = r.rect("dock.right"), bottom = r.rect("dock.bottom"), vp = r.rect("viewport");
    const Rect menu = r.rect("editor.menubar"), status = r.rect("editor.status");
    CHECK(left.w > 100 && right.w > 100 && bottom.h > 80 && vp.w > 400 && vp.h > 200);
    CHECK(left.x + left.w <= vp.x + 1 && vp.x + vp.w <= right.x + 1);
    CHECK(vp.y + vp.h <= bottom.y + 1);
    CHECK(menu.y < 1.f && status.y + status.h >= 899.f); // full-height chrome
    CHECK(std::fabs(left.y + left.h - bottom.y) < 12.f); // side docks reach the bottom panel (Qt default corners)
    // tab pages: the first page of each side is shown, switching works
    CHECK(r.rect("dock.hierarchy").h > 50 && r.rect("dock.inspector").h > 50 && r.rect("dock.console").h > 30);
    CHECK(r.shell.showPanel("dock.profiler"));
    r.frames();
    CHECK(r.rect("dock.profiler").h > 30);
    w::dockClose(r.ui(), r.ui().find("dock.right"));
    r.frames();
    CHECK(r.rect("viewport").w > vp.w); // centre grows into the closed side
    CHECK(r.c.trigger(ActionId::ViewResetLayout));
    r.frames();
    std::printf("layout: viewport w %.1f -> %.1f after reset; right %.0f,%.0f %.0fx%.0f\n", vp.w, r.rect("viewport").w, r.rect("dock.right").x, r.rect("dock.right").y, r.rect("dock.right").w, r.rect("dock.right").h);
    CHECK(std::fabs(r.rect("viewport").w - vp.w) < 2.f);
    CHECK(r.rect("dock.hierarchy").h > 50 && r.rect("dock.console").h > 30);
}

void gateTheme(Rig& r) {
    const fuse::ui::Image& img = r.win.image();
    auto at = [&](float x, float y) { return img.at(int(x), int(y)); };
    const Rect status = r.rect("editor.status"), menu = r.rect("editor.menubar"), info = r.rect("viewport.info");
    CHECK(near8(at(status.x + status.w - 4, status.y + status.h * 0.5f), "TitleBgActive"));
    CHECK(near8(at(menu.x + menu.w - 4, menu.y + menu.h * 0.5f), "MenuBarBg"));
    CHECK(near8(at(info.x + info.w - 4, info.y + info.h * 0.5f), "TitleBg"));
    CHECK(themeHex("WindowBg").rfind("#14171a", 0) == 0);
    CHECK(fui::editorShellCss(1.f).find("--bg: #14171a") != std::string::npos);
}

void gateFont(Rig& r, float scale) {
    const Rect menu = r.rect("editor.menubar"), status = r.rect("editor.status");
    std::printf("font %.2f: menubar h %.1f, status h %.1f\n", scale, menu.h, status.h);
    CHECK(menu.h >= 13.f * scale && status.h >= 13.f * scale);
    CHECK(r.rect("viewport").h > 150.f);
}

void gateFuseApi(Rig& r) {
    // toolbar + menu actions go through the FUSE command API (EditorHost::postFromUi) and back into the panels
    const u32 base = r.v().entityCount;
    const auto file = r.shell.menuItemIndex("Entity", ActionId::EntityCreateSdfSphere);
    CHECK(file >= 0);
    r.ui().emit(r.shell.menuNode("Entity"), fuse::ui::SignalKind::Submitted, {.value = float(file)});
    r.frames(2);
    CHECK(r.v().entityCount == base + 1);
    CHECK(r.ui().text(r.ui().find("hierarchy.tree")).find("SDF") != std::string_view::npos);
    CHECK(r.win.title().find('*') != std::string::npos);
    r.clickId("tool.edit.undo");
    CHECK(r.v().entityCount == base);
    r.clickId("tool.edit.redo");
    CHECK(r.v().entityCount == base + 1);
}

void gateWasd(Rig& r) {
    const auto p0 = r.c.viewport().position();
    // fly like the Qt viewport: hold RMB (drag a little), then W
    const Rect vp = r.ui().rect(r.ui().find("viewport"));
    Vec2 at{vp.x + vp.w * 0.5f, vp.y + vp.h * 0.5f};
    r.ui().pointerMove(at);
    r.ui().pointerDown(at, fuse::ui::MouseButton::Right);
    for (int i = 0; i < 3; ++i) { at.x += 4.f; r.ui().pointerMove(at); }
    r.frames();
    CHECK(r.c.viewportFlying());
    r.ui().keyDown(Key::W);
    r.frames(10);
    r.ui().keyUp(Key::W);
    r.ui().pointerUp(at, fuse::ui::MouseButton::Right);
    r.frames();
    CHECK(!r.c.contextMenuOpen()); // a drag is not a context click
    const auto p1 = r.c.viewport().position();
    const float moved = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y) + (p1.z - p0.z) * (p1.z - p0.z));
    std::printf("wasd: moved %.3f\n", moved);
    CHECK(moved > 0.05f);
    const auto p2 = r.c.viewport().position();
    r.frames(5);
    const auto p3 = r.c.viewport().position();
    CHECK(p2.x == p3.x && p2.y == p3.y && p2.z == p3.z); // stops on key up
    // Ctrl+S is a shortcut, not a camera move
    r.sv.files.push_back((std::filesystem::temp_directory_path() / "fuse_editor_ui_wasd.fscene").string());
    r.ui().keyDown(Key::S, fuse::ui::kModCtrl);
    r.frames();
    r.ui().keyUp(Key::S, fuse::ui::kModCtrl);
    CHECK(!r.sv.asked.empty());
}

void gateContextMenu(Rig& r) {
    const u32 base = r.v().entityCount;
    const Rect vp = r.ui().rect(r.ui().find("viewport"));
    const Vec2 at{vp.x + vp.w * 0.5f, vp.y + vp.h * 0.5f};
    r.ui().pointerMove(at);
    r.ui().pointerDown(at, fuse::ui::MouseButton::Right);
    r.ui().pointerUp(at, fuse::ui::MouseButton::Right);
    r.frames();
    CHECK(r.c.contextMenuOpen());
    const NodeHandle menu = r.ui().find("viewport.context");
    CHECK(r.ui().alive(menu));
    r.app.step(0.f);
    // first item: Create Empty (keyboard focus starts on it)
    r.ui().keyDown(Key::Enter);
    r.ui().keyUp(Key::Enter);
    r.frames(2);
    CHECK(r.v().entityCount == base + 1);
}

void gateUiFrame(Rig& r) {
    // a pointer sweep along the toolbar (hover in / out of each button, 6 px steps): update + damage raster per frame
    const Rect bar = r.rect("editor.toolbar");
    const auto t0 = std::chrono::steady_clock::now();
    int n = 0;
    for (float x = bar.x + 4.f; x < bar.x + 700.f; x += 6.f, ++n) {
        r.ui().pointerMove({x, bar.y + bar.h * 0.5f});
        r.app.step(0.f);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
    // full-window CPU raster for reference (the native presenters damage-track the same way)
    const auto t1 = std::chrono::steady_clock::now();
    r.win.rasterize({});
    const double full = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    std::printf("ui_frame: %.2f ms / hover frame over %d frames; full 1600x900 CPU raster %.1f ms\n", ms, n, full);
    CHECK(ms < 16.7);
}

void gateIdle(Rig& r) {
    r.frames(3);
    const auto presented = r.win.framesPresented();
    const u32 renders = r.shell.viewportRenders();
    for (int i = 0; i < 30; ++i) {
        r.shell.sync();
        r.app.step(0.f);
    }
    std::printf("idle: %llu -> %llu frames, %u -> %u viewport renders\n", (unsigned long long)presented,
                (unsigned long long)r.win.framesPresented(), renders, r.shell.viewportRenders());
    CHECK(r.win.framesPresented() == presented);
    CHECK(r.shell.viewportRenders() == renders);
}

void gateConsole(Rig& r) {
    CHECK(r.shell.showPanel("dock.console"));
    const NodeHandle in = r.ui().find("console.input");
    r.ui().setFocus(in);
    r.ui().textInput("help");
    r.ui().keyDown(Key::Enter);
    r.ui().keyUp(Key::Enter);
    r.frames(2);
    const std::string_view log = r.ui().text(r.ui().find("console.log"));
    CHECK(!log.empty());
    CHECK(r.ui().text(in).empty());
    std::printf("console: %zu bytes of output\n", log.size());
}

void gateTransport(Rig& r) {
    const NodeHandle play = r.ui().find("tool.play.play"), stop = r.ui().find("tool.play.stop");
    CHECK(!r.ui().disabled(play) && r.ui().disabled(stop));
    r.click(play);
    r.frames(3);
    CHECK(r.v().transport == TransportState::Playing);
    CHECK(r.win.title().find("[Playing]") != std::string::npos);
    CHECK(r.ui().disabled(play) && !r.ui().disabled(stop));
    r.ui().keyDown(Key::F5); // F5 while playing = pause
    r.frames();
    CHECK(r.v().transport == TransportState::Paused);
    r.click(stop);
    CHECK(r.v().transport == TransportState::Stopped);
}

void gateInspector(Rig& r) {
    r.c.runEntityAction(ContextMenuAction::CreateSdfSphere);
    r.frames(2);
    CHECK(r.v().primary.valid());
    const NodeHandle grid = r.ui().find("inspector.grid");
    CHECK(w::propertyCount(r.ui(), grid) > 3);
    // find the Position row and edit it like a user: focus, select all, type, Enter
    u32 row = ~0u;
    for (u32 i = 0; i < w::propertyCount(r.ui(), grid); ++i) {
        NodeHandle n = r.ui().firstChild(grid);
        for (u32 k = 0; k < i; ++k) n = r.ui().nextSibling(n);
        if (r.ui().text(r.ui().firstChild(n)) == "Position") { row = i; break; }
    }
    CHECK(row != ~0u);
    if (row != ~0u) {
        NodeHandle n = r.ui().firstChild(grid);
        for (u32 k = 0; k < row; ++k) n = r.ui().nextSibling(n);
        const NodeHandle editor = r.ui().nextSibling(r.ui().firstChild(n));
        r.ui().setFocus(editor);
        r.ui().keyDown(Key::A, fuse::ui::kModCtrl);
        r.ui().textInput("4,5,6");
        r.ui().keyDown(Key::Enter);
        r.ui().setFocus(r.ui().find("viewport"));
        r.frames(2);
        bool ok = false;
        for (const InspectorSectionView& s : r.v().inspector)
            for (const InspectorFieldView& f : s.fields)
                if (f.propertyName == "transform.position") { ok = f.value.rfind("4,5,6", 0) == 0; std::printf("position = %s\n", f.value.c_str()); }
        CHECK(ok);
    }
    // add / remove component through the panel buttons
    const std::size_t sections = r.v().inspector.size();
    if (!r.v().addableComponents.empty()) {
        r.clickId("inspector.add");
        CHECK(r.v().inspector.size() == sections + 1);
    }
}

void gateReparent(Rig& r) {
    r.c.runEntityAction(ContextMenuAction::CreateEmpty);
    r.frames();
    r.c.runEntityAction(ContextMenuAction::CreatePointLight);
    r.frames(2);
    // tree: click the light, Ctrl+click the empty (active = last clicked), Parent to Active
    const NodeHandle tree = r.ui().find("hierarchy.tree");
    i32 emptyLine = -1, lightLine = -1;
    for (u32 i = 0; i < r.v().hierarchy.size(); ++i) {
        if (r.v().hierarchy[i].label.find("Light") != std::string::npos) lightLine = i32(i);
        else if (r.v().hierarchy[i].label.find("Entity") != std::string::npos) emptyLine = i32(i);
    }
    CHECK(emptyLine >= 0 && lightLine >= 0);
    r.ui().setValue(tree, float(lightLine), true);
    r.frames();
    r.ui().setModifiers(fuse::ui::kModCtrl);
    r.ui().setValue(tree, float(emptyLine), true);
    r.ui().setModifiers(0);
    r.frames();
    CHECK(r.v().selection.size() == 2);
    r.clickId("hierarchy.parent");
    r.frames();
    bool nested = false;
    for (const HierarchyRowView& h : r.v().hierarchy)
        if (h.label.find("Light") != std::string::npos) nested = h.depth == 1;
    CHECK(nested);
    CHECK(r.ui().text(tree).find("\tLight") != std::string_view::npos || r.ui().text(tree).find("\t> ") != std::string_view::npos);
    r.clickId("hierarchy.unparent");
    for (const HierarchyRowView& h : r.v().hierarchy) CHECK(h.depth == 0);
}

void gateFileMenu(Rig& r) {
    const std::string path = (std::filesystem::temp_directory_path() / "fuse_editor_ui_gate.fscene").string();
    r.c.runEntityAction(ContextMenuAction::CreateCube);
    r.frames();
    r.sv.files.push_back(path);
    const i32 saveAs = r.shell.menuItemIndex("File", ActionId::FileSaveAs);
    CHECK(saveAs >= 0);
    r.ui().emit(r.shell.menuNode("File"), fuse::ui::SignalKind::Submitted, {.value = float(saveAs)});
    r.frames();
    CHECK(r.win.title() == "fuse_editor_ui_gate - FUSE Editor");
    CHECK(r.ui().text(r.shell.menuNode("File")).find("fuse_editor_ui_gate.fscene") != std::string_view::npos); // recent
    // dirty + New: Cancel keeps the scene
    r.c.runEntityAction(ContextMenuAction::CreateEmpty);
    r.frames();
    r.sv.answers.push_back(PromptAnswer::Cancel);
    r.ui().keyDown(Key::N, fuse::ui::kModCtrl);
    r.frames();
    CHECK(r.win.title().find('*') != std::string::npos);
    // open the File menu for the screenshot
    r.click(r.shell.menuNode("File"));
    r.app.step(0.f);
}
} // namespace

int main(int argc, char** argv) {
    std::string gate, out;
    for (int i = 1; i < argc; ++i) {
        if (!std::strncmp(argv[i], "--gate=", 7)) gate = argv[i] + 7;
        else if (!std::strncmp(argv[i], "--out=", 6)) out = argv[i] + 6;
    }
    const std::map<std::string, float> fonts{{"font_1", 1.f}, {"font_1_25", 1.25f}, {"font_1_5", 1.5f}, {"font_2", 2.f}};
    const float scale = fonts.count(gate) ? fonts.at(gate) : 1.f;
    Rig r(scale);
    const std::map<std::string, std::function<void(Rig&)>> gates{
        {"startup", gateStartup},        {"layout", gateLayout},           {"theme", gateTheme},
        {"fuse_api", gateFuseApi},       {"wasd", gateWasd},               {"context_menu", gateContextMenu},
        {"ui_frame", gateUiFrame},       {"idle_frames", gateIdle},        {"e15_console", gateConsole},
        {"e15_transport", gateTransport}, {"e20_inspector", gateInspector}, {"e20_reparent", gateReparent},
        {"e20_file_menu", gateFileMenu},
    };
    if (fonts.count(gate)) gateFont(r, scale);
    else if (gates.count(gate)) gates.at(gate)(r);
    else { std::fprintf(stderr, "unknown gate '%s'\n", gate.c_str()); return 2; }
    if (!out.empty()) {
        if (gate != "e20_file_menu" && gate != "context_menu") r.frames();
        r.win.rasterize({});
        if (!r.win.writePng(out)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); ++g_fail; }
    }
    if (g_fail) { std::fprintf(stderr, "fuse_editor_ui_%s: %d failure(s)\n", gate.c_str(), g_fail); return 1; }
    std::printf("fuse_editor_ui_%s: OK\n", gate.c_str());
    return 0;
}
