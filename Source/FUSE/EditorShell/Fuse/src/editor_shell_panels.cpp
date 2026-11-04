// UI9: Project Hub, Asset Browser, Material Editor, SDF Sculpt, Profiler, Sequencer and Timeline pages on EditorPanels.
#include "editor_shell_panels.hpp"

#include <fuse/ui/widgets/widgets.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace fuse::editor::fui {
namespace w = fuse::ui::widgets;
using fuse::ui::Color;
using fuse::ui::SignalArgs;
using fuse::ui::SignalKind;
using fuse::ui::UiContext;

namespace {
std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : "\n") + x;
    return s;
}
std::string leaf(const std::string& p) {
    const std::string n = std::filesystem::path(p).filename().string();
    return n.empty() ? p : n;
}
} // namespace

ShellPanelPages::ShellPanelPages(EditorShellWindow& shell, EditorPanels& models)
    : m_shell(shell), m_ui(shell.window().ui()), m_m(models) {}

NodeHandle ShellPanelPages::row_(NodeHandle parent) {
    const NodeHandle r = w::panel(m_ui, parent);
    m_ui.addClass(r, "row");
    m_ui.setCss(r, "gap", "6px");
    m_ui.setCss(r, "align-items", "center");
    return r;
}
NodeHandle ShellPanelPages::field_(NodeHandle parent, std::string_view label) {
    const NodeHandle r = row_(parent);
    const NodeHandle l = w::label(m_ui, r, label);
    m_ui.setCss(l, "width", "84px");
    return r;
}

bool ShellPanelPages::build(std::string_view id, NodeHandle page) {
    m_ui.setCss(page, "gap", "6px");
    m_ui.setCss(page, "padding", "6px");
    if (id == "project_hub") buildHub_(page);
    else if (id == "asset_browser") buildAssets_(page);
    else if (id == "material_editor") buildMaterial_(page);
    else if (id == "sdf_sculpt") buildSculpt_(page);
    else if (id == "profiler") buildProfiler_(page);
    else if (id == "sequencer") buildSequencer_(page);
    else if (id == "timeline") buildTimeline_(page);
    else if (id == "navigation") buildNavigation_(page);
    else if (id == "terrain") buildTerrain_(page);
    else if (id == "lighting") buildLighting_(page);
    else return false;
    return true;
}

void ShellPanelPages::sync(f32 uiMs, bool frame) {
    if (frame) {
        if (m_shell.panelVisible("profiler")) m_m.profilerSample(uiMs);
        const auto now = std::chrono::steady_clock::now();
        const f32 dt = m_lastTick.time_since_epoch().count() ? std::min(0.1f, std::chrono::duration<f32>(now - m_lastTick).count()) : 0.f;
        m_lastTick = now;
        if (m_m.timelineAdvance(dt)) m_shell.controller().refresh();
    }
    const u64 view = m_shell.controller().viewStateVersion();
    const bool all = view != m_seenView;
    if (!all && m_m.revision() == m_seenRevision) return;
    m_seenView = view;
    m_seenRevision = m_m.revision();
    if (all) { syncHub_(); syncAssets_(); syncMaterial_(); syncSculpt_(); }
    syncProfiler_();
    syncNavigation_();
    syncTerrain_();
    syncLighting_();
    syncSequencer_();
    syncTimeline_();
    if (!all) { syncMaterial_(); syncSculpt_(); syncAssets_(); syncHub_(); }
}

// ---- Project Hub ----------------------------------------------------------------------------------------------------
void ShellPanelPages::buildHub_(NodeHandle page) {
    m_hubInfo = w::label(m_ui, page, "", "hub.current");
    m_hubRoot = w::label(m_ui, page, "", "hub.root");
    m_ui.addClass(m_hubRoot, "ed-dim");
    m_hubList = w::listView(m_ui, page, "", -1, "hub.list");
    m_ui.setCss(m_hubList, "flex-grow", "1");
    m_ui.connect(m_hubList, SignalKind::Submitted, [this](UiContext& u, NodeHandle n, const SignalArgs&) {
        const i32 i = i32(u.value(n));
        if (i >= 0 && u32(i) < m_hubProjects.size()) m_m.hubOpen(m_hubProjects[u32(i)]);
        m_shell.sync();
    });
    const NodeHandle r = row_(page);
    const NodeHandle open = w::button(m_ui, r, "Open Project", "hub.open");
    m_ui.connect(open, SignalKind::Clicked, [this](UiContext& u, NodeHandle, const SignalArgs&) {
        const i32 i = i32(u.value(m_hubList));
        if (i >= 0 && u32(i) < m_hubProjects.size()) m_m.hubOpen(m_hubProjects[u32(i)]);
        m_shell.sync();
    });
    m_hubName = w::lineEdit(m_ui, r, "NewProject", "hub.name");
    m_ui.setCss(m_hubName, "flex-grow", "1");
    const NodeHandle mk = w::button(m_ui, r, "New Project...", "hub.new");
    m_ui.connect(mk, SignalKind::Clicked, [this](UiContext& u, NodeHandle, const SignalArgs&) {
        std::string name(u.text(m_hubName));
        if (name.empty()) name = "NewProject";
        const std::string root = m_m.projectHub().projectsRoot;
        m_m.hubNew((std::filesystem::path(root) / name).string(), name);
        m_shell.sync();
    });
}
void ShellPanelPages::syncHub_() {
    if (!m_hubList) return;
    const ProjectHubView v = m_m.projectHub();
    std::string key = v.name + "|" + v.projectsRoot + "|" + join(v.projects);
    if (key == m_hubKey) return;
    m_hubKey = std::move(key);
    m_ui.setText(m_hubInfo, v.hasProject ? "Open: " + v.name + "  (" + v.directory + ")" : std::string("No project open"));
    m_ui.setText(m_hubRoot, "Projects in " + v.projectsRoot);
    m_hubProjects = v.projects;
    std::vector<std::string> names;
    i32 cur = -1;
    for (const auto& p : v.projects) {
        if (v.hasProject && std::filesystem::path(p) == std::filesystem::path(v.directory)) cur = i32(names.size());
        names.push_back(leaf(p));
    }
    m_ui.setText(m_hubList, join(names));
    m_ui.setValue(m_hubList, f32(cur));
}

