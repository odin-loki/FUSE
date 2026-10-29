// fuse_editor_ui_* twin gates (UI9): the FUSE UI editor shell checked the way the fuse_editor_qt_* gates check the Qt
// shell (same names, same panel ids / actions / title format), headless on the CPU rasteriser. One executable:
//   fuse_editor_ui_gates --gate=<name> [--out=shot.png]
#include "fuse/editor/fui/editor_shell.hpp"
#include "scripted_services.hpp"

#include <fuse/ecs/components/script.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/scene_viewport_host.hpp>
#if defined(FUSE_EDITOR_UI_HAS_NATIVE)
#include "fuse/app/native.hpp"
#endif
#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/renderer/vk/instance.hpp>
#endif
#include <fuse/editor/editor_theme_tokens.hpp>
#include "fuse/editor/fui/app_icon.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>

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
            shell.frame();
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
    CHECK(std::fabs(r.rect("dock.right").w - 320.f) < 4.f && std::fabs(r.rect("dock.left").w - 260.f) < 4.f);
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

// Opens the viewport context menu at `at` and checks the panel matches EntityContextMenu::place (Qt QMenu::popup
// rules: top-left at the click, flip left / up at the right / bottom window edge, clamp) in window logical px.
static ContextMenuPlacement openMenuAt(Rig& r, Vec2 at, bool expectFlipY) {
    r.ui().pointerMove(at);
    r.ui().pointerDown(at, fuse::ui::MouseButton::Right);
    r.ui().pointerUp(at, fuse::ui::MouseButton::Right);
    r.frames();
    CHECK(r.c.contextMenuOpen());
    int popups = 0;
    for (NodeHandle n = r.ui().firstChild(r.ui().parent(r.ui().find("viewport.context"))); r.ui().alive(n); n = r.ui().nextSibling(n))
        popups += r.ui().idOf(n) == "viewport.context";
    CHECK(popups == 1);
    const NodeHandle menu = r.ui().find("viewport.context");
    CHECK(r.ui().alive(menu));
    const NodeHandle panel = r.ui().firstChild(menu);
    const Rect mr = r.ui().rect(panel);
    const fuse::ui::Size win = r.ui().viewport();
    ContextMenuHostGeometry host;
    host.windowWidth = win.w;
    host.windowHeight = win.h;
    const ContextMenuPlacement want = EntityContextMenu::place(host, at.x, at.y, mr.w, mr.h);
    const ContextMenuPlacement& got = r.shell.contextMenuPlacement();
    std::printf("context menu at (%.1f,%.1f): panel %.1f,%.1f %.1fx%.1f, Qt place %.1f,%.1f flip %d/%d\n", at.x, at.y, mr.x, mr.y,
                mr.w, mr.h, want.x, want.y, int(want.flippedX), int(want.flippedY));
    CHECK(std::fabs(got.clickX - at.x) < 0.5f && std::fabs(got.clickY - at.y) < 0.5f);
    CHECK(std::fabs(mr.x - want.x) <= 1.f && std::fabs(mr.y - want.y) <= 1.f);
    CHECK(mr.x >= 0.f && mr.y >= 0.f && mr.x + mr.w <= win.w + 1.f && mr.y + mr.h <= win.h + 1.f);
    CHECK(want.flippedY == (at.y + mr.h > win.h));
    if (expectFlipY) CHECK(want.flippedY);
    CHECK(want.flippedX == (at.x + mr.w > win.w));
    if (want.flippedX) CHECK(mr.x + mr.w <= at.x + 1.f); else CHECK(std::fabs(mr.x - at.x) <= 1.f);
    if (want.flippedY) CHECK(mr.y + mr.h <= at.y + 1.f); else CHECK(std::fabs(mr.y - at.y) <= 1.f);
    return want;
}

void gateContextMenuAt(Rig& r, bool dpr15) {
    const u32 base = r.v().entityCount;
    Rect vp = r.ui().rect(r.ui().find("viewport"));
    // centre: opens down-right at the click
    openMenuAt(r, {vp.x + vp.w * 0.5f, vp.y + vp.h * 0.5f}, false);
    r.app.step(0.f);
    // first item: Create Empty (keyboard focus starts on it)
    r.ui().keyDown(Key::Enter);
    r.ui().keyUp(Key::Enter);
    r.frames(2);
    CHECK(r.v().entityCount == base + 1);
    // a compact window: at the viewport's bottom-right corner the menu flips up (and left when it would overflow the
    // right window edge), Qt QMenu::popup rules; the DPR 1.5 twin's larger menu flips where the 1.0 one may not
    r.ui().setViewport({1000.f, 560.f});
    r.frames(2);
    vp = r.ui().rect(r.ui().find("viewport"));
    const Vec2 corner{vp.x + vp.w - 6.f, vp.y + vp.h - 6.f};
    openMenuAt(r, corner, dpr15);
}
void gateContextMenu(Rig& r) { gateContextMenuAt(r, false); }
void gateContextMenuDpr15(Rig& r) { gateContextMenuAt(r, true); }

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

// ---- UI9 panels (twins of the Qt panel gates) -------------------------------------------------------------------------
std::filesystem::path scratchDir(const char* tag) {
    const auto d = std::filesystem::temp_directory_path() / (std::string("fuse_editor_ui_") + tag);
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}
void setField(Rig& r, std::string_view id, std::string_view text) {
    const NodeHandle n = r.ui().find(id);
    r.ui().setFocus(n);
    r.ui().keyDown(Key::A, fuse::ui::kModCtrl);
    r.ui().textInput(text);
    r.frames();
}

void gateProjectHub(Rig& r) {
    CHECK(r.shell.showPanel("dock.project_hub"));
    const auto root = scratchDir("hub");
    r.shell.panels().setProjectsRoot(root.string());
    r.frames();
    CHECK(r.ui().text(r.ui().find("hub.current")) == "No project open");
    setField(r, "hub.name", "GateWorld");
    r.clickId("hub.new");
    r.frames(2);
    CHECK(std::filesystem::exists(root / "GateWorld" / "project.json"));
    CHECK(r.ui().text(r.ui().find("hub.list")).find("GateWorld") != std::string_view::npos);
    CHECK(r.ui().text(r.ui().find("hub.current")).find("GateWorld") != std::string_view::npos);
    CHECK(r.ui().value(r.ui().find("hub.list")) == 0.f);
    std::printf("project_hub: %s\n", std::string(r.ui().text(r.ui().find("hub.current"))).c_str());
}

void gateWizard(Rig& r) {
    // File > New Project... -> wizard (name, location, dimension, UI layer, modules) -> project.json -> project loads
    const auto root = scratchDir("wizard");
    r.shell.panels().setProjectsRoot(root.string());
    r.frames();
    CHECK(r.c.trigger(ActionId::FileNewProject));
    r.frames();
    CHECK(r.shell.newProjectWizardOpen());
    CHECK(r.ui().text(r.ui().find("wizard.location")) == root.string());
    r.clickId("wizard.cancel"); // Cancel creates nothing
    r.frames();
    CHECK(!r.shell.newProjectWizardOpen());
    CHECK(!std::filesystem::exists(root / "NewProject"));
    CHECK(r.c.trigger(ActionId::FileNewProject));
    r.frames();
    setField(r, "wizard.name", "WizWorld");
    CHECK(r.ui().text(r.ui().find("wizard.hint")).find("WizWorld") != std::string_view::npos);
    r.ui().setValue(r.ui().find("wizard.dimension"), 1.f, true); // 2D
    r.clickId("wizard.ui");                                      // untick the UI layer
    r.clickId("wizard.module.cinematics");
    r.clickId("wizard.module.fx");
    r.frames();
    r.clickId("wizard.create");
    r.frames(2);
    CHECK(!r.shell.newProjectWizardOpen());
    const auto loaded = fuse::project::loadFromDirectory((root / "WizWorld").string());
    CHECK(loaded.status == fuse::project::LoadStatus::Ok);
    const auto& m = loaded.manifest;
    CHECK(!m.dimensions.enable3D && m.dimensions.enable2D && !m.dimensions.enableUI);
    CHECK(m.modules.cinematics && m.modules.fx && !m.modules.ai && !m.modules.mechanics && !m.modules.adventure);
    const ProjectHubView hub = r.shell.panels().projectHub();
    CHECK(hub.hasProject && hub.name == "WizWorld");
    std::printf("e20_wizard: %s 2D=%d UI=%d cinematics=%d fx=%d\n", hub.directory.c_str(), int(m.dimensions.enable2D),
                int(m.dimensions.enableUI), int(m.modules.cinematics), int(m.modules.fx));
    // leave the wizard open for the screenshot
    CHECK(r.c.trigger(ActionId::FileNewProject));
    setField(r, "wizard.name", "Another");
    r.frames(2);
}

void gateProjectOpen(Rig& r) {
    // Open Project loads the project's default world; a second open replaces it (Qt project_open twin)
    const auto root = scratchDir("project_open");
    CHECK(r.shell.showPanel("dock.project_hub"));
    r.shell.panels().setProjectsRoot(root.string());
    CHECK(r.shell.panels().hubNew((root / "Alpha").string(), "Alpha"));
    CHECK(r.c.runEntityAction(ContextMenuAction::CreateSdfSphere));
    CHECK(r.c.fileSave());
    CHECK(r.shell.panels().hubNew((root / "Beta").string(), "Beta"));
    r.frames(2);
    const auto entities = [&] { return r.c.viewState().hierarchy.size(); };
    const std::size_t betaCount = entities();
    // select Alpha in the hub list and Open
    ProjectHubView hub = r.shell.panels().projectHub();
    CHECK(hub.projects.size() == 2u);
    const NodeHandle list = r.ui().find("hub.list");
    for (std::size_t i = 0; i < hub.projects.size(); ++i)
        if (hub.projects[i].find("Alpha") != std::string::npos) r.ui().setValue(list, float(i), true);
    r.clickId("hub.open");
    r.frames(2);
    hub = r.shell.panels().projectHub();
    CHECK(hub.hasProject && hub.name == "Alpha");
    const std::size_t alphaCount = entities();
    CHECK(alphaCount == betaCount + 1); // Alpha's saved world (with the sphere) replaced Beta's
    CHECK(r.shell.panels().hubOpen((root / "Beta").string()));
    r.frames(2);
    CHECK(r.shell.panels().projectHub().name == "Beta" && entities() == betaCount);
    CHECK(r.shell.panels().hubOpen((root / "Alpha").string()));
    r.frames(2);
    std::printf("project_open: Alpha %zu entities, Beta %zu\n", alphaCount, betaCount);
}

void gateAssetBrowser(Rig& r) {
    CHECK(r.shell.showPanel("dock.asset_browser"));
    const auto root = scratchDir("assets");
    std::filesystem::create_directories(root / "props");
    for (const char* f : {"props/rock.fscene", "props/tree.png", "level.fscene", "notes.txt"}) std::fclose(std::fopen((root / f).string().c_str(), "w"));
    r.shell.panels().setProjectsRoot(root.string());
    r.frames(2);
    const NodeHandle list = r.ui().find("assets.list");
    std::string text(r.ui().text(list));
    CHECK(text.find("props") != std::string::npos && text.find("level.fscene") != std::string::npos);
    setField(r, "assets.filter", "level");
    text = r.ui().text(list);
    CHECK(text.find("level.fscene") != std::string::npos && text.find("notes") == std::string::npos);
    setField(r, "assets.filter", "");
    // enter the folder: select its row, Open
    text = r.ui().text(list);
    i32 row = 0;
    for (std::size_t i = 0, l = 0; i < text.size(); ++i) {
        if (text.compare(i, 5, "[dir]") == 0 && (i == 0 || text[i - 1] == '\n')) { row = i32(l); break; }
        if (text[i] == '\n') ++l;
    }
    r.ui().setValue(list, float(row));
    r.clickId("assets.open");
    r.frames();
    text = r.ui().text(list);
    CHECK(text.find("rock.fscene") != std::string::npos && text.find("level.fscene") == std::string::npos);
    CHECK(r.ui().text(r.ui().find("assets.path")).find("props") != std::string_view::npos);
    r.clickId("assets.up");
    CHECK(std::string(r.ui().text(list)).find("level.fscene") != std::string::npos);
    std::printf("asset_browser: %s\n", std::string(r.ui().text(r.ui().find("assets.info"))).c_str());
}

