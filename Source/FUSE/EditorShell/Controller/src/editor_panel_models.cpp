#include <fuse/editor/editor_panel_models.hpp>

#include <fuse/ecs/components/transform.hpp>
#include <fuse/editor/asset_browser.hpp>
#include <fuse/editor/cinematics_seq_import.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/material_catalog.hpp>
#include <fuse/editor/viewport_scene_view.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace fuse::editor {

namespace {
namespace fs = std::filesystem;

f32 toLinear(f32 c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
f32 toSrgb(f32 c) {
    c = std::clamp(c, 0.f, 1.f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}
std::string fmt(const char* f, double a = 0, double b = 0, double c = 0, double d = 0, double e = 0, double g = 0, double h = 0) {
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b, c, d, e, g, h);
    return buf;
}
} // namespace

EditorPanels::EditorPanels(EditorController& controller)
    : m_c(controller), m_assets(std::make_unique<AssetBrowser>()), m_matPanel(std::make_unique<MaterialEditorPanel>()),
      m_sculpt(std::make_unique<SdfSculptPanel>()), m_profiler(std::make_unique<ProfilerPanel>()),
      m_seqImport(std::make_unique<CinematicsSeqImport>(controller.m_host)) {
    std::error_code ec;
    m_projectsRoot = fs::current_path(ec).string();
}

EditorPanels::~EditorPanels() = default;

void EditorPanels::withScene(const std::function<void(EditorHost&, const ViewportPanel&)>& fn) {
    std::lock_guard<std::mutex> lock(m_c.m_mutex);
    fn(m_c.m_host, m_c.m_viewport);
}

// ---- Project Hub (ProjectHubWidget) ---------------------------------------------------------------------------------

void EditorPanels::setProjectsRoot(std::string directory) {
    m_projectsRoot = std::move(directory);
    ++m_revision;
}

ProjectHubView EditorPanels::projectHub() const {
    ProjectHubView v;
    v.projectsRoot = m_projectsRoot;
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        v.hasProject = m_c.m_host.hasProject();
        if (v.hasProject) {
            v.name = m_c.m_host.projectManifest().name;
            v.directory = m_c.m_host.projectManifest().projectRoot;
        }
    }
    std::error_code ec;
    if (!m_projectsRoot.empty() && fs::is_directory(m_projectsRoot, ec)) {
        if (fs::exists(fs::path(m_projectsRoot) / "project.json", ec)) v.projects.push_back(fs::absolute(m_projectsRoot, ec).string());
        for (const fs::directory_entry& e : fs::directory_iterator(m_projectsRoot, ec))
            if (e.is_directory(ec) && fs::exists(e.path() / "project.json", ec)) v.projects.push_back(fs::absolute(e.path(), ec).string());
    }
    std::sort(v.projects.begin(), v.projects.end());
    return v;
}

bool EditorPanels::hubOpen(const std::string& directory) {
    const bool ok = m_c.openProject(directory);
    ++m_revision;
    return ok;
}

bool EditorPanels::hubNew(const std::string& directory, const std::string& name) {
    const bool ok = m_c.newProject(directory, name, kProjectEnable3D | kProjectEnable2D | kProjectEnableUI);
    ++m_revision;
    return ok;
}

// ---- Asset Browser (AssetBrowserWidget) -----------------------------------------------------------------------------

AssetBrowserView EditorPanels::assets() {
    std::string root;
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        if (m_c.m_host.hasProject()) root = m_c.m_host.projectManifest().projectRoot;
    }
    if (root.empty()) root = m_projectsRoot;
    if (root != m_assetsRoot) {
        m_assetsRoot = root;
        m_assets->init(root.c_str());
        m_assets->refresh();
    }
    AssetBrowserView v;
    v.root = m_assets->projectRoot();
    v.directory = m_assets->currentDirectory();
    v.filter = m_assets->searchFilter();
    if (const AssetBrowser::AssetEntry* s = m_assets->selectedEntry()) v.selected = s->path;
    for (const AssetBrowser::AssetEntry& e : m_assets->entries())
        v.entries.push_back({e.name, e.path, AssetBrowser::assetTypeLabel(e.type), e.type == AssetBrowser::AssetType::Folder});
    return v;
}

void EditorPanels::assetsSetFilter(std::string_view filter) {
    m_assets->setSearchFilter(std::string(filter).c_str());
    ++m_revision;
}