// ---- Asset Browser (Qt AssetBrowserWidget: grid / list, type filter, breadcrumb, status, context menu) -----------
void ShellPanelPages::buildAssets_(NodeHandle page) {
    const NodeHandle r = row_(page);
    const NodeHandle up = w::button(m_ui, r, "Up", "assets.up");
    m_ui.connect(up, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.assetsUp(); m_shell.sync(); });
    const NodeHandle rescan = w::button(m_ui, r, "Rescan", "assets.rescan");
    m_ui.connect(rescan, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.assetsRescan(); m_shell.sync(); });
    const NodeHandle r2 = row_(page); // filter + type on their own row (the dock is narrow)
    m_assetFilter = w::lineEdit(m_ui, r2, "", "assets.filter");
    m_ui.setCss(m_assetFilter, "flex-grow", "1");
    w::setTooltip(m_ui, m_assetFilter, "Filter assets by name");
    m_ui.connect(m_assetFilter, SignalKind::TextChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
        m_m.assetsSetFilter(a.text);
        m_shell.sync();
    });
    std::string types;
    for (const std::string& l : EditorPanels::assetTypeFilterLabels()) types += (types.empty() ? "" : "\n") + l;
    m_assetType = w::comboBox(m_ui, r2, types, 0, "assets.type");
    w::setTooltip(m_ui, m_assetType, "Show only one asset type (folders stay visible)");
    m_ui.connect(m_assetType, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
        m_m.assetsSetTypeFilter(u32(a.value));
        m_shell.sync();
    });
    m_assetGridToggle = w::checkBox(m_ui, r, "Grid", false, "assets.grid");
    w::setTooltip(m_ui, m_assetGridToggle, "Toggle grid / list view");
    m_ui.connect(m_assetGridToggle, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) {
        m_m.assetsSetGrid(a.flag);
        m_shell.sync();
    });
    m_assetCrumbs = row_(page);
    m_ui.setCss(m_assetCrumbs, "gap", "2px");
    m_assetPath = w::label(m_ui, page, "", "assets.path");
    m_ui.addClass(m_assetPath, "ed-dim");
    m_assetList = w::listView(m_ui, page, "", -1, "assets.list");
    m_ui.setCss(m_assetList, "flex-grow", "1");
    m_assetModel.user = this;
    m_assetModel.rows = [](const void* u) { return u32(static_cast<const ShellPanelPages*>(u)->m_assetNames.size()); };
    m_assetModel.data = [](const void* u, u32 row, u32, std::string&) -> std::string_view {
        const auto* p = static_cast<const ShellPanelPages*>(u);
        return row < p->m_assetNames.size() ? std::string_view(p->m_assetNames[row]) : std::string_view();
    };
    m_assetGrid = w::gridView(m_ui, page, &m_assetModel, 104.f, 56.f, "assets.grid_view");
    m_ui.setCss(m_assetGrid, "flex-grow", "1");
    m_ui.setCss(m_assetGrid, "display", "none");
    auto activate = [this](UiContext&, NodeHandle, const SignalArgs&) {
        const i32 i = selectedAsset_();
        if (i >= 0) m_m.assetsActivate(m_assetPaths[u32(i)]);
        m_shell.sync();
    };
    auto select = [this](UiContext&, NodeHandle n, const SignalArgs& a) {
        const i32 i = i32(a.value);
        if (i >= 0 && u32(i) < m_assetPaths.size()) m_m.assetsSelect(m_assetPaths[u32(i)]);
        const NodeHandle other = n == m_assetList ? m_assetGrid : m_assetList;
        m_ui.setValue(other, a.value);
        if (i >= 0 && u32(i) < m_assetTips.size()) w::setTooltip(m_ui, n, m_assetTips[u32(i)]);
        m_shell.sync();
    };
    for (const NodeHandle v : {m_assetList, m_assetGrid}) {
        m_ui.connect(v, SignalKind::Submitted, activate);
        m_ui.connect(v, SignalKind::ValueChanged, select);
    }
    const NodeHandle open = w::button(m_ui, r, "Open", "assets.open");
    m_ui.connect(open, SignalKind::Clicked, activate);
    // inline name prompt (Qt QInputDialog) for Rename... / New Folder...
    m_assetNameRow = w::panel(m_ui, page, "assets.name_row");
    m_ui.addClass(m_assetNameRow, "row");
    m_ui.setCss(m_assetNameRow, "gap", "6px");
    m_assetName = w::lineEdit(m_ui, m_assetNameRow, "", "assets.name");
    m_ui.setCss(m_assetName, "flex-grow", "1");
    auto commit = [this](UiContext& u, NodeHandle, const SignalArgs&) {
        const std::string name(u.text(m_assetName));
        const bool ok = m_assetRenaming.empty() ? m_m.assetsCreateFolder(name) : m_m.assetsRename(m_assetRenaming, name);
        if (ok) m_ui.setCss(m_assetNameRow, "display", "none");
        m_assetKey.clear();
        m_shell.sync();
        if (!ok) m_ui.setText(m_assetInfo, (m_assetRenaming.empty() ? "Could not create \"" : "Could not rename to \"") + name + "\"");
    };
    m_ui.connect(m_assetName, SignalKind::Submitted, commit);
    m_ui.connect(w::button(m_ui, m_assetNameRow, "OK", "assets.name_ok"), SignalKind::Clicked, commit);
    m_ui.connect(w::button(m_ui, m_assetNameRow, "Cancel", "assets.name_cancel"), SignalKind::Clicked,
                 [this](UiContext&, NodeHandle, const SignalArgs&) { m_ui.setCss(m_assetNameRow, "display", "none"); });
    m_ui.setCss(m_assetNameRow, "display", "none");
    m_assetInfo = w::label(m_ui, page, "", "assets.info");
}
i32 ShellPanelPages::selectedAsset_() const {
    const NodeHandle v = m_m.assets().grid ? m_assetGrid : m_assetList;
    const i32 i = i32(m_ui.value(v));
    return i >= 0 && u32(i) < m_assetPaths.size() ? i : -1;
}
bool ShellPanelPages::within_(NodeHandle n, NodeHandle ancestor) const {
    for (; m_ui.alive(n); n = m_ui.parent(n))
        if (n == ancestor) return true;
    return false;
}
void ShellPanelPages::assetsMenuAction(u32 action) {
    const i32 i = selectedAsset_();
    const std::string rel = i >= 0 ? m_assetPaths[u32(i)] : std::string();
    switch (action) {
    case 0: if (i >= 0) m_m.assetsActivate(rel); break;
    case 1: if (i >= 0) m_ui.setClipboardText(m_m.assetsAbsolutePath(rel)); break;
    case 2: if (i >= 0) m_ui.setClipboardText(rel); break;
    case 3:
    case 4:
        if (action == 3 && i < 0) break;
        m_assetRenaming = action == 3 ? rel : std::string();
        m_ui.setText(m_assetName, action == 3 ? m_assetNames[u32(i)] : std::string("New Folder"));
        m_ui.setCss(m_assetNameRow, "display", "flex");
        m_ui.setFocus(m_assetName, fuse::ui::FocusReason::Programmatic);
        break;
    case 5: m_m.assetsRescan(); break;
    default: break;
    }
    m_shell.sync();
}
void ShellPanelPages::syncAssets_() {
    if (!m_assetList) return;
    const AssetBrowserView v = m_m.assets();
    m_ui.setDisabled(m_ui.find("assets.up"), v.crumbs.size() <= 1); // Qt: Up disabled at the project root
    std::string lines;
    m_assetPaths.clear();
    m_assetNames.clear();
    m_assetTips.clear();
    i32 sel = -1;
    for (const auto& e : v.entries) {
        if (e.path == v.selected) sel = i32(m_assetPaths.size());
        lines += (lines.empty() ? "" : "\n") + std::string(e.folder ? "[dir]  " : "") + e.name + (e.folder ? "" : "   (" + e.type + ")");
        m_assetPaths.push_back(e.path);
        m_assetNames.push_back((e.folder ? "[dir] " : "") + e.name);
        m_assetTips.push_back(e.tooltip);
    }
    std::string key = v.root + "|" + v.directory + "|" + lines + "|" + std::to_string(v.grid) + "|" + std::to_string(v.typeFilter) +
                      "|" + v.status;
    if (key == m_assetKey) return;
    m_assetKey = std::move(key);
    m_ui.setText(m_assetPath, v.root + (v.directory.empty() ? "" : "/" + v.directory));
    m_ui.setText(m_assetList, lines);
    m_ui.setValue(m_assetList, f32(sel));
    m_ui.setValue(m_assetGrid, f32(sel));
    m_ui.markAllDirty();
    m_ui.setCss(m_assetList, "display", v.grid ? "none" : "flex");
    m_ui.setCss(m_assetGrid, "display", v.grid ? "flex" : "none");
    m_ui.setChecked(m_assetGridToggle, v.grid);
    m_ui.setValue(m_assetType, f32(v.typeFilter));
    if (m_ui.text(m_assetFilter) != v.filter) m_ui.setText(m_assetFilter, v.filter);
    // breadcrumb: Project > a > b (each jumps there)
    while (m_ui.alive(m_ui.firstChild(m_assetCrumbs))) m_ui.destroy(m_ui.firstChild(m_assetCrumbs));
    for (std::size_t k = 0; k < v.crumbs.size(); ++k) {
        if (k > 0) w::label(m_ui, m_assetCrumbs, ">");
        const NodeHandle b = w::button(m_ui, m_assetCrumbs, v.crumbs[k], "assets.crumb." + std::to_string(k));
        m_ui.connect(b, SignalKind::Clicked, [this, k](UiContext&, NodeHandle, const SignalArgs&) {
            const AssetBrowserView cur = m_m.assets();
            std::string rel;
            for (std::size_t j = 1; j <= k && j < cur.crumbs.size(); ++j) rel += (rel.empty() ? "" : "/") + cur.crumbs[j];
            m_m.assetsNavigateTo(rel);
            m_shell.sync();
        });
    }
    m_ui.setText(m_assetInfo, v.status);
}