namespace { void shot(Rig& r); }

// Asset browser polish (Qt 399b8de7b: grid, type filter, breadcrumb, status, context menu, rename / new folder).
void gateAssetBrowserPolish(Rig& r) {
    CHECK(r.shell.showPanel("dock.asset_browser"));
    const auto root = scratchDir("assets_polish");
    std::filesystem::create_directories(root / "meshes" / "props");
    std::filesystem::create_directories(root / "textures");
    for (const char* f : {"meshes/props/crate.fusemesh", "meshes/rock.obj", "textures/wood.png", "level.scene", "notes.txt"})
        std::fclose(std::fopen((root / f).string().c_str(), "w"));
    auto& p = r.shell.panels();
    p.setProjectsRoot(root.string());
    r.frames(2);
    auto alive = [&](const char* id) { return r.ui().alive(r.ui().find(id)); };
    CHECK(alive("assets.crumb.0") && !alive("assets.crumb.1"));
    CHECK(p.assetsNavigateTo("meshes/props"));
    r.frames();
    CHECK(alive("assets.crumb.2") && r.ui().text(r.ui().find("assets.crumb.2")) == "props");
    AssetBrowserView v = p.assets();
    CHECK(v.entries.size() == 1 && v.entries[0].type == "Mesh"); // .fusemesh is a Mesh
    r.clickId("assets.crumb.0");
    CHECK(p.assets().directory.empty() && !alive("assets.crumb.1"));
    // type filter: Texture keeps folders, drops the scene / text
    r.ui().setValue(r.ui().find("assets.type"), 2.f, true);
    r.frames();
    v = p.assets();
    CHECK(v.typeFilter == 2 && v.folders == 2 && v.entries.size() == 2);
    r.ui().setValue(r.ui().find("assets.type"), 0.f, true);
    r.frames();
    v = p.assets();
    CHECK(v.entries.size() == 4 && v.status.find("4 items (2 folders)") == 0);
    // grid toggle
    r.clickId("assets.grid");
    r.frames();
    CHECK(p.assets().grid && r.rect("assets.grid_view").w > 0.f && r.rect("assets.list").w == 0.f);
    // context menu -> New Folder...
    const Rect g = r.rect("assets.grid_view");
    auto menu = [&](float line) {
        r.ui().pointerDown({g.x + g.w - 8.f, g.bottom() - 8.f}, fuse::ui::MouseButton::Right);
        r.ui().pointerUp({g.x + g.w - 8.f, g.bottom() - 8.f}, fuse::ui::MouseButton::Right);
        r.frames();
        const NodeHandle m = r.ui().find("assets.context");
        CHECK(r.ui().alive(m));
        if (line >= 0.f) { r.ui().emit(m, fuse::ui::SignalKind::Submitted, {.value = line}); w::closePopup(r.ui(), m); r.frames(); }
    };
    menu(5.f); // New Folder... (line 4 is the separator)
    CHECK(r.rect("assets.name_row").w > 0.f && r.ui().text(r.ui().find("assets.name")) == "New Folder");
    setField(r, "assets.name", "audio");
    r.clickId("assets.name_ok");
    CHECK(std::filesystem::is_directory(root / "audio"));
    menu(5.f);
    setField(r, "assets.name", "audio");
    r.clickId("assets.name_ok");
    CHECK(std::string(r.ui().text(r.ui().find("assets.info"))).find("Could not create") == 0); // existing name
    setField(r, "assets.name", "../escape");
    r.clickId("assets.name_ok");
    CHECK(!std::filesystem::exists(root / ".." / "escape"));
    r.clickId("assets.name_cancel");
    // Rename... level.scene -> main.scene
    CHECK(p.assetsSelect("level.scene"));
    r.frames();
    CHECK(p.assets().status.find("level.scene (Scene)") != std::string::npos);
    menu(3.f);
    CHECK(r.ui().text(r.ui().find("assets.name")) == "level.scene");
    setField(r, "assets.name", "main.scene");
    r.clickId("assets.name_ok");
    CHECK(std::filesystem::exists(root / "main.scene") && !std::filesystem::exists(root / "level.scene"));
    menu(2.f); // Copy Relative Path
    std::string clip;
    CHECK(r.ui().clipboardText(clip) && clip == "main.scene");
    menu(1.f); // Copy Path
    CHECK(r.ui().clipboardText(clip) && clip.size() > 10 && clip.find("main.scene") != std::string::npos);
    CHECK(p.assets().entries.size() == 5);
    menu(-1.f); // leave the menu open for the shot
    shot(r);
    std::printf("asset_browser_polish: %s\n", p.assets().status.c_str());
}

// Timeline navigation (Qt f3bc702bf: zoom / pan / fit, loop, snap, previous / next key, adaptive ruler).
void gateTimelineNav(Rig& r) {
    CHECK(r.shell.showPanel("dock.timeline"));
    r.c.runEntityAction(ContextMenuAction::CreateSdfSphere);
    r.frames(2);
    auto& p = r.shell.panels();
    for (int ms : {0, 2000, 4000}) {
        r.ui().setValue(r.ui().find("timeline.time"), float(ms), true);
        r.frames();
        r.clickId("timeline.key");
    }
    const i32 dur = p.timeline().durationMs;
    p.timelineSetPlayhead(500);
    r.clickId("timeline.next_key");
    CHECK(p.timeline().playheadMs == 2000);
    p.timelineSetPlayhead(500);
    r.clickId("timeline.prev_key");
    CHECK(p.timeline().playheadMs == 0);
    CHECK(!p.timelineJumpToKey(-1));
    // zoom 4x about 0, pan, fit
    p.timelineZoomBy(4.f, 0);
    r.frames();
    CHECK(p.timelineViewSpanMs() == dur / 4 && p.timelineViewStartMs() == 0);
    CHECK(std::abs(w::timelineView(r.ui(), r.ui().find("timeline.track")).y - float(dur) / 4000.f) < 1e-3f);
    p.timelinePanBy(500);
    CHECK(p.timelineViewStartMs() == 500);
    // next key off-screen recentres the view
    p.timelineSetPlayhead(2000);
    CHECK(p.timelineJumpToKey(+1) && p.timeline().playheadMs == 4000);
    CHECK(p.timelineViewStartMs() <= 4000 && p.timelineViewStartMs() + p.timelineViewSpanMs() >= 4000);
    r.clickId("timeline.fit");
    CHECK(p.timelineViewSpanMs() == dur && p.timelineViewStartMs() == 0);
    // Ctrl+wheel zooms, Shift+wheel pans
    r.frames();
    const Rect tl = r.rect("timeline.track");
    r.ui().setModifiers(fuse::ui::kModCtrl);
    r.ui().pointerMove({tl.x + tl.w * 0.3f, tl.bottom() - 10.f});
    r.frames();
    r.ui().wheel({tl.x + tl.w * 0.3f, tl.bottom() - 10.f}, {0.f, 1.f});
    r.frames();
    const i32 zoomed = p.timelineViewSpanMs();
    CHECK(zoomed < dur && std::abs(zoomed - i32(dur * 0.8f)) <= 2);
    r.ui().setModifiers(fuse::ui::kModShift);
    const i32 s0 = p.timelineViewStartMs();
    r.ui().wheel({tl.x + tl.w * 0.3f, tl.bottom() - 10.f}, {0.f, -1.f});
    r.frames();
    CHECK(p.timelineViewStartMs() > s0);
    r.ui().setModifiers(0);
    // snap
    CHECK(p.timelineSnapFps() == 30 && p.timelineSnapMs(1010) == 1000 && p.timelineSnapMs(1020) == 1033);
    r.ui().setValue(r.ui().find("timeline.snap"), 0.f, true);
    r.frames();
    CHECK(p.timelineSnapFps() == 0 && p.timelineSnapMs(1014) == 1010);
    r.ui().setValue(r.ui().find("timeline.snap"), 2.f, true);
    // keys on the focused track: F fit, L loop, [ ] keys, Home / End
    r.ui().setFocus(r.ui().find("timeline.track"));
    r.ui().keyDown(Key::F);
    CHECK(p.timelineViewSpanMs() == dur);
    p.timelineSetPlayhead(1000);
    r.ui().keyDown(Key::BracketRight);
    CHECK(p.timeline().playheadMs == 2000);
    r.ui().keyDown(Key::BracketLeft);
    CHECK(p.timeline().playheadMs == 0);
    r.ui().keyDown(Key::End);
    CHECK(p.timeline().playheadMs == dur);
    r.ui().keyDown(Key::Home);
    CHECK(p.timeline().playheadMs == 0);
    CHECK(p.timelineLooping());
    r.ui().keyDown(Key::L);
    r.frames();
    CHECK(!p.timelineLooping() && !r.ui().checked(r.ui().find("timeline.loop")));
    // not looping: playback stops at the end
    p.timelineSetPlayhead(dur - 10);
    p.timelineSetPlaying(true);
    p.timelineAdvance(0.05f);
    CHECK(p.timeline().playheadMs == dur && !p.timeline().playing);
    // looping wraps and keeps playing
    r.clickId("timeline.loop");
    CHECK(p.timelineLooping());
    p.timelineSetPlayhead(dur - 10);
    p.timelineSetPlaying(true);
    p.timelineAdvance(0.05f);
    CHECK(p.timeline().playing && p.timeline().playheadMs == 40);
    p.timelineSetPlaying(false);
    p.timelineSetPlayhead(2000);
    p.timelineZoomBy(2.f, 2000);
    r.frames(2);
    shot(r);
    std::printf("timeline_nav: view %d+%d ms of %d, %s\n", p.timelineViewStartMs(), p.timelineViewSpanMs(), dur, p.timeline().status.c_str());
}

void gateMaterialEditor(Rig& r) {
    CHECK(r.shell.showPanel("dock.material_editor"));
    r.c.runEntityAction(ContextMenuAction::CreateSdfSphere);
    r.frames(2);
    const NodeHandle list = r.ui().find("material.list");
    const std::size_t before = std::count(r.ui().text(list).begin(), r.ui().text(list).end(), '\n');
    r.clickId("material.new");
    CHECK(std::size_t(std::count(r.ui().text(list).begin(), r.ui().text(list).end(), '\n')) == before + 1);
    const u32 mat = u32(r.ui().value(list));
    r.ui().setValue(r.ui().find("material.roughness"), 0.9f, true);
    r.ui().setValue(r.ui().find("material.metallic"), 1.f, true);
    r.ui().setValue(r.ui().find("material.shader"), 2.f, true);
    r.frames();
    MaterialView mv = r.shell.panels().materials();
    CHECK(std::fabs(mv.state.roughness - 0.9f) < 1e-3f && mv.state.metallic > 0.99f && mv.shader == 2u);
    // preview sphere drawn (centre pixel differs from the corner)
    const fuse::ui::Image& img = r.win.image();
    const Rect pv = r.rect("material.preview");
    CHECK(pv.w >= 95.f && img.at(int(pv.x + pv.w * .5f), int(pv.y + pv.h * .5f)) != img.at(int(pv.x + 1), int(pv.y + 1)));
    r.clickId("material.assign");
    mv = r.shell.panels().materials();
    CHECK(mv.selectionHasMaterial && mv.selectionMaterial == mat);
    r.clickId("tool.edit.undo");
    CHECK(!r.shell.panels().materials().selectionHasMaterial || r.shell.panels().materials().selectionMaterial != mat);
    r.clickId("tool.edit.redo");
    std::printf("material_editor: %s\n", r.shell.panels().materials().summary.c_str());
}

