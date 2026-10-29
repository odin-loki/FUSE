#include <fuse/editor/editor_panel_models.hpp>

#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/navigation/grid_navmesh.hpp>
#include <fuse/editor/asset_browser.hpp>
#include <fuse/editor/cinematics_seq_import.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/material_catalog.hpp>
#include <fuse/editor/terrain_builder.hpp>
#include <fuse/editor/undo_stack.hpp>
#include <fuse/editor/viewport_scene_view.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
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
bool EditorPanels::hubNew(const std::string& directory, const std::string& name, u32 dimensionFlags,
                          const project::ModuleSettings& modules) {
    const bool ok = m_c.newProject(directory, name, dimensionFlags, modules);
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
    v.typeFilter = m_assetTypeFilter;
    v.grid = m_assetsGrid;
    v.crumbs.push_back("Project");
    {
        std::string part;
        for (const char ch : v.directory + "/") {
            if (ch == '/' || ch == '\\') { if (!part.empty()) v.crumbs.push_back(part); part.clear(); }
            else part += ch;
        }
    }
    const AssetBrowser::AssetEntry* sel = m_assets->selectedEntry();
    if (sel) v.selected = sel->path;
    for (const AssetBrowser::AssetEntry& e : m_assets->entries()) {
        const bool folder = e.type == AssetBrowser::AssetType::Folder;
        if (!folder && m_assetTypeFilter != 0u && u32(e.type) != m_assetTypeFilter) continue;
        AssetEntryView ev{e.name, e.path, AssetBrowser::assetTypeLabel(e.type), folder, e.name};
        std::error_code ec;
        const fs::path abs = fs::u8path(m_assets->absolutePath(e.path));
        if (!folder) {
            const auto sz = fs::file_size(abs, ec);
            if (!ec) ev.tooltip += sz < 1024 ? "\n" + std::to_string(sz) + " B" : fmt("\n%.1f KB", double(sz) / 1024.0);
        }
        const auto wt = fs::last_write_time(abs, ec);
        if (!ec) {
            const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                wt - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            const std::time_t t = std::chrono::system_clock::to_time_t(sys);
            char buf[32];
            if (std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", std::localtime(&t)))
                ev.tooltip += std::string(folder ? "\n" : " · ") + "modified " + buf;
        }
        v.folders += folder;
        v.entries.push_back(std::move(ev));
    }
    v.status = std::to_string(v.entries.size()) + " items (" + std::to_string(v.folders) + " folders)";
    if (sel) v.status += std::string(" · ") + sel->name + " (" + AssetBrowser::assetTypeLabel(sel->type) + ")";
    return v;
}

const std::vector<std::string>& EditorPanels::assetTypeFilterLabels() {
    static const std::vector<std::string> labels = [] {
        std::vector<std::string> l{"All types"};
        for (u32 t = u32(AssetBrowser::AssetType::Mesh); t <= u32(AssetBrowser::AssetType::Unknown); ++t)
            l.push_back(AssetBrowser::assetTypeLabel(AssetBrowser::AssetType(t)));
        return l;
    }();
    return labels;
}

void EditorPanels::assetsSetTypeFilter(u32 index) {
    m_assetTypeFilter = std::min<u32>(index, u32(AssetBrowser::AssetType::Unknown));
    ++m_revision;
}
void EditorPanels::assetsSetGrid(bool on) { m_assetsGrid = on; ++m_revision; }

bool EditorPanels::assetsNavigateTo(const std::string& relDir) {
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(m_assets->absolutePath(relDir)), ec)) return false;
    m_assets->setSearchFilter("");
    m_assets->setCurrentDirectory(relDir.c_str());
    m_assets->refresh();
    ++m_revision;
    return true;
}

bool EditorPanels::assetsSelect(const std::string& path) {
    ++m_revision;
    return m_assets->selectEntry(path.c_str());
}

namespace {
bool validAssetName(const std::string& n) {
    return !n.empty() && n != "." && n != ".." && n.find_first_of("/\\") == std::string::npos;
}
} // namespace

bool EditorPanels::assetsCreateFolder(const std::string& name) {
    if (!validAssetName(name) || m_assets->projectRoot().empty()) return false;
    const fs::path dir = fs::u8path(m_assets->absolutePath(m_assets->currentDirectory())) / fs::u8path(name);
    std::error_code ec;
    if (fs::exists(dir, ec) || !fs::create_directory(dir, ec) || ec) return false;
    m_assets->refresh();
    ++m_revision;
    return true;
}