bool EditorPanels::assetsActivate(const std::string& path) {
    const AssetBrowser::ActivateResult r = m_assets->activate(path.c_str());
    ++m_revision;
    if (r == AssetBrowser::ActivateResult::OpenScene) return m_c.fileOpenPath(m_assets->absolutePath(path));
    return r != AssetBrowser::ActivateResult::None;
}

bool EditorPanels::assetsUp() {
    ++m_revision;
    return m_assets->navigateUp();
}

void EditorPanels::assetsRescan() {
    m_assets->refresh();
    ++m_revision;
}

// ---- Material Editor (MaterialEditorWidget) -------------------------------------------------------------------------

MaterialView EditorPanels::materials() {
    MaterialView v;
    std::lock_guard<std::mutex> lock(m_c.m_mutex);
    const MaterialCatalog& cat = m_c.m_host.materials();
    const ecs::EntityID sel = m_c.m_host.editorState().primarySelection;
    v.hasSelection = sel != ecs::EntityID::null();
    if (v.hasSelection)
        v.selectionHasMaterial = AssignMaterialCommand::materialOf(m_c.m_host.editorScene().registry(), sel, v.selectionMaterial);
    if (!cat.valid(m_material)) m_material = 0;
    for (u32 i = 0; i < cat.count(); ++i) v.names.push_back(cat.entry(i).name);
    v.current = m_material;
    if (cat.valid(m_material)) v.state = cat.entry(m_material).state;
    for (u32 i = 0; i < 3; ++i) v.srgb[i] = toSrgb(i == 0 ? v.state.baseColorR : i == 1 ? v.state.baseColorG : v.state.baseColorB);
    v.shader = v.state.procedural ? std::min<u32>(v.state.proceduralFnId, 3u) : 0u;
    v.summary = v.selectionHasMaterial ? "Selection uses material " + std::to_string(v.selectionMaterial)
                : v.hasSelection       ? std::string("Selection has no Mesh / SDF object")
                                       : std::string("No entity selected");
    m_matPanel->setEditState(v.state);
    return v;
}

bool EditorPanels::materialSelect(u32 id) {
    std::lock_guard<std::mutex> lock(m_c.m_mutex);
    if (!m_c.m_host.materials().valid(id)) return false;
    m_material = id;
    ++m_revision;
    return true;
}

u32 EditorPanels::materialNew() {
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        MaterialCatalog& cat = m_c.m_host.materials();
        MaterialCatalogEntry entry;
        entry.name = "Material " + std::to_string(cat.count());
        if (cat.valid(m_material)) entry.state = cat.entry(m_material).state;
        m_c.m_host.undoStack().execute(std::make_unique<AddMaterialCommand>(cat, std::move(entry)));
        m_material = cat.count() - 1u;
    }
    ++m_revision;
    m_c.refresh();
    return m_material;
}

bool EditorPanels::applyMaterial_(const MaterialEditState& s, const char* what) {
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        MaterialCatalog& cat = m_c.m_host.materials();
        if (!cat.valid(m_material)) return false;
        const MaterialEditState before = cat.entry(m_material).state;
        m_c.m_host.undoStack().execute(std::make_unique<MaterialEditCommand>(cat, m_material, before, s, std::string("Material ") + what));
    }
    ++m_revision;
    m_c.refresh();
    return true;
}

#define FUSE_MAT_EDIT(field_expr, what)                                                                              \
    MaterialEditState s;                                                                                             \
    {                                                                                                                \
        std::lock_guard<std::mutex> lock(m_c.m_mutex);                                                               \
        if (!m_c.m_host.materials().valid(m_material)) return false;                                                 \
        s = m_c.m_host.materials().entry(m_material).state;                                                          \
    }                                                                                                                \
    field_expr;                                                                                                      \
    return applyMaterial_(s, what)

bool EditorPanels::materialSetRoughness(f32 v) { FUSE_MAT_EDIT(s.roughness = std::clamp(v, 0.f, 1.f), "roughness"); }
bool EditorPanels::materialSetMetallic(f32 v) { FUSE_MAT_EDIT(s.metallic = std::clamp(v, 0.f, 1.f), "metallic"); }
bool EditorPanels::materialSetUvScale(f32 v) { FUSE_MAT_EDIT(s.uvScale = std::clamp(v, 0.01f, 100.f), "UV scale"); }
bool EditorPanels::materialSetTexture(const std::string& path) { FUSE_MAT_EDIT(s.baseColorTexture = path, "texture"); }
bool EditorPanels::materialSetShader(u32 fnId) {
    if (fnId > 3u) return false;
    FUSE_MAT_EDIT((s.procedural = fnId != 0u, s.proceduralFnId = fnId), "shader");
}
bool EditorPanels::materialSetBaseColorSrgb(f32 r, f32 g, f32 b) {
    FUSE_MAT_EDIT((s.baseColorR = toLinear(r), s.baseColorG = toLinear(g), s.baseColorB = toLinear(b)), "base colour");
}
#undef FUSE_MAT_EDIT