void gateSdfSculpt(Rig& r) {
    CHECK(r.shell.showPanel("dock.sdf_sculpt"));
    r.c.runEntityAction(ContextMenuAction::CreateSdfSphere); // strokes extend the selected SDF object (Qt rule)
    r.frames(2);
    const u32 base = r.v().entityCount;
    r.ui().setValue(r.ui().find("sculpt.radius"), 2.f, true);
    r.clickId("sculpt.mode");
    CHECK(r.shell.panels().sculptMode());
    const Rect vp = r.rect("viewport");
    Vec2 at{vp.x + vp.w * 0.5f, vp.y + vp.h * 0.65f};
    r.ui().pointerMove(at);
    r.ui().pointerDown(at, fuse::ui::MouseButton::Left);
    for (int i = 0; i < 6; ++i) { at.x += 12.f; r.ui().pointerMove(at); }
    r.ui().pointerUp(at, fuse::ui::MouseButton::Left);
    r.frames(2);
    const SculptView sv = r.shell.panels().sculpt();
    std::printf("sdf_sculpt: %u strokes, entities %u -> %u, %s\n", sv.strokes, base, r.v().entityCount, sv.summary.c_str());
    CHECK(sv.strokes >= 1u && r.v().entityCount > base);
    r.clickId("tool.edit.undo"); // the whole stroke is one step
    CHECK(r.v().entityCount == base);
    r.clickId("tool.edit.redo");
    CHECK(r.v().entityCount > base);
}

void gateProfiler(Rig& r) {
    CHECK(r.shell.showPanel("dock.profiler"));
    r.frames(12);
    ProfilerView pv = r.shell.panels().profiler();
    CHECK(pv.frames >= 10u && !pv.history.empty());
    CHECK(!r.ui().text(r.ui().find("profiler.summary")).empty());
    r.clickId("profiler.pause");
    const u32 f = r.shell.panels().profiler().frames;
    r.frames(5);
    CHECK(r.shell.panels().profiler().frames == f);
    r.clickId("profiler.pause");
    r.clickId("profiler.clear");
    CHECK(r.shell.panels().profiler().history.size() <= 2u); // clear drops the history (the frame counter keeps counting)
    r.frames(6);
    std::printf("profiler: %s\n", r.shell.panels().profiler().summary.c_str());
    // hidden: no samples (the idle editor stays idle)
    r.shell.showPanel("dock.console");
    const u32 g = r.shell.panels().profiler().frames;
    r.frames(5);
    CHECK(r.shell.panels().profiler().frames == g);
    r.shell.showPanel("dock.profiler");
    r.frames();
}

void gateTimeline(Rig& r) {
    CHECK(r.shell.showPanel("dock.timeline"));
    r.c.runEntityAction(ContextMenuAction::CreateSdfSphere);
    r.frames(2);
    r.clickId("timeline.key");
    TimelineView tv = r.shell.panels().timeline();
    CHECK(tv.keyCount >= 3u && !tv.lanes.empty());
    // move the entity, key at 2 s
    r.ui().setValue(r.ui().find("timeline.time"), 2000.f, true);
    r.frames();
    CHECK(r.shell.panels().timeline().playheadMs == 2000);
    r.clickId("timeline.key");
    tv = r.shell.panels().timeline();
    CHECK(tv.keyCount >= 6u);
    // scrub through the widget ruler
    const Rect tl = r.rect("timeline.track");
    r.ui().pointerMove({tl.x + tl.w * 0.5f, tl.y + 6.f});
    r.ui().pointerDown({tl.x + tl.w * 0.5f, tl.y + 6.f});
    r.ui().pointerUp({tl.x + tl.w * 0.5f, tl.y + 6.f});
    r.frames();
    CHECK(r.shell.panels().timeline().playheadMs != 2000);
    r.ui().setValue(r.ui().find("timeline.time"), 2000.f, true);
    r.clickId("timeline.delete_key");
    CHECK(r.shell.panels().timeline().keyCount < tv.keyCount);
    r.clickId("tool.edit.undo");
    CHECK(r.shell.panels().timeline().keyCount == tv.keyCount);
    r.clickId("timeline.play");
    CHECK(r.shell.panels().timeline().playing && r.ui().text(r.ui().find("timeline.play")) == "Pause");
    const i32 p0 = r.shell.panels().timeline().playheadMs;
    for (int i = 0; i < 4; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); r.frames(); }
    CHECK(r.shell.panels().timeline().playheadMs != p0);
    r.clickId("timeline.play");
    std::printf("timeline: %s\n", r.shell.panels().timeline().status.c_str());
}

void gateSequencer(Rig& r) {
    CHECK(r.shell.showPanel("dock.sequencer"));
    r.frames();
    CHECK(r.ui().checked(r.ui().find("sequencer.link")));
    r.ui().setValue(r.ui().find("sequencer.scrub"), 0.5f, true);
    r.frames();
    SequencerView sv = r.shell.panels().sequencer();
    CHECK(sv.scrubMs == sv.rangeMs / 2 && !r.ui().text(r.ui().find("sequencer.preview")).empty());
    // linked: the timeline playhead drives the sequencer
    r.shell.panels().timelineSetPlayhead(1250);
    r.frames();
    CHECK(r.shell.panels().sequencer().scrubMs == 1250);
    r.clickId("sequencer.link");
    CHECK(!r.shell.panels().sequencer().linked);
    r.shell.panels().timelineSetPlayhead(3000);
    r.frames();
    CHECK(r.shell.panels().sequencer().scrubMs == 1250);
    std::printf("sequencer: %s\n", std::string(r.ui().text(r.ui().find("sequencer.preview"))).c_str());
}

// ---- live 3D viewport (zero-copy) -------------------------------------------------------------------------------------
std::string g_out;         // --out path (gates that write their own shot set g_shotWritten)
bool g_shotWritten = false;

[[noreturn]] void skip(const std::string& why) {
    std::printf("SKIP: %s\n", why.c_str());
    std::exit(77);
}

u32 validationErrors() {
#if defined(FUSE_HAS_VULKAN_RHI)
    const fuse::renderer::VulkanValidationCounters v = fuse::renderer::vulkanValidationCounters();
    if (v.errors) std::fprintf(stderr, "validation: %u error(s), last: %s\n", v.errors, v.lastError.c_str());
    return v.errors;
#else
    return 0;
#endif
}

struct GateScene { EntityRef cube, sphere, sun, lamp; };

void place(Rig& r, EntityRef e, float x, float y, float z) {
    r.shell.panels().withScene([&](EditorHost& h, const ViewportPanel&) {
        if (auto* t = h.editorScene().registry().get<fuse::ecs::Transform>(fuse::ecs::EntityID{e.index, e.generation}))
            t->position = {x, y, z, 1.f};
    });
}

/// SDF box left, sphere right (no overlap), a sun angled down onto them and a fill point light.
GateScene buildScene(Rig& r) {
    GateScene g;
    auto make = [&](ActionId id) {
        CHECK(r.c.trigger(id));
        r.frames();
        return r.c.lastCreated();
    };
    g.cube = make(ActionId::EntityCreateSdfBox); // a Mesh cube has no buffers yet (nothing to draw, Qt too)
    g.sphere = make(ActionId::EntityCreateSdfSphere);
    g.sun = make(ActionId::EntityCreateDirectionalLight);
    g.lamp = make(ActionId::EntityCreatePointLight);
    place(r, g.cube, 1.4f, 0.f, 0.f); // screen left (the camera looks down +Z)
    place(r, g.sphere, -1.4f, 0.f, 0.f);
    place(r, g.lamp, 0.f, 2.5f, -2.5f);
    r.shell.panels().withScene([&](EditorHost& h, const ViewportPanel&) {
        auto& reg = h.editorScene().registry();
        const fuse::ecs::EntityID sun{g.sun.index, g.sun.generation}, lamp{g.lamp.index, g.lamp.generation};
        if (auto* t = reg.get<fuse::ecs::Transform>(sun)) t->rotation = TransformGizmo::axisAngle({1.f, 0.f, 0.f, 0.f}, 0.8f);
        if (auto* l = reg.get<fuse::ecs::DirectionalLight>(sun)) l->intensity = 3.f;
        if (auto* l = reg.get<fuse::ecs::PointLight>(lamp)) l->intensity = 8.f;
    });
    r.c.refresh();
    r.frames(2);
    return g;
}

/// Clicks the cube, drags the Move gizmo's X handle, checks the overlay, the edit and its undo.
void gizmoDrag(Rig& r, const GateScene& g) {
    float px = 0.f, py = 0.f;
    CHECK(r.c.projectPoint(1.4f, 0.f, 0.f, px, py));
    using PE = ViewportPointerEvent;
    r.c.viewportPointer({PE::Kind::Down, PE::Button::Left, px, py});
    r.c.viewportPointer({PE::Kind::Up, PE::Button::Left, px, py});
    r.frames();
    CHECK(r.v().primary == g.cube);
    r.c.setTransformTool(TransformTool::Translate);
    r.frames();
    const EditorController::GizmoView gv = r.c.viewportGizmo();
    CHECK(gv.visible && gv.handles[0].visible);
    const fuse::ui::NodeHandle vp = r.ui().find("viewport");
    CHECK(w::viewportOverlay(r.ui(), vp).lines.size() == 33u); // 3 axes + 10-stroke arrow heads
    auto cubeX = [&] {
        float x = 0.f;
        r.shell.panels().withScene([&](EditorHost& h, const ViewportPanel&) {
            if (auto* t = h.editorScene().registry().get<fuse::ecs::Transform>(fuse::ecs::EntityID{g.cube.index, g.cube.generation})) x = t->position.x;
        });
        return x;
    };
    const float x0 = cubeX();
    const GizmoHandle2D& hx = gv.handles[0];
    const float dx = hx.x1 - hx.x0, dy = hx.y1 - hx.y0;
    r.c.viewportPointer({PE::Kind::Down, PE::Button::Left, hx.x1 - dx * 0.1f, hx.y1 - dy * 0.1f});
    CHECK(r.c.gizmoDragging());
    for (int i = 1; i <= 4; ++i) {
        r.c.viewportPointer({PE::Kind::Move, PE::Button::Left, hx.x1 + dx * 0.25f * float(i), hx.y1 + dy * 0.25f * float(i)});
        r.frames();
    }
    CHECK(w::viewportOverlay(r.ui(), vp).lines.size() == 33u); // hot axis drawn while dragging
    r.c.viewportPointer({PE::Kind::Up, PE::Button::Left, hx.x1 + dx, hx.y1 + dy});
    r.frames();
    CHECK(!r.c.gizmoDragging());
    const float x1 = cubeX();
    std::printf("gizmo: cube x %.2f -> %.2f\n", double(x0), double(x1));
    CHECK(std::fabs(x1 - x0) > 0.2f);
    CHECK(r.v().primary == g.cube); // a handle drag does not re-pick
    r.c.undo();
    r.c.tick();
    r.c.refresh();
    r.frames();
    CHECK(std::fabs(cubeX() - x0) < 1e-4f); // one undo step per drag
    r.c.redo();
    r.c.tick();
    r.c.refresh();
    r.frames();
    CHECK(std::fabs(cubeX() - x1) < 1e-4f);
    r.c.setTransformTool(TransformTool::Select);
    r.frames();
    CHECK(w::viewportOverlay(r.ui(), vp).lines.empty());
    // keys 1-4 pick the tool (viewport focused); ring / box handle shapes
    r.ui().setFocus(vp);
    r.ui().keyDown(Key::Num3);
    r.ui().keyUp(Key::Num3);
    r.frames();
    CHECK(r.c.transformTool() == TransformTool::Rotate);
    CHECK(w::viewportOverlay(r.ui(), vp).lines.size() == 3u + 3u * 20u); // axes + 20-segment rings
    r.ui().keyDown(Key::Num4);
    r.ui().keyUp(Key::Num4);
    r.frames();
    CHECK(r.c.transformTool() == TransformTool::Scale);
    CHECK(w::viewportOverlay(r.ui(), vp).lines.size() == 6u); // axes + boxes
    // space / snap buttons
    r.clickId("viewport.space");
    r.frames();
    CHECK(r.c.transformSpace() == TransformSpace::Local);
    CHECK(r.ui().text(r.ui().find("viewport.space")) == "Local");
    r.clickId("viewport.snap");
    r.frames();
    CHECK(r.c.transformSnap());
    CHECK(r.ui().hasClass(r.ui().find("viewport.snap"), "active"));
    r.clickId("viewport.snap");
    r.clickId("viewport.space");
    r.frames();
    CHECK(!r.c.transformSnap() && r.c.transformSpace() == TransformSpace::World);
    r.ui().setFocus(vp); // the buttons took focus
    r.ui().keyDown(Key::Num2);
    r.ui().keyUp(Key::Num2);
    r.frames();
    CHECK(r.c.transformTool() == TransformTool::Translate);
}

