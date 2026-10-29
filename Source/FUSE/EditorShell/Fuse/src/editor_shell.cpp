// UI9: the FUSE UI editor shell (see editor_shell.hpp).
#include "fuse/editor/fui/editor_shell.hpp"
#include "editor_shell_panels.hpp"

#include <chrono>

#include <fuse/editor/editor_theme_tokens.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace fuse::editor::fui {

namespace w = fuse::ui::widgets;
using fuse::ui::Color;
using fuse::ui::widgets::DockSide;
using fuse::ui::Event;
using fuse::ui::EventType;
using fuse::ui::Key;
using fuse::ui::SignalArgs;
using fuse::ui::SignalKind;
using fuse::ui::UiContext;

namespace {

constexpr ShellPanel kPanels[] = {
    {"dock.hierarchy", "Hierarchy", DockSide::Left, true},
    {"dock.project_hub", "Project Hub", DockSide::Left, true},
    {"dock.asset_browser", "Asset Browser", DockSide::Left, true},
    {"dock.inspector", "Inspector", DockSide::Right, true},
    {"dock.material_editor", "Material Editor", DockSide::Right, true},
    {"dock.sdf_sculpt", "SDF Sculpt", DockSide::Right, true},
    {"dock.navigation", "Navigation", DockSide::Right, true},
    {"dock.terrain", "Terrain", DockSide::Right, true},
    {"dock.console", "Console", DockSide::Bottom, true},
    {"dock.profiler", "Profiler", DockSide::Bottom, true},
    {"dock.sequencer", "Sequencer", DockSide::Bottom, true},
    {"dock.timeline", "Timeline", DockSide::Bottom, true},
};
constexpr const char* kSideDockIds[3] = {"dock.left", "dock.right", "dock.bottom"};
constexpr const char* kSideTabIds[3] = {"tabs.left", "tabs.right", "tabs.bottom"};
constexpr DockSide kSides[3] = {DockSide::Left, DockSide::Right, DockSide::Bottom};

u32 sideIndex(DockSide s) { return s == DockSide::Left ? 0u : s == DockSide::Right ? 1u : 2u; }

struct ShellRootW {
    static constexpr std::string_view kName = "editorshellroot";
    static constexpr fuse::ui::A11yRole kRole = fuse::ui::A11yRole::Window;
    static bool onEvent(fuse::ui::EventContext& ec, const Event& e) {
        if (ec.phase != fuse::ui::Phase::Capture) return false;
        auto* s = reinterpret_cast<EditorShellWindow*>(static_cast<std::uintptr_t>(ec.ui.userData(ec.node)));
        return s && s->rootEvent(e, ec.target);
    }
};

std::string tokenHex(const char* role) { return themeHex(role).substr(0, 7); }

Color tokenColor(const char* role) {
    for (const ThemeToken& t : kEditorDarkTheme)
        if (std::string_view(role) == t.role) return {t.r, t.g, t.b, t.a};
    return {1.f, 0.f, 1.f, 1.f};
}

std::string formatNumber(f64 v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// Schema "r,g,b" (linear, may exceed 1) <-> "#RRGGBB" for the colour row (clamped; HDR edits go through the text form).
std::string schemaToHex(std::string_view v) {
    float c[3] = {0.f, 0.f, 0.f};
    std::sscanf(std::string(v).c_str(), "%f,%f,%f", &c[0], &c[1], &c[2]);
    char b[16];
    auto q = [](float x) { return int(std::clamp(x, 0.f, 1.f) * 255.f + 0.5f); };
    std::snprintf(b, sizeof b, "#%02X%02X%02X", q(c[0]), q(c[1]), q(c[2]));
    return b;
}
std::string hexToSchema(std::string_view h) {
    if (h.size() < 7 || h[0] != '#') return std::string(h);
    const unsigned v = unsigned(std::strtoul(std::string(h.substr(1, 6)).c_str(), nullptr, 16));
    char b[64];
    std::snprintf(b, sizeof b, "%.4g,%.4g,%.4g", ((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0);
    return b;
}

ContextMenuAction entityActionOf(ActionId id) {
    switch (id) {
    case ActionId::EntityCreateCube: return ContextMenuAction::CreateCube;
    case ActionId::EntityCreateSdfSphere: return ContextMenuAction::CreateSdfSphere;
    case ActionId::EntityCreateSdfBox: return ContextMenuAction::CreateSdfBox;
    case ActionId::EntityCreatePointLight: return ContextMenuAction::CreatePointLight;
    case ActionId::EntityCreateDirectionalLight: return ContextMenuAction::CreateDirectionalLight;
    case ActionId::EntityCreateCamera: return ContextMenuAction::CreateCamera;
    case ActionId::EntityDuplicate: return ContextMenuAction::Duplicate;
    case ActionId::EntityDelete: return ContextMenuAction::Delete;
    default: return ContextMenuAction::CreateEmpty;
    }
}

// ---- software viewport ----------------------------------------------------------------------------------------------
struct Canvas {
    fuse::ui::Image& img;
    void px(int x, int y, u32 c) {
        if (x < 0 || y < 0 || x >= img.width || y >= img.height) return;
        img.pixels[std::size_t(y) * std::size_t(img.width) + std::size_t(x)] = c;
    }
    void line(f32 x0, f32 y0, f32 x1, f32 y1, u32 c) {
        const f32 dx = x1 - x0, dy = y1 - y0;
        const int n = int(std::max(std::fabs(dx), std::fabs(dy)));
        if (n > 8192) return;
        for (int i = 0; i <= n; ++i) {
            const f32 t = n ? f32(i) / f32(n) : 0.f;
            px(int(x0 + dx * t), int(y0 + dy * t), c);
        }
    }
    void disc(f32 cx, f32 cy, f32 r, u32 c) {
        for (int y = int(cy - r); y <= int(cy + r); ++y)
            for (int x = int(cx - r); x <= int(cx + r); ++x)
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) px(x, y, c);
    }
    void ring(f32 cx, f32 cy, f32 r, f32 t, u32 c) {
        for (int y = int(cy - r - t); y <= int(cy + r + t); ++y)
            for (int x = int(cx - r - t); x <= int(cx + r + t); ++x) {
                const f32 d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
                if (d >= r && d <= r + t) px(x, y, c);
            }
    }
};

} // namespace

std::span<const ShellPanel> shellPanels() { return kPanels; }

std::string editorShellCss(f32 fontScale) {
    const int fs = int(std::lround(13.f * fontScale));
    std::string css = ":root {";
    css += " --bg: " + tokenHex("WindowBg") + ";";
    css += " --surface: " + tokenHex("FrameBg") + ";";
    css += " --surface-hi: " + tokenHex("FrameBgHovered") + ";";
    css += " --border: " + tokenHex("Border") + ";";
    css += " --text: " + tokenHex("Text") + ";";
    css += " --text-dim: " + tokenHex("TextDisabled") + ";";
    css += " --accent: " + tokenHex("CheckMark") + ";";
    css += " --accent-hi: " + tokenHex("SliderGrabActive") + ";";
    css += " font-size: " + std::to_string(fs) + "px; }\n";
    css += ".editorshellroot { padding: 0px; gap: 0px; flex-grow: 1; background: " + tokenHex("WindowBg") + "; }\n";
    css += ".menubar { background: " + tokenHex("MenuBarBg") + "; }\n";
    css += ".ed-toolbar { gap: 2px; padding: 3px 6px; background: " + tokenHex("TitleBgActive") + "; border: 1px " +
           tokenHex("Border") + "; }\n";
    css += ".ed-sep { width: 1px; margin: 2px 6px; background: " + tokenHex("Separator") + "; }\n";
    css += ".ed-page { padding: 4px; gap: 4px; flex-grow: 1; background: " + tokenHex("ChildBg") + "; }\n";
    css += ".ed-dim { color: " + tokenHex("TextDisabled") + "; }\n";
    css += ".ed-centre { gap: 0px; padding: 0px; flex-grow: 1; }\n";
    css += ".ed-vpinfo { padding: 2px 8px; color: " + tokenHex("TextDisabled") + "; }\n";
    css += ".ed-vpbar { background: " + tokenHex("TitleBg") + "; align-items: center; }\n";
    css += ".ed-vptool { padding: 1px 8px; margin: 1px 0px 1px 2px; }\n";
    css += ".ed-vptool.active { background: " + tokenHex("ButtonActive") + "; }\n";
    css += ".ed-status { padding: 3px 8px; color: " + tokenHex("Text") + "; background: " + tokenHex("TitleBgActive") + "; }\n";
    return css;
}

// =====================================================================================================================
EditorShellWindow::EditorShellWindow(fuse::app::AppWindow& window, EditorController& controller, EditorShellOptions options)
    : m_win(window), m_ui(window.ui()), m_c(controller), m_opt(options) {
    w::install(m_ui);
    loadBindings_(); // menus / tooltips show the effective shortcuts
    if (m_ui.registry().find("editorshellroot") == 0xFFFFFFFFu) m_ui.registry().registerWidget<ShellRootW>();
    m_ui.addStyleSheet(editorShellCss(m_opt.fontScale));
    m_ui.setCss(m_ui.root(), "padding", "0px");
    m_ui.setCss(m_ui.root(), "gap", "0px");
    m_image.resize(640, 360, fuse::ui::packPremultiplied(tokenColor("ChildBg")));
    m_imageIndex = m_win.addImage(&m_image);
    m_models = std::make_unique<EditorPanels>(m_c);
    m_pages = std::make_unique<ShellPanelPages>(*this, *m_models);
    build_();
    m_c.setShellActionHook(
        [](void* user, ActionId id) {
            auto* self = static_cast<EditorShellWindow*>(user);
            switch (id) {
            case ActionId::ViewResetLayout: self->resetLayout(); return true;
            case ActionId::FileNewProject: self->openNewProjectWizard(); return true;
            case ActionId::EditPreferences: self->openPreferences(); return true;
            case ActionId::FileQuit:
                if (self->m_c.requestClose()) self->m_win.requestClose();
                return true;
            default: return false;
            }
        },
        this);
    {
        // Edit > Preferences, persisted like the Qt QSettings prefs/* keys
        const fuse::app::Settings& s = m_win.app().settings();
        EditorPreferences p;
        p.previewSun = s.flag("prefs.previewSun", p.previewSun);
        p.cameraSpeed = s.number("prefs.cameraSpeed", p.cameraSpeed);
        p.lookSensitivity = s.number("prefs.lookSensitivity", p.lookSensitivity);
        p.restoreLayout = s.flag("prefs.restoreLayout", p.restoreLayout);
        m_c.applyPreferences(p);
    }
    m_c.refresh();
    sync();
    // poll the controller (game thread ticks, console output); sync() touches widgets only on change
    m_ui.startTimer(1.f / 30.f, true, [this](UiContext&) { frame(); });
}

EditorShellWindow::~EditorShellWindow() = default;

NodeHandle EditorShellWindow::node(std::string_view id) const { return m_ui.find(id); }

NodeHandle EditorShellWindow::panelPage(std::string_view panelId) const {
    for (const ShellPanel& p : kPanels) {
        if (panelId != p.id) continue;
        return m_ui.find(p.id);
    }
    return {};
}

bool EditorShellWindow::showPanel(std::string_view panelId) {
    for (u32 s = 0; s < 3; ++s) {
        u32 tab = 0;
        for (const ShellPanel& p : kPanels) {
            if (sideIndex(p.side) != s) continue;
            if (panelId == p.id) {
                m_ui.setValue(w::tabWidgetBar(m_ui, m_tabs[s]), f32(tab), true);
                return true;
            }
            ++tab;
        }
    }
    return false;
}

bool EditorShellWindow::panelVisible(std::string_view panelId) const {
    for (u32 s = 0; s < 3; ++s) {
        u32 tab = 0;
        for (const ShellPanel& p : kPanels) {
            if (sideIndex(p.side) != s) continue;
            if (std::string_view(p.id).substr(5) == panelId || panelId == p.id)
                return m_ui.alive(m_tabs[s]) && u32(m_ui.value(w::tabWidgetBar(m_ui, m_tabs[s]))) == tab;
            ++tab;
        }
    }
    return false;
}

void EditorShellWindow::resetLayout() {
    for (u32 s = 0; s < 3; ++s) {
        w::dockTo(m_ui, m_docks[s], kSides[s]);
        m_ui.setValue(w::tabWidgetBar(m_ui, m_tabs[s]), 0.f, true);
    }
    // dock regions are splitter panes sized by flex-basis (a splitter drag rewrites the basis; width is ignored)
    m_ui.setCss(w::dockRegion(m_ui, m_dockArea, DockSide::Left), "flex-basis", "260px");
    m_ui.setCss(w::dockRegion(m_ui, m_dockArea, DockSide::Right), "flex-basis", "320px");
    m_ui.setCss(w::dockRegion(m_ui, m_dockArea, DockSide::Bottom), "flex-basis", "200px");
}

void EditorShellWindow::build_() {
    m_root = m_ui.create("editorshellroot", m_ui.root(), "editor.root").value_or(NodeHandle{});
    m_ui.setUserData(m_root, static_cast<u64>(reinterpret_cast<std::uintptr_t>(this)));
    m_ui.addClass(m_root, "editorshellroot");
    m_menuBar = w::menuBar(m_ui, m_root, "editor.menubar");
    m_ui.setCss(m_menuBar, "background", tokenHex("MenuBarBg"));
    buildMenus_();
    m_toolbar = w::panel(m_ui, m_root, "editor.toolbar");
    m_ui.addClass(m_toolbar, "row");
    m_ui.addClass(m_toolbar, "ed-toolbar");
    buildToolbar_(m_toolbar);

    m_dockArea = w::dockArea(m_ui, m_root, "editor.dockarea");
    m_ui.setCss(m_dockArea, "flex-grow", "1");
    const char* titles[3] = {"Scene", "Properties", "Output"};
    for (u32 s = 0; s < 3; ++s) {
        m_docks[s] = w::dockWidget(m_ui, m_dockArea, titles[s], kSides[s], kSideDockIds[s]);
        std::string tabTitles;
        for (const ShellPanel& p : kPanels)
            if (sideIndex(p.side) == s) tabTitles += std::string(tabTitles.empty() ? "" : "\n") + p.title;
        m_tabs[s] = w::tabWidget(m_ui, w::dockBody(m_ui, m_docks[s]), tabTitles, kSideTabIds[s]);
        m_ui.setCss(m_tabs[s], "flex-grow", "1");
        u32 tab = 0;
        for (const ShellPanel& p : kPanels) {
            if (sideIndex(p.side) != s) continue;
            const NodeHandle holder = w::tabPage(m_ui, m_tabs[s], tab++);
            const NodeHandle page = w::panel(m_ui, holder, p.id);
            m_ui.addClass(page, "ed-page");
            m_ui.setCss(holder, "padding", "0px");
            m_ui.setCss(holder, "flex-grow", "1");
            if (std::string_view(p.id) == "dock.hierarchy") buildHierarchy_(page);
            else if (std::string_view(p.id) == "dock.inspector") buildInspector_(page);
            else if (std::string_view(p.id) == "dock.console") buildConsole_(page);
            else if (!m_pages->build(std::string_view(p.id).substr(5), page)) buildPlaceholder_(page, p);
        }
    }
    // centre: viewport + info strip
    const NodeHandle centre = w::panel(m_ui, w::dockRegion(m_ui, m_dockArea, DockSide::Center), "editor.centre");
    m_ui.addClass(centre, "ed-centre");
    const NodeHandle vpBar = w::panel(m_ui, centre, "viewport.bar");
    m_ui.addClass(vpBar, "row");
    m_ui.addClass(vpBar, "ed-vpbar");
    static constexpr std::pair<const char*, TransformTool> kTools[] = {
        {"Select", TransformTool::Select}, {"Move", TransformTool::Translate}, {"Rotate", TransformTool::Rotate},
        {"Scale", TransformTool::Scale}};
    for (std::size_t i = 0; i < 4; ++i) {
        const NodeHandle b = w::button(m_ui, vpBar, kTools[i].first, std::string("viewport.tool.") + kTools[i].first);
        m_ui.addClass(b, "ed-vptool");
        m_gizmoTools[i] = b;
        m_ui.connect(b, SignalKind::Clicked, [this, t = kTools[i].second](UiContext&, NodeHandle, const SignalArgs&) {
            m_c.setTransformTool(t);
            sync();
        });
    }
    // gizmo space (World / Local toggle) and snap (Qt toolbar parity)
    m_gizmoSpace = w::button(m_ui, vpBar, "World", "viewport.space");
    m_ui.addClass(m_gizmoSpace, "ed-vptool");
    m_ui.connect(m_gizmoSpace, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
        m_c.setTransformSpace(m_c.transformSpace() == TransformSpace::World ? TransformSpace::Local : TransformSpace::World);
        sync();
    });
    m_gizmoSnap = w::button(m_ui, vpBar, "Snap", "viewport.snap");
    m_ui.addClass(m_gizmoSnap, "ed-vptool");
    m_ui.connect(m_gizmoSnap, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
        m_c.setTransformSnap(!m_c.transformSnap());
        sync();
    });
    m_viewportInfo = w::label(m_ui, vpBar, "Perspective", "viewport.info");
    m_ui.addClass(m_viewportInfo, "ed-vpinfo");
    m_ui.setCss(m_viewportInfo, "flex-grow", "1");
    // multi-viewport grid: [perspective | Top] over [Front | Side]; the ortho views start hidden (single layout)
    const NodeHandle grid = w::panel(m_ui, centre, "viewport.grid");
    m_ui.setCss(grid, "flex-grow", "1");
    m_ui.setCss(grid, "height", "0px");
    m_ui.setCss(grid, "gap", "2px");
    m_ui.setCss(grid, "padding", "0px");
    for (NodeHandle* row : {&m_vpTopRow, &m_vpBottomRow}) {
        *row = w::panel(m_ui, grid, row == &m_vpTopRow ? "viewport.row.top" : "viewport.row.bottom");
        m_ui.addClass(*row, "row");
        m_ui.setCss(*row, "flex-grow", "1");
        m_ui.setCss(*row, "height", "0px");
        m_ui.setCss(*row, "gap", "2px");
        m_ui.setCss(*row, "padding", "0px");
        m_ui.setCss(*row, "align-items", "stretch");
    }
    // each view sits in a column cell: the cell takes the row share, the view fills it (height 0 + grow, so the
    // widget never sizes to its image: feedback on shrink)
    auto cell = [&](NodeHandle row, const char* id) {
        const NodeHandle c = w::panel(m_ui, row, id);
        m_ui.setCss(c, "flex-grow", "1");
        m_ui.setCss(c, "width", "0px");
        m_ui.setCss(c, "padding", "0px");
        return c;
    };
    auto fill = [&](NodeHandle vp) {
        m_ui.setCss(vp, "flex-grow", "1");
        m_ui.setCss(vp, "height", "0px");
        m_ui.setCss(vp, "min-width", "0px");
    };
    m_viewport = w::viewportWidget(m_ui, cell(m_vpTopRow, "viewport.cell"), m_imageIndex, u32(m_image.width), u32(m_image.height), "viewport");
    fill(m_viewport);
    m_orthoBg.resize(1, 1, fuse::ui::packPremultiplied(fuse::ui::Color::rgb(0x1E2128)));
    m_orthoBgIndex = m_win.addImage(&m_orthoBg);
    static constexpr const char* kOrthoCells[3] = {"viewport.cell.top", "viewport.cell.front", "viewport.cell.side"};
    static constexpr const char* kOrthoIds[3] = {"viewport.ortho.top", "viewport.ortho.front", "viewport.ortho.side"};
    for (int i = 0; i < 3; ++i) {
        m_orthoView[i].axis = OrthoView::Axis(i);
        m_orthoCell[i] = cell(i == 0 ? m_vpTopRow : m_vpBottomRow, kOrthoCells[i]);
        m_ortho[i] = w::viewportWidget(m_ui, m_orthoCell[i], m_orthoBgIndex, 1, 1, kOrthoIds[i]);
        fill(m_ortho[i]);
        m_ui.setCss(m_orthoCell[i], "display", "none");
        m_ui.setFocusable(m_ortho[i], true);
        m_ui.connect(m_ortho[i], SignalKind::Event, [this, i](UiContext&, NodeHandle, const SignalArgs&) { orthoInput_(i); });
    }
    m_ui.setCss(m_vpBottomRow, "display", "none");
    m_ui.setCss(w::dockRegion(m_ui, m_dockArea, DockSide::Center), "width", "0px");
    m_ui.setCss(w::dockRegion(m_ui, m_dockArea, DockSide::Center), "flex-grow", "1");
    m_ui.setFocusable(m_viewport, true);
    m_ui.connect(m_viewport, SignalKind::Event, [this](UiContext&, NodeHandle, const SignalArgs&) { viewportInput_(); });