bool EditorPanels::materialPickBaseColor() {
    const MaterialView v = materials();
    const std::optional<Rgba> c = m_c.m_services.pickColour({v.srgb[0], v.srgb[1], v.srgb[2], 1.f});
    return c && materialSetBaseColorSrgb(c->r, c->g, c->b);
}

bool EditorPanels::materialPickTexture() {
    const std::optional<std::string> p = m_c.m_services.pickOpenFile("Base colour texture", "Images (*.png *.jpg *.jpeg *.tga *.bmp)", {});
    return p && materialSetTexture(*p);
}

bool EditorPanels::materialAssignToSelection() {
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        const ecs::EntityID sel = m_c.m_host.editorState().primarySelection;
        ecs::Registry& registry = m_c.m_host.editorScene().registry();
        u32 before = 0;
        if (sel == ecs::EntityID::null() || !AssignMaterialCommand::materialOf(registry, sel, before) || before == m_material) return false;
        m_c.m_host.undoStack().execute(std::make_unique<AssignMaterialCommand>(registry, sel, before, m_material));
    }
    ++m_revision;
    m_c.refresh();
    return true;
}

bool EditorPanels::materialPreview(u32 w, u32 h, u8* rgba) const { return m_matPanel->renderPreview(w, h, rgba); }

// ---- SDF Sculpt (SdfSculptWidget + the viewport's sculpt mode) -------------------------------------------------------

SculptView EditorPanels::sculpt() {
    SculptView v;
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        m_sculpt->sync(m_c.m_host.editorState(), m_c.m_host.editorScene());
        const MaterialCatalog& cat = m_c.m_host.materials();
        for (u32 i = 0; i < cat.count(); ++i) v.materials.push_back(std::to_string(i) + ": " + cat.entry(i).name);
        if (m_sculpt->brush().materialId >= cat.count() && cat.count() > 0) m_sculpt->setMaterialId(cat.count() - 1);
    }
    v.active = m_sculptMode;
    v.strokes = m_sculpt->strokeCount();
    v.brush = m_sculpt->brush();
    v.spacing = m_sculpt->strokeSpacing();
    v.summary = "Strokes: " + std::to_string(v.strokes) + " | " + (m_sculptMode ? "sculpting - left-drag in the viewport" : "idle");
    return v;
}

void EditorPanels::setSculptMode(bool on) {
    if (!on && m_sculptDragging) sculptPointer({ViewportPointerEvent::Kind::Up, ViewportPointerEvent::Button::Left, 0.f, 0.f});
    m_sculptMode = on;
    m_sculpt->endStroke();
    ++m_revision;
}
void EditorPanels::setBrushOp(u32 op) { m_sculpt->setBrushOperation(static_cast<SdfSculptPanel::BrushOp>(std::min(op, 4u))); ++m_revision; }
void EditorPanels::setBrushShape(u32 shape) { m_sculpt->setBrushShape(static_cast<ecs::SDFPrimitive>(std::min(shape, 4u))); ++m_revision; }
void EditorPanels::setBrushMaterial(u32 id) { m_sculpt->setMaterialId(id); ++m_revision; }
void EditorPanels::setBrushRadius(f32 v) { m_sculpt->setBrushRadius(v); ++m_revision; }
void EditorPanels::setBrushStrength(f32 v) { m_sculpt->setBrushStrength(v); ++m_revision; }
void EditorPanels::setBrushSpacing(f32 v) { m_sculpt->setStrokeSpacing(v); ++m_revision; }
void EditorPanels::setBrushAlpha(f32 v) { m_sculpt->setBlendAlpha(v); ++m_revision; }
void EditorPanels::setBrushSymmetry(bool on) { m_sculpt->setSymmetryX(on); ++m_revision; }