void gateViewportGizmo(Rig& r) {
    // software viewport (no device needed): picking + Move gizmo drag + overlay + undo
    const GateScene g = buildScene(r);
    gizmoDrag(r, g);
}

/// Distinct colours in the centre half of an image (a flat fill has 1-2; a lit 3D frame many).
size_t colourCount(const fuse::ui::Image& img, int x0, int y0, int w, int h) {
    std::map<u32, int> seen;
    for (int y = y0 + h / 4; y < y0 + h * 3 / 4; y += 2)
        for (int x = x0 + w / 4; x < x0 + w * 3 / 4; x += 2) seen[img.at(x, y) & 0xF8F8F8u]++;
    return seen.size();
}

void gateSceneViewport(Rig& r) {
    // CPU-copy path (headless shells, screenshots): SceneViewportHost renders the scene, the frame is read back into
    // the viewport image and the CPU rasteriser shows it.
    auto host = SceneViewportHost::create({.validation = true});
    if (!host->valid()) skip("scene renderer host: " + host->reason());
    std::printf("scene host device: %s\n", host->deviceName().c_str());
    const GateScene scene = buildScene(r);
    (void)scene;
    r.shell.setSceneViewport(host.get());
    r.frames(2);
    CHECK(r.shell.sceneFrames() >= 1 && host->framesRendered() >= 1);
    const fuse::ui::Image& img = r.shell.viewportImage();
    const Rect vp = r.rect("viewport");
    CHECK(img.width >= int(vp.w) - 2 && img.height >= int(vp.h) - 2);
    CHECK(img.externalSlot == -1);
    const size_t colours = colourCount(img, 0, 0, img.width, img.height);
    std::printf("scene_viewport: %dx%d frame, %zu colours, %u frames\n", img.width, img.height, colours, r.shell.sceneFrames());
    CHECK(colours >= 8);
    CHECK(r.ui().text(r.ui().find("viewport.info")).find("GPU (readback)") != std::string_view::npos);
    // the window raster shows the frame
    r.win.rasterize({});
    const fuse::ui::Image& shot = r.win.image();
    const fuse::ui::Rect ir = w::viewportImageRect(r.ui(), r.ui().find("viewport"));
    CHECK(colourCount(shot, int(ir.x), int(ir.y), int(ir.w), int(ir.h)) >= 8);
    // idle: no new frame without a change; a camera move renders one
    const fuse::u64 n = host->framesRendered();
    r.frames(3);
    CHECK(host->framesRendered() == n);
    r.c.viewportWheel(2.f);
    r.frames();
    CHECK(host->framesRendered() == n + 1);
    // scene edits re-render too
    CHECK(r.c.trigger(ActionId::EntityCreateCube));
    r.frames(2);
    CHECK(host->framesRendered() >= n + 2);
    CHECK(validationErrors() == 0);
    r.shell.setSceneViewport(nullptr); // the host dies with this gate
}

void gateGpuViewport(Rig& r) {
    // Zero-copy: the shell window is presented by fuse_app's Vulkan presenter ON the scene host's device; the viewport
    // Image is external slot 0, so the UI kernel samples the engine frame directly (no readback, no upload).
#if defined(FUSE_EDITOR_UI_HAS_NATIVE)
    auto host = SceneViewportHost::create({.validation = true});
    if (!host->valid()) skip("scene renderer host: " + host->reason());
    fuse::app::NativeOptions o;
    o.adoptDevice = {host->instanceHandle(), host->physicalDeviceHandle(), host->deviceHandle(), host->queueHandle(),
                     host->queueFamily()};
    o.drawMode = fuse::app::VkDrawMode::GpuDraw;
    o.vsync = false;
    {
        fuse::app::NativePlatform native(r.app, o);
        if (!native.available()) skip("no display");
        const fuse::app::PresenterKind kind = native.show(r.win);
        if (kind != fuse::app::PresenterKind::Vulkan) skip(std::string("no Vulkan presenter: ") + std::string(native.lastError()));
        CHECK(native.gpuDraws(r.win));
        CHECK(native.gpuName() == host->deviceName());
        std::printf("gpu_viewport: presenter + scene renderer on one device: %s\n", host->deviceName().c_str());
        const GateScene scene = buildScene(r);
        u32 binds = 0;
        r.shell.setSceneViewport(host.get(), [&](const SceneViewportFrame& f) {
            ++binds;
            CHECK(f.image && f.view && f.layout != 0);
            CHECK(native.setExternalTexture(r.win, 0, {f.image, f.view, f.layout}));
        });
        for (int i = 0; i < 6; ++i) {
            r.frames();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        CHECK(binds >= 1 && r.shell.sceneFrames() >= 1);
        const u32 framesBeforeDrag = r.shell.sceneFrames();
        gizmoDrag(r, scene); // pick + drag in the GPU viewport: every edit renders a new engine frame
        CHECK(r.shell.sceneFrames() > framesBeforeDrag);
        {
            // Create Cube renders (builtin cube mesh): the engine frame changes where the cube appears
            std::vector<u32> before, after;
            u32 bw = 0, bh = 0, aw = 0, ah = 0;
            CHECK(host->readback(before, bw, bh));
            const u32 f0 = r.shell.sceneFrames();
            CHECK(r.c.trigger(ActionId::EntityCreateCube));
            for (int i = 0; i < 100 && r.shell.sceneFrames() <= f0; ++i) {
                r.frames();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            CHECK(host->readback(after, aw, ah));
            // the cube's projected bounds (unit cube at the origin), in engine-frame pixels
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (int k = 0; k < 8; ++k) {
                float px = 0.f, py = 0.f;
                if (!r.c.projectPoint(k & 1 ? 0.5f : -0.5f, k & 2 ? 0.5f : -0.5f, k & 4 ? 0.5f : -0.5f, px, py)) continue;
                x0 = std::min(x0, px), y0 = std::min(y0, py), x1 = std::max(x1, px), y1 = std::max(y1, py);
            }
            CHECK(x1 > x0 && y1 > y0);
            // significant change (> 6 in a channel; ignores frame-to-frame noise), inside vs outside the bounds
            std::size_t inN = 0, inChanged = 0, outN = 0, outChanged = 0;
            if (bw == aw && bh == ah && x1 > x0)
                for (u32 y = 0; y < ah; ++y)
                    for (u32 x = 0; x < aw; ++x) {
                        const u32 a = before[std::size_t(y) * aw + x], b = after[std::size_t(y) * aw + x];
                        bool diff = false;
                        for (int sh = 0; sh < 24; sh += 8)
                            if (std::abs(int((a >> sh) & 255) - int((b >> sh) & 255)) > 6) diff = true;
                        // inset by 2 px so edge pixels count on neither side
                        const bool in = float(x) > x0 + 2.f && float(x) < x1 - 2.f && float(y) > y0 + 2.f && float(y) < y1 - 2.f;
                        const bool out = float(x) < x0 - 2.f || float(x) > x1 + 2.f || float(y) < y0 - 2.f || float(y) > y1 + 2.f;
                        if (in) ++inN, inChanged += diff;
                        else if (out) ++outN, outChanged += diff;
                    }
            const double inFrac = inN ? double(inChanged) / double(inN) : 0.0, outFrac = outN ? double(outChanged) / double(outN) : 1.0;
            std::printf("gpu_viewport: Create Cube frame %ux%u bounds (%.0f,%.0f)-(%.0f,%.0f): %.1f%% changed inside, %.2f%% outside\n",
                        aw, ah, double(x0), double(y0), double(x1), double(y1), inFrac * 100.0, outFrac * 100.0);
            CHECK(inN > 400 && inFrac > 0.5);  // the cube covers most of its projected box
            CHECK(outFrac < 0.05);             // and little else moved (shadow / contact only)
        }
        const fuse::ui::Image& img = r.shell.viewportImage();
        CHECK(img.externalSlot == 0);
        // zero-copy: the CPU pixels were never written (still the ChildBg fill), yet the window shows the 3D frame
        CHECK(near8(img.at(img.width / 2, img.height / 2), "ChildBg"));
        CHECK(r.ui().text(r.ui().find("viewport.info")).find("GPU (zero-copy)") != std::string_view::npos);
        // let Map / Expose settle, then wait for a fresh present of the current frame
        for (int i = 0; i < 10; ++i) r.app.step(0.005f);
        r.win.requestRedraw();
        const auto presented = r.win.framesPresented();
        for (int i = 0; i < 200 && r.win.framesPresented() <= presented; ++i) {
            r.frames();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(r.win.framesPresented() > presented);
        fuse::ui::Image cap;
        CHECK(native.capture(r.win, cap));
        const fuse::ui::Rect ir = w::viewportImageRect(r.ui(), r.ui().find("viewport"));
        size_t colours = 0;
        if (cap.width > 0) colours = colourCount(cap, int(ir.x), int(ir.y), int(ir.w), int(ir.h));
        // the window's viewport pixels are the engine frame: compare with the host's own readback copy
        std::vector<u32> ref;
        u32 rw = 0, rh = 0;
        CHECK(host->readback(ref, rw, rh));
        int matched = 0, total = 0;
        if (cap.width > 0 && rw > 0) {
            for (int k = 1; k < 8; ++k)
                for (int j = 1; j < 8; ++j) {
                    const int fx = int(rw) * k / 8, fy = int(rh) * j / 8;
                    const int wx = int(ir.x + (fx + 0.5f) * ir.w / float(rw)), wy = int(ir.y + (fy + 0.5f) * ir.h / float(rh));
                    if (wx >= cap.width || wy >= cap.height) continue;
                    const u32 a = cap.at(wx, wy), b = ref[std::size_t(fy) * rw + std::size_t(fx)];
                    bool ok = true;
                    for (int s = 0; s < 24; s += 8)
                        if (std::abs(int((a >> s) & 255) - int((b >> s) & 255)) > 6) ok = false;
                    matched += ok;
                    ++total;
                }
        }
        std::printf("gpu_viewport: capture %dx%d, viewport %.0fx%.0f, %zu colours, %d/%d samples match the engine frame, "
                    "%u binds\n", cap.width, cap.height, ir.w, ir.h, colours, matched, total, binds);
        CHECK(colours >= 8);
        CHECK(total > 0 && matched >= total * 9 / 10);
        // camera move: a new engine frame, re-bound, shown
        const fuse::u64 n = host->framesRendered();
        r.c.viewportWheel(2.f);
        r.frames(2);
        CHECK(host->framesRendered() > n);
        CHECK(validationErrors() == 0);
        if (!g_out.empty() && cap.width > 0) {
            CHECK(fuse::app::writePng(cap, g_out));
            g_shotWritten = true;
        }
        r.shell.setSceneViewport(nullptr);
    } // presenter destroyed before the device it adopted
#else
    (void)r;
    skip("built without fuse_app_native");
#endif
}

// ---- icon: window + executable icon (Qt twin: fuse_editor_qt_icon, Editor/src/qt/app_icon.cpp) -----------------------
void gateIcon(Rig& r) {
    const std::vector<fuse::ui::Image> icons = fui::applicationIcon();
    CHECK(icons.size() == fui::kAppIconSizes.size());
    // atlas of every size side by side (the gate's screenshot)
    fuse::ui::Image atlas;
    atlas.resize(16 + 32 + 48 + 256 + 5 * 8, 256 + 16, 0xFF202020u);
    int ax = 8;
    for (std::size_t i = 0; i < icons.size() && i < fui::kAppIconSizes.size(); ++i) {
        const fuse::ui::Image& img = icons[i];
        CHECK(img.width == fui::kAppIconSizes[i] && img.height == fui::kAppIconSizes[i]);
        std::size_t opaque = 0;
        std::vector<u32> distinct;
        for (u32 p : img.pixels) {
            opaque += (p >> 24) > 128;
            if (distinct.size() < 64 && std::find(distinct.begin(), distinct.end(), p) == distinct.end()) distinct.push_back(p);
        }
        std::printf("icon: %dx%d, %zu%% opaque, %zu+ colours\n", img.width, img.height, opaque * 100 / std::max<std::size_t>(1, img.pixels.size()),
                    distinct.size());
        CHECK(opaque * 4 >= img.pixels.size()); // a real raster, not an empty placeholder
        CHECK(distinct.size() >= 2);
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x) {
                const u32 p = img.pixels[std::size_t(y) * std::size_t(img.width) + std::size_t(x)];
                if ((p >> 24) > 0) atlas.pixels[std::size_t(y + 8) * std::size_t(atlas.width) + std::size_t(ax + x)] = p | 0xFF000000u;
            }
        ax += img.width + 8;
    }
#if defined(_WIN32)
    // executable icon: Branding/fuse_editor.rc (fuse.ico) linked into the exe, like the Qt fuse_editor.exe
    HICON exeIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), L"IDI_ICON1", IMAGE_ICON, 32, 32, 0));
    std::printf("icon: executable icon resource IDI_ICON1 %s\n", exeIcon ? "present" : "MISSING");
    CHECK(exeIcon != nullptr);
    if (exeIcon) DestroyIcon(exeIcon);