// ---- Material Editor ------------------------------------------------------------------------------------------------
void ShellPanelPages::buildMaterial_(NodeHandle page) {
    const NodeHandle top = row_(page);
    m_ui.setCss(top, "align-items", "flex-start");
    m_previewImg.resize(96, 96);
    m_previewIndex = m_shell.window().addImage(&m_previewImg);
    m_matPreview = w::image(m_ui, top, m_previewIndex, 96.f, 96.f, "material.preview");
    const NodeHandle col = w::panel(m_ui, top);
    m_ui.setCss(col, "flex-grow", "1");
    m_ui.setCss(col, "gap", "4px");
    m_matList = w::comboBox(m_ui, col, "", 0, "material.list");
    m_ui.connect(m_matList, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
        m_m.materialSelect(u32(a.value));
        m_shell.sync();
    });
    const NodeHandle r = row_(col);
    const NodeHandle nb = w::button(m_ui, r, "New", "material.new");
    m_ui.connect(nb, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.materialNew(); m_shell.sync(); });
    m_matAssign = w::button(m_ui, r, "Assign to Selection", "material.assign");
    m_ui.connect(m_matAssign, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
        m_m.materialAssignToSelection();
        m_shell.sync();
    });
    m_matSummary = w::label(m_ui, col, "", "material.summary");
    m_ui.addClass(m_matSummary, "ed-dim");
    // Qt usability row: name (Enter renames), presets, usage + Select users
    NodeHandle nf = field_(page, "Name");
    m_matName = w::lineEdit(m_ui, nf, "", "material.name");
    m_ui.setCss(m_matName, "flex-grow", "1");
    m_ui.connect(m_matName, SignalKind::Submitted, [this](UiContext& u, NodeHandle n, const SignalArgs&) { m_m.materialRename(u.text(n)); m_shell.sync(); });
    nf = field_(page, "Preset");
    std::string presets = "(apply preset)";
    for (const std::string& n : EditorPanels::materialPresetNames()) presets += "\n" + n;
    m_matPreset = w::comboBox(m_ui, nf, presets, 0, "material.preset");
    m_ui.connect(m_matPreset, SignalKind::ValueChanged, [this](UiContext& u, NodeHandle n, const SignalArgs& a) {
        if (a.value >= 1.f) m_m.materialApplyPreset(u32(a.value) - 1u);
        u.setValue(n, 0.f);
        m_shell.sync();
    });
    nf = row_(page);
    m_matUsage = w::label(m_ui, nf, "", "material.usage");
    m_ui.setCss(m_matUsage, "flex-grow", "1");
    m_matUsers = w::button(m_ui, nf, "Select users", "material.select_users");
    w::setTooltip(m_ui, m_matUsers, "Select every entity whose mesh / SDF object uses this material");
    m_ui.connect(m_matUsers, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.materialSelectUsers(); m_shell.sync(); });

    NodeHandle f = field_(page, "Base colour");
    m_matColor = w::button(m_ui, f, "Pick...", "material.color");
    m_ui.connect(m_matColor, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.materialPickBaseColor(); m_shell.sync(); });
    f = field_(page, "Roughness");
    m_matRough = w::slider(m_ui, f, 0.f, 1.f, 0.5f, 0.01f, "material.roughness");
    m_ui.setCss(m_matRough, "flex-grow", "1");
    m_ui.connect(m_matRough, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.materialSetRoughness(a.value); m_shell.sync(); });
    f = field_(page, "Metallic");
    m_matMetal = w::slider(m_ui, f, 0.f, 1.f, 0.f, 0.01f, "material.metallic");
    m_ui.setCss(m_matMetal, "flex-grow", "1");
    m_ui.connect(m_matMetal, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.materialSetMetallic(a.value); m_shell.sync(); });
    f = field_(page, "Shader");
    m_matShader = w::comboBox(m_ui, f, "Standard (PBR)\nWood\nMetal\nConcrete", 0, "material.shader");
    m_ui.connect(m_matShader, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.materialSetShader(u32(a.value)); m_shell.sync(); });
    f = field_(page, "UV scale");
    m_matUv = w::spinBox(m_ui, f, 0.05f, 64.f, 1.f, 0.05f, "material.uv_scale"); // steps from the minimum: 0.05 keeps whole values
    m_ui.connect(m_matUv, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.materialSetUvScale(a.value); m_shell.sync(); });
    f = field_(page, "Texture");
    m_matTexture = w::label(m_ui, f, "(none)", "material.texture");
    m_ui.setCss(m_matTexture, "flex-grow", "1");
    const NodeHandle tb = w::button(m_ui, f, "Browse...", "material.texture_pick");
    m_ui.connect(tb, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.materialPickTexture(); m_shell.sync(); });
    const NodeHandle tc = w::button(m_ui, f, "Clear", "material.texture_clear");
    m_ui.connect(tc, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.materialSetTexture(""); m_shell.sync(); });
}
void ShellPanelPages::syncMaterial_() {
    if (!m_matList) return;
    const MaterialView v = m_m.materials();
    char b[256];
    std::snprintf(b, sizeof b, "%u|%.3f,%.3f,%.3f|%.3f|%.3f|%u|%.3f|%s|%d%d%u", v.current, double(v.srgb[0]), double(v.srgb[1]), double(v.srgb[2]),
                  double(v.state.roughness), double(v.state.metallic), v.shader, double(v.state.uvScale), v.state.baseColorTexture.c_str(),
                  v.hasSelection, v.selectionHasMaterial, v.selectionMaterial);
    const u32 users = m_m.materialUsage();
    std::string key = join(v.names) + "#" + b + "#" + std::to_string(users);
    if (key == m_matKey) return;
    m_matKey = std::move(key);
    m_ui.setText(m_matList, join(v.names));
    m_ui.setValue(m_matList, f32(v.current));
    m_ui.setValue(m_matRough, v.state.roughness);
    m_ui.setValue(m_matMetal, v.state.metallic);
    m_ui.setValue(m_matShader, f32(v.shader));
    m_ui.setValue(m_matUv, v.state.uvScale);
    m_ui.setText(m_matTexture, v.state.baseColorTexture.empty() ? std::string("(none)") : leaf(v.state.baseColorTexture));
    m_ui.setText(m_matSummary, v.summary);
    if (m_ui.focused() != m_matName) m_ui.setText(m_matName, v.current < v.names.size() ? v.names[v.current] : std::string());
    m_ui.setText(m_matUsage, users == 1 ? std::string("Used by 1 entity") : "Used by " + std::to_string(users) + " entities");
    m_ui.setDisabled(m_matUsers, users == 0);
    m_ui.setDisabled(m_matAssign, !v.hasSelection);
    std::snprintf(b, sizeof b, "#%02X%02X%02X", int(v.srgb[0] * 255.f + .5f), int(v.srgb[1] * 255.f + .5f), int(v.srgb[2] * 255.f + .5f));
    m_ui.setText(m_matColor, std::string(b));
    m_ui.setCss(m_matColor, "background", b);
    m_ui.setCss(m_matColor, "color", v.srgb[0] * .3f + v.srgb[1] * .59f + v.srgb[2] * .11f > .5f ? "#101014" : "#F0F0F4");
    std::vector<u8> rgba(96u * 96u * 4u, 0);
    if (m_m.materialPreview(96, 96, rgba.data())) {
        for (u32 i = 0; i < 96u * 96u; ++i) {
            const u8* p = &rgba[i * 4u];
            const f32 a = f32(p[3]) / 255.f;
            m_previewImg.pixels[i] = fuse::ui::packPremultiplied({f32(p[0]) / 255.f, f32(p[1]) / 255.f, f32(p[2]) / 255.f, a});
        }
        m_ui.invalidate(m_matPreview);
    }
}