    m_status = w::label(m_ui, m_root, "Ready", "editor.status");
    m_ui.addClass(m_status, "ed-status");
    resetLayout();
}

void EditorShellWindow::buildMenus_() {
    const auto menus = editorMenus();
    m_menus.clear();
    m_menuMap.assign(menus.size(), {});
    for (u32 m = 0; m < menus.size(); ++m) {
        const NodeHandle h = w::menuBarMenu(m_ui, m_menuBar, menus[m], "-", std::string("menu.") + menus[m]);
        m_menus.push_back(h);
        const u32 mi = m;
        m_ui.connect(h, SignalKind::Submitted, [this, mi](UiContext&, NodeHandle, const SignalArgs& a) { menuPicked_(mi, u32(a.value)); });
    }
    m_menuKey.clear();
    syncMenus_();
}

NodeHandle EditorShellWindow::menuNode(std::string_view menu) const {
    const auto menus = editorMenus();
    for (u32 m = 0; m < menus.size() && m < m_menus.size(); ++m)
        if (menu == menus[m]) return m_menus[m];
    return {};
}

i32 EditorShellWindow::menuItemIndex(std::string_view menu, ActionId id) const {
    const auto menus = editorMenus();
    for (u32 m = 0; m < menus.size() && m < m_menuMap.size(); ++m) {
        if (menu != menus[m]) continue;
        for (u32 i = 0; i < m_menuMap[m].size(); ++i)
            if (m_menuMap[m][i] == i32(id)) return i32(i);
    }
    return -1;
}

void EditorShellWindow::syncMenus_() {
    // menu text carries enable state ("(disabled)" suffix is not supported by popupMenu; disabled actions are no-ops
    // in trigger()), recent files and the undo / redo labels
    const EditorViewState& v = m_c.viewState();
    std::string key = v.undoLabel + '\x1f' + v.redoLabel;
    for (const std::string& r : v.recentFiles) key += '\x1f' + r;
    key += '\x1f' + std::to_string(int(m_c.viewMode())) + (m_c.showMeshBounds() ? "b" : "") + (m_c.showAllColliders() ? "c" : "") + (m_c.maximizeOnPlay() ? "m" : "") + std::to_string(m_c.viewportLayout());
    for (const std::string& r : v.recentProjects) key += "\x1fp" + r;
    for (const std::string& b : m_bindings) key += '\x1e' + b;
    if (key == m_menuKey) return;
    m_menuKey = key;
    const auto menus = editorMenus();
    for (u32 m = 0; m < menus.size(); ++m) {
        std::string items;
        std::vector<i32>& map = m_menuMap[m];
        map.clear();
        auto add = [&](const std::string& t, i32 code) {
            if (!items.empty()) items += '\n';
            items += t;
            map.push_back(code);
        };
        for (const ActionDesc& a : editorActions()) {
            if (std::string_view(a.menu) != menus[m]) continue;
            if (a.separatorBefore && !items.empty()) add("-", -1);
            std::string label = a.label;
            if (a.id == ActionId::EditUndo && !v.undoLabel.empty()) label += " " + v.undoLabel;
            if (a.id == ActionId::EditRedo && !v.redoLabel.empty()) label += " " + v.redoLabel;
            // checkable View actions (Qt: exclusive view-mode group + Show Mesh Bounds toggle)
            if (a.id >= ActionId::ViewModeLit && a.id <= ActionId::ViewModeLitWireframe)
                label = (int(m_c.viewMode()) == int(a.id) - int(ActionId::ViewModeLit) ? "[x] " : "[ ] ") + label;
            if (a.id == ActionId::ViewShowMeshBounds) label = (m_c.showMeshBounds() ? "[x] " : "[ ] ") + label;
            if (a.id == ActionId::ViewShowColliders) label = (m_c.showAllColliders() ? "[x] " : "[ ] ") + label;
            if (a.id == ActionId::PlayMaximizeOnPlay) label = (m_c.maximizeOnPlay() ? "[x] " : "[ ] ") + label;
            if (a.id >= ActionId::ViewLayoutSingle && a.id <= ActionId::ViewLayoutQuad)
                label = (m_c.viewportLayout() == int(a.id) - int(ActionId::ViewLayoutSingle) ? "[x] " : "[ ] ") + label;
            if (!binding(a.id).empty()) label += "   (" + binding(a.id) + ")";
            add(label, i32(a.id));
            if (a.id == ActionId::FileOpenProject && !v.recentProjects.empty()) {
                // Qt "Recent Projects" submenu, inlined: missing folders are listed but disabled
                for (u32 r = 0; r < v.recentProjects.size(); ++r) {
                    const std::string& path = v.recentProjects[r];
                    const bool ok = EditorController::projectExists(path);
                    add("   " + std::filesystem::path(path).filename().string() + "  -  " + path + (ok ? "" : "  (missing)"),
                        ok ? -100 - i32(r) : -1);
                }
                add("   Clear Recent Projects", -99);
            }
            if (a.id == ActionId::FileOpenScene && !v.recentFiles.empty()) {
                for (u32 r = 0; r < v.recentFiles.size(); ++r)
                    add("   " + std::to_string(r + 1) + "  " + std::filesystem::path(v.recentFiles[r]).filename().string(), -2 - i32(r));
            }
        }
        m_ui.setText(m_menus[m], std::string(menus[m]) + "\n" + items);
    }
}