#endif
#if defined(FUSE_EDITOR_UI_HAS_NATIVE)
    {
        fuse::app::NativeOptions o;
        o.vulkan = false; // the icon needs only the OS window
        fuse::app::NativePlatform native(r.app, o);
        if (native.available()) {
            native.show(r.win);
            CHECK(native.setIcon(r.win, icons));
            r.frames(2);
            const std::vector<int> got = native.iconSizes(r.win);
            std::string list;
            for (int v : got) list += (list.empty() ? "" : ",") + std::to_string(v);
            std::printf("icon: OS window icon sizes [%s]\n", list.c_str());
            CHECK(!got.empty());
            for (int v : got) CHECK(std::find(fui::kAppIconSizes.begin(), fui::kAppIconSizes.end(), v) != fui::kAppIconSizes.end());
#if !defined(_WIN32)
            CHECK(got.size() == fui::kAppIconSizes.size()); // _NET_WM_ICON carries every raster
#endif
        } else {
            std::printf("icon: no display, OS window icon not checked (%s)\n", std::string(native.lastError()).c_str());
        }
    }
#endif
    if (!g_out.empty()) {
        CHECK(fuse::app::writePng(atlas, g_out));
        g_shotWritten = true;
    }
}

// ---- live_present: the live editor presents the viewport, resizes, forwards input, tears down cleanly ----------------
void gateLivePresent(Rig& r) {
#if defined(FUSE_EDITOR_UI_HAS_NATIVE)
    auto host = SceneViewportHost::create({.validation = true, .readback = true});
    if (!host->valid()) skip("scene renderer host: " + host->reason());
    fuse::app::NativeOptions o;
    o.adoptDevice = {host->instanceHandle(), host->physicalDeviceHandle(), host->deviceHandle(), host->queueHandle(),
                     host->queueFamily()};
    o.drawMode = fuse::app::VkDrawMode::GpuDraw;
    o.vsync = false;
    fuse::ui::Image shot;
    {
        fuse::app::NativePlatform native(r.app, o);
        if (!native.available()) skip("no display");
        if (native.show(r.win) != fuse::app::PresenterKind::Vulkan) skip(std::string("no Vulkan presenter: ") + std::string(native.lastError()));
        CHECK(native.setIcon(r.win, fui::applicationIcon()));
        r.shell.setSceneViewport(host.get(), [&](const SceneViewportFrame& f) { native.setExternalTexture(r.win, 0, {f.image, f.view, f.layout}); });
        const auto settle = [&](int n) {
            for (int i = 0; i < n; ++i) {
                r.frames();
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        };
        const auto presentNow = [&] {
            r.win.requestRedraw();
            const auto before = r.win.framesPresented();
            for (int i = 0; i < 200 && r.win.framesPresented() <= before; ++i) settle(1);
            return r.win.framesPresented() > before;
        };
        settle(8);
        // 1. presents: the window shows the engine frame inside the viewport
        CHECK(presentNow());
        fuse::ui::Image cap;
        CHECK(native.capture(r.win, cap));
        fuse::ui::Rect ir = w::viewportImageRect(r.ui(), r.ui().find("viewport"));
        const size_t colours0 = cap.width > 0 ? colourCount(cap, int(ir.x), int(ir.y), int(ir.w), int(ir.h)) : 0;
        std::printf("live_present: %dx%d window, viewport %.0fx%.0f, %zu colours, %llu scene frames\n", cap.width, cap.height,
                    ir.w, ir.h, colours0, (unsigned long long)host->framesRendered());
        CHECK(colours0 >= 8);
        // 2. resize: the UI relayouts, the viewport and the engine frame follow, presenting continues
        const fuse::ui::Rect irBefore = ir;
        const fuse::u64 rendered0 = host->framesRendered();
        // an OS-level resize (XResizeWindow / SetWindowPos), delivered back like a user drag of the window edge
        CHECK(native.resizeWindow(r.win, 1280, 720));
        for (int i = 0; i < 200 && r.ui().viewport().w != 1280.f; ++i) settle(1);
        settle(10);
        CHECK(presentNow());
        ir = w::viewportImageRect(r.ui(), r.ui().find("viewport"));
        CHECK(r.ui().viewport().w == 1280.f && r.ui().viewport().h == 720.f);
        CHECK(ir.w < irBefore.w && ir.h < irBefore.h);
        CHECK(host->framesRendered() > rendered0);
        CHECK(native.capture(r.win, cap));
        std::printf("live_present: resized to 1280x720: viewport %.0fx%.0f, capture %dx%d\n", ir.w, ir.h, cap.width, cap.height);
        CHECK(cap.width > 0 && colourCount(cap, int(ir.x), int(ir.y), int(std::min<float>(ir.w, float(cap.width) - ir.x)),
                                           int(std::min<float>(ir.h, float(cap.height) - ir.y))) >= 8);
        // 3. forwarded input: RMB-fly + W through the window's input path moves the engine camera and renders anew
        const auto p0 = r.c.viewport().position();
        const fuse::u64 rendered1 = host->framesRendered();
        Vec2 at{ir.x + ir.w * 0.5f, ir.y + ir.h * 0.5f};
        r.win.pointerMove(at);
        r.ui().pointerDown(at, fuse::ui::MouseButton::Right);
        for (int i = 0; i < 3; ++i) { at.x += 4.f; r.win.pointerMove(at); }
        r.ui().keyDown(Key::W);
        settle(10);
        r.ui().keyUp(Key::W);
        r.ui().pointerUp(at, fuse::ui::MouseButton::Right);
        settle(2);
        const auto p1 = r.c.viewport().position();
        const float moved = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y) + (p1.z - p0.z) * (p1.z - p0.z));
        std::printf("live_present: forwarded input moved the camera %.3f, %llu new scene frames\n", moved,
                    (unsigned long long)(host->framesRendered() - rendered1));
        CHECK(moved > 0.05f);
        CHECK(host->framesRendered() > rendered1);
        CHECK(!r.c.contextMenuOpen());
        CHECK(presentNow());
        CHECK(native.capture(r.win, shot));
        CHECK(validationErrors() == 0);
        // 4. teardown: unbind the scene, then the presenter goes before the device it adopted
        r.shell.setSceneViewport(nullptr);
    }
    CHECK(r.win.framesPresented() > 0);
    host.reset();
    r.frames(2); // the shell keeps running headless after the native window is gone
    CHECK(r.win.image().width == 1280);
    CHECK(validationErrors() == 0);
    std::printf("live_present: clean teardown\n");
    if (!g_out.empty() && shot.width > 0) {
        CHECK(fuse::app::writePng(shot, g_out));
        g_shotWritten = true;
    }
#else
    (void)r;
    skip("built without fuse_app_native");
#endif
}


// View > View Mode (Lit / Unlit / Wireframe / Lit Wireframe, Alt+1..4) and Show Mesh Bounds: Qt parity.
std::size_t overlayLines(Rig& r, fuse::u32 hex) {
    std::size_t n = 0;
    const fuse::ui::Color want = fuse::ui::Color::rgb(hex);
    for (const auto& l : w::viewportOverlay(r.ui(), r.ui().find("viewport")).lines)
        if (std::fabs(l.color.r - want.r) < 0.01f && std::fabs(l.color.g - want.g) < 0.01f && std::fabs(l.color.b - want.b) < 0.01f) ++n;
    return n;
}
std::string menuText(Rig& r, const char* menu) { return std::string(r.ui().text(r.shell.menuNode(menu))); }

void gateViewModes(Rig& r) {
    CHECK(r.c.viewMode() == EditorViewMode::Lit && !r.c.showMeshBounds());
    CHECK(menuText(r, "View").find("[x] View Mode: Lit   (Alt+1)") != std::string::npos);
    CHECK(menuText(r, "View").find("[ ] Show Mesh Bounds") != std::string::npos);
    auto host = SceneViewportHost::create({.validation = true});
    const bool gpu = host->valid();
    if (!gpu) std::printf("view_modes: no scene renderer (%s): overlay checks only\n", host->reason().c_str());
    const GateScene scene = buildScene(r);
    CHECK(r.c.trigger(ActionId::EntityCreateCube)); // a Mesh entity (bounds)
    r.frames();
    if (gpu) r.shell.setSceneViewport(host.get());
    r.frames(3);
    auto grab = [&](std::vector<u32>& px, u32& w, u32& h) {
        const fuse::u64 f0 = host->framesRendered();
        for (int i = 0; i < 50 && host->framesRendered() <= f0; ++i) r.frames();
        return host->readback(px, w, h);
    };
    std::vector<u32> lit, unlit;
    u32 w0 = 0, h0 = 0, w1 = 0, h1 = 0;
    if (gpu) { r.c.viewportWheel(0.5f); CHECK(grab(lit, w0, h0)); }
    r.ui().setFocus(r.ui().find("viewport")); // window shortcuts route through the focused widget
    // Alt+2: Unlit (menu radio moves; the engine frame changes on the lit surfaces)
    r.ui().keyDown(Key::Num2, fuse::ui::kModAlt);
    r.frames();
    r.ui().keyUp(Key::Num2, fuse::ui::kModAlt);
    CHECK(r.c.viewMode() == EditorViewMode::Unlit);
    CHECK(menuText(r, "View").find("[x] View Mode: Unlit") != std::string::npos);
    CHECK(menuText(r, "View").find("[ ] View Mode: Lit ") != std::string::npos);
    if (gpu) {
        CHECK(grab(unlit, w1, h1));
        std::size_t changed = 0;
        if (w0 == w1 && h0 == h1)
            for (std::size_t i = 0; i < lit.size(); ++i)
                for (int sh = 0; sh < 24; sh += 8)
                    if (std::abs(int((lit[i] >> sh) & 255) - int((unlit[i] >> sh) & 255)) > 6) { ++changed; break; }
        std::printf("view_modes: unlit changed %.1f%% of the frame\n", lit.empty() ? 0.0 : 100.0 * double(changed) / double(lit.size()));
        CHECK(changed > lit.size() / 100);
    }
    CHECK(overlayLines(r, 0xE6E6F0) == 0); // no wire in unlit
    // Alt+3: Wireframe -> the real triangle edges of the SDF proxies + mesh are drawn over the frame
    r.ui().keyDown(Key::Num3, fuse::ui::kModAlt);
    r.frames(2);
    r.ui().keyUp(Key::Num3, fuse::ui::kModAlt);
    CHECK(r.c.viewMode() == EditorViewMode::Wireframe && editorViewModeWire(r.c.viewMode()));
    const std::size_t wire = overlayLines(r, 0xE6E6F0), wireSel = overlayLines(r, 0xFFC850);
    std::printf("view_modes: wireframe %zu edges (+%zu selected)\n", wire, wireSel);
    if (gpu) CHECK(wire + wireSel >= 24);
    // Show Mesh Bounds (menu): 12 AABB edges of the selected cube mesh in the Qt selected-bounds colour
    const auto idx = r.shell.menuItemIndex("View", ActionId::ViewShowMeshBounds);
    CHECK(idx >= 0);
    r.ui().emit(r.shell.menuNode("View"), fuse::ui::SignalKind::Submitted, {.value = float(idx)});
    r.frames();
    CHECK(r.c.showMeshBounds());
    CHECK(menuText(r, "View").find("[x] Show Mesh Bounds") != std::string::npos);
    const std::size_t bounds = overlayLines(r, 0x78FF8C) + overlayLines(r, 0x5AC86E);
    std::printf("view_modes: mesh bounds %zu edges\n", bounds);
    CHECK(bounds == 12);
    // Alt+4 Lit Wireframe keeps the wire; Alt+1 Lit drops it (bounds stay)
    r.ui().keyDown(Key::Num4, fuse::ui::kModAlt);
    r.frames(2);
    r.ui().keyUp(Key::Num4, fuse::ui::kModAlt);
    CHECK(r.c.viewMode() == EditorViewMode::LitWireframe);
    if (gpu) CHECK(overlayLines(r, 0xE6E6F0) + overlayLines(r, 0xFFC850) == wire + wireSel);
    CHECK(r.c.trigger(ActionId::ViewModeLit));
    r.frames();
    CHECK(overlayLines(r, 0xE6E6F0) + overlayLines(r, 0xFFC850) == 0 && overlayLines(r, 0x78FF8C) + overlayLines(r, 0x5AC86E) == 12);
    (void)scene;
    // screenshot: Lit Wireframe + bounds
    CHECK(r.c.trigger(ActionId::ViewModeLitWireframe));
    for (int i = 0; i < 3; ++i) r.frames();
    if (gpu) CHECK(validationErrors() == 0);
    std::vector<u32> keep;
    if (gpu) { u32 kw = 0, kh = 0; CHECK(grab(keep, kw, kh)); }
    r.win.rasterize({});
    if (!g_out.empty()) { CHECK(r.win.writePng(g_out)); g_shotWritten = true; }
    r.shell.setSceneViewport(nullptr); // the host dies with this gate
}