bool EditorPanels::assetsRename(const std::string& path, const std::string& newName) {
    if (!validAssetName(newName)) return false;
    const fs::path from = fs::u8path(m_assets->absolutePath(path));
    const fs::path to = from.parent_path() / fs::u8path(newName);
    std::error_code ec;
    if (!fs::exists(from, ec) || fs::exists(to, ec)) return false;
    fs::rename(from, to, ec);
    if (ec) return false;
    m_assets->refresh();
    const std::string dir = m_assets->currentDirectory();
    m_assets->selectEntry((dir.empty() ? newName : dir + "/" + newName).c_str());
    ++m_revision;
    return true;
}

std::string EditorPanels::assetsAbsolutePath(const std::string& path) const { return m_assets->absolutePath(path); }

void EditorPanels::assetsSetFilter(std::string_view filter) {
    m_assets->setSearchFilter(std::string(filter).c_str());
    ++m_revision;
}

bool EditorPanels::assetsActivate(const std::string& path) {
    const AssetBrowser::ActivateResult r = m_assets->activate(path.c_str());
    if (r == AssetBrowser::ActivateResult::EnteredFolder && !m_assets->searchFilter().empty()) {
        m_assets->setSearchFilter(""); // Qt: entering a folder clears the filter
        m_assets->refresh();
    }
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
    if (next > dur) {
        if (m_tlLoop) next = dur > 0 ? next % dur : 0;
        else { next = dur; m_timelinePlaying = false; }
    }
    timelineSetPlayhead(next);
    return true;
}

i32 EditorPanels::timelineViewSpanMs() const {
    std::lock_guard<std::mutex> lock(m_c.m_mutex);
    const i32 dur = std::max(1, m_c.m_host.timeline().durationMs());
    return m_tlViewSpan > 0 ? std::min(m_tlViewSpan, dur) : dur;
}

void EditorPanels::timelineSetView(i32 startMs, i32 spanMs) {
    i32 dur = 1;
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        dur = std::max(1, m_c.m_host.timeline().durationMs());
    }
    spanMs = std::clamp(spanMs, std::min(50, dur), dur);
    m_tlViewSpan = spanMs >= dur ? 0 : spanMs;
    m_tlViewStart = m_tlViewSpan == 0 ? 0 : std::clamp(startMs, 0, dur - spanMs);
    ++m_revision;
}

void EditorPanels::timelineZoomBy(f32 factor, i32 anchorMs) {
    if (factor <= 0.f) return;
    const i32 span = timelineViewSpanMs(), start = m_tlViewStart;
    i32 dur = 1;
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        dur = std::max(1, m_c.m_host.timeline().durationMs());
    }
    const i32 ns = std::clamp(i32(std::lround(f32(span) / factor)), std::min(50, dur), dur);
    const f32 rel = f32(anchorMs - start) / f32(std::max(1, span));
    timelineSetView(anchorMs - i32(std::lround(rel * f32(ns))), ns);
    m_tlViewSpan = ns >= dur ? 0 : ns; // keep an explicit span even when the start clamps
}

void EditorPanels::timelinePanBy(i32 deltaMs) { timelineSetView(m_tlViewStart + deltaMs, timelineViewSpanMs()); }
void EditorPanels::timelineZoomToFit() { m_tlViewSpan = 0; m_tlViewStart = 0; ++m_revision; }

i32 EditorPanels::timelineSnapMs(i32 ms) const {
    i32 dur = 1;
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        dur = m_c.m_host.timeline().durationMs();
    }
    i32 out = ms;
    if (m_tlSnapFps > 0) {
        const double frame = 1000.0 / double(m_tlSnapFps);
        out = i32(std::lround(std::round(double(ms) / frame) * frame));
    } else {
        out = i32(std::lround(double(ms) / 10.0)) * 10;
    }
    return std::clamp(out, 0, std::max(0, dur));
}

bool EditorPanels::timelineJumpToKey(int dir) {
    i32 ph = 0, best = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        const EditorTimeline& tl = m_c.m_host.timeline();
        ph = tl.playheadMs();
        for (const cinematics::PropertyTrack& t : tl.tracks())
            for (const cinematics::PropertyKeyframe& k : t.keyframes()) {
                const i32 km = k.time_ms;
                if (dir > 0 ? km > ph && (!found || km < best) : km < ph && (!found || km > best)) { best = km; found = true; }
            }
    }
    if (!found) return false;
    timelineSetPlayhead(best);
    const i32 span = timelineViewSpanMs();
    if (m_tlViewSpan > 0 && (best < m_tlViewStart || best > m_tlViewStart + span)) timelineSetView(best - span / 2, span);
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