void EditorShellWindow::menuPicked_(u32 menuIndex, u32 line) {
    if (menuIndex >= m_menuMap.size() || line >= m_menuMap[menuIndex].size()) return;
    const i32 code = m_menuMap[menuIndex][line];
    if (code <= -100) m_c.openRecentProject(u32(-100 - code));
    else if (code == -99) m_c.clearRecentProjects();
    else if (code <= -2) m_c.fileOpenRecent(u32(-2 - code));
    else if (code >= 0) m_c.trigger(ActionId(code));
    sync();
}

void EditorShellWindow::buildToolbar_(NodeHandle bar) {
    const char* lastMenu = "";
    for (const ActionDesc& a : editorActions()) {
        if (!a.toolbar) continue;
        if (*lastMenu && std::string_view(lastMenu) != a.menu) m_ui.addClass(w::panel(m_ui, bar), "ed-sep");
        lastMenu = a.menu;
        const NodeHandle b = w::toolButton(m_ui, bar, a.label, std::string("tool.") + a.stableId);
        if (!binding(a.id).empty()) w::setTooltip(m_ui, b, std::string(a.label) + "  (" + binding(a.id) + ")");
        const ActionId id = a.id;
        m_ui.connect(b, SignalKind::Clicked, [this, id](UiContext&, NodeHandle, const SignalArgs&) { m_c.trigger(id); sync(); });
        m_toolButtons.emplace_back(id, b);
    }
}

void EditorShellWindow::syncToolbar_() {
    for (const auto& [id, b] : m_toolButtons) m_ui.setDisabled(b, !m_c.actionEnabled(id));
}

// ---- hierarchy ------------------------------------------------------------------------------------------------------
void EditorShellWindow::buildHierarchy_(NodeHandle page) {
    m_hierFilter = w::lineEdit(m_ui, page, "", "hierarchy.filter");
    m_ui.connect(m_hierFilter, SignalKind::TextChanged, [this](UiContext& u, NodeHandle n, const SignalArgs&) {
        m_c.setHierarchyFilter(u.text(n));
        sync();
    });
    const NodeHandle row = w::panel(m_ui, page);
    m_ui.addClass(row, "row");
    m_ui.setCss(row, "gap", "4px");
    const NodeHandle parentBtn = w::toolButton(m_ui, row, "Parent to Active", "hierarchy.parent");
    w::setTooltip(m_ui, parentBtn, "Parent the other selected entities under the active (last clicked) one");
    m_ui.connect(parentBtn, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
        const EditorViewState& v = m_c.viewState();
        std::vector<EntityRef> others;
        for (const EntityRef& e : v.selection)
            if (!(e == v.primary)) others.push_back(e);
        m_c.reparent(others, v.primary);
        sync();
    });
    const NodeHandle clearBtn = w::toolButton(m_ui, row, "Clear Parent", "hierarchy.unparent");
    m_ui.connect(clearBtn, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
        m_c.reparent(m_c.viewState().selection, EntityRef{});
        sync();
    });
    m_hierTree = w::treeView(m_ui, page, "", "hierarchy.tree");
    m_ui.setCss(m_hierTree, "flex-grow", "1");
    m_ui.connect(m_hierTree, SignalKind::ValueChanged, [this](UiContext& u, NodeHandle, const SignalArgs& a) {
        const i32 line = i32(a.value);
        if (line < 0 || u32(line) >= m_hierRows.size()) return;
        const EntityRef e = m_hierRows[u32(line)];
        if (u.modifiers() & (fuse::ui::kModCtrl | fuse::ui::kModShift)) {
            std::vector<EntityRef> sel = m_c.viewState().selection;
            const auto it = std::find(sel.begin(), sel.end(), e);
            if (it != sel.end() && sel.size() > 1) sel.erase(it);
            else if (it == sel.end()) sel.push_back(e);
            m_c.select(sel, it != sel.end() ? sel.back() : e);
        } else {
            m_c.selectOnly(e);
        }
        sync();
    });
    m_hierInfo = w::label(m_ui, page, "", "hierarchy.info");
    m_ui.addClass(m_hierInfo, "ed-dim");
}

void EditorShellWindow::syncHierarchy_() {
    const EditorViewState& v = m_c.viewState();
    std::string text;
    m_hierRows.clear();
    i32 primaryLine = -1;
    for (const HierarchyRowView& r : v.hierarchy) {
        if (!text.empty()) text += '\n';
        text.append(r.depth, '\t');
        text += (r.selected ? "> " : "") + r.label;
        if (r.entity == v.primary) primaryLine = i32(m_hierRows.size());
        m_hierRows.push_back(r.entity);
    }
    if (text != m_hierText) {
        m_hierText = text;
        m_ui.setText(m_hierTree, text);
    }
    if (i32(m_ui.value(m_hierTree)) != primaryLine) m_ui.setValue(m_hierTree, f32(primaryLine));
    m_ui.setText(m_hierInfo, std::to_string(v.entityCount) + " entities, " + std::to_string(v.selection.size()) + " selected");
}

// ---- inspector ------------------------------------------------------------------------------------------------------
void EditorShellWindow::buildInspector_(NodeHandle page) {
    m_inspTitle = w::label(m_ui, page, "No selection", "inspector.title");
    m_inspBox = w::scrollArea(m_ui, page, "inspector.scroll");
    m_ui.setCss(m_inspBox, "flex-grow", "1");
    m_inspGrid = w::propertyGrid(m_ui, m_inspBox, "inspector.grid");
    const NodeHandle addRow = w::panel(m_ui, page);
    m_ui.addClass(addRow, "row");
    m_ui.setCss(addRow, "gap", "4px");
    m_inspAddCombo = w::comboBox(m_ui, addRow, "-", 0, "inspector.add_kind");
    m_ui.setCss(m_inspAddCombo, "flex-grow", "1");
    const NodeHandle add = w::button(m_ui, addRow, "Add Component", "inspector.add");
    m_ui.connect(add, SignalKind::Clicked, [this](UiContext& u, NodeHandle, const SignalArgs&) {
        const auto& names = m_c.viewState().addableComponents;
        const u32 i = u32(u.value(m_inspAddCombo));
        if (m_inspEntity.valid() && i < names.size()) m_c.addComponent(m_inspEntity, names[i]);
        sync();
    });
    const NodeHandle remRow = w::panel(m_ui, page);
    m_ui.addClass(remRow, "row");
    m_ui.setCss(remRow, "gap", "4px");
    m_inspRemoveCombo = w::comboBox(m_ui, remRow, "-", 0, "inspector.remove_kind");
    m_ui.setCss(m_inspRemoveCombo, "flex-grow", "1");
    const NodeHandle rem = w::button(m_ui, remRow, "Remove", "inspector.remove");
    m_ui.connect(rem, SignalKind::Clicked, [this](UiContext& u, NodeHandle, const SignalArgs&) {
        std::vector<std::string> removable;
        for (const InspectorSectionView& s : m_c.viewState().inspector)
            if (s.removable) removable.push_back(s.componentName);
        const u32 i = u32(u.value(m_inspRemoveCombo));
        if (m_inspEntity.valid() && i < removable.size()) m_c.removeComponent(m_inspEntity, removable[i]);
        sync();
    });
}

void EditorShellWindow::syncInspector_() {
    const EditorViewState& v = m_c.viewState();
    std::string key = std::to_string(v.primary.index) + ":" + std::to_string(v.primary.generation);
    for (const InspectorSectionView& s : v.inspector) {
        key += '|' + s.componentName;
        for (const InspectorFieldView& f : s.fields) key += ';' + f.label + '=' + f.value;
    }
    if (key == m_inspKey) return;
    // do not rebuild under the user's caret: an edit in progress would be lost (values refresh once focus leaves)
    for (NodeHandle f = m_ui.focused(); m_ui.alive(f); f = m_ui.parent(f))
        if (f == m_inspGrid && v.primary == m_inspEntity) return;
    m_inspKey = key;
    m_inspEntity = v.primary;
    m_ui.destroy(m_inspGrid);
    m_inspGrid = w::propertyGrid(m_ui, m_inspBox, "inspector.grid");
    m_gridRows.clear();
    if (!v.primary.valid()) {
        m_ui.setText(m_inspTitle, v.selection.size() > 1 ? std::to_string(v.selection.size()) + " entities selected" : "No selection");
    } else {
        std::string title;
        for (const HierarchyRowView& r : v.hierarchy)
            if (r.entity == v.primary) title = r.label;
        m_ui.setText(m_inspTitle, title.empty() ? "Entity #" + std::to_string(v.primary.index) : title);
    }
    std::string removable;
    for (const InspectorSectionView& s : v.inspector) {
        w::propertyRow(m_ui, m_inspGrid, {.name = s.displayName, .kind = w::PropKind::Group});
        m_gridRows.push_back({});
        if (s.removable) removable += (removable.empty() ? "" : "\n") + s.displayName;
        for (const InspectorFieldView& f : s.fields) {
            w::PropertyDesc d{.name = f.label};
            std::string value = f.value;
            std::string options;
            switch (f.type) {
            case PropertyFieldType::Bool: d.kind = w::PropKind::Bool; value = (f.value == "1" || f.value == "true") ? "true" : "false"; break;
            case PropertyFieldType::Float:
            case PropertyFieldType::UInt:
                d.kind = w::PropKind::Number;
                d.lo = f32(f.minValue);
                d.hi = f.maxValue > f.minValue ? f32(f.maxValue) : 1e6f;
                if (f.maxValue <= f.minValue && f.type == PropertyFieldType::Float) d.lo = -1e6f;
                d.step = f32(f.step > 0.0 ? f.step : 0.1);
                break;
            case PropertyFieldType::Enum:
                d.kind = w::PropKind::Enum;
                for (const std::string& e : f.enumNames) options += (options.empty() ? "" : "\n") + e;
                d.options = options;
                // the combo takes the index: schema enums read as "<index>" or "<index>;<extra>" (SDF type is
                // "1;0.5,0.5,0.5"), some as a name; writes take the bare index
                if (const std::size_t semi = value.find(';'); semi != std::string::npos) value.resize(semi);
                for (std::size_t i = 0; i < f.enumNames.size(); ++i)
                    if (f.enumNames[i] == value) value = std::to_string(i);
                break;
            case PropertyFieldType::Color: d.kind = w::PropKind::Color; value = schemaToHex(f.value); break;
            default:
                d.kind = w::PropKind::Text;
                if (f.value == kPropertyEmptyString) value.clear();
                break;
            }
            const NodeHandle ed = w::propertyRow(m_ui, m_inspGrid, d, value);
            if (f.propertyName.empty()) m_ui.setDisabled(ed, true);
            m_gridRows.push_back({f.propertyName, f.type, f.enumNames});
        }
    }
    m_ui.connect(m_inspGrid, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs& a) { inspectorEdited_(u32(a.value)); });
    std::string addable;
    for (const std::string& n : v.addableComponents) addable += (addable.empty() ? "" : "\n") + n;
    m_ui.setText(m_inspAddCombo, addable.empty() ? "-" : addable);
    m_ui.setText(m_inspRemoveCombo, removable.empty() ? "-" : removable);
}