// Edit > Preferences... (Qt PreferencesDialog parity): general options, shortcut list + filter, conflicts, persist.
void gatePreferences(Rig& r) {
    CHECK(menuText(r, "Edit").find("Preferences...") != std::string::npos);
    CHECK(fui::EditorShellWindow::shortcutConflicts().empty());
    CHECK(editorPreviewSun() && std::fabs(r.c.viewport().camera().moveSpeed - 10.f) < 1e-4f);
    const auto idx = r.shell.menuItemIndex("Edit", ActionId::EditPreferences);
    CHECK(idx >= 0);
    r.ui().emit(r.shell.menuNode("Edit"), fuse::ui::SignalKind::Submitted, {.value = float(idx)});
    r.frames();
    CHECK(r.shell.preferencesOpen());
    CHECK(r.ui().checked(r.ui().find("prefs.preview_sun")) && r.ui().checked(r.ui().find("prefs.restore_layout")));
    CHECK(std::fabs(r.ui().value(r.ui().find("prefs.camera_speed")) - 10.f) < 1e-4f);
    const std::string all(r.ui().text(r.ui().find("prefs.shortcuts")));
    CHECK(all.find("Undo  [Edit]   Ctrl+Z") != std::string::npos && all.find("View Mode: Wireframe  [View]   Alt+3") != std::string::npos);
    CHECK(r.ui().text(r.ui().find("prefs.conflicts")) == "No shortcut conflicts.");
    // filter (Qt filterRows): "alt+" keeps only the four view modes
    r.ui().setText(r.ui().find("prefs.filter"), "alt+");
    r.ui().emit(r.ui().find("prefs.filter"), fuse::ui::SignalKind::TextChanged, {});
    r.frames();
    const std::string filtered(r.ui().text(r.ui().find("prefs.shortcuts")));
    CHECK(std::count(filtered.begin(), filtered.end(), '\n') == 3 && filtered.find("Lit Wireframe") != std::string::npos);
    // Cancel discards edits
    r.ui().setChecked(r.ui().find("prefs.preview_sun"), false);
    r.clickId("prefs.cancel");
    CHECK(!r.shell.preferencesOpen() && editorPreviewSun());
    // edit + OK: applied to the viewport camera / preview sun and persisted in the app settings
    CHECK(r.c.trigger(ActionId::EditPreferences));
    r.frames();
    r.ui().setChecked(r.ui().find("prefs.preview_sun"), false);
    r.ui().setValue(r.ui().find("prefs.camera_speed"), 25.f);
    r.ui().setValue(r.ui().find("prefs.look_sensitivity"), 0.5f);
    r.ui().setChecked(r.ui().find("prefs.restore_layout"), false);
    r.frames();
    if (!g_out.empty()) { r.win.rasterize({}); CHECK(r.win.writePng(g_out)); g_shotWritten = true; }
    r.clickId("prefs.ok");
    CHECK(!r.shell.preferencesOpen());
    CHECK(!editorPreviewSun());
    CHECK(std::fabs(r.c.viewport().camera().moveSpeed - 25.f) < 1e-4f && std::fabs(r.c.viewport().camera().lookSensitivity - 0.5f) < 1e-4f);
    const auto& s = r.app.settings();
    CHECK(!s.flag("prefs.previewSun", true) && std::fabs(s.number("prefs.cameraSpeed") - 25.0) < 1e-6 && !s.flag("prefs.restoreLayout", true));
    // a new shell on the same app picks them up (startup load)
    auto& win2 = r.app.createWindow({.title = "FUSE Editor 2", .width = 1200, .height = 700});
    EditorHost host2;
    test::ScriptedServices sv2;
    EditorController c2(host2, sv2);
    setEditorPreviewSun(true);
    {
        fui::EditorShellWindow shell2(win2, c2, {});
        CHECK(!c2.preferences().previewSun && !editorPreviewSun() && std::fabs(c2.viewport().camera().moveSpeed - 25.f) < 1e-4f);
        std::printf("preferences: speed %.1f look %.2f sun %d restore %d (reloaded)\n", c2.preferences().cameraSpeed,
                    c2.preferences().lookSensitivity, int(c2.preferences().previewSun), int(c2.preferences().restoreLayout));
    }
    setEditorPreviewSun(true);
}


// ---- Qt parity: shortcut rebinding, recent projects, group / frame / duplicate subtree, colliders, open script ----
namespace {
EntityRef makeEntity(Rig& r, float x, float y, float z) {
    CHECK(r.c.trigger(ActionId::EntityCreateEmpty));
    r.frames();
    const EntityRef e = r.c.lastCreated();
    r.shell.panels().withScene([&](EditorHost& h, const ViewportPanel&) {
        if (auto* t = h.editorScene().registry().get<fuse::ecs::Transform>(fuse::ecs::EntityID{e.index, e.generation})) t->position = {x, y, z, 1.f};
    });
    r.c.refresh();
    r.frames();
    return e;
}
void viewportKey(Rig& r, Key k, fuse::ui::u8 mods) {
    r.ui().setFocus(r.ui().find("viewport"));
    r.ui().keyDown(k, mods);
    r.ui().keyUp(k, mods);
    r.frames();
}
std::size_t rowsLabelled(Rig& r, std::string_view label) {
    std::size_t n = 0;
    for (const auto& row : r.v().hierarchy) n += row.label == label;
    return n;
}
void shot(Rig& r) {
    if (!g_out.empty()) { r.win.rasterize({}); CHECK(r.win.writePng(g_out)); g_shotWritten = true; }
}
} // namespace

// Preferences shortcut rebinding (Qt PreferencesDialog: QKeySequenceEdit, conflicts, Reset, persisted per action).
void gateShortcutRebind(Rig& r) {
    CHECK(r.shell.binding(ActionId::EntityGroup) == "Ctrl+G");
    CHECK(fui::EditorShellWindow::normalizeShortcut("shift+ctrl+k") == "Ctrl+Shift+K");
    CHECK(fui::EditorShellWindow::normalizeShortcut("Ctrl+Nope").empty());
    CHECK(menuText(r, "Entity").find("Group Selection   (Ctrl+G)") != std::string::npos);
    CHECK(r.c.trigger(ActionId::EditPreferences));
    r.frames();
    // a conflicting binding is shown live and OK / Apply refuse it
    CHECK(r.shell.setPreferencesShortcut("entity.group", "Ctrl+D"));
    r.frames();
    const std::string conflicts(r.ui().text(r.ui().find("prefs.conflicts")));
    std::printf("shortcut_rebind: %s\n", conflicts.c_str());
    CHECK(conflicts.find("Ctrl+D: ") != std::string::npos && conflicts.find("Group Selection") != std::string::npos);
    r.clickId("prefs.apply");
    CHECK(r.shell.preferencesOpen() && r.shell.binding(ActionId::EntityGroup) == "Ctrl+G");
    r.clickId("prefs.reset_all");
    CHECK(r.shell.preferencesConflicts().empty());
    // select the row, record Ctrl+Shift+K in the shortcut field, Assign
    setField(r, "prefs.filter", "group sel");
    r.ui().emit(r.ui().find("prefs.filter"), fuse::ui::SignalKind::TextChanged, {});
    r.frames();
    CHECK(std::string(r.ui().text(r.ui().find("prefs.shortcuts"))).find("Group Selection  [Entity]   Ctrl+G") == 0);
    r.ui().setValue(r.ui().find("prefs.shortcuts"), 0.f, true);
    r.frames();
    CHECK(r.ui().text(r.ui().find("prefs.shortcut_edit")) == "Ctrl+G");
    r.ui().setFocus(r.ui().find("prefs.shortcut_edit"));
    r.ui().keyDown(Key::K, fuse::ui::kModCtrl | fuse::ui::kModShift);
    r.frames();
    CHECK(r.ui().text(r.ui().find("prefs.shortcut_edit")) == "Ctrl+Shift+K");
    r.clickId("prefs.assign");
    CHECK(std::string(r.ui().text(r.ui().find("prefs.shortcuts"))).find("Group Selection  [Entity]   Ctrl+Shift+K") == 0);
    CHECK(r.ui().text(r.ui().find("prefs.shortcut_edit")) == "Ctrl+Shift+K"); // the field keeps the assigned binding
    CHECK(r.ui().text(r.ui().find("prefs.conflicts")) == "No shortcut conflicts.");
    shot(r);
    r.clickId("prefs.ok");
    CHECK(!r.shell.preferencesOpen() && r.shell.binding(ActionId::EntityGroup) == "Ctrl+Shift+K");
    CHECK(r.app.settings().get("shortcut.entity.group") == "Ctrl+Shift+K");
    CHECK(menuText(r, "Entity").find("Group Selection   (Ctrl+Shift+K)") != std::string::npos);
    // the new key groups, the old one no longer does
    const EntityRef a = makeEntity(r, -1.f, 0.f, 0.f), b = makeEntity(r, 1.f, 0.f, 0.f);
    const EntityRef both[] = {a, b};
    r.c.select(both, a);
    r.frames();
    viewportKey(r, Key::G, fuse::ui::kModCtrl);
    CHECK(rowsLabelled(r, "Group") == 0);
    viewportKey(r, Key::K, fuse::ui::kModCtrl | fuse::ui::kModShift);
    CHECK(rowsLabelled(r, "Group") == 1);
    // a new shell on the same settings loads the binding
    auto& win2 = r.app.createWindow({.title = "FUSE Editor 2", .width = 1200, .height = 700});
    EditorHost host2;
    test::ScriptedServices sv2;
    EditorController c2(host2, sv2);
    {
        fui::EditorShellWindow shell2(win2, c2, {});
        CHECK(shell2.binding(ActionId::EntityGroup) == "Ctrl+Shift+K" && shell2.binding(ActionId::EntityUngroup) == "Ctrl+Shift+G");
    }
    std::printf("shortcut_rebind: Group Selection -> %s (persisted, reloaded)\n", r.shell.binding(ActionId::EntityGroup).c_str());
}

