// UI9: the FUSE UI editor shell (see editor_shell.hpp).
#include "fuse/editor/fui/editor_shell.hpp"
#include "editor_shell_panels.hpp"

#include <chrono>

#include <fuse/editor/editor_theme_tokens.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

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
    css += ".ed-vpinfo { padding: 2px 8px; color: " + tokenHex("TextDisabled") + "; background: " + tokenHex("TitleBg") + "; }\n";
    css += ".ed-status { padding: 3px 8px; color: " + tokenHex("Text") + "; background: " + tokenHex("TitleBgActive") + "; }\n";
    return css;
}

// =====================================================================================================================
EditorShellWindow::EditorShellWindow(fuse::app::AppWindow& window, EditorController& controller, EditorShellOptions options)
    : m_win(window), m_ui(window.ui()), m_c(controller), m_opt(options) {
    w::install(m_ui);
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
            case ActionId::FileQuit:
                if (self->m_c.requestClose()) self->m_win.requestClose();
                return true;
            default: return false;
            }
        },
        this);
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
    m_viewportInfo = w::label(m_ui, centre, "Perspective", "viewport.info");
    m_ui.addClass(m_viewportInfo, "ed-vpinfo");
    m_viewport = w::viewportWidget(m_ui, centre, m_imageIndex, u32(m_image.width), u32(m_image.height), "viewport");
    m_ui.setCss(m_viewport, "flex-grow", "1");
    // the image follows the widget size: keep the widget from sizing to the image (feedback on shrink)
    m_ui.setCss(m_viewport, "height", "0px");
    m_ui.setCss(m_viewport, "min-width", "0px");
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
            if (*a.shortcut) label += "   (" + std::string(a.shortcut) + ")";
            add(label, i32(a.id));
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
    if (code <= -2) m_c.fileOpenRecent(u32(-2 - code));
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
        if (*a.shortcut) w::setTooltip(m_ui, b, std::string(a.label) + "  (" + a.shortcut + ")");
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
        if (m_models->sculptPointer({kind, button(), in.pos.x, in.pos.y})) {
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
        m_c.viewportPointer({PE::Kind::Up, button(), in.pos.x, in.pos.y});
        if (m_c.contextMenuOpen()) {
            std::string items;
            for (const ContextMenuItem& it : m_c.contextMenu().items()) {
                if (it.separatorBefore && !items.empty()) items += "\n-";
                items += std::string(items.empty() ? "" : "\n") + it.label;
            }
            const fuse::ui::Rect r = w::viewportImageRect(m_ui, m_viewport);
            const f32 sx = r.w / std::max(1.f, f32(m_image.width)), sy = r.h / std::max(1.f, f32(m_image.height));
            const NodeHandle menu = w::popupMenu(m_ui, m_viewport, items, {r.x + in.pos.x * sx, r.y + in.pos.y * sy}, "viewport.context");
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

// ---- shortcuts ------------------------------------------------------------------------------------------------------
bool EditorShellWindow::shortcut(Key key, u8 mods) {
    const bool ctrl = (mods & fuse::ui::kModCtrl) != 0, shift = (mods & fuse::ui::kModShift) != 0;
    std::optional<ActionId> id;
    if (ctrl) {
        switch (key) {
        case Key::N: id = ActionId::FileNewScene; break;
        case Key::O: id = ActionId::FileOpenScene; break;
        case Key::S: id = shift ? ActionId::FileSaveAs : ActionId::FileSave; break;
        case Key::Z: id = shift ? ActionId::EditRedo : ActionId::EditUndo; break;
        case Key::Y: id = ActionId::EditRedo; break;
        case Key::D: id = ActionId::EntityDuplicate; break;
        default: break;
        }
    } else if (key == Key::F5) {
        id = shift ? ActionId::PlayStop : m_c.viewState().transport == TransportState::Stopped ? ActionId::PlayPlay
             : m_c.viewState().transport == TransportState::Playing                         ? ActionId::PlayPause
                                                                                            : ActionId::PlayResume;
    } else if (key == Key::F10) {
        id = ActionId::PlayStep;
    }
    if (!id) return false;
    m_c.trigger(*id);
    sync();
    return true;
}

bool EditorShellWindow::rootEvent(const Event& e, NodeHandle target) {
    if (e.type != EventType::KeyDown) return false;
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
    if (console) syncConsole_();
    m_pages->sync(m_lastSyncMs, m_frameTick);
    renderViewport_();
    m_lastSyncMs = f32(std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return changed || console;
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

} // namespace fuse::editor::fui