// ---- SDF Sculpt -----------------------------------------------------------------------------------------------------
// ---- Navigation (Qt NavmeshPanel) ----------------------------------------------------------------------------------
void ShellPanelPages::buildNavigation_(NodeHandle page) {
    const NavigationView v = m_m.navigation();
    NodeHandle f = field_(page, "Cell size (m)");
    m_navCell = w::spinBox(m_ui, f, 0.05f, 50.f, v.cellSize, 0.25f, "nav.cell_size");
    m_ui.connect(m_navCell, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setNavCellSize(a.value); });
    f = field_(page, "Half extent");
    m_navExtent = w::spinBox(m_ui, f, 1.f, 512.f, f32(v.halfExtent), 1.f, "nav.extent");
    w::setTooltip(m_ui, m_navExtent, "Half size of the grid in cells, centred on the world origin");
    m_ui.connect(m_navExtent, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setNavHalfExtent(u32(a.value)); });
    const NodeHandle bake = w::button(m_ui, page, "Bake Navmesh", "nav.bake");
    m_ui.connect(bake, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.navBake(); m_shell.sync(); });
    m_navShow = w::checkBox(m_ui, page, "Show in viewport", v.show, "nav.show");
    m_ui.connect(m_navShow, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setNavShow(a.flag); m_shell.sync(); });
    static constexpr const char* kAxis[2] = {"x", "z"};
    f = field_(page, "From x/z");
    for (int i = 0; i < 2; ++i) m_navFrom[i] = w::spinBox(m_ui, f, -10000.f, 10000.f, i == 0 ? -5.f : 0.f, 0.5f, std::string("nav.from_") + kAxis[i]);
    f = field_(page, "To x/z");
    for (int i = 0; i < 2; ++i) m_navTo[i] = w::spinBox(m_ui, f, -10000.f, 10000.f, i == 0 ? 5.f : 0.f, 0.5f, std::string("nav.to_") + kAxis[i]);
    const NodeHandle path = w::button(m_ui, page, "Test Path", "nav.test_path");
    m_ui.connect(path, SignalKind::Clicked, [this](UiContext& u, NodeHandle, const SignalArgs&) {
        m_m.navTestPath(u.value(m_navFrom[0]), u.value(m_navFrom[1]), u.value(m_navTo[0]), u.value(m_navTo[1]));
        m_shell.sync();
    });
    m_navStatus = w::label(m_ui, page, v.status, "nav.status");
    m_ui.addClass(m_navStatus, "ed-dim");
}
void ShellPanelPages::syncNavigation_() {
    if (!m_navStatus) return;
    const NavigationView v = m_m.navigation();
    std::string key = v.status + (v.show ? "1" : "0");
    if (key == m_navKey) return;
    m_navKey = std::move(key);
    m_ui.setText(m_navStatus, v.status);
    m_ui.setChecked(m_navShow, v.show);
}

// ---- Terrain (Qt TerrainPanel) --------------------------------------------------------------------------------------
void ShellPanelPages::buildTerrain_(NodeHandle page) {
    const TerrainView v = m_m.terrain();
    w::label(m_ui, page, "New terrain");
    auto settings = [this](UiContext& u) {
        m_m.setTerrainSettings(u32(u.value(m_trCells)), u.value(m_trSize), u.value(m_trHeight), u32(u.value(m_trSeed)));
    };
    NodeHandle f = field_(page, "Cells");
    m_trCells = w::spinBox(m_ui, f, 2.f, 256.f, f32(v.cells), 1.f, "terrain.cells");
    f = field_(page, "Size (m)");
    m_trSize = w::spinBox(m_ui, f, 1.f, 4096.f, v.size, 1.f, "terrain.size");
    f = field_(page, "Height (m)");
    m_trHeight = w::spinBox(m_ui, f, 0.f, 512.f, v.height, 0.5f, "terrain.height");
    f = field_(page, "Seed");
    m_trSeed = w::spinBox(m_ui, f, 0.f, 99999.f, f32(v.seed), 1.f, "terrain.seed");
    for (NodeHandle n : {m_trCells, m_trSize, m_trHeight, m_trSeed})
        m_ui.connect(n, SignalKind::ValueChanged, [settings](UiContext& u, NodeHandle, const SignalArgs&) { settings(u); });
    const NodeHandle create = w::button(m_ui, page, "Create Terrain", "terrain.create");
    m_ui.connect(create, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.terrainCreate(); m_shell.sync(); });
    w::label(m_ui, page, "Sculpt brush");
    auto brush = [this](UiContext& u) { m_m.setTerrainBrush(u32(u.value(m_trMode)), u.value(m_trRadius), u.value(m_trStrength)); };
    f = field_(page, "Mode");
    m_trMode = w::comboBox(m_ui, f, "Raise\nLower\nFlatten", 0, "terrain.mode");
    f = field_(page, "Radius (m)");
    m_trRadius = w::spinBox(m_ui, f, 0.1f, 512.f, v.radius, 0.5f, "terrain.radius");
    f = field_(page, "Strength");
    m_trStrength = w::spinBox(m_ui, f, -64.f, 64.f, v.strength, 0.25f, "terrain.strength");
    for (NodeHandle n : {m_trMode, m_trRadius, m_trStrength})
        m_ui.connect(n, SignalKind::ValueChanged, [brush](UiContext& u, NodeHandle, const SignalArgs&) { brush(u); });
    m_trSculpt = w::checkBox(m_ui, page, "Sculpt in viewport", v.sculpt, "terrain.sculpt");
    w::setTooltip(m_ui, m_trSculpt, "Left-drag on a terrain in the viewport stamps the brush (one undo step per stroke)");
    m_ui.connect(m_trSculpt, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setTerrainSculpt(a.flag); m_shell.sync(); });
    const NodeHandle clear = w::button(m_ui, page, "Clear Sculpts", "terrain.clear");
    m_ui.connect(clear, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.terrainClear(); m_shell.sync(); });
    m_trStatus = w::label(m_ui, page, v.status, "terrain.status");
    m_ui.addClass(m_trStatus, "ed-dim");
}
void ShellPanelPages::syncTerrain_() {
    if (!m_trStatus) return;
    const TerrainView v = m_m.terrain();
    std::string key = v.status + (v.sculpt ? "1" : "0");
    if (key == m_trKey) return;
    m_trKey = std::move(key);
    m_ui.setText(m_trStatus, v.status);
    m_ui.setChecked(m_trSculpt, v.sculpt);
}