// ---- Navigation (Qt NavmeshPanel) -----------------------------------------------------------------------------------
namespace {
navigation::GridNavmesh& navMesh(std::shared_ptr<void>& p) {
    if (!p) p = std::make_shared<navigation::GridNavmesh>();
    return *static_cast<navigation::GridNavmesh*>(p.get());
}
} // namespace

bool EditorPanels::navBake() {
    const u32 n = m_nav.halfExtent * 2u;
    const f32 cs = m_nav.cellSize;
    const f32 o = -f32(m_nav.halfExtent) * cs;
    struct Box { f32 x0, x1, z0, z1; };
    std::vector<Box> boxes;
    withScene([&](EditorHost& host, const ViewportPanel&) {
        ecs::Registry& reg = host.editorScene().registry();
        ecs::TransformSystem::update(reg);
        reg.each<ecs::Transform, ecs::Collider>([&](ecs::EntityID, ecs::Transform& t, ecs::Collider& c) {
            if (c.is_trigger) return;
            f32 h[3] = {0.f, 0.f, 0.f};
            switch (c.shape) {
            case ecs::Collider::Sphere: h[0] = h[1] = h[2] = c.params.x; break;
            case ecs::Collider::Box: h[0] = c.params.x; h[1] = c.params.y; h[2] = c.params.z; break;
            case ecs::Collider::Capsule: h[0] = h[2] = c.params.x; h[1] = c.params.x + c.params.y; break;
            default: return;
            }
            const auto& m = t.local_to_world.data;
            const f32 ex = std::fabs(m[0]) * h[0] + std::fabs(m[4]) * h[1] + std::fabs(m[8]) * h[2];
            const f32 ez = std::fabs(m[2]) * h[0] + std::fabs(m[6]) * h[1] + std::fabs(m[10]) * h[2];
            const f32 ey = std::fabs(m[1]) * h[0] + std::fabs(m[5]) * h[1] + std::fabs(m[9]) * h[2];
            if (m[13] + ey <= 0.05f) return; // ground slabs are walkable
            boxes.push_back({m[12] - ex, m[12] + ex, m[14] - ez, m[14] + ez});
        });
    });
    m_navBlocked.assign(usize(n) * n, 0u);
    m_navPath.clear();
    u32 blocked = 0;
    const bool ok = n > 0 && cs > 0.f && navMesh(m_navMesh).build(n, n, cs, [&](u32 x, u32 y) {
        const f32 cx = o + (f32(x) + 0.5f) * cs, cz = o + (f32(y) + 0.5f) * cs;
        for (const Box& b : boxes)
            if (cx >= b.x0 && cx <= b.x1 && cz >= b.z0 && cz <= b.z1) {
                m_navBlocked[usize(y) * n + x] = 1u;
                ++blocked;
                return true;
            }
        return false;
    });
    m_nav.built = ok;
    m_nav.cells = ok ? n : 0u;
    m_nav.blocked = blocked;
    m_nav.colliders = u32(boxes.size());
    m_nav.waypoints = 0;
    char b[160];
    if (ok) std::snprintf(b, sizeof b, "Baked %u x %u cells (%g m): %u blocked by %zu collider(s)", n, n, double(cs), blocked, boxes.size());
    else std::snprintf(b, sizeof b, "Bake failed: invalid grid");
    m_nav.status = b;
    ++m_revision;
    return ok;
}

u32 EditorPanels::navTestPath(f32 ax, f32 az, f32 bx, f32 bz) {
    m_navPath.clear();
    m_nav.waypoints = 0;
    ++m_revision;
    if (!m_nav.built) {
        m_nav.status = "Bake the navmesh first";
        return 0;
    }
    navigation::GridNavmesh& mesh = navMesh(m_navMesh);
    const f32 o = -f32(m_nav.cells / 2u) * mesh.cell_size();
    navigation::Cell a, c;
    if (!mesh.cell_at(ax - o, az - o, a) || !mesh.cell_at(bx - o, bz - o, c)) {
        m_nav.status = "Path endpoints are outside the navmesh";
        return 0;
    }
    const auto points = mesh.find_path(a, c);
    for (const auto& p : points) {
        m_navPath.push_back(o + p.x);
        m_navPath.push_back(o + p.y);
    }
    m_nav.waypoints = u32(points.size());
    char b[96];
    if (points.empty()) std::snprintf(b, sizeof b, "No path");
    else std::snprintf(b, sizeof b, "Path: %zu waypoints, %g m", points.size(), double(f32(points.size() - 1) * mesh.cell_size()));
    m_nav.status = b;
    return m_nav.waypoints;
}

