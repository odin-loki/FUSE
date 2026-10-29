// fuse_editor_ui_* twin gates (UI9): the FUSE UI editor shell checked the way the fuse_editor_qt_* gates check the Qt
// shell (same names, same panel ids / actions / title format), headless on the CPU rasteriser. One executable:
//   fuse_editor_ui_gates --gate=<name> [--out=shot.png]
#include "fuse/editor/fui/editor_shell.hpp"
#include "scripted_services.hpp"

#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/scene_viewport_host.hpp>
#if defined(FUSE_EDITOR_UI_HAS_NATIVE)
#include "fuse/app/native.hpp"
#endif
#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/renderer/vk/instance.hpp>
#endif
#include <fuse/editor/editor_theme_tokens.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <thread>
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
    CHECK(w::viewportOverlay(r.ui(), vp).lines.size() == 3u);
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
    CHECK(w::viewportOverlay(r.ui(), vp).lines.size() == 3u); // hot axis drawn while dragging
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
    r.c.setTransformTool(TransformTool::Translate);
    r.frames();
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
        {"e20_file_menu", gateFileMenu},  {"project_hub", gateProjectHub}, {"asset_browser", gateAssetBrowser},
        {"material_editor", gateMaterialEditor}, {"sdf_sculpt", gateSdfSculpt}, {"profiler", gateProfiler},
        {"sequencer", gateSequencer}, {"timeline", gateTimeline},
        {"scene_viewport", gateSceneViewport}, {"viewport_gizmo", gateViewportGizmo}, {"gpu_viewport", gateGpuViewport},
    };
    g_out = out;
    if (fonts.count(gate)) gateFont(r, scale);
    else if (gates.count(gate)) gates.at(gate)(r);
    else { std::fprintf(stderr, "unknown gate '%s'\n", gate.c_str()); return 2; }
    if (!out.empty() && !g_shotWritten) {
        if (gate != "e20_file_menu" && gate != "context_menu") r.frames();
        r.win.rasterize({});
        if (!r.win.writePng(out)) { std::fprintf(stderr, "cannot write %s\n", out.c_str()); ++g_fail; }
    }
    if (g_fail) { std::fprintf(stderr, "fuse_editor_ui_%s: %d failure(s)\n", gate.c_str(), g_fail); return 1; }
    std::printf("fuse_editor_ui_%s: OK\n", gate.c_str());
    return 0;
}