// File > Recent Projects (Qt): newest first, missing folders disabled, open from the menu, clear.
void gateRecentProjects(Rig& r) {
    const auto dir = scratchDir("recent_projects");
    CHECK(r.c.newProject((dir / "Alpha").string(), "Alpha", 1u));
    CHECK(r.c.newProject((dir / "Beta").string(), "Beta", 1u));
    r.frames();
    const auto recent = r.v().recentProjects;
    CHECK(recent.size() >= 2);
    CHECK(recent[0].find("Beta") != std::string::npos && recent[1].find("Alpha") != std::string::npos);
    std::string file = menuText(r, "File");
    CHECK(file.find("Recent Projects") != std::string::npos);
    std::error_code ec;
    std::filesystem::remove_all(recent[0], ec); // Beta goes missing
    r.c.showMessage("refresh");
    CHECK(r.c.openRecentProject(1)); // Alpha
    r.frames();
    file = menuText(r, "File");
    std::printf("recent_projects: menu\n%s\n", file.c_str());
    CHECK(file.find("Beta  -  " + recent[0] + "  (missing)") != std::string::npos);
    CHECK(!r.c.openRecentProject(r.v().recentProjects[0].find("Alpha") != std::string::npos ? 1 : 0));
    CHECK(r.v().status.find("Recent project is missing") != std::string::npos);
    // pick Alpha from the menu: the line index of its entry
    std::vector<std::string> lines;
    {
        std::size_t at = file.find('\n') + 1;
        while (at <= file.size()) {
            const std::size_t e = file.find('\n', at);
            lines.push_back(file.substr(at, e == std::string::npos ? std::string::npos : e - at));
            if (e == std::string::npos) break;
            at = e + 1;
        }
    }
    int alpha = -1, clear = -1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].find("Alpha  -  ") != std::string::npos) alpha = int(i);
        if (lines[i].find("Clear Recent Projects") != std::string::npos) clear = int(i);
    }
    CHECK(alpha >= 0 && clear > alpha);
    shot(r);
    r.ui().emit(r.shell.menuNode("File"), fuse::ui::SignalKind::Submitted, {.value = float(alpha)});
    r.frames();
    CHECK(r.v().hasProject && r.v().recentProjects[0].find("Alpha") != std::string::npos);
    r.ui().emit(r.shell.menuNode("File"), fuse::ui::SignalKind::Submitted, {.value = float(clear)});
    r.frames();
    CHECK(r.v().recentProjects.empty() && menuText(r, "File").find("Clear Recent Projects") == std::string::npos);
    std::printf("recent_projects: %zu entries, missing disabled, opened from menu, cleared\n", recent.size());
}

// Entity > Group / Ungroup, Duplicate (whole subtree), View > Frame Selected (Qt parity, one undo step each).
void gateEntityGroup(Rig& r) {
    const EntityRef a = makeEntity(r, -2.f, 0.f, 4.f), b = makeEntity(r, 2.f, 0.f, 4.f);
    const u32 base = r.v().entityCount;
    const EntityRef both[] = {a, b};
    r.c.select(both, a);
    r.frames();
    viewportKey(r, Key::G, fuse::ui::kModCtrl);
    CHECK(rowsLabelled(r, "Group") == 1 && r.v().entityCount == base + 1);
    std::size_t children = 0;
    EntityRef group;
    for (const auto& row : r.v().hierarchy)
        if (row.label == "Group") group = row.entity;
    for (const auto& row : r.v().hierarchy) children += row.parent == group && row.depth == 1;
    CHECK(children == 2);
    CHECK(r.v().primary == group);
    // Duplicate copies the group with its children
    viewportKey(r, Key::D, fuse::ui::kModCtrl);
    CHECK(rowsLabelled(r, "Group") == 2 && r.v().entityCount == base + 4);
    CHECK(r.c.trigger(ActionId::EditUndo));
    r.frames();
    CHECK(rowsLabelled(r, "Group") == 1 && r.v().entityCount == base + 1);
    const EntityRef g1[] = {group};
    r.c.select(g1, group);
    r.frames();
    // Frame Selected (F on the viewport): the camera moves to fit the group; F in a text field does nothing
    const auto cam0 = r.c.viewport().camera();
    CHECK(r.shell.showPanel("dock.hierarchy"));
    r.frames();
    r.ui().setFocus(r.ui().find("hierarchy.filter"));
    CHECK(r.ui().focused() == r.ui().find("hierarchy.filter"));
    r.ui().keyDown(Key::F, 0);
    r.frames();
    CHECK(r.c.viewport().camera().positionZ == cam0.positionZ);
    r.ui().setText(r.ui().find("hierarchy.filter"), "");
    r.ui().emit(r.ui().find("hierarchy.filter"), fuse::ui::SignalKind::TextChanged, {});
    r.frames();
    viewportKey(r, Key::F, 0);
    const auto cam1 = r.c.viewport().camera();
    std::printf("entity_group: frame camera (%.2f %.2f %.2f) -> (%.2f %.2f %.2f)\n", cam0.positionX, cam0.positionY, cam0.positionZ,
                cam1.positionX, cam1.positionY, cam1.positionZ);
    CHECK(std::fabs(cam1.positionX - cam0.positionX) + std::fabs(cam1.positionY - cam0.positionY) + std::fabs(cam1.positionZ - cam0.positionZ) > 0.1f);
    float px = 0.f, py = 0.f;
    CHECK(r.c.projectPoint(0.f, 0.f, 4.f, px, py));
    const Rect vr = r.rect("viewport");
    CHECK(std::fabs(px - vr.w * 0.5f) < vr.w * 0.1f && std::fabs(py - vr.h * 0.5f) < vr.h * 0.1f);
    shot(r);
    // Ungroup: children back at the top level, the group entity gone
    viewportKey(r, Key::G, fuse::ui::kModCtrl | fuse::ui::kModShift);
    CHECK(rowsLabelled(r, "Group") == 0 && r.v().entityCount == base);
    for (const auto& row : r.v().hierarchy) CHECK(row.depth == 0);
    CHECK(r.c.trigger(ActionId::EditUndo));
    r.frames();
    CHECK(rowsLabelled(r, "Group") == 1);
    std::printf("entity_group: group / duplicate subtree / undo / frame / ungroup ok\n");
}

// Collider wireframes (selection, View > Show All Colliders) and drag handles with one undo step (Qt viewport).
void gateColliders(Rig& r) {
    const EntityRef e = makeEntity(r, 0.f, 0.f, 3.f);
    const fuse::ecs::EntityID id{e.index, e.generation};
    r.shell.panels().withScene([&](EditorHost& h, const ViewportPanel&) {
        fuse::ecs::Collider c;
        c.shape = fuse::ecs::Collider::Box;
        c.params = {0.5f, 0.5f, 0.5f, 0.f};
        h.editorScene().registry().add<fuse::ecs::Collider>(id, c);
    });
    r.c.select({}, {});
    r.frames();
    CHECK(overlayLines(r, 0x78FF8C) == 0 && overlayLines(r, 0x5AC86E) == 0);
    const EntityRef one[] = {e};
    r.c.select(one, e);
    r.frames();
    const std::size_t sel = overlayLines(r, 0x78FF8C);
    std::printf("colliders: %zu selected collider lines\n", sel);
    CHECK(sel == 12); // box edges
    const auto handles = r.c.colliderHandles();
    CHECK(handles.size() == 6);
    std::size_t dots = 0;
    for (const auto& d : w::viewportOverlay(r.ui(), r.ui().find("viewport")).dots) dots += std::fabs(d.radius - 4.5f) < 0.01f;
    CHECK(dots == 6);
    // drag the +X face handle outward: one "Resize collider" undo step
    std::size_t hx = handles.size();
    for (std::size_t i = 0; i < handles.size(); ++i)
        if (handles[i].param == 0 && handles[i].sign > 0) hx = i;
    CHECK(hx < handles.size());
    const auto h = handles[hx];
    const float len = std::sqrt(h.axisX * h.axisX + h.axisY * h.axisY);
    CHECK(len > 1.f);
    const float ux = h.axisX / len, uy = h.axisY / len;
    using PE = ViewportPointerEvent;
    std::size_t undo0 = 0, undo1 = 0;
    r.shell.panels().withScene([&](EditorHost& host, const ViewportPanel&) { undo0 = host.undoStack().undoCount(); });
    r.c.viewportPointer({PE::Kind::Down, PE::Button::Left, h.x, h.y});
    CHECK(r.c.colliderDragging());
    for (int i = 1; i <= 4; ++i) r.c.viewportPointer({PE::Kind::Move, PE::Button::Left, h.x + ux * 10.f * float(i), h.y + uy * 10.f * float(i)});
    r.frames();
    shot(r);
    r.c.viewportPointer({PE::Kind::Up, PE::Button::Left, h.x + ux * 40.f, h.y + uy * 40.f});
    r.frames();
    float half = 0.f;
    r.shell.panels().withScene([&](EditorHost& host, const ViewportPanel&) {
        if (auto* c = host.editorScene().registry().get<fuse::ecs::Collider>(id)) half = c->params.x;
    });
    r.shell.panels().withScene([&](EditorHost& host, const ViewportPanel&) { undo1 = host.undoStack().undoCount(); });
    std::printf("colliders: +X half extent 0.50 -> %.3f (%zu undo step)\n", half, undo1 - undo0);
    CHECK(half > 0.6f && undo1 == undo0 + 1);
    CHECK(r.v().selection.size() == 1 && r.v().primary == e); // the drag did not reselect
    CHECK(r.c.trigger(ActionId::EditUndo));
    r.frames();
    r.shell.panels().withScene([&](EditorHost& host, const ViewportPanel&) {
        if (auto* c = host.editorScene().registry().get<fuse::ecs::Collider>(id)) half = c->params.x;
    });
    CHECK(std::fabs(half - 0.5f) < 1e-5f);
    // Show All Colliders draws unselected colliders too
    r.c.select({}, {});
    r.frames();
    CHECK(overlayLines(r, 0x5AC86E) == 0);
    const auto idx = r.shell.menuItemIndex("View", ActionId::ViewShowColliders);
    CHECK(idx >= 0);
    r.ui().emit(r.shell.menuNode("View"), fuse::ui::SignalKind::Submitted, {.value = float(idx)});
    r.frames();
    CHECK(r.c.showAllColliders() && menuText(r, "View").find("[x] Show All Colliders") != std::string::npos);
    CHECK(overlayLines(r, 0x5AC86E) == 12);
    std::printf("colliders: show all -> %zu lines\n", overlayLines(r, 0x5AC86E));
}

// Entity > Open Script (Qt: QDesktopServices on the selection's .lua; Ctrl+E).
void gateOpenScript(Rig& r) {
    const auto dir = scratchDir("open_script");
    const auto lua = dir / "mover.lua";
    std::FILE* f = std::fopen(lua.string().c_str(), "wb");
    CHECK(f != nullptr);
    if (f) { std::fputs("return {}\n", f); std::fclose(f); }
    const EntityRef plain = makeEntity(r, 0.f, 0.f, 3.f), scripted = makeEntity(r, 1.f, 0.f, 3.f);
    r.shell.panels().withScene([&](EditorHost& h, const ViewportPanel&) {
        fuse::ecs::Script s;
        s.set_path(lua.string());
        h.editorScene().registry().add<fuse::ecs::Script>(fuse::ecs::EntityID{scripted.index, scripted.generation}, s);
    });
    const EntityRef p1[] = {plain};
    r.c.select(p1, plain);
    r.frames();
    CHECK(!r.c.actionEnabled(ActionId::EntityOpenScript) || !r.c.openScript());
    CHECK(r.sv.opened.empty());
    const EntityRef s1[] = {scripted};
    r.c.select(s1, scripted);
    r.frames();
    CHECK(r.c.actionEnabled(ActionId::EntityOpenScript));
    viewportKey(r, Key::E, fuse::ui::kModCtrl);
    CHECK(r.sv.opened.size() == 1);
    CHECK(!r.sv.opened.empty() && std::filesystem::equivalent(r.sv.opened[0], lua));
    std::printf("open_script: opened %s; status \"%s\"\n", r.sv.opened.empty() ? "-" : r.sv.opened[0].c_str(), r.v().status.c_str());
    CHECK(r.v().status.find("Opened ") != std::string::npos);
    CHECK(menuText(r, "Entity").find("Open Script   (Ctrl+E)") != std::string::npos);
    shot(r);
}