std::vector<f32> EditorPanels::navSegments() const {
    std::vector<f32> segs;
    if (!m_nav.show || !m_nav.built || !m_navMesh) return segs;
    const auto& mesh = *static_cast<const navigation::GridNavmesh*>(m_navMesh.get());
    const u32 n = m_nav.cells;
    const f32 cs = mesh.cell_size(), o = -f32(n / 2u) * cs, y = 0.02f;
    const auto seg = [&](f32 ax, f32 az, f32 bx, f32 bz, f32 yy) { segs.insert(segs.end(), {ax, yy, az, bx, yy, bz}); };
    const f32 e = o + f32(n) * cs;
    seg(o, o, e, o, y);
    seg(e, o, e, e, y);
    seg(e, e, o, e, y);
    seg(o, e, o, o, y);
    for (u32 cy = 0; cy < n; ++cy)
        for (u32 cx = 0; cx < n; ++cx) {
            if (m_navBlocked[usize(cy) * n + cx] == 0u) continue;
            const f32 x0 = o + f32(cx) * cs, z0 = o + f32(cy) * cs;
            seg(x0, z0, x0 + cs, z0 + cs, y);
            seg(x0 + cs, z0, x0, z0 + cs, y);
        }
    for (usize i = 0; i + 3 < m_navPath.size(); i += 2) seg(m_navPath[i], m_navPath[i + 1], m_navPath[i + 2], m_navPath[i + 3], 0.1f);
    return segs;
}

// ---- Terrain (Qt TerrainPanel) --------------------------------------------------------------------------------------
namespace {
class TerrainEditCommand final : public UndoCommand {
public:
    TerrainEditCommand(ecs::Registry& r, ecs::EntityID e, const ecs::Terrain& before, const ecs::Terrain& after, std::string what)
        : m_registry(r), m_entity(e), m_before(std::make_unique<ecs::Terrain>(before)), m_after(std::make_unique<ecs::Terrain>(after)),
          m_what(std::move(what)) {}
    void execute() override { apply(*m_after); }
    void undo() override { apply(*m_before); }
    std::string description() const override { return m_what; }

private:
    void apply(const ecs::Terrain& t) {
        if (ecs::Terrain* cur = m_registry.get<ecs::Terrain>(m_entity)) *cur = t;
    }
    ecs::Registry& m_registry;
    ecs::EntityID m_entity;
    std::unique_ptr<ecs::Terrain> m_before, m_after; // ~6 KiB each (256 stamps)
    std::string m_what;
};

/// Inverse of an affine column-major 4x4; false when singular (Qt terrain_panel invertAffine).
bool invertAffine(const f32* m, f32* out) {
    const f32 a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const f32 A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const f32 det = a * A + b * B + c * C;
    if (std::fabs(det) < 1e-12f) return false;
    const f32 s = 1.f / det;
    const f32 inv[9] = {A * s, -(b * i - c * h) * s, (b * f - c * e) * s, B * s, (a * i - c * g) * s, -(a * f - c * d) * s,
                        C * s, -(a * h - b * g) * s, (a * e - b * d) * s};
    std::fill(out, out + 16, 0.f);
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) out[r + 4 * k] = inv[r * 3 + k];
    for (int r = 0; r < 3; ++r) out[r + 12] = -(out[r] * m[12] + out[r + 4] * m[13] + out[r + 8] * m[14]);
    out[15] = 1.f;
    return true;
}

ecs::EntityID activeTerrainOf(EditorHost& host) {
    ecs::Registry& registry = host.editorScene().registry();
    const ecs::EntityID sel = host.editorState().primarySelection;
    if (sel.valid() && registry.get<ecs::Terrain>(sel) != nullptr) return sel;
    ecs::EntityID first;
    registry.each<ecs::Terrain>([&](ecs::EntityID id, ecs::Terrain&) {
        if (!first.valid()) first = id;
    });
    return first;
}
} // namespace

TerrainView EditorPanels::terrain() {
    TerrainView v = m_terrain;
    withScene([&](EditorHost& host, const ViewportPanel&) {
        ecs::Registry& registry = host.editorScene().registry();
        registry.each<ecs::Terrain>([&](ecs::EntityID, ecs::Terrain&) { ++v.terrains; });
        const ecs::EntityID e = activeTerrainOf(host);
        if (const ecs::Terrain* t = e.valid() ? registry.get<ecs::Terrain>(e) : nullptr) v.stamps = t->stamp_count;
    });
    v.status = std::to_string(v.terrains) + " terrain(s); active terrain sculpts " + std::to_string(v.stamps) + " / " +
               std::to_string(ecs::Terrain::kMaxStamps);
    return v;
}
void EditorPanels::setTerrainSettings(u32 cells, f32 size, f32 height, u32 seed) {
    m_terrain.cells = std::clamp(cells, 2u, 256u);
    m_terrain.size = std::max(1.f, size);
    m_terrain.height = std::max(0.f, height);
    m_terrain.seed = seed;
    ++m_revision;
}
void EditorPanels::setTerrainBrush(u32 mode, f32 radius, f32 strength) {
    m_terrain.mode = std::min(mode, 2u);
    m_terrain.radius = std::max(0.1f, radius);
    m_terrain.strength = strength;
    ++m_revision;
}
void EditorPanels::setTerrainSculpt(bool on) {
    if (!on && m_strokeEntity.valid()) terrainPointer({ViewportPointerEvent::Kind::Up, ViewportPointerEvent::Button::Left, 0.f, 0.f});
    m_terrain.sculpt = on;
    if (on && m_sculptMode) setSculptMode(false); // one viewport brush at a time
    ++m_revision;
}