void ShellPanelPages::buildSculpt_(NodeHandle page) {
    m_scMode = w::checkBox(m_ui, page, "Sculpt in viewport", false, "sculpt.mode");
    w::setTooltip(m_ui, m_scMode, "Left-drag in the viewport stamps the brush (one undo step per stroke)");
    m_ui.connect(m_scMode, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setSculptMode(a.flag); m_shell.sync(); });
    NodeHandle f = field_(page, "Operation");
    m_scOp = w::comboBox(m_ui, f, "Add\nSubtract\nSmooth\nRoughen\nPaint", 0, "sculpt.op");
    m_ui.connect(m_scOp, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setBrushOp(u32(a.value)); m_shell.sync(); });
    f = field_(page, "Shape");
    m_scShape = w::comboBox(m_ui, f, "Sphere\nBox\nCapsule\nTorus\nCylinder", 0, "sculpt.shape");
    m_ui.connect(m_scShape, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setBrushShape(u32(a.value)); m_shell.sync(); });
    f = field_(page, "Material");
    m_scMaterial = w::comboBox(m_ui, f, "", 0, "sculpt.material");
    m_ui.connect(m_scMaterial, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setBrushMaterial(u32(a.value)); m_shell.sync(); });
    f = field_(page, "Radius");
    m_scRadius = w::spinBox(m_ui, f, 0.05f, 20.f, 1.f, 0.05f, "sculpt.radius");
    m_ui.connect(m_scRadius, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setBrushRadius(a.value); m_shell.sync(); });
    f = field_(page, "Strength");
    m_scStrength = w::slider(m_ui, f, 0.f, 1.f, 1.f, 0.01f, "sculpt.strength");
    m_ui.setCss(m_scStrength, "flex-grow", "1");
    m_ui.connect(m_scStrength, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setBrushStrength(a.value); m_shell.sync(); });
    f = field_(page, "Spacing");
    m_scSpacing = w::spinBox(m_ui, f, 0.05f, 4.f, 0.5f, 0.05f, "sculpt.spacing");
    m_ui.connect(m_scSpacing, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setBrushSpacing(a.value); m_shell.sync(); });
    f = field_(page, "Blend");
    m_scAlpha = w::slider(m_ui, f, 0.f, 1.f, 0.f, 0.01f, "sculpt.blend");
    m_ui.setCss(m_scAlpha, "flex-grow", "1");
    m_ui.connect(m_scAlpha, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setBrushAlpha(a.value); m_shell.sync(); });
    m_scSym = w::checkBox(m_ui, page, "Mirror X", false, "sculpt.symmetry");
    m_ui.connect(m_scSym, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.setBrushSymmetry(a.flag); m_shell.sync(); });
    m_scSummary = w::label(m_ui, page, "", "sculpt.summary");
    m_ui.addClass(m_scSummary, "ed-dim");
}
void ShellPanelPages::syncSculpt_() {
    if (!m_scMode) return;
    const SculptView v = m_m.sculpt();
    char b[192];
    std::snprintf(b, sizeof b, "%d|%u|%u|%u|%.3f|%.3f|%.3f|%.3f|%d|%u", v.active, unsigned(v.brush.op), unsigned(v.brush.shape), v.brush.materialId,
                  double(v.brush.radius), double(v.brush.strength), double(v.spacing), double(v.brush.blendAlpha), v.brush.symmetryX, v.strokes);
    std::string key = join(v.materials) + "#" + b + v.summary;
    if (key == m_scKey) return;
    m_scKey = std::move(key);
    m_ui.setChecked(m_scMode, v.active);
    m_ui.setValue(m_scOp, f32(v.brush.op));
    m_ui.setValue(m_scShape, f32(v.brush.shape));
    m_ui.setText(m_scMaterial, join(v.materials));
    m_ui.setValue(m_scMaterial, f32(v.brush.materialId));
    m_ui.setValue(m_scRadius, v.brush.radius);
    m_ui.setValue(m_scStrength, v.brush.strength);
    m_ui.setValue(m_scSpacing, v.spacing);
    m_ui.setValue(m_scAlpha, v.brush.blendAlpha);
    m_ui.setChecked(m_scSym, v.brush.symmetryX);
    m_ui.setText(m_scSummary, v.summary);
}

// ---- Profiler -------------------------------------------------------------------------------------------------------
void ShellPanelPages::buildProfiler_(NodeHandle page) {
    const NodeHandle r = row_(page);
    m_prPause = w::checkBox(m_ui, r, "Pause", false, "profiler.pause");
    m_ui.connect(m_prPause, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.profilerSetPaused(a.flag); m_shell.sync(); });
    const NodeHandle clr = w::button(m_ui, r, "Clear", "profiler.clear");
    m_ui.connect(clr, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.profilerClear(); m_shell.sync(); });
    m_prSummary = w::label(m_ui, r, "", "profiler.summary");
    m_prChart = w::chart(m_ui, page, w::ChartKind::Line, "profiler.chart");
    m_ui.setCss(m_prChart, "flex-grow", "1");
    m_ui.setCss(m_prChart, "min-height", "80px");
}
void ShellPanelPages::syncProfiler_() {
    if (!m_prChart) return;
    const ProfilerView v = m_m.profiler();
    if (v.frames == m_prSeen && !v.history.empty()) return;
    m_prSeen = v.frames;
    m_ui.setText(m_prSummary, v.summary);
    m_ui.setChecked(m_prPause, v.paused);
    const std::size_t n = std::min<std::size_t>(v.history.size(), 120);
    std::vector<f32> tail(v.history.end() - std::ptrdiff_t(n), v.history.end());
    if (tail.empty()) tail.push_back(0.f);
    w::chartSetSeries(m_ui, m_prChart, 0, "CPU ms", tail, Color::rgb(0x4FC3F7));
    const std::vector<f32> budget(tail.size(), 1000.f / std::max(1.f, v.targetFps));
    w::chartSetSeries(m_ui, m_prChart, 1, "budget", budget, Color::rgb(0xFF8A65));
}