bool EditorPanels::sculptAt_(f32 x, f32 y) {
    // same hit rule as ViewportPlaceholderWidget::sculptHit_: the picked surface, else the ground plane
    const ViewportRay ray = m_c.m_viewport.screenRay(x, y);
    ecs::vec3 point{}, normal{-ray.direction.x, -ray.direction.y, -ray.direction.z, 0.f};
    const ViewportPickResult hit = m_c.m_sceneView->pickRay(m_c.m_host.editorScene(), ray);
    if (hit.hit) {
        point = hit.point;
    } else {
        if (std::fabs(ray.direction.y) < 1e-5f) return false;
        const f32 t = -ray.origin.y / ray.direction.y;
        if (t <= 0.f) return false;
        point = ecs::vec3{ray.origin.x + ray.direction.x * t, 0.f, ray.origin.z + ray.direction.z * t, 1.f};
        normal = ecs::vec3{0.f, 1.f, 0.f, 0.f};
    }
    const bool changed = m_sculpt->applyBrushStroke(point, normal, m_c.m_host.editorScene(), m_c.m_host.undoStack());
    return changed;
}

bool EditorPanels::sculptPointer(const ViewportPointerEvent& e) {
    using K = ViewportPointerEvent::Kind;
    if (!m_sculptMode || e.button != ViewportPointerEvent::Button::Left) return false;
    if (e.kind == K::Move && !m_sculptDragging) return false; // hover: let the controller see it
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        if (e.kind == K::Down) {
            m_c.m_host.undoStack().beginMacro("Sculpt stroke");
            m_sculptDragging = true;
            m_sculpt->sync(m_c.m_host.editorState(), m_c.m_host.editorScene()); // needs a selected SDF object (Qt rule)
            m_sculpt->endStroke(); // the first sample of a drag always lands
            sculptAt_(e.x, e.y);
        } else if (e.kind == K::Move) {
            sculptAt_(e.x, e.y);
        } else if (m_sculptDragging) {
            m_sculptDragging = false;
            m_sculpt->endStroke();
            m_c.m_host.undoStack().endMacro();
        }
    }
    ++m_revision;
    if (e.kind == K::Up) m_c.refresh();
    return true;
}

// ---- Profiler (ProfilerWidget) --------------------------------------------------------------------------------------

void EditorPanels::profilerSample(f32 uiMs) {
    if (m_profiler->isPaused()) return;
    ProfilerPanel::FrameProfileData d;
    d.cpuMs = m_c.m_lastTickMs.load();
    d.postprocessMs = uiMs; // the Qt summary shows the editor UI time in this slot ("Editor UI: .. ms")
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        u32 n = 0;
        m_c.m_host.editorScene().registry().each<ecs::Transform>([&](ecs::EntityID, ecs::Transform&) { ++n; });
        d.sdfObjects = n;
    }
    m_profiler->pushFrameData(d);
    ++m_revision;
}

ProfilerView EditorPanels::profiler() const {
    ProfilerView v;
    v.stats = m_profiler->cpuStats();
    v.latest = m_profiler->latestFrame();
    m_profiler->copyCpuHistory(v.history);
    v.frames = m_profiler->frameCount();
    v.paused = m_profiler->isPaused();
    v.targetFps = m_profiler->targetFps();
    v.summary = fmt("Frames: %.0f   CPU: %.3f ms   min %.2f / avg %.2f / max %.2f ms   ~%.0f fps   over budget: %.0f", v.frames,
                    v.latest.cpuMs, v.stats.minMs, v.stats.avgMs, v.stats.maxMs, v.stats.avgFps, v.stats.overBudget) +
                fmt("   Editor UI: %.3f ms   Target: %.0f fps", v.latest.postprocessMs, v.targetFps);
    return v;
}

void EditorPanels::profilerSetPaused(bool paused) { m_profiler->setPaused(paused); ++m_revision; }
void EditorPanels::profilerClear() { m_profiler->clearHistory(); ++m_revision; }

// ---- Timeline (TimelineWidget) --------------------------------------------------------------------------------------

TimelineView EditorPanels::timeline() const {
    TimelineView v;
    std::lock_guard<std::mutex> lock(m_c.m_mutex);
    const EditorTimeline& tl = m_c.m_host.timeline();
    v.durationMs = tl.durationMs();
    v.playheadMs = tl.playheadMs();
    v.keyCount = tl.keyCount();
    v.playing = m_timelinePlaying;
    for (const cinematics::PropertyTrack& t : tl.tracks()) {
        TimelineLaneView lane;
        lane.label = "#" + std::to_string(EditorTimeline::entityOf(t)) + " " + t.property_path();
        for (const cinematics::PropertyKeyframe& k : t.keyframes()) lane.keysMs.push_back(k.time_ms);
        v.lanes.push_back(std::move(lane));
    }
    v.status = std::to_string(v.lanes.size()) + " lanes - " + std::to_string(v.keyCount) + " keys";
    return v;
}