EntityRef EditorPanels::terrainCreate() {
    ecs::EntityID created;
    withScene([&](EditorHost& host, const ViewportPanel&) {
        ecs::Registry& registry = host.editorScene().registry();
        EntityComponentSet set{};
        std::get<std::optional<ecs::Transform>>(set) = ecs::Transform{};
        auto terrain = std::make_unique<ecs::Terrain>();
        terrain->cells = m_terrain.cells;
        terrain->size = m_terrain.size;
        terrain->height = m_terrain.height;
        terrain->seed = m_terrain.seed;
        std::get<std::optional<ecs::Terrain>>(set) = *terrain;
        std::get<std::optional<ecs::Mesh>>(set) = ecs::Mesh{};
        auto cmd = std::make_unique<CreateEntityCommand>(registry, std::move(set), "Create terrain");
        CreateEntityCommand* raw = cmd.get();
        host.undoStack().execute(std::move(cmd));
        created = raw->createdEntity();
        if (created.valid()) {
            ecs::TransformSystem::update(registry);
            host.editorState().selectedEntities = {created};
            host.editorState().primarySelection = created;
        }
    });
    ++m_revision;
    m_c.refresh();
    return EntityRef{created.index, created.generation};
}

static ecs::TerrainStamp brushStampOf(const TerrainView& v, f32 x, f32 z) {
    ecs::TerrainStamp s{};
    s.x = x;
    s.z = z;
    s.radius = v.radius;
    s.amount = v.strength;
    s.mode = v.mode;
    return s;
}

bool EditorPanels::terrainStampLocal(f32 x, f32 z) {
    bool ok = false;
    withScene([&](EditorHost& host, const ViewportPanel&) {
        ecs::Registry& registry = host.editorScene().registry();
        const ecs::EntityID e = activeTerrainOf(host);
        ecs::Terrain* t = e.valid() ? registry.get<ecs::Terrain>(e) : nullptr;
        if (t == nullptr) return;
        auto after = std::make_unique<ecs::Terrain>(*t);
        if (!addTerrainStamp(*after, brushStampOf(m_terrain, x, z))) return;
        host.undoStack().execute(std::make_unique<TerrainEditCommand>(registry, e, *t, *after, "Sculpt terrain"));
        ok = true;
    });
    ++m_revision;
    m_c.refresh();
    return ok;
}

bool EditorPanels::terrainClear() {
    bool ok = false;
    withScene([&](EditorHost& host, const ViewportPanel&) {
        ecs::Registry& registry = host.editorScene().registry();
        const ecs::EntityID e = activeTerrainOf(host);
        ecs::Terrain* t = e.valid() ? registry.get<ecs::Terrain>(e) : nullptr;
        if (t == nullptr || t->stamp_count == 0) return;
        auto after = std::make_unique<ecs::Terrain>(*t);
        after->stamp_count = 0;
        for (ecs::TerrainStamp& s : after->stamps) s = ecs::TerrainStamp{};
        host.undoStack().execute(std::make_unique<TerrainEditCommand>(registry, e, *t, *after, "Clear terrain sculpts"));
        ok = true;
    });
    ++m_revision;
    m_c.refresh();
    return ok;
}