// ---- Sequencer ------------------------------------------------------------------------------------------------------
void ShellPanelPages::buildSequencer_(NodeHandle page) {
    m_sqLink = w::checkBox(m_ui, page, "Link to Timeline", true, "sequencer.link");
    m_ui.connect(m_sqLink, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.sequencerSetLinked(a.flag); m_shell.sync(); });
    const NodeHandle f = field_(page, "Scrub");
    m_sqScrub = w::slider(m_ui, f, 0.f, 1.f, 0.f, 0.f, "sequencer.scrub");
    m_ui.setCss(m_sqScrub, "flex-grow", "1");
    m_ui.connect(m_sqScrub, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
        m_m.sequencerScrubUi(i32(a.value * f32(m_m.sequencer().rangeMs) + .5f));
        m_shell.sync();
    });
    m_sqPreview = w::label(m_ui, page, "", "sequencer.preview");
    m_ui.setCss(m_sqPreview, "white-space", "pre-wrap");
}
void ShellPanelPages::syncSequencer_() {
    if (!m_sqLink) return;
    const SequencerView v = m_m.sequencer();
    std::string key = std::to_string(v.linked) + "|" + std::to_string(v.scrubMs) + "|" + std::to_string(v.rangeMs) + "|" + v.preview;
    if (key == m_sqKey) return;
    m_sqKey = std::move(key);
    m_ui.setChecked(m_sqLink, v.linked);
    m_ui.setValue(m_sqScrub, v.rangeMs > 0 ? f32(v.scrubMs) / f32(v.rangeMs) : 0.f);
    m_ui.setText(m_sqPreview, v.preview.empty() ? std::string("t=" + std::to_string(v.scrubMs) + "ms") : v.preview);
}

// ---- Timeline -------------------------------------------------------------------------------------------------------
void ShellPanelPages::buildTimeline_(NodeHandle page) {
    const NodeHandle r = row_(page);
    m_tlPlay = w::button(m_ui, r, "Play", "timeline.play");
    m_ui.connect(m_tlPlay, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) {
        m_m.timelineSetPlaying(!m_m.timeline().playing);
        m_shell.sync();
    });
    const NodeHandle key = w::button(m_ui, r, "Key Selection", "timeline.key");
    m_ui.connect(key, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.timelineKeySelection(); m_shell.sync(); });
    const NodeHandle del = w::button(m_ui, r, "Delete Key", "timeline.delete_key");
    m_ui.connect(del, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.timelineDeleteKeysAtPlayhead(); m_shell.sync(); });
    w::label(m_ui, r, "Time (ms)");
    m_tlTime = w::spinBox(m_ui, r, 0.f, 600000.f, 0.f, 10.f, "timeline.time");
    m_ui.connect(m_tlTime, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.timelineSetPlayhead(i32(a.value)); m_shell.sync(); });
    w::label(m_ui, r, "Length");
    m_tlLength = w::spinBox(m_ui, r, 100.f, 600000.f, 5000.f, 100.f, "timeline.length");
    m_ui.connect(m_tlLength, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.timelineSetDuration(i32(a.value)); m_shell.sync(); });
    m_tlStatus = w::label(m_ui, r, "", "timeline.status");
    // navigation row (Qt TimelineCanvas): previous / next key, loop, snap, zoom
    const NodeHandle nav = row_(page);
    auto btn = [&](const char* text, const char* id, const char* tip, auto fn) {
        const NodeHandle b = w::button(m_ui, nav, text, id);
        w::setTooltip(m_ui, b, tip);
        m_ui.connect(b, SignalKind::Clicked, [this, fn](UiContext&, NodeHandle, const SignalArgs&) { fn(); m_shell.sync(); });
    };
    btn("|<", "timeline.prev_key", "Previous key ([)", [this] { m_m.timelineJumpToKey(-1); });
    btn(">|", "timeline.next_key", "Next key (])", [this] { m_m.timelineJumpToKey(+1); });
    m_tlLoop = w::checkBox(m_ui, nav, "Loop", true, "timeline.loop");
    w::setTooltip(m_ui, m_tlLoop, "Loop playback (L)");
    m_ui.connect(m_tlLoop, SignalKind::Toggled, [this](UiContext&, NodeHandle, const SignalArgs& a) { m_m.timelineSetLooping(a.flag); m_shell.sync(); });
    w::label(m_ui, nav, "Snap");
    m_tlSnap = w::comboBox(m_ui, nav, "Off\n24 fps\n30 fps\n60 fps", 2, "timeline.snap");
    m_ui.connect(m_tlSnap, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
        static constexpr u32 kFps[4] = {0, 24, 30, 60};
        m_m.timelineSetSnapFps(kFps[std::min(3u, u32(a.value))]);
        m_shell.sync();
    });
    btn("Zoom In", "timeline.zoom_in", "Zoom in (Ctrl+wheel)", [this] {
        m_m.timelineZoomBy(2.f, m_m.timeline().playheadMs);
    });
    btn("Zoom Out", "timeline.zoom_out", "Zoom out (Ctrl+wheel)", [this] {
        m_m.timelineZoomBy(0.5f, m_m.timeline().playheadMs);
    });
    btn("Fit", "timeline.fit", "Zoom to fit (F)", [this] { m_m.timelineZoomToFit(); });
    // key row (Qt TimelineWidget curve controls): pick a key on a lane, retime it, interpolation, tangents
    const NodeHandle keys = row_(page);
    w::label(m_ui, keys, "Lane");
    m_tlLane = w::comboBox(m_ui, keys, "(none)", 0, "timeline.lane");
    m_ui.setCss(m_tlLane, "min-width", "160px");
    auto keyBtn = [&](const char* text, const char* id, const char* tip, auto fn) {
        const NodeHandle b = w::button(m_ui, keys, text, id);
        w::setTooltip(m_ui, b, tip);
        m_ui.connect(b, SignalKind::Clicked, [this, fn](UiContext&, NodeHandle, const SignalArgs&) { fn(); m_shell.sync(); });
    };
    keyBtn("Select Key", "timeline.select_key", "Select the lane's key nearest the playhead", [this] {
        const TimelineView v = m_m.timeline();
        const i32 lane = i32(m_ui.value(m_tlLane));
        if (lane < 0 || u32(lane) >= v.lanes.size() || v.lanes[u32(lane)].keysMs.empty()) return;
        const auto& ks = v.lanes[u32(lane)].keysMs;
        i32 best = 0;
        for (i32 k = 1; k < i32(ks.size()); ++k)
            if (std::abs(ks[u32(k)] - v.playheadMs) < std::abs(ks[u32(best)] - v.playheadMs)) best = k;
        m_m.timelineSelectKey(lane, best);
    });
    keyBtn("Retime", "timeline.retime", "Move the selected key to the playhead (one undo step)", [this] {
        m_m.timelineMoveSelectedKey(m_m.timeline().playheadMs);
    });
    keyBtn("Delete", "timeline.delete_selected", "Delete the selected key", [this] { m_m.timelineDeleteSelectedKey(); });
    m_tlInterp = w::comboBox(m_ui, keys, "Linear\nConstant\nCubic (auto)\nCubic (flat)", 0, "timeline.interp");
    m_ui.connect(m_tlInterp, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
        static constexpr std::pair<u32, u32> kModes[4] = {{0, 0}, {1, 0}, {2, 0}, {2, 1}}; // KeyInterp, TangentMode
        const u32 i = std::min(3u, u32(a.value));
        m_m.timelineSetSelectedKeyInterp(kModes[i].first, kModes[i].second);
        m_shell.sync();
    });
    w::label(m_ui, keys, "In / out");
    m_tlTanIn = w::spinBox(m_ui, keys, -1000.f, 1000.f, 0.f, 0.5f, "timeline.tan_in");
    m_tlTanOut = w::spinBox(m_ui, keys, -1000.f, 1000.f, 0.f, 0.5f, "timeline.tan_out");
    for (NodeHandle n : {m_tlTanIn, m_tlTanOut})
        m_ui.connect(n, SignalKind::Submitted, [this](UiContext& u, NodeHandle, const SignalArgs&) {
            m_m.timelineSetSelectedKeyTangents(u.value(m_tlTanIn), u.value(m_tlTanOut));
            m_shell.sync();
        });
    m_tlKeyInfo = w::label(m_ui, keys, "", "timeline.key_info");
    m_ui.addClass(m_tlKeyInfo, "ed-dim");
    m_tlBox = w::panel(m_ui, page);
    m_ui.setCss(m_tlBox, "flex-grow", "1");
    // curve strip: the selected lane sampled over the clip
    m_tlCurve = w::chart(m_ui, page, w::ChartKind::Line, "timeline.curve");
    m_ui.setCss(m_tlCurve, "min-height", "60px");
}
void ShellPanelPages::syncTimeline_() {
    if (!m_tlBox) return;
    const TimelineView v = m_m.timeline();
    std::string tracks = std::to_string(v.durationMs);
    for (const auto& l : v.lanes) {
        tracks += "|" + l.label;
        for (i32 k : l.keysMs) tracks += "," + std::to_string(k);
    }
    m_ui.setText(m_tlPlay, v.playing ? "Pause" : "Play");
    m_ui.setValue(m_tlTime, f32(v.playheadMs));
    m_ui.setValue(m_tlLength, f32(v.durationMs));
    m_ui.setText(m_tlStatus, v.status);
    if (tracks != m_tlKey || !m_tlWidget) {
        m_tlKey = std::move(tracks);
        if (m_tlWidget) m_ui.destroy(m_tlWidget);
        m_tlWidget = w::timeline(m_ui, m_tlBox, f32(v.durationMs) / 1000.f, "timeline.track");
        m_ui.setCss(m_tlWidget, "flex-grow", "1");
        if (v.lanes.empty()) w::timelineAddTrack(m_ui, m_tlWidget, "(no keys: select an entity, Key Selection)");
        for (const auto& l : v.lanes) {
            const u32 t = w::timelineAddTrack(m_ui, m_tlWidget, l.label);
            for (i32 k : l.keysMs) {
                const f32 s = f32(k) / 1000.f;
                w::timelineAddClip(m_ui, m_tlWidget, t, std::max(0.f, s - 0.04f), s + 0.04f, "", Color::rgb(0xFFD54F));
            }
        }
        m_ui.connect(m_tlWidget, SignalKind::ValueChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
            m_m.timelineSetPlayhead(m_m.timelineSnapMs(i32(a.value * 1000.f + .5f)));
            m_shell.sync();
        });
        m_ui.connect(m_tlWidget, SignalKind::Event, [this](UiContext&, NodeHandle, const SignalArgs&) {
            const auto view = w::timelineView(m_ui, m_tlWidget); // Ctrl / Shift + wheel
            m_m.timelineSetView(i32(view.x * 1000.f + .5f), i32(view.y * 1000.f + .5f));
            m_shell.sync();
        });
    }
    {
        const u32 fps = m_m.timelineSnapFps();
        w::timelineSetSnap(m_ui, m_tlWidget, fps ? 1.f / f32(fps) : 0.01f);
        m_ui.setValue(m_tlSnap, f32(fps == 24 ? 1 : fps == 30 ? 2 : fps == 60 ? 3 : 0));
        m_ui.setChecked(m_tlLoop, m_m.timelineLooping());
        w::timelineSetView(m_ui, m_tlWidget, f32(m_m.timelineViewStartMs()) / 1000.f, f32(m_m.timelineViewSpanMs()) / 1000.f);
    }
    m_ui.setValue(m_tlWidget, f32(v.playheadMs) / 1000.f);
    m_ui.invalidate(m_tlWidget);
    // lanes, selected key, curve strip
    std::string lanes;
    for (const auto& l : v.lanes) lanes += (lanes.empty() ? "" : "\n") + l.label;
    if (lanes.empty()) lanes = "(none)";
    if (std::string_view(m_ui.text(m_tlLane)) != lanes) m_ui.setText(m_tlLane, lanes);
    std::string curveKey = m_tlKey + "#" + std::to_string(v.selTrack) + ":" + std::to_string(v.selKey) + ":" + std::to_string(v.selInterp) + ":" +
                           std::to_string(v.selIn) + ":" + std::to_string(v.selOut);
    if (curveKey == m_tlCurveKey) return;
    m_tlCurveKey = std::move(curveKey);
    if (v.selKey >= 0) {
        if (v.selTrack < i32(v.lanes.size())) m_ui.setValue(m_tlLane, f32(v.selTrack));
        const u32 mode = m_m.timelineSelectedKeyTangentMode();
        m_ui.setValue(m_tlInterp, f32(v.selInterp == 2 ? (mode == 1 ? 3 : 2) : v.selInterp));
        m_ui.setValue(m_tlTanIn, v.selIn);
        m_ui.setValue(m_tlTanOut, v.selOut);
        m_ui.setText(m_tlKeyInfo, "key " + std::to_string(v.selKey) + " @ " + std::to_string(m_m.timelineKeyTime(v.selTrack, v.selKey)) + " ms");
    } else {
        m_ui.setText(m_tlKeyInfo, "no key selected");
    }
    std::vector<f32> curve;
    if (v.selTrack >= 0)
        for (u32 i = 0; i <= 120; ++i) curve.push_back(m_m.timelineSampleSelected(i32(f32(v.durationMs) * f32(i) / 120.f)));
    if (curve.empty()) curve.push_back(0.f);
    w::chartSetSeries(m_ui, m_tlCurve, 0, "value", curve, Color::rgb(0x81C784));
}