void EditorShellWindow::inspectorEdited_(u32 row) {
    if (row >= m_gridRows.size() || m_gridRows[row].property.empty() || !m_inspEntity.valid()) return;
    const GridRow& r = m_gridRows[row];
    std::string value = w::propertyValue(m_ui, m_inspGrid, row);
    switch (r.type) {
    case PropertyFieldType::Bool: value = value == "true" ? "1" : "0"; break;
    case PropertyFieldType::Color: value = hexToSchema(value); break;
    case PropertyFieldType::UInt: value = std::to_string(u32(std::max(0.0, std::atof(value.c_str())))); break;
    case PropertyFieldType::String:
    case PropertyFieldType::AssetPath:
        if (value.empty()) value = std::string(kPropertyEmptyString);
        break;
    default: break;
    }
    m_c.setProperty(m_inspEntity, r.property, value);
    m_inspKey.clear(); // pick up the applied (normalised) value next sync
}

// ---- console --------------------------------------------------------------------------------------------------------
void EditorShellWindow::buildConsole_(NodeHandle page) {
    m_consoleList = w::listView(m_ui, page, "", -1, "console.log");
    m_ui.setCss(m_consoleList, "flex-grow", "1");
    m_consoleInput = w::lineEdit(m_ui, page, "", "console.input");
    m_ui.connect(m_consoleInput, SignalKind::Submitted, [this](UiContext& u, NodeHandle n, const SignalArgs&) {
        const std::string line(u.text(n));
        if (line.empty()) return;
        m_c.submitConsole(line);
        u.setText(n, "");
        sync();
    });
}

void EditorShellWindow::syncConsole_() {
    if (m_c.consoleVersion() == m_consoleSeen) return;
    m_consoleSeen = m_c.consoleVersion();
    std::string text;
    const auto& lines = m_c.console().lines();
    const std::size_t first = lines.size() > 500 ? lines.size() - 500 : 0;
    for (std::size_t i = first; i < lines.size(); ++i) {
        if (!text.empty()) text += '\n';
        const char* tag = lines[i].level == fuse::log::Level::Error ? "[error] " : lines[i].level == fuse::log::Level::Warn ? "[warn] " : "";
        text += tag + lines[i].text;
    }
    m_ui.setText(m_consoleList, text);
    if (!lines.empty()) m_ui.setValue(m_consoleList, f32(lines.size() - first - 1)); // follow the tail
}

void EditorShellWindow::buildPlaceholder_(NodeHandle page, const ShellPanel& p) {
    const NodeHandle t = w::label(m_ui, page, p.title, std::string(p.id) + ".title");
    (void)t;
    const NodeHandle l = w::label(m_ui, page, "Not ported to FUSE UI yet (Qt shell: --editor-shell=qt).", std::string(p.id) + ".note");
    m_ui.addClass(l, "ed-dim");
}

// ---- viewport -------------------------------------------------------------------------------------------------------
void EditorShellWindow::viewportInput_() {
    const w::ViewportInput& in = w::viewportLastInput(m_ui, m_viewport);
    using PE = ViewportPointerEvent;
    auto button = [&] { return in.button == fuse::ui::MouseButton::Right ? PE::Button::Right
                              : in.button == fuse::ui::MouseButton::Middle ? PE::Button::Middle : PE::Button::Left; };
    // SDF Sculpt mode: left-button strokes go to the brush (one "Sculpt stroke" undo step per drag)
    if (in.type == EventType::PointerDown || in.type == EventType::PointerMove || in.type == EventType::PointerUp) {
        const PE::Kind kind = in.type == EventType::PointerDown ? PE::Kind::Down : in.type == EventType::PointerMove ? PE::Kind::Move : PE::Kind::Up;
        if (m_models->terrainPointer({kind, button(), in.pos.x, in.pos.y}) || m_models->sculptPointer({kind, button(), in.pos.x, in.pos.y})) {
            if (in.type == EventType::PointerDown) m_ui.setFocus(m_viewport, fuse::ui::FocusReason::Pointer);
            sync();
            return;
        }
    }
    switch (in.type) {
    case EventType::PointerDown:
        m_ui.setFocus(m_viewport, fuse::ui::FocusReason::Pointer);
        m_c.viewportPointer({PE::Kind::Down, button(), in.pos.x, in.pos.y});
        break;
    case EventType::PointerMove: m_c.viewportPointer({PE::Kind::Move, button(), in.pos.x, in.pos.y}); break;
    case EventType::PointerUp: {
        // one popup per open: the Up reaches this handler once per dispatch phase, and a stale menu must not stack
        const bool wasOpen = m_c.contextMenuOpen() && m_ui.alive(m_ui.find("viewport.context"));
        m_c.viewportPointer({PE::Kind::Up, button(), in.pos.x, in.pos.y});
        if (m_c.contextMenuOpen() && !wasOpen) {
            std::string items;
            for (const ContextMenuItem& it : m_c.contextMenu().items()) {
                if (it.separatorBefore && !items.empty()) items += "\n-";
                items += std::string(items.empty() ? "" : "\n") + it.label;
            }
            const fuse::ui::Rect r = w::viewportImageRect(m_ui, m_viewport);
            const f32 sx = r.w / std::max(1.f, f32(m_image.width)), sy = r.h / std::max(1.f, f32(m_image.height));
            const fuse::ui::Vec2 at{r.x + in.pos.x * sx, r.y + in.pos.y * sy};
            const NodeHandle menu = w::popupMenu(m_ui, m_viewport, items, at, "viewport.context");
            // Qt QMenu::popup placement (EntityContextMenu::place): top-left at the click, flipped left / up when it
            // would overflow the right / bottom window edge, then clamped (the popup layer alone only shifts)
            m_ui.update(0.f);
            if (const NodeHandle panel = m_ui.firstChild(menu); m_ui.alive(panel)) {
                const fuse::ui::Rect mr = m_ui.rect(panel);
                ContextMenuHostGeometry host;
                host.viewportOriginX = r.x;
                host.viewportOriginY = r.y;
                host.devicePixelRatio = sx > 0.f ? 1.f / sx : 1.f;
                host.windowWidth = m_ui.viewport().w;
                host.windowHeight = m_ui.viewport().h;
                m_menuPlacement = EntityContextMenu::place(host, at.x, at.y, mr.w, mr.h);
                m_ui.setCss(panel, "left", std::to_string(m_menuPlacement.x) + "px");
                m_ui.setCss(panel, "top", std::to_string(m_menuPlacement.y) + "px");
            }
            m_ui.connect(menu, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs& a) {
                // map the popup line (separators count) back to the item
                u32 line = 0;
                for (const ContextMenuItem& it : m_c.contextMenu().items()) {
                    if (it.separatorBefore && line > 0) ++line;
                    if (line == u32(a.value)) { m_c.activateContextMenu(it.action); break; }
                    ++line;
                }
                m_c.closeContextMenu();
                sync();
            });
        }
        break;
    }
    case EventType::Wheel: m_c.viewportWheel(in.wheel.y); break;
    case EventType::KeyDown:
    case EventType::KeyUp: {
        const bool down = in.type == EventType::KeyDown;
        std::optional<ViewportKey> k;
        switch (in.key) {
        case Key::W: k = ViewportKey::W; break;
        case Key::A: k = ViewportKey::A; break;
        case Key::S: k = ViewportKey::S; break;
        case Key::D: k = ViewportKey::D; break;
        case Key::Q: k = ViewportKey::Q; break;
        case Key::E: k = ViewportKey::E; break;
        case Key::Num1: case Key::Num2: case Key::Num3: case Key::Num4:
            // Qt keys 1-4: Select / Translate / Rotate / Scale (not while flying)
            if (down && !m_c.viewportFlying() && !(m_ui.modifiers() & fuse::ui::kModCtrl)) {
                static constexpr TransformTool kKeyTools[] = {TransformTool::Select, TransformTool::Translate,
                                                              TransformTool::Rotate, TransformTool::Scale};
                m_c.setTransformTool(kKeyTools[u32(in.key) - u32(Key::Num1)]);
            }
            break;
        default: break;
        }
        if (k && !(m_ui.modifiers() & fuse::ui::kModCtrl)) {
            m_c.viewportKey(*k, down);
            m_c.viewportKey(ViewportKey::Shift, (m_ui.modifiers() & fuse::ui::kModShift) != 0);
            m_keysHeld = m_keysHeld || down;
        }
        break;
    }
    default: break;
    }
    sync();
}

void EditorShellWindow::renderViewport_() {
    const fuse::ui::Rect cr = m_ui.contentRect(m_viewport);
    const int wpx = std::max(64, int(cr.w)), hpx = std::max(64, int(cr.h));
    std::vector<EditorController::ViewportMarker> markers = m_c.viewportMarkers();
    // render key: size + camera + markers (skip identical frames: the idle gate counts presents)
    std::string key = std::to_string(wpx) + "x" + std::to_string(hpx);
    f32 ox = 0.f, oy = 0.f;
    if (m_c.projectPoint(0.f, 0.f, 0.f, ox, oy)) key += "|" + std::to_string(int(ox * 4.f)) + "," + std::to_string(int(oy * 4.f));
    f32 ux = 0.f, uy = 0.f;
    if (m_c.projectPoint(1.f, 1.f, 1.f, ux, uy)) key += "|" + std::to_string(int(ux * 4.f)) + "," + std::to_string(int(uy * 4.f));
    for (const auto& m : markers)
        key += "|" + std::to_string(m.entity.index) + ":" + std::to_string(int(m.x)) + "," + std::to_string(int(m.y)) + (m.selected ? "s" : "");
    key += "|vm" + std::to_string(int(m_c.viewMode())); // Unlit / Wireframe re-render the engine frame
    if (m_scene) {
        // scene edits / material edits / playback also change the 3D frame
        const u64 version = m_c.viewStateVersion() * 1000003ull + m_models->revision() +
                            (m_c.viewState().transport == TransportState::Playing ? m_syncs : 0u);
        const bool changed = key != m_viewportKey || version != m_sceneVersion;
        m_sceneVersion = version;
        m_viewportKey = key;
        if (renderSceneViewport_(wpx, hpx, changed)) return;
        m_viewportKey.clear(); // host failed: software viewport below
    }
    if (key == m_viewportKey) return;
    m_viewportKey = key;
    if (wpx != m_image.width || hpx != m_image.height) {
        m_image.resize(wpx, hpx);
        w::viewportSetImage(m_ui, m_viewport, m_imageIndex, u32(wpx), u32(hpx));
        m_c.viewportResize(u32(wpx), u32(hpx));
    }
    // background: vertical gradient ChildBg -> WindowBg
    const Color top = tokenColor("TitleBgActive"), bottom = tokenColor("ChildBg");
    for (int y = 0; y < hpx; ++y) {
        const f32 t = f32(y) / f32(hpx);
        const u32 c = fuse::ui::packPremultiplied({top.r + (bottom.r - top.r) * t, top.g + (bottom.g - top.g) * t, top.b + (bottom.b - top.b) * t, 1.f});
        std::fill_n(m_image.pixels.begin() + std::ptrdiff_t(y) * wpx, wpx, c);
    }
    Canvas cv{m_image};
    auto seg = [&](f32 ax, f32 ay, f32 az, f32 bx, f32 by, f32 bz, u32 c) {
        constexpr int kSteps = 16;
        f32 px0 = 0.f, py0 = 0.f;
        bool ok0 = m_c.projectPoint(ax, ay, az, px0, py0);
        for (int i = 1; i <= kSteps; ++i) {
            const f32 t = f32(i) / kSteps;
            f32 px1 = 0.f, py1 = 0.f;
            const bool ok1 = m_c.projectPoint(ax + (bx - ax) * t, ay + (by - ay) * t, az + (bz - az) * t, px1, py1);
            if (ok0 && ok1) cv.line(px0, py0, px1, py1, c);
            px0 = px1, py0 = py1, ok0 = ok1;
        }
    };
    const u32 grid = fuse::ui::packPremultiplied({0.22f, 0.24f, 0.27f, 1.f});
    for (int i = -10; i <= 10; ++i) {
        if (i == 0) continue;
        seg(f32(i), 0.f, -10.f, f32(i), 0.f, 10.f, grid);
        seg(-10.f, 0.f, f32(i), 10.f, 0.f, f32(i), grid);
    }
    seg(-10.f, 0.f, 0.f, 10.f, 0.f, 0.f, fuse::ui::packPremultiplied({0.75f, 0.25f, 0.25f, 1.f}));
    seg(0.f, 0.f, -10.f, 0.f, 0.f, 10.f, fuse::ui::packPremultiplied({0.25f, 0.45f, 0.85f, 1.f}));
    seg(0.f, 0.f, 0.f, 0.f, 1.5f, 0.f, fuse::ui::packPremultiplied({0.3f, 0.75f, 0.3f, 1.f}));
    std::sort(markers.begin(), markers.end(), [](const auto& a, const auto& b) { return a.depth > b.depth; });
    const u32 kinds[5] = {fuse::ui::packPremultiplied({0.75f, 0.77f, 0.80f, 1.f}), fuse::ui::packPremultiplied({0.45f, 0.70f, 0.95f, 1.f}),
                          fuse::ui::packPremultiplied({0.40f, 0.85f, 0.55f, 1.f}), fuse::ui::packPremultiplied({0.98f, 0.85f, 0.35f, 1.f}),
                          fuse::ui::packPremultiplied({0.75f, 0.55f, 0.95f, 1.f})};
    const u32 sel = fuse::ui::packPremultiplied({1.f, 0.6f, 0.15f, 1.f});
    for (const auto& m : markers) {
        const f32 r = std::clamp(60.f / std::max(0.5f, m.depth), 3.f, 18.f);
        cv.disc(m.x, m.y, r, kinds[std::min<u32>(m.kind, 4)]);
        if (m.selected) cv.ring(m.x, m.y, r + 1.f, 2.f, sel);
    }
    ++m_renders;
    m_ui.invalidate(m_viewport);
    const ecs::vec3 p = m_c.viewport().position();
    char info[160];
    std::snprintf(info, sizeof info, "Perspective   camera (%.1f, %.1f, %.1f)   %zu visible%s", double(p.x), double(p.y), double(p.z),
                  markers.size(), m_c.viewportFlying() ? "   [flying: WASD QE, Shift fast]" : "");
    m_ui.setText(m_viewportInfo, info);
}