bool EditorPanels::terrainPick_(f32 x, f32 y, bool onlyStroke, ecs::EntityID& best, f32 bestLocal[3]) {
    // caller holds the scene mutex
    const ViewportRay ray = m_c.m_viewport.screenRay(x, y);
    const f32 origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z}, dir[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
    ecs::Registry& registry = m_c.m_host.editorScene().registry();
    ecs::TransformSystem::update(registry);
    f32 bestT = 1e30f;
    best = {};
    registry.each<ecs::Transform, ecs::Terrain>([&](ecs::EntityID id, ecs::Transform& tr, ecs::Terrain& terrain) {
        if (onlyStroke && id != m_strokeEntity) return;
        f32 inv[16];
        const f32* mm = tr.local_to_world.data.data();
        if (!invertAffine(mm, inv)) return;
        const f32 o[3] = {inv[0] * origin[0] + inv[4] * origin[1] + inv[8] * origin[2] + inv[12],
                          inv[1] * origin[0] + inv[5] * origin[1] + inv[9] * origin[2] + inv[13],
                          inv[2] * origin[0] + inv[6] * origin[1] + inv[10] * origin[2] + inv[14]};
        const f32 d[3] = {inv[0] * dir[0] + inv[4] * dir[1] + inv[8] * dir[2], inv[1] * dir[0] + inv[5] * dir[1] + inv[9] * dir[2],
                          inv[2] * dir[0] + inv[6] * dir[1] + inv[10] * dir[2]};
        terrain::Heightfield field;
        buildTerrainHeightfield(terrain, field);
        f32 hit[3];
        if (!raycastTerrain(field, terrain, o, d, hit)) return;
        const f32 tl = std::fabs(d[0]) > std::fabs(d[2]) ? (hit[0] - o[0]) / d[0]
                                                         : (std::fabs(d[2]) > 1e-8f ? (hit[2] - o[2]) / d[2] : (hit[1] - o[1]) / d[1]);
        if (tl < bestT) {
            bestT = tl;
            best = id;
            std::copy(hit, hit + 3, bestLocal);
        }
    });
    return best.valid();
}

bool EditorPanels::terrainPointer(const ViewportPointerEvent& e) {
    using K = ViewportPointerEvent::Kind;
    if (!m_terrain.sculpt || e.button != ViewportPointerEvent::Button::Left) return false;
    if (e.kind == K::Move && !m_strokeEntity.valid()) return false; // hover
    bool consumed = true;
    {
        std::lock_guard<std::mutex> lock(m_c.m_mutex);
        ecs::Registry& registry = m_c.m_host.editorScene().registry();
        f32 local[3] = {0.f, 0.f, 0.f};
        ecs::EntityID hit;
        if (e.kind == K::Down) {
            if (!terrainPick_(e.x, e.y, false, hit, local)) {
                consumed = false; // missed every terrain: a normal click (pick / gizmo)
            } else if (ecs::Terrain* t = registry.get<ecs::Terrain>(hit)) {
                m_strokeBefore = std::make_shared<ecs::Terrain>(*t);
                m_strokeEntity = hit;
                addTerrainStamp(*t, brushStampOf(m_terrain, local[0], local[2]));
                m_strokeLast[0] = local[0];
                m_strokeLast[1] = local[2];
            }
        } else if (e.kind == K::Move) {
            ecs::Terrain* t = registry.get<ecs::Terrain>(m_strokeEntity);
            if (t != nullptr && terrainPick_(e.x, e.y, true, hit, local)) {
                const f32 spacing = std::max(0.25f, m_terrain.radius * 0.5f);
                const f32 dx = local[0] - m_strokeLast[0], dz = local[2] - m_strokeLast[1];
                const f32 len = std::sqrt(dx * dx + dz * dz);
                if (len >= spacing) {
                    const int n = int(len / spacing);
                    for (int i = 1; i <= n; ++i) {
                        const f32 k = f32(i) * spacing / len;
                        if (!addTerrainStamp(*t, brushStampOf(m_terrain, m_strokeLast[0] + dx * k, m_strokeLast[1] + dz * k))) break;
                    }
                    const f32 k = f32(n) * spacing / len;
                    m_strokeLast[0] += dx * k;
                    m_strokeLast[1] += dz * k;
                }
            }
        } else if (m_strokeEntity.valid()) {
            const ecs::EntityID id = m_strokeEntity;
            m_strokeEntity = {};
            if (ecs::Terrain* t = registry.get<ecs::Terrain>(id); t != nullptr && m_strokeBefore) {
                auto after = std::make_unique<ecs::Terrain>(*t);
                m_c.m_host.undoStack().execute(std::make_unique<TerrainEditCommand>(
                    registry, id, *static_cast<const ecs::Terrain*>(m_strokeBefore.get()), *after, "Sculpt terrain stroke"));
            }
            m_strokeBefore.reset();
        } else {
            consumed = false;
        }
    }
    if (!consumed) return false;
    ++m_revision;
    m_c.refresh();
    return true;
}

// =====================================================================================================================
// Multi-viewport: orthographic views (port of the Qt OrthoViewWidget)