// ---- Lighting (Qt LightingPanel) ------------------------------------------------------------------------------------
void ShellPanelPages::buildLighting_(NodeHandle page) {
    auto colourBtn = [&](NodeHandle parent, const char* id, const char* which) {
        const NodeHandle b = w::button(m_ui, parent, "#FFFFFF", id);
        m_ui.connect(b, SignalKind::Clicked, [this, which](UiContext&, NodeHandle, const SignalArgs&) { m_m.lightingPickColour(which); m_shell.sync(); });
        return b;
    };
    auto spin = [&](NodeHandle parent, f32 lo, f32 hi, f32 step, const char* id, auto fn) {
        const NodeHandle n = w::spinBox(m_ui, parent, lo, hi, 0.f, step, id);
        m_ui.setCss(n, "flex-grow", "1");
        m_ui.connect(n, SignalKind::ValueChanged, [this, fn](UiContext&, NodeHandle, const SignalArgs& a) {
            if (m_ltUpdating) return;
            fn(a.value);
            m_shell.sync();
        });
        return n;
    };
    w::label(m_ui, page, "Sun");
    NodeHandle r = row_(page);
    m_ltSunLabel = w::label(m_ui, r, "(no sun)", "lighting.sun");
    m_ui.setCss(m_ltSunLabel, "flex-grow", "1");
    m_ltAddSun = w::button(m_ui, r, "Add Sun", "lighting.add_sun");
    m_ui.connect(m_ltAddSun, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.lightingAddSun(); m_shell.sync(); });
    r = field_(page, "Elevation");
    m_ltElev = spin(r, -90.f, 90.f, 1.f, "lighting.elevation", [this](f32 v) { m_m.lightingSetSunAngles(v, m_ui.value(m_ltAzim)); });
    r = field_(page, "Azimuth");
    m_ltAzim = spin(r, -180.f, 180.f, 5.f, "lighting.azimuth", [this](f32 v) { m_m.lightingSetSunAngles(m_ui.value(m_ltElev), v); });
    r = field_(page, "Intensity");
    m_ltSunInt = spin(r, 0.f, 1.0e6f, 0.1f, "lighting.sun_intensity", [this](f32 v) { m_m.lightingSetSunIntensity(v); });
    r = field_(page, "Color");
    m_ltSunColor = colourBtn(r, "lighting.sun_color", "sun");
    w::label(m_ui, page, "Ambient");
    m_ltAddAmb = w::button(m_ui, page, "Add Ambient Light", "lighting.add_ambient");
    m_ui.connect(m_ltAddAmb, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.lightingAddAmbient(); m_shell.sync(); });
    r = field_(page, "Intensity");
    m_ltAmbInt = spin(r, 0.f, 1000.f, 0.05f, "lighting.ambient_intensity", [this](f32 v) { m_m.lightingSetAmbientIntensity(v); });
    r = field_(page, "Color");
    m_ltAmbColor = colourBtn(r, "lighting.ambient_color", "ambient");
    w::label(m_ui, page, "Fog");
    m_ltAddFog = w::button(m_ui, page, "Add Fog", "lighting.add_fog");
    m_ui.connect(m_ltAddFog, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.lightingAddFog(); m_shell.sync(); });
    r = field_(page, "Density");
    m_ltFogDensity = spin(r, 0.f, 1.f, 0.001f, "lighting.fog_density", [this](f32 v) { m_m.lightingSetFogDensity(v); });
    r = field_(page, "Visible dist.");
    m_ltFogDist = spin(r, 1.f, 1.0e6f, 10.f, "lighting.fog_distance", [this](f32 v) { m_m.lightingSetFogDistance(v); });
    r = field_(page, "Color");
    m_ltFogColor = colourBtn(r, "lighting.fog_color", "fog");
}