void EditorShellWindow::setSceneViewport(SceneViewportHost* host, std::function<void(const SceneViewportFrame&)> present) {
    m_scene = host && host->valid() ? host : nullptr;
    m_scenePresent = m_scene ? std::move(present) : std::function<void(const SceneViewportFrame&)>{};
    m_image.externalSlot = m_scenePresent ? 0 : -1;
    if (m_scenePresent) // zero-copy: the CPU pixels are never written again (CPU raster shows the panel colour)
        std::fill(m_image.pixels.begin(), m_image.pixels.end(), fuse::ui::packPremultiplied(tokenColor("ChildBg")));
    m_viewportKey.clear();
    m_sceneVersion = ~0ull;
    renderViewport_();
    updateOverlay_();
}

bool EditorShellWindow::renderSceneViewport_(int wpx, int hpx, bool changed) {
    if (!changed) return true;
    if (wpx != m_image.width || hpx != m_image.height) {
        m_image.resize(wpx, hpx, fuse::ui::packPremultiplied(tokenColor("ChildBg")));
        w::viewportSetImage(m_ui, m_viewport, m_imageIndex, u32(wpx), u32(hpx));
        m_c.viewportResize(u32(wpx), u32(hpx));
    }
    bool ok = false;
    m_models->withScene([&](EditorHost& host, const ViewportPanel& view) { ok = m_scene->render(host, view, u32(wpx), u32(hpx)); });
    if (!ok) return false;
    ++m_sceneFrames;
    if (m_scenePresent) {
        m_scenePresent(m_scene->frame()); // zero-copy: the kernel samples the engine frame
    } else {
        u32 rw = 0, rh = 0;
        if (m_scene->readback(m_image.pixels, rw, rh) && (int(rw) != m_image.width || int(rh) != m_image.height))
            m_image.resize(wpx, hpx);
    }
    ++m_renders;
    m_ui.invalidate(m_viewport);
    const ecs::vec3 p = m_c.viewport().position();
    char info[200];
    std::snprintf(info, sizeof info, "Perspective   camera (%.1f, %.1f, %.1f)   %s scene frame %u%s", double(p.x), double(p.y),
                  double(p.z), m_scenePresent ? "GPU (zero-copy)" : "GPU (readback)", m_sceneFrames,
                  m_c.viewportFlying() ? "   [flying: WASD QE, Shift fast]" : "");
    m_ui.setText(m_viewportInfo, info);
    return true;
}

void EditorShellWindow::updateOverlay_() {
    const EditorController::GizmoView g = m_c.viewportGizmo();
    const u32 tool = u32(g.tool);
    if (tool != m_toolShown) {
        m_toolShown = tool;
        for (u32 i = 0; i < 4; ++i) {
        if (i == tool) m_ui.addClass(m_gizmoTools[i], "active");
        else m_ui.removeClass(m_gizmoTools[i], "active");
        }
    }
    const bool local = m_c.transformSpace() == TransformSpace::Local, snap = m_c.transformSnap();
    if (local != m_spaceShown) {
        m_spaceShown = local;
        m_ui.setText(m_gizmoSpace, local ? "Local" : "World");
    }
    if (snap) m_ui.addClass(m_gizmoSnap, "active");
    else m_ui.removeClass(m_gizmoSnap, "active");
    std::string key = std::to_string(tool) + "|" + std::to_string(g.dragAxis);
    if (g.visible)
        for (const GizmoHandle2D& h : g.handles)
            key += "|" + std::to_string(int(h.x0)) + "," + std::to_string(int(h.y0)) + "," + std::to_string(int(h.x1)) + "," +
                   std::to_string(int(h.y1)) + (h.visible ? "v" : "");
    // View Mode wireframe / Show Mesh Bounds (Qt viewport overlay roles and colours)
    const std::vector<EditorController::ViewportSegment> segs = m_c.viewportSegments(
        [](void* user, u32 id) -> const std::vector<f32>* {
            auto* host = static_cast<SceneViewportHost*>(user);
            return host ? host->meshWireframe(id) : nullptr;
        },
        m_scene);
    if (!segs.empty()) {
        u64 h = 1469598103934665603ull;
        for (const auto& s : segs)
            for (f32 v : {s.x0, s.y0, s.x1, s.y1}) h = (h ^ u64(std::int64_t(v * 4.f))) * 1099511628211ull;
        key += "|seg" + std::to_string(segs.size()) + ":" + std::to_string(h);
    }
    key += "|vm" + std::to_string(int(m_c.viewMode())) + (m_c.showMeshBounds() ? "b" : "");
    // Navigation panel overlay (Qt nav role: light blue), world segments projected per frame
    std::vector<f32> nav;
    {
        const std::vector<f32> w3 = m_models->navSegments();
        for (usize k = 0; k + 6u <= w3.size(); k += 6u) {
            f32 ax = 0.f, ay = 0.f, bx = 0.f, by = 0.f;
            if (m_c.projectPoint(w3[k], w3[k + 1], w3[k + 2], ax, ay) && m_c.projectPoint(w3[k + 3], w3[k + 4], w3[k + 5], bx, by))
                nav.insert(nav.end(), {ax, ay, bx, by});
        }
        if (!nav.empty()) {
            u64 h = 1469598103934665603ull;
            for (f32 v : nav) h = (h ^ u64(std::int64_t(v * 4.f))) * 1099511628211ull;
            key += "|nav" + std::to_string(nav.size()) + ":" + std::to_string(h);
        }
    }
    syncPlayState_();
    if (m_playState != 0) key += "|play" + playBadge();
    if (key == m_overlayKey) return;
    m_overlayKey = key;
    w::ViewportOverlay o;
    for (const auto& s : segs) {
        Color c = s.role == 4 ? Color::rgb(0xFFC850, 220.f / 255.f) : s.role == 3 ? Color::rgb(0xE6E6F0, 120.f / 255.f)
                : s.role == 1 ? Color::rgb(0x78FF8C, 230.f / 255.f) : Color::rgb(0x5AC86E, 150.f / 255.f);
        o.lines.push_back({{s.x0, s.y0}, {s.x1, s.y1}, c, s.role >= 3 ? 1.f : 1.4f});
    }
    for (usize k = 0; k + 4u <= nav.size(); k += 4u)
        o.lines.push_back({{nav[k], nav[k + 1]}, {nav[k + 2], nav[k + 3]}, Color::rgb(0x46C8FF, 170.f / 255.f), 1.4f});
    if (g.visible) {
        static const Color kAxis[3] = {Color::rgb(0xE5484D), Color::rgb(0x46C46A), Color::rgb(0x3E8EF7)};
        const Color hot = Color::rgb(0xF5D547);
        for (int a = 0; a < 3; ++a) {
            const GizmoHandle2D& h = g.handles[std::size_t(a)];
            if (!h.visible) continue;
            const Color c = g.dragAxis == a ? hot : kAxis[a];
            o.lines.push_back({{h.x0, h.y0}, {h.x1, h.y1}, c, g.dragAxis == a ? 4.f : 3.f});
            // handle shapes mirror the Qt viewport: arrow head / ring (r 7) / box (10 px)
            const f32 dx = h.x1 - h.x0, dy = h.y1 - h.y0, len = std::sqrt(dx * dx + dy * dy);
            const f32 ux = len > 1.f ? dx / len : 1.f, uy = len > 1.f ? dy / len : 0.f;
            switch (g.tool) {
            case TransformTool::Translate:
                // filled triangle: stacked cross strokes narrowing to the tip
                for (int s = 0; s < 10; ++s) {
                    const f32 t = f32(s), half = 5.f * (1.f - t / 10.f);
                    const f32 cx = h.x1 + ux * t, cy = h.y1 + uy * t;
                    o.lines.push_back({{cx - uy * half, cy + ux * half}, {cx + uy * half, cy - ux * half}, c, 1.5f});
                }
                break;
            case TransformTool::Rotate: {
                constexpr int kSeg = 20;
                for (int s = 0; s < kSeg; ++s) {
                    const f32 a0 = 6.2831853f * f32(s) / kSeg, a1 = 6.2831853f * f32(s + 1) / kSeg;
                    o.lines.push_back({{h.x1 + 7.f * std::cos(a0), h.y1 + 7.f * std::sin(a0)},
                                       {h.x1 + 7.f * std::cos(a1), h.y1 + 7.f * std::sin(a1)}, c, 2.f});
                }
                break;
            }
            case TransformTool::Scale: o.lines.push_back({{h.x1 - 5.f, h.y1}, {h.x1 + 5.f, h.y1}, c, 10.f}); break;
            default: break;
            }
        }
        o.dots.push_back({{g.handles[0].x0, g.handles[0].y0}, 4.f, Color::rgb(0xF0F0F0), Color::rgb(0x101010), 1.f});
    }
    // collider drag handles (Qt paintColliders_: 9 px squares, yellow while dragged)
    {
        const auto handles = m_c.colliderHandles();
        for (std::size_t i = 0; i < handles.size(); ++i) {
            const bool active = m_c.colliderDragging() && i == m_c.colliderDragHandle();
            o.dots.push_back({{handles[i].x, handles[i].y}, 4.5f, active ? Color::rgb(0xFFE65A) : Color::rgb(0x78FF8C), Color::rgb(0x142814), 1.f});
        }
    }
    // play-mode frame + badge (Qt setPlayBanner): green PLAYING / amber PAUSED, session clock and ticks
    if (m_playState != 0) {
        const Color c = m_playState == 2 ? Color::rgb(0xE6AA28) : Color::rgb(0x3CC864);
        o.frame = c;
        o.frameWidth = 4.f;
        o.badge = playBadge();
        o.badgeColor = c;
        o.badgeText = Color::rgb(0x0F0F0F);
    }
    w::viewportSetOverlay(m_ui, m_viewport, std::move(o));
}