// Top: looking down -Y, u = X, v = -Z. Front: looking down -Z, u = X, v = Y. Side: from +X, u = -Z, v = Y.
void OrthoView::uv(f32 x, f32 y, f32 z, f32& u, f32& v) const {
    switch (axis) {
    case Axis::Top: u = x; v = -z; break;
    case Axis::Front: u = x; v = y; break;
    case Axis::Side: u = -z; v = y; break;
    }
}
void OrthoView::project(f32 x, f32 y, f32 z, f32& px, f32& py) const {
    f32 u = 0, v = 0;
    uv(x, y, z, u, v);
    px = w * 0.5f + (u - centerU) * scale;
    py = h * 0.5f - (v - centerV) * scale;
}
void OrthoView::setScale(f32 s) { scale = std::clamp(s, 0.5f, 2000.f); }
void OrthoView::pan(f32 dx, f32 dy) {
    framed = true;
    centerU -= dx / scale;
    centerV += dy / scale;
}
void OrthoView::zoomAt(f32 px, f32 py, bool in) {
    framed = true;
    const f32 u = centerU + (px - w * 0.5f) / scale, v = centerV - (py - h * 0.5f) / scale;
    setScale(scale * (in ? 1.2f : 1.f / 1.2f));
    // keep (u, v) under the cursor
    centerU = u - (px - w * 0.5f) / scale;
    centerV = v + (py - h * 0.5f) / scale;
}

std::vector<OrthoItem> EditorPanels::orthoItems(const OrthoView& v) const {
    std::vector<OrthoItem> items;
    std::lock_guard<std::mutex> lock(m_c.m_mutex);
    ecs::Registry& registry = m_c.m_host.editorScene().registry();
    ecs::TransformSystem::update(registry);
    const auto& sel = m_c.m_host.editorState().selectedEntities;
    const ecs::EntityID primary = m_c.m_host.editorState().primarySelection;
    registry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform& t) {
        OrthoItem it;
        it.entity = EntityRef{id.index, id.generation};
        it.selected = id == primary || std::find(sel.begin(), sel.end(), id) != sel.end();
        const auto& m = t.local_to_world.data;
        v.project(m[12], m[13], m[14], it.cx, it.cy);
        const ecs::Mesh* mesh = registry.get<ecs::Mesh>(id);
        if (mesh && mesh->aabb_max.x >= mesh->aabb_min.x && mesh->aabb_max.y >= mesh->aabb_min.y && mesh->aabb_max.z >= mesh->aabb_min.z) {
            f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (int c = 0; c < 8; ++c) {
                const f32 lx = (c & 1) ? mesh->aabb_max.x : mesh->aabb_min.x;
                const f32 ly = (c & 2) ? mesh->aabb_max.y : mesh->aabb_min.y;
                const f32 lz = (c & 4) ? mesh->aabb_max.z : mesh->aabb_min.z;
                f32 px = 0, py = 0;
                v.project(m[0] * lx + m[4] * ly + m[8] * lz + m[12], m[1] * lx + m[5] * ly + m[9] * lz + m[13],
                          m[2] * lx + m[6] * ly + m[10] * lz + m[14], px, py);
                x0 = std::min(x0, px); y0 = std::min(y0, py); x1 = std::max(x1, px); y1 = std::max(y1, py);
            }
            it.x0 = x0; it.y0 = y0; it.x1 = x1; it.y1 = y1;
        }
        items.push_back(it);
    });
    std::stable_partition(items.begin(), items.end(), [](const OrthoItem& i) { return !i.selected; });
    return items;
}

EntityRef EditorPanels::orthoPick(const OrthoView& v, f32 px, f32 py) const {
    EntityRef best;
    f32 bestScore = 1e30f;
    for (const OrthoItem& it : orthoItems(v)) {
        f32 score;
        if (it.x1 - it.x0 > 1.f && it.y1 - it.y0 > 1.f && px >= it.x0 - 3 && px <= it.x1 + 3 && py >= it.y0 - 3 && py <= it.y1 + 3) {
            score = (it.x1 - it.x0) * (it.y1 - it.y0); // smallest box under the cursor wins
        } else {
            const f32 d = std::hypot(it.cx - px, it.cy - py);
            if (d > 8.f) continue;
            score = d; // markers beat boxes
        }
        if (score < bestScore) { bestScore = score; best = it.entity; }
    }
    return best;
}