// Terrain panel (Qt TerrainPanel parity): create a heightfield terrain, see it in the GPU viewport, sculpt a stroke
// with one undo step, clear + undo.
void gateTerrain(Rig& r) {
    CHECK(r.shell.showPanel("dock.terrain"));
    r.frames();
    CHECK(r.rect("dock.terrain").h > 50);
    auto status = [&] { return std::string(r.ui().text(r.ui().find("terrain.status"))); };
    CHECK(status() == "0 terrain(s); active terrain sculpts 0 / 256");
    auto host = SceneViewportHost::create({.validation = true});
    const bool gpu = host->valid();
    if (!gpu) std::printf("terrain: no scene renderer (%s): model checks only\n", host->reason().c_str());
    if (gpu) r.shell.setSceneViewport(host.get());
    auto grab = [&](std::vector<u32>& px) {
        u32 w = 0, h = 0;
        const fuse::u64 f0 = host->framesRendered();
        for (int i = 0; i < 50 && host->framesRendered() <= f0; ++i) r.frames();
        return host->readback(px, w, h);
    };
    auto diffPct = [](const std::vector<u32>& a, const std::vector<u32>& b) {
        if (a.size() != b.size() || a.empty()) return 0.0;
        std::size_t n = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            for (int sh = 0; sh < 24; sh += 8)
                if (std::abs(int((a[i] >> sh) & 255) - int((b[i] >> sh) & 255)) > 6) { ++n; break; }
        return 100.0 * double(n) / double(a.size());
    };
    std::vector<u32> empty, made, sculpted;
    if (gpu) { r.frames(3); CHECK(grab(empty)); }
    r.ui().setValue(r.ui().find("terrain.cells"), 48.f, true);
    r.ui().setValue(r.ui().find("terrain.size"), 24.f, true);
    r.ui().setValue(r.ui().find("terrain.height"), 1.5f, true);
    r.frames();
    const TerrainView s = r.shell.panels().terrain();
    CHECK(s.cells == 48 && std::fabs(s.size - 24.f) < 1e-4f);
    r.clickId("terrain.create");
    r.frames();
    CHECK(status() == "1 terrain(s); active terrain sculpts 0 / 256");
    CHECK(r.v().primary.valid());
    {
        bool labelled = false;
        for (const HierarchyRowView& row : r.v().hierarchy) labelled |= row.entity == r.v().primary && row.label.find("Terrain") == 0;
        CHECK(labelled);
    }
    double noise = 0.0;
    if (gpu) {
        CHECK(grab(made));
        std::vector<u32> again;
        CHECK(grab(again));
        noise = diffPct(made, again);
        std::printf("terrain: idle frame-to-frame noise %.2f%%\n", noise);
        const double pct = diffPct(empty, made);
        std::printf("terrain: create changed %.1f%% of the engine frame\n", pct);
        CHECK(pct > 5.0);
    }
    // sculpt: drag across the terrain centre (Raise, radius 3)
    r.ui().setValue(r.ui().find("terrain.radius"), 3.f, true);
    r.ui().setValue(r.ui().find("terrain.strength"), 3.f, true);
    r.ui().setChecked(r.ui().find("terrain.sculpt"), true);
    r.ui().emit(r.ui().find("terrain.sculpt"), fuse::ui::SignalKind::Toggled, {.flag = true});
    r.frames();
    CHECK(r.shell.panels().terrain().sculpt);
    using PE = ViewportPointerEvent;
    float ax = 0.f, ay = 0.f, bx = 0.f, by = 0.f;
    CHECK(r.c.projectPoint(-4.f, 0.f, 0.f, ax, ay) && r.c.projectPoint(4.f, 0.f, 0.f, bx, by));
    auto undoCount = [&] { u32 n = 0; r.shell.panels().withScene([&](EditorHost& h, const ViewportPanel&) { n = h.undoStack().undoCount(); }); return n; };
    const u32 undo0 = undoCount();
    CHECK(r.shell.panels().terrainPointer({PE::Kind::Down, PE::Button::Left, ax, ay}));
    for (int i = 1; i <= 8; ++i)
        r.shell.panels().terrainPointer({PE::Kind::Move, PE::Button::Left, ax + (bx - ax) * float(i) / 8.f, ay + (by - ay) * float(i) / 8.f});
    CHECK(r.shell.panels().terrainPointer({PE::Kind::Up, PE::Button::Left, bx, by}));
    r.frames();
    const u32 stamps = r.shell.panels().terrain().stamps;
    std::printf("terrain: stroke -> %u stamps, %s\n", stamps, status().c_str());
    CHECK(stamps >= 4 && stamps <= 8);
    CHECK(undoCount() == undo0 + 1); // one undo step per stroke
    if (gpu) {
        CHECK(grab(sculpted));
        const double pct = diffPct(made, sculpted);
        std::printf("terrain: stroke changed %.1f%% of the engine frame\n", pct);
        CHECK(pct > noise + 0.5);
    }
    // a click off every terrain is not consumed (falls through to pick / gizmo)
    CHECK(!r.shell.panels().terrainPointer({PE::Kind::Down, PE::Button::Left, 2.f, 2.f}));
    CHECK(r.c.trigger(ActionId::EditUndo));
    r.frames();
    CHECK(r.shell.panels().terrain().stamps == 0);
    CHECK(r.c.trigger(ActionId::EditRedo));
    r.frames();
    CHECK(r.shell.panels().terrain().stamps == stamps);
    r.clickId("terrain.clear");
    CHECK(r.shell.panels().terrain().stamps == 0);
    CHECK(r.c.trigger(ActionId::EditUndo));
    r.frames(3);
    CHECK(r.shell.panels().terrain().stamps == stamps);
    if (gpu) { std::vector<u32> keep; CHECK(grab(keep)); CHECK(validationErrors() == 0); }
    r.win.rasterize({});
    if (!g_out.empty()) { CHECK(r.win.writePng(g_out)); g_shotWritten = true; }
    r.shell.setSceneViewport(nullptr); // the host dies with this gate
}

// Navigation panel (Qt NavmeshPanel parity): bake from colliders, test path around the obstacle, viewport overlay.
void gateNavigation(Rig& r) {
    CHECK(r.shell.showPanel("dock.navigation"));
    r.frames();
    CHECK(r.rect("dock.navigation").h > 50);
    CHECK(r.ui().text(r.ui().find("nav.status")) == "Not baked");
    r.clickId("nav.test_path");
    CHECK(r.ui().text(r.ui().find("nav.status")) == "Bake the navmesh first");
    // a 2 x 2 x 6 m wall across the straight line from (-5,0) to (5,0)
    CHECK(r.c.trigger(ActionId::EntityCreateEmpty));
    r.frames();
    const EntityRef wall = r.c.lastCreated();
    r.shell.panels().withScene([&](EditorHost& h, const ViewportPanel&) {
        auto& reg = h.editorScene().registry();
        const fuse::ecs::EntityID id{wall.index, wall.generation};
        if (auto* t = reg.get<fuse::ecs::Transform>(id)) t->position = {0.f, 1.f, 0.f, 1.f};
        fuse::ecs::Collider c;
        c.shape = fuse::ecs::Collider::Box;
        c.params = {1.f, 1.f, 3.f, 0.f};
        reg.add<fuse::ecs::Collider>(id, c);
    });
    r.ui().setValue(r.ui().find("nav.extent"), 12.f, true); // 24 x 24 cells of 0.5 m
    r.frames();
    r.clickId("nav.bake");
    const NavigationView v = r.shell.panels().navigation();
    std::printf("navigation: %s\n", v.status.c_str());
    CHECK(v.built && v.cells == 24 && v.colliders == 1);
    CHECK(v.blocked == 4u * 12u); // 2 m x 6 m at 0.5 m cells
    r.clickId("nav.test_path");
    const NavigationView p = r.shell.panels().navigation();
    std::printf("navigation: %s\n", p.status.c_str());
    CHECK(p.waypoints > 21); // straight would be 21 cells; it detours around the wall
    CHECK(r.ui().text(r.ui().find("nav.status")).find("Path: ") == 0);
    // overlay: outline (4) + 2 per blocked cell + path segments, light blue
    const std::size_t lines = overlayLines(r, 0x46C8FF);
    std::printf("navigation: %zu overlay lines\n", lines);
    CHECK(lines >= 4 + 2 * 48 && lines <= 4 + 2 * 48 + p.waypoints);
    r.ui().setChecked(r.ui().find("nav.show"), false);
    r.ui().emit(r.ui().find("nav.show"), fuse::ui::SignalKind::Toggled, {.flag = false});
    r.frames();
    CHECK(overlayLines(r, 0x46C8FF) == 0);
    r.ui().emit(r.ui().find("nav.show"), fuse::ui::SignalKind::Toggled, {.flag = true});
    r.frames(2);
    CHECK(overlayLines(r, 0x46C8FF) == lines);
}

int main(int argc, char** argv) {
    std::string gate, out;
    for (int i = 1; i < argc; ++i) {
        if (!std::strncmp(argv[i], "--gate=", 7)) gate = argv[i] + 7;
        else if (!std::strncmp(argv[i], "--out=", 6)) out = argv[i] + 6;
    }
    const std::map<std::string, float> fonts{{"font_1", 1.f}, {"font_1_25", 1.25f}, {"font_1_5", 1.5f}, {"font_2", 2.f}};
    const float scale = fonts.count(gate) ? fonts.at(gate) : gate == "context_menu_dpr1_5" ? 1.5f : 1.f;
    Rig r(scale);
    const std::map<std::string, std::function<void(Rig&)>> gates{
        {"startup", gateStartup},        {"layout", gateLayout},           {"theme", gateTheme},
        {"fuse_api", gateFuseApi},       {"wasd", gateWasd},               {"context_menu", gateContextMenu}, {"context_menu_dpr1_5", gateContextMenuDpr15}, {"icon", gateIcon}, {"view_modes", gateViewModes}, {"preferences", gatePreferences}, {"navigation", gateNavigation}, {"terrain", gateTerrain}, {"shortcut_rebind", gateShortcutRebind}, {"recent_projects", gateRecentProjects}, {"entity_group", gateEntityGroup}, {"colliders", gateColliders}, {"open_script", gateOpenScript}, {"live_present", gateLivePresent},
        {"ui_frame", gateUiFrame},       {"idle_frames", gateIdle},        {"e15_console", gateConsole},
        {"e15_transport", gateTransport}, {"e20_inspector", gateInspector}, {"e20_reparent", gateReparent},
        {"e20_file_menu", gateFileMenu}, {"e20_wizard", gateWizard}, {"project_open", gateProjectOpen},  {"project_hub", gateProjectHub}, {"asset_browser", gateAssetBrowser}, {"asset_browser_polish", gateAssetBrowserPolish},
        {"material_editor", gateMaterialEditor}, {"sdf_sculpt", gateSdfSculpt}, {"profiler", gateProfiler},
        {"sequencer", gateSequencer}, {"timeline", gateTimeline}, {"timeline_nav", gateTimelineNav},
        {"scene_viewport", gateSceneViewport}, {"viewport_gizmo", gateViewportGizmo}, {"gpu_viewport", gateGpuViewport},
    };
    g_out = out;
    if (fonts.count(gate)) gateFont(r, scale);
    else if (gates.count(gate)) gates.at(gate)(r);
    else { std::fprintf(stderr, "unknown gate '%s'\n", gate.c_str()); return 2; }
    if (!out.empty() && !g_shotWritten) {
        if (gate != "e20_file_menu" && gate.rfind("context_menu", 0) != 0) r.frames();
        r.win.rasterize({});
        if (!r.win.writePng(out)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); ++g_fail; }
    }
    if (g_fail) { std::fprintf(stderr, "fuse_editor_ui_%s: %d failure(s)\n", gate.c_str(), g_fail); return 1; }
    std::printf("fuse_editor_ui_%s: OK\n", gate.c_str());
    return 0;
}