void EditorShellWindow::syncPlayState_() {
    const EditorViewState& v = m_c.viewState();
    const int state = v.transport == TransportState::Playing ? 1 : v.transport == TransportState::Paused ? 2 : 0;
    const auto now = std::chrono::steady_clock::now();
    if (state != m_playState) {
        if (m_playState == 1) m_playAccumMs += std::chrono::duration<f64, std::milli>(now - m_playClock).count();
        if (state != 0 && m_playState == 0) {
            m_playAccumMs = 0.0;
            m_playTicks = 0;
            m_playLastTick = v.gameTicks;
            if (m_c.maximizeOnPlay()) {
                m_maximized = true;
                for (const DockSide side : kSides) m_ui.setCss(w::dockRegion(m_ui, m_dockArea, side), "display", "none");
            }
        }
        if (state == 1) m_playClock = now;
        if (state == 0 && m_maximized) {
            m_maximized = false;
            for (const DockSide side : kSides) m_ui.setCss(w::dockRegion(m_ui, m_dockArea, side), "display", "flex");
        }
        m_playState = state;
    }
    if (state != 0) {
        if (v.gameTicks > m_playLastTick) m_playTicks += v.gameTicks - m_playLastTick; // Step counts too
        m_playLastTick = v.gameTicks;
    }
}

f64 EditorShellWindow::playElapsedMs() const {
    return m_playAccumMs +
           (m_playState == 1 ? std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - m_playClock).count() : 0.0);
}

std::string EditorShellWindow::playBadge() const {
    if (m_playState == 0) return {};
    char b[96];
    std::snprintf(b, sizeof b, "%s  %.1f s  -  %u ticks", m_playState == 2 ? "PAUSED" : "PLAYING", playElapsedMs() / 1000.0, m_playTicks);
    return b;
}

// ---- shortcuts ------------------------------------------------------------------------------------------------------
namespace {
struct KeyName {
    Key key;
    const char* name;
};
constexpr KeyName kKeyNames[] = {
    {Key::A, "A"}, {Key::B, "B"}, {Key::C, "C"}, {Key::D, "D"}, {Key::E, "E"}, {Key::F, "F"}, {Key::G, "G"},
    {Key::H, "H"}, {Key::I, "I"}, {Key::J, "J"}, {Key::K, "K"}, {Key::L, "L"}, {Key::M, "M"}, {Key::N, "N"},
    {Key::O, "O"}, {Key::P, "P"}, {Key::Q, "Q"}, {Key::R, "R"}, {Key::S, "S"}, {Key::T, "T"}, {Key::U, "U"},
    {Key::V, "V"}, {Key::W, "W"}, {Key::X, "X"}, {Key::Y, "Y"}, {Key::Z, "Z"},
    {Key::Num0, "0"}, {Key::Num1, "1"}, {Key::Num2, "2"}, {Key::Num3, "3"}, {Key::Num4, "4"}, {Key::Num5, "5"},
    {Key::Num6, "6"}, {Key::Num7, "7"}, {Key::Num8, "8"}, {Key::Num9, "9"},
    {Key::F1, "F1"}, {Key::F2, "F2"}, {Key::F3, "F3"}, {Key::F4, "F4"}, {Key::F5, "F5"}, {Key::F6, "F6"},
    {Key::F7, "F7"}, {Key::F8, "F8"}, {Key::F9, "F9"}, {Key::F10, "F10"}, {Key::F11, "F11"}, {Key::F12, "F12"},
    {Key::Delete, "Del"}, {Key::Space, "Space"}, {Key::Home, "Home"}, {Key::End, "End"},
};
std::string lowerAscii(std::string_view v) {
    std::string o(v);
    for (char& c : o) c = char(std::tolower(static_cast<unsigned char>(c)));
    return o;
}
} // namespace

std::string EditorShellWindow::shortcutText(Key key, u8 mods) {
    const char* name = nullptr;
    for (const KeyName& k : kKeyNames)
        if (k.key == key) name = k.name;
    if (!name) return {};
    std::string t;
    if (mods & fuse::ui::kModCtrl) t += "Ctrl+";
    if (mods & fuse::ui::kModAlt) t += "Alt+";
    if (mods & fuse::ui::kModShift) t += "Shift+";
    return t + name;
}

std::string EditorShellWindow::normalizeShortcut(std::string_view text) {
    u8 mods = 0;
    std::optional<Key> key;
    std::size_t at = 0;
    while (at <= text.size()) {
        std::size_t e = text.find('+', at);
        if (e == at && e + 1 == text.size()) e = std::string_view::npos; // "Ctrl++" is not supported; tolerate
        const std::string tok = lowerAscii(text.substr(at, e == std::string_view::npos ? std::string_view::npos : e - at));
        std::string t;
        for (char c : tok)
            if (c != ' ') t += c;
        if (t == "ctrl" || t == "control" || t == "cmd") mods |= fuse::ui::kModCtrl;
        else if (t == "alt") mods |= fuse::ui::kModAlt;
        else if (t == "shift") mods |= fuse::ui::kModShift;
        else if (!t.empty()) {
            if (key) return {};
            for (const KeyName& k : kKeyNames)
                if (lowerAscii(k.name) == t || (t == "delete" && k.key == Key::Delete)) key = k.key;
            if (!key) return {};
        }
        if (e == std::string_view::npos) break;
        at = e + 1;
    }
    return key ? shortcutText(*key, mods) : std::string{};
}

void EditorShellWindow::loadBindings_() {
    const fuse::app::Settings& s = m_win.app().settings();
    m_bindings.assign(editorActions().size(), {});
    for (const ActionDesc& a : editorActions()) {
        std::string b = normalizeShortcut(a.shortcut);
        // "shortcut.<stableId>": "" = unbound, absent = the default (Qt QSettings shortcuts/<objectName>)
        const std::string_view stored = s.get(std::string("shortcut.") + a.stableId, "\x01");
        if (stored != "\x01") b = normalizeShortcut(stored);
        m_bindings[std::size_t(a.id)] = b;
    }
    m_menuKey.clear();
}

bool EditorShellWindow::shortcut(Key key, u8 mods) {
    const std::string combo = shortcutText(key, mods);
    if (combo.empty()) return false;
    std::optional<ActionId> id;
    for (const ActionDesc& a : editorActions())
        if (m_bindings[std::size_t(a.id)] == combo) {
            id = a.id;
            break;
        }
    // fixed alternates (Qt QKeySequence::Redo also answers Ctrl+Shift+Z / Ctrl+Y)
    if (!id && (combo == "Ctrl+Shift+Z" || combo == "Ctrl+Y")) id = ActionId::EditRedo;
    if (!id) return false;
    // F5 cycles play / pause / resume on the Play binding (Qt updateTransportActions)
    if (*id == ActionId::PlayPlay || *id == ActionId::PlayPause || *id == ActionId::PlayResume) {
        const TransportState t = m_c.viewState().transport;
        id = t == TransportState::Stopped ? ActionId::PlayPlay : t == TransportState::Playing ? ActionId::PlayPause : ActionId::PlayResume;
    }
    m_c.trigger(*id);
    sync();
    return true;
}

bool EditorShellWindow::rootEvent(const Event& e, NodeHandle target) {
    if (m_pages && m_pages->event(e, target)) return true;
    if (e.type != EventType::KeyDown) return false;
    // Preferences shortcut field records the pressed combination (QKeySequenceEdit)
    if (preferencesOpen() && target == m_ui.find("prefs.shortcut_edit") && e.mods != 0) {
        const std::string combo = shortcutText(e.key, e.mods);
        if (!combo.empty()) {
            m_ui.setText(target, combo);
            return true;
        }
    }
    // unmodified letter / digit shortcuts (F = Frame Selected) only act on the viewport or hierarchy; elsewhere
    // they are typing
    const std::string bare = shortcutText(e.key, 0);
    const bool plainKey = (e.mods & (fuse::ui::kModCtrl | fuse::ui::kModAlt)) == 0 &&
                          (bare.size() == 1 || e.key == Key::Delete || e.key == Key::Space);
    if (plainKey && target != m_hierTree && target != m_viewport) return false;
    if (shortcut(e.key, e.mods)) return true;
    // Delete removes the selection unless a text field has the focus
    if (e.key == Key::Delete && (target == m_hierTree || target == m_viewport)) {
        m_c.trigger(ActionId::EntityDelete);
        sync();
        return true;
    }
    return false;
}

void EditorShellWindow::frame() {
    m_frameTick = true;
    sync();
    m_frameTick = false;
}