bool EditorPanels::orthoFrame(OrthoView& v, bool selectionOnly) const {
    const auto items = orthoItems(v);
    const bool selOnly = selectionOnly && std::any_of(items.begin(), items.end(), [](const OrthoItem& i) { return i.selected; });
    f32 x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool any = false;
    for (const OrthoItem& it : items) {
        if (selOnly && !it.selected) continue;
        const bool b = it.box();
        x0 = std::min(x0, b ? it.x0 : it.cx - 4); y0 = std::min(y0, b ? it.y0 : it.cy - 4);
        x1 = std::max(x1, b ? it.x1 : it.cx + 4); y1 = std::max(y1, b ? it.y1 : it.cy + 4);
        any = true;
    }
    if (!any) return false;
    const f32 minUnits = selectionOnly ? 2.f : 8.f;
    v.framed = true;
    v.centerU += ((x0 + x1) * 0.5f - v.w * 0.5f) / v.scale;
    v.centerV -= ((y0 + y1) * 0.5f - v.h * 0.5f) / v.scale;
    const f32 wu = std::max((x1 - x0) / v.scale, minUnits), hu = std::max((y1 - y0) / v.scale, minUnits);
    v.setScale(std::min(v.w / (wu * 1.6f), v.h / (hu * 1.6f)));
    return true;
}

bool EditorPanels::orthoDrag(const OrthoView& v, f32 fx, f32 fy, f32 tx, f32 ty) {
    std::lock_guard<std::mutex> lock(m_c.m_mutex);
    ecs::Registry& registry = m_c.m_host.editorScene().registry();
    const ecs::EntityID id = m_c.m_host.editorState().primarySelection;
    const ecs::Transform* t = id.valid() && registry.alive(id) ? registry.get<ecs::Transform>(id) : nullptr;
    if (t == nullptr || m_c.m_host.editorState().playing) return false;
    const TransformCommand::State before = TransformCommand::capture(*t);
    TransformCommand::State after = before;
    // view delta -> world delta (u/v back to the axis' world components)
    const f32 du = (tx - fx) / v.scale, dv = -(ty - fy) / v.scale;
    switch (v.axis) {
    case OrthoView::Axis::Top: after.position.x += du; after.position.z -= dv; break;
    case OrthoView::Axis::Front: after.position.x += du; after.position.y += dv; break;
    case OrthoView::Axis::Side: after.position.z -= du; after.position.y += dv; break;
    }
    m_c.m_host.undoStack().execute(std::make_unique<TransformCommand>(registry, id, before, after));
    m_c.m_host.editorState().sceneModified = true;
    return true;
}

bool EditorPanels::orthoPointer(OrthoView& v, const ViewportPointerEvent& e, bool additive) {
    using K = ViewportPointerEvent::Kind;
    using B = ViewportPointerEvent::Button;
    bool changed = false;
    if (e.kind == K::Down) {
        v.lastX = v.pressX = e.x;
        v.lastY = v.pressY = e.y;
        if (e.button != B::Left) { v.drag = OrthoView::Drag::Pan; return false; }
        const EntityRef hit = orthoPick(v, e.x, e.y);
        std::vector<EntityRef> sel = m_c.viewState().selection;
        EntityRef primary = m_c.viewState().primary;
        const auto found = std::find(sel.begin(), sel.end(), hit);
        const bool alreadySel = hit.valid() && found != sel.end();
        if (!additive) {
            if (!alreadySel) sel.clear();
            if (hit.valid() && !alreadySel) sel.push_back(hit);
            primary = hit;
        } else if (hit.valid()) {
            if (found != sel.end()) {
                sel.erase(found);
                if (primary == hit) primary = sel.empty() ? EntityRef{} : sel.back();
            } else {
                sel.push_back(hit);
                primary = hit;
            }
        }
        m_c.select(sel, primary);
        if (hit.valid() && !additive) v.drag = OrthoView::Drag::Move;
        return true;
    }
    if (e.kind == K::Move) {
        if (v.drag == OrthoView::Drag::Pan) {
            v.pan(e.x - v.lastX, e.y - v.lastY);
        } else if (v.drag == OrthoView::Drag::Move) {
            if (!v.macroOpen) {
                if (std::hypot(e.x - v.pressX, e.y - v.pressY) < 3.f) return false;
                std::lock_guard<std::mutex> lock(m_c.m_mutex);
                m_c.m_host.undoStack().beginMacro(std::string("Move entity (") + v.name() + " view)");
                v.macroOpen = true;
            }
            changed = orthoDrag(v, v.lastX, v.lastY, e.x, e.y);
        }
        v.lastX = e.x;
        v.lastY = e.y;
        return changed;
    }
    // Up
    if (v.macroOpen) {
        {
            std::lock_guard<std::mutex> lock(m_c.m_mutex);
            m_c.m_host.undoStack().endMacro();
        }
        v.macroOpen = false;
        m_c.refresh();
        changed = true;
    }
    v.drag = OrthoView::Drag::None;
    return changed;
}

} // namespace fuse::editor
