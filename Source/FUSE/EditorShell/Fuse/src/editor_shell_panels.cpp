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

// ---- Asset Browser --------------------------------------------------------------------------------------------------
void ShellPanelPages::buildAssets_(NodeHandle page) {
    const NodeHandle r = row_(page);
    const NodeHandle up = w::button(m_ui, r, "Up", "assets.up");
    m_ui.connect(up, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.assetsUp(); m_shell.sync(); });
    const NodeHandle rescan = w::button(m_ui, r, "Rescan", "assets.rescan");
    m_ui.connect(rescan, SignalKind::Clicked, [this](UiContext&, NodeHandle, const SignalArgs&) { m_m.assetsRescan(); m_shell.sync(); });
    m_assetFilter = w::lineEdit(m_ui, r, "", "assets.filter");
    m_ui.setCss(m_assetFilter, "flex-grow", "1");
    w::setTooltip(m_ui, m_assetFilter, "Filter assets by name");
    m_ui.connect(m_assetFilter, SignalKind::TextChanged, [this](UiContext&, NodeHandle, const SignalArgs& a) {
        m_m.assetsSetFilter(a.text);
        m_shell.sync();
    });
    m_assetPath = w::label(m_ui, page, "", "assets.path");
    m_ui.addClass(m_assetPath, "ed-dim");
    m_assetList = w::listView(m_ui, page, "", -1, "assets.list");
    m_ui.setCss(m_assetList, "flex-grow", "1");
    auto activate = [this](UiContext& u, NodeHandle, const SignalArgs&) {
        const i32 i = i32(u.value(m_assetList));
        if (i >= 0 && u32(i) < m_assetPaths.size()) m_m.assetsActivate(m_assetPaths[u32(i)]);
        m_shell.sync();
    };
    m_ui.connect(m_assetList, SignalKind::Submitted, activate);
    const NodeHandle open = w::button(m_ui, r, "Open", "assets.open");
    m_ui.connect(open, SignalKind::Clicked, activate);
    m_assetInfo = w::label(m_ui, page, "", "assets.info");
}
void ShellPanelPages::syncAssets_() {
    if (!m_assetList) return;
    const AssetBrowserView v = m_m.assets();
    std::string lines;
    m_assetPaths.clear();
    for (const auto& e : v.entries) {
        lines += (lines.empty() ? "" : "\n") + std::string(e.folder ? "[dir]  " : "") + e.name + (e.folder ? "" : "   (" + e.type + ")");
        m_assetPaths.push_back(e.path);
    }
    std::string key = v.root + "|" + v.directory + "|" + lines;
    if (key == m_assetKey) return;
    m_assetKey = std::move(key);
    m_ui.setText(m_assetPath, v.root + (v.directory.empty() ? "" : "/" + v.directory));
    m_ui.setText(m_assetList, lines);
    char b[64];
    std::snprintf(b, sizeof b, "%zu item(s)", v.entries.size());
    m_ui.setText(m_assetInfo, b);
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
    m_matUv = w::spinBox(m_ui, f, 0.05f, 64.f, 1.f, 0.25f, "material.uv_scale");
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
    std::string key = join(v.names) + "#" + b;
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
        m_m.sequencerScrub(i32(a.value * f32(m_m.sequencer().rangeMs) + .5f));
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
    m_tlBox = w::panel(m_ui, page);
    m_ui.setCss(m_tlBox, "flex-grow", "1");
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
            m_m.timelineSetPlayhead(i32(a.value * 1000.f + .5f));
            m_shell.sync();
        });
    }
    m_ui.setValue(m_tlWidget, f32(v.playheadMs) / 1000.f);
    m_ui.invalidate(m_tlWidget);
}

} // namespace fuse::editor::fui