// ---- sync -----------------------------------------------------------------------------------------------------------
bool EditorShellWindow::sync() {
    const auto t0 = std::chrono::steady_clock::now();
    ++m_syncs;
    if (m_keysHeld || m_c.viewportFlying()) {
        m_keysHeld = m_c.viewportTick(1.f / 30.f) || m_c.viewportFlying();
    }
    const bool changed = m_c.refresh();
    const bool console = m_c.consoleVersion() != m_consoleSeen;
    if (changed || m_seenVersion != m_c.viewStateVersion()) {
        m_seenVersion = m_c.viewStateVersion();
        const EditorViewState& v = m_c.viewState();
        m_win.setTitle(v.title);
        m_ui.setText(m_status, v.status.empty() ? std::string("Ready") : v.status);
        syncMenus_();
        syncToolbar_();
        syncHierarchy_();
        syncInspector_();
    }
    syncMenus_(); // keyed: also picks up the View Mode / Show Mesh Bounds checks
    if (console) syncConsole_();
    m_pages->sync(m_lastSyncMs, m_frameTick);
    renderViewport_();
    updateOverlay_();
    syncOrtho_();
    m_lastSyncMs = f32(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return changed || console;
}

// ---- Multi-viewport (Qt OrthoViewWidget / setViewportLayout) -----------------------------------------------------
void EditorShellWindow::syncOrtho_() {
    const int layout = m_c.viewportLayout();
    if (layout != m_layoutShown) {
        m_layoutShown = layout;
        m_ui.setCss(m_orthoCell[0], "display", layout >= 1 ? "flex" : "none");
        m_ui.setCss(m_orthoCell[1], "display", layout == 2 ? "flex" : "none");
        m_ui.setCss(m_orthoCell[2], "display", layout == 2 ? "flex" : "none");
        m_ui.setCss(m_vpBottomRow, "display", layout == 2 ? "flex" : "none");
        for (std::string& k : m_orthoKey) k.clear();
    }
    for (int i = 0; i < 3; ++i) {
        const bool shown = i == 0 ? layout >= 1 : layout == 2;
        if (!shown) continue;
        OrthoView& v = m_orthoView[i];
        const fuse::ui::Rect r = m_ui.contentRect(m_ortho[i]);
        if (r.w < 2.f || r.h < 2.f) continue;
        const bool resized = v.w != std::floor(r.w) || v.h != std::floor(r.h);
        v.w = std::floor(r.w);
        v.h = std::floor(r.h);
        // until the user pans / zooms / frames, keep the whole scene fitted as the layout settles (Qt resizeEvent)
        if (resized && !v.framed && v.w > 50 && v.h > 50) { m_models->orthoFrame(v, false); v.framed = false; }
        auto items = m_models->orthoItems(v);
        char head[96];
        std::snprintf(head, sizeof head, "%.0fx%.0f|%.4f|%.4f|%.4f|%d", v.w, v.h, v.centerU, v.centerV, v.scale,
                      int(m_ui.focused() == m_ortho[i]));
        std::string key = head;
        for (const OrthoItem& it : items) {
            char b[96];
            std::snprintf(b, sizeof b, "|%u.%u:%.1f,%.1f,%.1f,%.1f,%.1f,%.1f%c", it.entity.index, it.entity.generation, it.x0, it.y0,
                          it.x1, it.y1, it.cx, it.cy, it.selected ? 's' : '-');
            key += b;
        }
        if (key == m_orthoKey[i]) continue;
        m_orthoKey[i] = std::move(key);
        w::viewportSetImage(m_ui, m_ortho[i], m_orthoBgIndex, u32(v.w), u32(v.h));
        w::ViewportOverlay o;
        using C = fuse::ui::Color;
        // grid: minor every 10^k units (>= 12 px apart), major every 10 minor
        f32 step = 0.01f;
        while (step * v.scale < 12.f) step *= 10.f;
        auto grid = [&](f32 s, C c) {
            const f32 u0 = v.centerU - v.w * 0.5f / v.scale, u1 = v.centerU + v.w * 0.5f / v.scale;
            const f32 v0 = v.centerV - v.h * 0.5f / v.scale, v1 = v.centerV + v.h * 0.5f / v.scale;
            for (f32 u = std::floor(u0 / s) * s; u <= u1; u += s) {
                const f32 x = v.w * 0.5f + (u - v.centerU) * v.scale;
                o.lines.push_back({{x, 0}, {x, v.h}, c, 1.f});
            }
            for (f32 q = std::floor(v0 / s) * s; q <= v1; q += s) {
                const f32 y = v.h * 0.5f - (q - v.centerV) * v.scale;
                o.lines.push_back({{0, y}, {v.w, y}, c, 1.f});
            }
        };
        grid(step, C::rgb(0x282C34));
        grid(step * 10.f, C::rgb(0x373C46));
        f32 ox = 0, oy = 0;
        v.project(0, 0, 0, ox, oy);
        const C red = C::rgb(0xDC4646), green = C::rgb(0x50C85A), blue = C::rgb(0x466EE6);
        o.lines.push_back({{0, oy}, {v.w, oy}, v.axis == OrthoView::Axis::Side ? blue : red, 1.5f});
        o.lines.push_back({{ox, 0}, {ox, v.h}, v.axis == OrthoView::Axis::Top ? blue : green, 1.5f});
        u32 drawn = 0;
        for (const OrthoItem& it : items) {
            const C c = it.selected ? C::rgb(0xFFA528) : C::rgb(0x96AFCD);
            const f32 lw = it.selected ? 2.f : 1.2f;
            if (it.box()) {
                f32 x0 = it.x0, y0 = it.y0, x1 = it.x1, y1 = it.y1;
                if (x1 - x0 < 2) { x0 -= 1; x1 += 1; }
                if (y1 - y0 < 2) { y0 -= 1; y1 += 1; }
                o.lines.push_back({{x0, y0}, {x1, y0}, c, lw});
                o.lines.push_back({{x1, y0}, {x1, y1}, c, lw});
                o.lines.push_back({{x1, y1}, {x0, y1}, c, lw});
                o.lines.push_back({{x0, y1}, {x0, y0}, c, lw});
            } else {
                o.lines.push_back({{it.cx - 5, it.cy}, {it.cx + 5, it.cy}, c, lw});
                o.lines.push_back({{it.cx, it.cy - 5}, {it.cx, it.cy + 5}, c, lw});
                o.dots.push_back({{it.cx, it.cy}, 3.f, C{}, c, lw});
            }
            ++drawn;
        }
        m_orthoDrawn[i] = drawn;
        o.badge = std::string(v.name()) + "  (" + v.axes() + ")";
        o.badgeColor = C::rgb(0x262A32, 0.85f);
        o.badgeText = C::rgb(0xC8CDD7);
        if (m_ui.focused() == m_ortho[i]) {
            o.frame = C::rgb(0x5A96E6);
            o.frameWidth = 1.f;
        }
        w::viewportSetOverlay(m_ui, m_ortho[i], std::move(o));
    }
}

void EditorShellWindow::orthoInput_(int i) {
    const w::ViewportInput& in = w::viewportLastInput(m_ui, m_ortho[i]);
    OrthoView& v = m_orthoView[i];
    using PE = ViewportPointerEvent;
    bool changed = false;
    switch (in.type) {
    case EventType::PointerDown:
    case EventType::PointerMove:
    case EventType::PointerUp: {
        const PE::Kind kind = in.type == EventType::PointerDown ? PE::Kind::Down : in.type == EventType::PointerMove ? PE::Kind::Move : PE::Kind::Up;
        const PE::Button b = in.button == fuse::ui::MouseButton::Right ? PE::Button::Right
                             : in.button == fuse::ui::MouseButton::Middle ? PE::Button::Middle : PE::Button::Left;
        if (kind == PE::Kind::Down) m_ui.setFocus(m_ortho[i], fuse::ui::FocusReason::Pointer);
        if (kind == PE::Kind::Move && v.drag == OrthoView::Drag::None) return;
        const bool additive = (m_ui.modifiers() & (fuse::ui::kModCtrl | fuse::ui::kModShift)) != 0;
        changed = m_models->orthoPointer(v, {kind, b, in.pos.x, in.pos.y}, additive);
        break;
    }
    case EventType::Wheel:
        if (in.wheel.y == 0.f) return;
        v.zoomAt(in.pos.x, in.pos.y, in.wheel.y > 0.f);
        break;
    case EventType::KeyDown:
        if (in.key != Key::F) return;
        m_models->orthoFrame(v, true);
        break;
    default: return;
    }
    (void)changed;
    sync();
}

// ---- New Project wizard (Qt NewProjectWizard parity) ---------------------------------------------------------------
void EditorShellWindow::openNewProjectWizard() {
    if (newProjectWizardOpen()) return;
    std::string location = m_models->projectHub().projectsRoot;
    if (location.empty()) {
        std::error_code ec;
        location = std::filesystem::current_path(ec).string();
    }
    m_wizard = w::dialog(m_ui, "New FUSE Project", "wizard");
    const NodeHandle body = w::dialogBody(m_ui, m_wizard);
    m_ui.setCss(body, "gap", "4px");
    const auto row = [&](std::string_view label) {
        const NodeHandle r = w::panel(m_ui, body);
        m_ui.addClass(r, "row");
        m_ui.setCss(r, "gap", "6px");
        const NodeHandle l = w::label(m_ui, r, label);
        m_ui.setCss(l, "min-width", "80px");
        return r;
    };
    NodeHandle r = row("Name");
    const NodeHandle name = w::lineEdit(m_ui, r, "NewProject", "wizard.name");
    m_ui.setCss(name, "flex-grow", "1");
    r = row("Location");
    const NodeHandle loc = w::lineEdit(m_ui, r, location, "wizard.location");
    m_ui.setCss(loc, "flex-grow", "1");
    m_ui.setCss(loc, "min-width", "260px");
    const NodeHandle browse = w::button(m_ui, r, "...", "wizard.browse");
    m_ui.connect(browse, SignalKind::Clicked, [this, loc](UiContext& u, NodeHandle, const SignalArgs&) {
        if (const auto dir = m_c.services().pickDirectory("Project Location", u.text(loc)); dir && !dir->empty()) u.setText(loc, *dir);
    });
    const NodeHandle hint = w::label(m_ui, body, "", "wizard.hint");
    r = row("Dimension");
    w::comboBox(m_ui, r, "3D\n2D\nHybrid (3D + 2D)", 2, "wizard.dimension");
    w::checkBox(m_ui, r, "UI layer", true, "wizard.ui");
    r = row("Modules");
    static constexpr std::pair<const char*, const char*> kModules[] = {
        {"ai", "AI"}, {"cinematics", "Cinematics"}, {"fx", "FX"}, {"mechanics", "Mechanics"}, {"adventure", "Adventure"}};
    for (const auto& [key, text] : kModules) w::checkBox(m_ui, r, text, false, std::string("wizard.module.") + key);
    // project name + directory from the fields (captures stay small: InlineFn)
    static constexpr auto target = [](const UiContext& u) {
        std::string n(u.text(u.find("wizard.name")));
        if (n.empty()) n = "NewProject";
        return std::pair{n, (std::filesystem::path(std::string(u.text(u.find("wizard.location")))) / n).string()};
    };
    const auto updateHint = [hint](UiContext& u, NodeHandle, const SignalArgs&) { u.setText(hint, "Creates " + target(u).second); };
    updateHint(m_ui, {}, {});
    m_ui.connect(name, SignalKind::TextChanged, updateHint);
    m_ui.connect(loc, SignalKind::TextChanged, updateHint);
    const NodeHandle buttons = w::panel(m_ui, body);
    m_ui.addClass(buttons, "row");
    m_ui.setCss(buttons, "gap", "6px");
    m_ui.setCss(buttons, "justify-content", "flex-end");
    const NodeHandle create = w::button(m_ui, buttons, "Create", "wizard.create");
    const NodeHandle cancel = w::button(m_ui, buttons, "Cancel", "wizard.cancel");
    m_ui.connect(cancel, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { closeNewProjectWizard_(); });
    m_ui.connect(create, SignalKind::Clicked, [this](UiContext& u, NodeHandle, const SignalArgs&) {
        const auto [n, dir] = target(u);
        const u32 dim = u32(u.value(u.find("wizard.dimension"))); // 0 3D, 1 2D, 2 Hybrid
        u32 flags = 0;
        if (dim != 1) flags |= kProjectEnable3D;
        if (dim != 0) flags |= kProjectEnable2D;
        if (u.checked(u.find("wizard.ui"))) flags |= kProjectEnableUI;
        project::ModuleSettings m;
        m.ai = u.checked(u.find("wizard.module.ai"));
        m.cinematics = u.checked(u.find("wizard.module.cinematics"));
        m.fx = u.checked(u.find("wizard.module.fx"));
        m.mechanics = u.checked(u.find("wizard.module.mechanics"));
        m.adventure = u.checked(u.find("wizard.module.adventure"));
        closeNewProjectWizard_();
        if (!m_models->hubNew(dir, n, flags, m) && !m_c.lastError().empty())
            m_c.services().warn("New Project", m_c.lastError());
        sync();
    });
    w::openDialog(m_ui, m_wizard);
    m_ui.setFocus(name);
}

void EditorShellWindow::closeNewProjectWizard_() {
    if (!newProjectWizardOpen()) return;
    const NodeHandle d = m_wizard;
    m_wizard = {};
    w::closeDialog(m_ui, d);
    m_ui.destroy(d);
}

// ---- Preferences (Qt PreferencesDialog parity) --------------------------------------------------------------------
std::vector<std::string> EditorShellWindow::shortcutConflicts() {
    std::vector<std::string> defaults;
    for (const ActionDesc& a : editorActions()) defaults.push_back(normalizeShortcut(a.shortcut));
    return shortcutConflicts(defaults);
}

std::vector<std::string> EditorShellWindow::shortcutConflicts(std::span<const std::string> bindings) {
    std::vector<std::pair<std::string, std::string>> bound;
    for (const ActionDesc& a : editorActions())
        if (std::size_t(a.id) < bindings.size() && !bindings[std::size_t(a.id)].empty())
            bound.emplace_back(bindings[std::size_t(a.id)], a.label);
    std::sort(bound.begin(), bound.end());
    std::vector<std::string> out;
    for (std::size_t i = 0; i < bound.size();) {
        std::size_t j = i + 1;
        while (j < bound.size() && bound[j].first == bound[i].first) ++j;
        if (j - i > 1) {
            std::string line = bound[i].first + ":";
            for (std::size_t k = i; k < j; ++k) line += (k == i ? " " : ", ") + bound[k].second;
            out.push_back(line);
        }
        i = j;
    }
    return out;
}

void EditorShellWindow::openPreferences() {
    if (preferencesOpen()) return;
    const EditorPreferences p = m_c.preferences();
    m_prefsDialog = w::dialog(m_ui, "Preferences", "prefs");
    const NodeHandle body = w::dialogBody(m_ui, m_prefsDialog);
    m_ui.setCss(body, "gap", "4px");
    const auto row = [&](std::string_view label) {
        const NodeHandle r = w::panel(m_ui, body);
        m_ui.addClass(r, "row");
        m_ui.setCss(r, "gap", "6px");
        const NodeHandle l = w::label(m_ui, r, label);
        m_ui.setCss(l, "min-width", "150px");
        return r;
    };
    w::label(m_ui, body, "General", "prefs.general");
    w::checkBox(m_ui, row("Viewport"), "Preview sun when the scene has no light", p.previewSun, "prefs.preview_sun");
    w::spinBox(m_ui, row("Camera speed (units/s)"), 0.5f, 500.f, f32(p.cameraSpeed), 0.5f, "prefs.camera_speed");
    w::spinBox(m_ui, row("Look sensitivity (deg/px)"), 0.01f, 2.f, f32(p.lookSensitivity), 0.01f, "prefs.look_sensitivity");
    w::checkBox(m_ui, row("Layout"), "Restore the dock layout at startup", p.restoreLayout, "prefs.restore_layout");
    // Shortcuts: every named action with its key (Qt editableActions, sorted by text), filterable
    w::label(m_ui, body, "Shortcuts", "prefs.shortcuts_title");
    const NodeHandle filter = w::lineEdit(m_ui, body, "", "prefs.filter");
    m_prefsBindings = m_bindings;
    const NodeHandle list = w::listView(m_ui, body, prefsRows_(""), -1, "prefs.shortcuts");
    m_ui.setCss(list, "height", "180px");
    m_ui.setCss(list, "min-width", "460px");
    m_ui.connect(filter, SignalKind::TextChanged, [this, list](UiContext& u, NodeHandle n, const SignalArgs&) { u.setText(list, prefsRows_(u.text(n))); });
    // rebinding row (Qt: QKeySequenceEdit + Reset): press a combination in the field or type it, then Assign
    // the row is a capture node like the shell root, so key presses in the field reach rootEvent (dialogs are not
    // under the shell root) and are recorded as combinations
    const NodeHandle bindRow = m_ui.create("editorshellroot", body, "prefs.shortcut_row").value_or(NodeHandle{});
    m_ui.setUserData(bindRow, static_cast<u64>(reinterpret_cast<std::uintptr_t>(this)));
    m_ui.addClass(bindRow, "row");
    m_ui.setCss(bindRow, "gap", "6px");
    m_ui.setCss(w::label(m_ui, bindRow, "Shortcut"), "min-width", "150px");
    const NodeHandle edit = w::lineEdit(m_ui, bindRow, "", "prefs.shortcut_edit");
    m_ui.setCss(edit, "min-width", "140px");
    const NodeHandle assign = w::button(m_ui, bindRow, "Assign", "prefs.assign");
    const NodeHandle clear = w::button(m_ui, bindRow, "Clear", "prefs.clear_shortcut");
    const NodeHandle reset = w::button(m_ui, bindRow, "Reset", "prefs.reset_shortcut");
    const NodeHandle resetAll = w::button(m_ui, bindRow, "Reset All", "prefs.reset_all");
    m_ui.connect(list, SignalKind::ValueChanged, [this, edit](UiContext& u, NodeHandle n, const SignalArgs&) {
        const i32 r = i32(u.value(n));
        if (r >= 0 && std::size_t(r) < m_prefsRows.size()) u.setText(edit, m_prefsBindings[std::size_t(m_prefsRows[std::size_t(r)])]);
    });
    const auto selected = [this, list]() -> std::optional<ActionId> {
        const i32 r = i32(m_ui.value(list));
        if (r < 0 || std::size_t(r) >= m_prefsRows.size()) return std::nullopt;
        return m_prefsRows[std::size_t(r)];
    };
    m_ui.connect(assign, SignalKind::Clicked, [this, selected, edit](UiContext& u, NodeHandle, const SignalArgs&) {
        if (const auto id = selected()) setPreferencesShortcut(editorActions()[std::size_t(*id)].stableId, u.text(edit));
    });
    m_ui.connect(clear, SignalKind::Clicked, [this, selected](UiContext&, NodeHandle, const SignalArgs&) {
        if (const auto id = selected()) setPreferencesShortcut(editorActions()[std::size_t(*id)].stableId, "");
    });
    m_ui.connect(reset, SignalKind::Clicked, [this, selected](UiContext&, NodeHandle, const SignalArgs&) {
        if (const auto id = selected()) setPreferencesShortcut(editorActions()[std::size_t(*id)].stableId, editorActions()[std::size_t(*id)].shortcut);
    });
    m_ui.connect(resetAll, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
        for (const ActionDesc& a : editorActions()) m_prefsBindings[std::size_t(a.id)] = normalizeShortcut(a.shortcut);
        prefsRefresh_();
    });
    w::label(m_ui, body, "", "prefs.conflicts");
    prefsRefresh_();
    const NodeHandle buttons = w::panel(m_ui, body);
    m_ui.addClass(buttons, "row");
    m_ui.setCss(buttons, "gap", "6px");
    m_ui.setCss(buttons, "justify-content", "flex-end");
    const NodeHandle ok = w::button(m_ui, buttons, "OK", "prefs.ok");
    const NodeHandle apply = w::button(m_ui, buttons, "Apply", "prefs.apply");
    const NodeHandle cancel = w::button(m_ui, buttons, "Cancel", "prefs.cancel");
    m_ui.connect(cancel, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { closePreferences_(); });
    m_ui.connect(apply, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { applyPreferencesDialog_(); });
    m_ui.connect(ok, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
        if (applyPreferencesDialog_()) closePreferences_();
    });
    w::openDialog(m_ui, m_prefsDialog);
    m_ui.setFocus(filter);
}