bool EditorPanels::timelineKeySelection() {
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        const ecs::EntityID e = m_c.m_host.editorState().primarySelection;
        if (e == ecs::EntityID::null()) return false;
        EditorTimeline& tl = m_c.m_host.timeline();
        EditorTimeline::Tracks before = tl.tracks();
        ok = tl.keyTransform(m_c.m_host.editorScene().registry(), e, tl.playheadMs());
        if (ok) {
            m_c.m_host.undoStack().execute(std::make_unique<TimelineEditCommand>(tl, std::move(before), tl.tracks(), "Key selection"));
        }
    }
    ++m_revision;
    m_c.refresh();
    return ok;
}

bool EditorPanels::timelineDeleteKeysAtPlayhead() {
    bool any = false;
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        EditorTimeline& tl = m_c.m_host.timeline();
        EditorTimeline::Tracks before = tl.tracks();
        for (int t = 0; t < int(tl.tracks().size()); ++t) {
            const auto& keys = tl.tracks()[usize(t)].keyframes();
            for (int k = int(keys.size()) - 1; k >= 0; --k)
                if (keys[usize(k)].time_ms == tl.playheadMs()) any = tl.deleteKey(t, k) || any;
        }
        if (any) {
            m_c.m_host.undoStack().execute(std::make_unique<TimelineEditCommand>(tl, std::move(before), tl.tracks(), "Delete key"));
        }
    }
    ++m_revision;
    m_c.refresh();
    return any;
}

void EditorPanels::timelineSetPlayhead(i32 ms) {
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        EditorTimeline& tl = m_c.m_host.timeline();
        tl.setPlayheadMs(ms);
        tl.apply(m_c.m_host.editorScene().registry(), tl.playheadMs());
        ms = tl.playheadMs();
    }
    if (m_seq.linked) sequencerScrub(ms);
    ++m_revision;
}

void EditorPanels::timelineSetDuration(i32 ms) {
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        m_c.m_host.timeline().setDurationMs(ms);
        m_seq.rangeMs = m_c.m_host.timeline().durationMs();
    }
    ++m_revision;
}

void EditorPanels::timelineSetPlaying(bool on) {
    m_timelinePlaying = on;
    m_timelineAccum = 0.f;
    if (on) {
        bool atEnd = false;
        {
            std::lock_guard<std::mutex> lock(m_c.m_mutex);
            atEnd = m_c.m_host.timeline().playheadMs() >= m_c.m_host.timeline().durationMs();
        }
        if (atEnd) timelineSetPlayhead(0);
    }
    ++m_revision;
}

bool EditorPanels::timelineAdvance(f32 dt) {
    if (!m_timelinePlaying) return false;
    m_timelineAccum += dt * 1000.f;
    const i32 step = i32(m_timelineAccum);
    if (step <= 0) return false;
    m_timelineAccum -= f32(step);
    i32 next = 0, dur = 1;
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        next = m_c.m_host.timeline().playheadMs() + step;
        dur = m_c.m_host.timeline().durationMs();
    }
    if (next > dur) next = 0; // loop
    timelineSetPlayhead(next);
    return true;
}

// ---- Sequencer (SeqPreviewPaneWidget) -------------------------------------------------------------------------------

void EditorPanels::sequencerScrub(i32 ms) {
    std::lock_guard<std::mutex> lock(m_c.m_mutex);
    if (m_seqImport->importCount() == 0u) m_seqImport->wirePreviewPaneToHost(0);
    if (ms > m_seq.rangeMs) m_seq.rangeMs = ms;
    m_seq.scrubMs = ms;
    if (m_seqImport->postScrubPreviewAtMs(ms)) ++m_seq.scrubPosts;
    const SeqPreviewPaneSample s = m_seqImport->previewPaneSampleAtMs(ms);
    if (s.valid) {
        char buf[320];
        std::snprintf(buf, sizeof buf, "t=%dms sprite=(%.1f,%.1f) mount=%s bone=%s yaw=%.1f pitch=%.1f roll=%.1f", int(s.time_ms), s.sprite_x,
                      s.sprite_y, s.mount_point.c_str(), s.bone_name.c_str(), s.mount_yaw_deg, s.mount_pitch_deg, s.mount_roll_deg);
        m_seq.preview = buf;
    }
    m_seqImport->postViewportSeqPreviewAtMs(ms);
    ++m_revision;
}

} // namespace fuse::editor