void ShellPanelPages::syncLighting_() {
    if (!m_ltSunLabel) return;
    const LightingView v = m_m.lighting();
    char b[512];
    std::snprintf(b, sizeof b, "%u.%u|%u|%u|%.3f,%.3f,%.4f|%.4f|%.5f,%.3f|%.3f%.3f%.3f|%.3f%.3f%.3f|%.3f%.3f%.3f", v.sun.index, v.sun.generation,
                  v.ambient.generation, v.fog.generation, double(v.elevation), double(v.azimuth), double(v.sunIntensity), double(v.ambientIntensity),
                  double(v.fogDensity), double(v.fogDistance), double(v.sunColor[0]), double(v.sunColor[1]), double(v.sunColor[2]),
                  double(v.ambientColor[0]), double(v.ambientColor[1]), double(v.ambientColor[2]), double(v.fogColor[0]), double(v.fogColor[1]),
                  double(v.fogColor[2]));
    if (m_ltKey == b) return;
    m_ltKey = b;
    m_ltUpdating = true;
    const bool sun = v.sun.valid(), amb = v.ambient.valid(), fog = v.fog.valid();
    m_ui.setText(m_ltSunLabel, v.sunLabel);
    m_ui.setCss(m_ltAddSun, "display", sun ? "none" : "flex");
    m_ui.setCss(m_ltAddAmb, "display", amb ? "none" : "flex");
    m_ui.setCss(m_ltAddFog, "display", fog ? "none" : "flex");
    for (NodeHandle n : {m_ltElev, m_ltAzim, m_ltSunInt, m_ltSunColor}) m_ui.setDisabled(n, !sun);
    for (NodeHandle n : {m_ltAmbInt, m_ltAmbColor}) m_ui.setDisabled(n, !amb);
    for (NodeHandle n : {m_ltFogDensity, m_ltFogDist, m_ltFogColor}) m_ui.setDisabled(n, !fog);
    const auto swatch = [&](NodeHandle n, const f32* c) {
        char hex[16];
        auto q = [](f32 x) { return int(std::clamp(x, 0.f, 1.f) * 255.f + .5f); };
        std::snprintf(hex, sizeof hex, "#%02X%02X%02X", q(c[0]), q(c[1]), q(c[2]));
        m_ui.setText(n, hex);
        m_ui.setCss(n, "background", hex);
        m_ui.setCss(n, "color", c[0] * .3f + c[1] * .59f + c[2] * .11f > .5f ? "#101014" : "#F0F0F4");
    };
    if (sun) {
        if (m_ui.focused() != m_ltElev) m_ui.setValue(m_ltElev, v.elevation);
        if (m_ui.focused() != m_ltAzim) m_ui.setValue(m_ltAzim, v.azimuth);
        if (m_ui.focused() != m_ltSunInt) m_ui.setValue(m_ltSunInt, v.sunIntensity);
        swatch(m_ltSunColor, v.sunColor);
    }
    if (amb) {
        if (m_ui.focused() != m_ltAmbInt) m_ui.setValue(m_ltAmbInt, v.ambientIntensity);
        swatch(m_ltAmbColor, v.ambientColor);
    }
    if (fog) {
        if (m_ui.focused() != m_ltFogDensity) m_ui.setValue(m_ltFogDensity, v.fogDensity);
        if (m_ui.focused() != m_ltFogDist) m_ui.setValue(m_ltFogDist, v.fogDistance);
        swatch(m_ltFogColor, v.fogColor);
    }
    m_ltUpdating = false;
}

bool ShellPanelPages::event(const fuse::ui::Event& e, NodeHandle target) {
    using fuse::ui::EventType;
    using fuse::ui::Key;
    // asset list / grid: right click selects nothing new but opens the context menu
    if (e.type == EventType::PointerUp && e.button == fuse::ui::MouseButton::Right && m_assetList &&
        (within_(target, m_assetList) || within_(target, m_assetGrid))) {
        if (m_ui.alive(m_ui.find("assets.context"))) return true;
        const NodeHandle menu = w::popupMenu(m_ui, m_assetInfo, kAssetMenu, e.pos, "assets.context");
        m_ui.connect(menu, SignalKind::Submitted, [this](UiContext&, NodeHandle, const SignalArgs& a) {
            const u32 line = u32(a.value); // the separator is line 4
            assetsMenuAction(line < 4 ? line : line - 1);
        });
        return true;
    }
    if (e.type != EventType::KeyDown || !m_tlWidget || !within_(target, m_tlBox)) return false;
    const i32 dur = m_m.timeline().durationMs;
    switch (e.key) {
    case Key::BracketLeft: m_m.timelineJumpToKey(-1); break;
    case Key::BracketRight: m_m.timelineJumpToKey(+1); break;
    case Key::Home: m_m.timelineSetPlayhead(0); break;
    case Key::End: m_m.timelineSetPlayhead(dur); break;
    case Key::L: m_m.timelineSetLooping(!m_m.timelineLooping()); break;
    case Key::F: m_m.timelineZoomToFit(); break;
    default: return false;
    }
    m_shell.sync();
    return true;
}

} // namespace fuse::editor::fui