bool EditorShellWindow::applyPreferencesDialog_() {
    if (!preferencesOpen()) return false;
    if (!preferencesConflicts().empty()) return false; // Qt refuses while shortcuts conflict
    EditorPreferences p;
    p.previewSun = m_ui.checked(m_ui.find("prefs.preview_sun"));
    p.cameraSpeed = m_ui.value(m_ui.find("prefs.camera_speed"));
    p.lookSensitivity = m_ui.value(m_ui.find("prefs.look_sensitivity"));
    p.restoreLayout = m_ui.checked(m_ui.find("prefs.restore_layout"));
    m_c.applyPreferences(p);
    fuse::app::Settings& s = m_win.app().settings();
    s.set("prefs.previewSun", p.previewSun ? 1.0 : 0.0);
    s.set("prefs.cameraSpeed", p.cameraSpeed);
    s.set("prefs.lookSensitivity", p.lookSensitivity);
    s.set("prefs.restoreLayout", p.restoreLayout ? 1.0 : 0.0);
    for (const ActionDesc& a : editorActions()) {
        const std::string& b = m_prefsBindings[std::size_t(a.id)];
        if (b == normalizeShortcut(a.shortcut)) {
            if (s.get(std::string("shortcut.") + a.stableId, "\x01") != "\x01") s.set(std::string("shortcut.") + a.stableId, b);
        } else {
            s.set(std::string("shortcut.") + a.stableId, b);
        }
    }
    m_bindings = m_prefsBindings;
    m_menuKey.clear();
    syncMenus_();
    (void)s.save(); // no settings file (tests, --settings unset): kept in memory
    m_viewportKey.clear(); // the preview sun changes the engine frame
    sync();
    return true;
}

std::string EditorShellWindow::prefsRows_(std::string_view f) {
    // every named action with its key (Qt editableActions, sorted by text), filterable
    std::vector<std::pair<std::string, ActionId>> lines;
    const std::string fl = lowerAscii(f);
    for (const ActionDesc& a : editorActions()) {
        std::string text = std::string(a.label);
        if (text.size() > 3 && text.compare(text.size() - 3, 3, "...") == 0) text.resize(text.size() - 3);
        const std::string& b = m_prefsBindings[std::size_t(a.id)];
        std::string line = text + "  [" + std::string(a.menu) + "]   " + (b.empty() ? "-" : b);
        if (fl.empty() || lowerAscii(line).find(fl) != std::string::npos) lines.emplace_back(line, a.id);
    }
    std::sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return lowerAscii(a.first) < lowerAscii(b.first); });
    std::string items;
    m_prefsRows.clear();
    for (const auto& l : lines) {
        items += (items.empty() ? "" : "\n") + l.first;
        m_prefsRows.push_back(l.second);
    }
    return items;
}

void EditorShellWindow::prefsRefresh_() {
    if (!preferencesOpen()) return;
    const NodeHandle list = m_ui.find("prefs.shortcuts");
    const i32 sel = i32(m_ui.value(list));
    m_ui.setText(list, prefsRows_(m_ui.text(m_ui.find("prefs.filter"))));
    if (sel >= 0) m_ui.setValue(list, f32(sel));
    // the field shows the selected action's (pending) binding, also right after Assign / Reset
    if (sel >= 0 && std::size_t(sel) < m_prefsRows.size())
        m_ui.setText(m_ui.find("prefs.shortcut_edit"), m_prefsBindings[std::size_t(m_prefsRows[std::size_t(sel)])]);
    const std::vector<std::string> conflicts = preferencesConflicts();
    std::string c = conflicts.empty() ? "No shortcut conflicts." : "Conflicts: ";
    for (std::size_t i = 0; i < conflicts.size(); ++i) c += (i ? "; " : "") + conflicts[i];
    m_ui.setText(m_ui.find("prefs.conflicts"), c);
}

bool EditorShellWindow::setPreferencesShortcut(std::string_view stableId, std::string_view combo) {
    if (!preferencesOpen()) return false;
    const std::string b = normalizeShortcut(combo);
    if (b.empty() && !combo.empty()) return false;
    for (const ActionDesc& a : editorActions())
        if (stableId == a.stableId) {
            m_prefsBindings[std::size_t(a.id)] = b;
            prefsRefresh_();
            return true;
        }
    return false;
}

void EditorShellWindow::closePreferences_() {
    if (!preferencesOpen()) return;
    const NodeHandle d = m_prefsDialog;
    m_prefsDialog = {};
    w::closeDialog(m_ui, d);
    m_ui.destroy(d);
}

// =====================================================================================================================
DialogServices::DialogServices(fuse::app::Application& app, fuse::app::AppWindow& window) : m_app(app), m_win(window) {}

std::optional<std::string> DialogServices::file_(std::string_view title, std::string_view filter, std::string_view directory, bool save) {
    UiContext& ui = m_win.ui();
    std::string dir(directory);
    if (dir.empty()) {
        std::error_code ec;
        dir = std::filesystem::current_path(ec).string();
    }
    // Qt filters look like "FUSE scene (*.fscene)": keep the patterns
    std::string pats;
    if (const auto a = filter.find('('), b = filter.rfind(')'); a != std::string_view::npos && b != std::string_view::npos && b > a) {
        for (char c : filter.substr(a + 1, b - a - 1)) pats += c == ' ' ? ';' : c;
    }
    const NodeHandle dlg = w::fileDialog(ui, title, dir, pats, save, "editor.file_dialog");
    struct R { bool done = false; std::string path; } r;
    ui.connect(dlg, SignalKind::Submitted, [p = &r](UiContext&, NodeHandle, const SignalArgs& a) { p->done = true; p->path = std::string(a.text); });
    while (!r.done && !m_app.quitting() && !m_win.closing()) m_app.step(0.05f);
    ui.destroy(dlg);
    if (r.path.empty()) return std::nullopt;
    return r.path;
}
std::optional<std::string> DialogServices::pickOpenFile(std::string_view t, std::string_view f, std::string_view d) { return file_(t, f, d, false); }
std::optional<std::string> DialogServices::pickSaveFile(std::string_view t, std::string_view f, std::string_view d) { return file_(t, f, d, true); }
std::optional<std::string> DialogServices::pickDirectory(std::string_view t, std::string_view d) {
    auto p = file_(t, "", d, true);
    if (!p) return p;
    std::error_code ec;
    if (std::filesystem::is_directory(*p, ec)) return p;
    return std::filesystem::path(*p).parent_path().string();
}
std::optional<Rgba> DialogServices::pickColour(Rgba initial) { return initial; }

i32 DialogServices::buttons_(std::string_view title, std::string_view text, std::string_view buttons) {
    UiContext& ui = m_win.ui();
    const NodeHandle dlg = w::dialog(ui, title, "editor.message");
    const NodeHandle body = w::dialogBody(ui, dlg);
    w::label(ui, body, text);
    const NodeHandle row = w::panel(ui, body);
    ui.addClass(row, "row");
    ui.setCss(row, "gap", "6px");
    struct R { i32 pick = -2; } r;
    i32 i = 0;
    std::size_t start = 0;
    while (start <= buttons.size()) {
        const std::size_t end = std::min(buttons.find('\n', start), buttons.size());
        const NodeHandle b = w::button(ui, row, buttons.substr(start, end - start));
        const i32 idx = i++;
        ui.connect(b, SignalKind::Clicked, [p = &r, idx](UiContext&, NodeHandle, const SignalArgs&) { p->pick = idx; });
        start = end + 1;
    }
    w::openDialog(ui, dlg);
    while (r.pick == -2 && !m_app.quitting() && !m_win.closing()) m_app.step(0.05f);
    w::closeDialog(ui, dlg);
    ui.destroy(dlg);
    return r.pick;
}

PromptAnswer DialogServices::askSaveDiscardCancel(std::string_view text) {
    switch (buttons_("Unsaved changes", text, "Save\nDiscard\nCancel")) {
    case 0: return PromptAnswer::Save;
    case 1: return PromptAnswer::Discard;
    default: return PromptAnswer::Cancel;
    }
}
void DialogServices::warn(std::string_view title, std::string_view text) { buttons_(title, text, "OK"); }

bool DialogServices::openExternal(std::string_view path) {
    // Qt QDesktopServices::openUrl(QUrl::fromLocalFile(path)): the system's handler for the file
#ifdef _WIN32
    const std::wstring w = std::filesystem::path(std::string(path)).wstring();
    return reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    std::string q = "'";
    for (char c : path) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    q += "'";
    return std::system(("xdg-open " + q + " >/dev/null 2>&1 &").c_str()) == 0;
#endif
}

} // namespace fuse::editor::fui
