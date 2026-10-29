#pragma once
// UI9 panel models: the Qt-free state + commands behind the seven panels that are not hierarchy / inspector / console
// (Project Hub, Asset Browser, Material Editor, SDF Sculpt, Profiler, Sequencer, Timeline). Each method locks the
// controller's scene mutex, edits through the host's UndoStack (so the controller's unified undo journal sees it) and
// returns plain view structs. The FUSE UI shell is the first client; the Qt widgets in Editor/src/qt keep their own
// copies of this logic until they are routed through here.
#include <fuse/editor/editor_controller.hpp>
#include <fuse/editor/material_editor_panel.hpp>
#include <fuse/editor/profiler_panel.hpp>
#include <fuse/editor/sdf_sculpt_panel.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fuse::editor {

struct ProjectHubView {
    bool hasProject = false;
    std::string name, directory; ///< the open project
    std::string projectsRoot;    ///< folder scanned for projects (subfolders holding project.json)
    std::vector<std::string> projects; ///< absolute directories, sorted
};

struct AssetEntryView {
    std::string name, path, type; ///< path is relative to the root (AssetBrowser convention)
    bool folder = false;
};
struct AssetBrowserView {
    std::string root, directory, filter, selected;
    std::vector<AssetEntryView> entries;
};

struct MaterialView {
    std::vector<std::string> names;
    u32 current = 0;
    MaterialEditState state{};
    f32 srgb[3] = {1.f, 1.f, 1.f};
    u32 shader = 0;            ///< 0 Standard (PBR), 1 Wood, 2 Metal, 3 Concrete (procedural fn id)
    bool hasSelection = false;
    bool selectionHasMaterial = false;
    u32 selectionMaterial = 0;
    std::string summary;
};

struct SculptView {
    bool active = false;
    u32 strokes = 0;
    SdfSculptPanel::BrushState brush{};
    f32 spacing = 0.5f;
    std::vector<std::string> materials;
    std::string summary;
};

/// Navigation panel (Qt NavmeshPanel): grid navmesh baked from the scene's non-trigger colliders.
struct NavigationView {
    f32 cellSize = 0.5f;
    u32 halfExtent = 40;   ///< cells each side of the world origin
    bool show = true;      ///< draw in the viewport
    bool built = false;
    u32 cells = 0;         ///< grid side (2 * halfExtent)
    u32 blocked = 0;
    u32 colliders = 0;
    u32 waypoints = 0;     ///< last test path
    std::string status = "Not baked";
};

/// Terrain panel (Qt TerrainPanel): new Terrain settings, the stamp brush and the viewport sculpt toggle.
struct TerrainView {
    u32 cells = 64;
    f32 size = 64.f, height = 6.f;
    u32 seed = 1;
    u32 mode = 0;          ///< ecs::TerrainStamp Raise / Lower / Flatten
    f32 radius = 6.f, strength = 2.f;
    bool sculpt = false;   ///< left-drag in the viewport stamps the active terrain
    u32 terrains = 0;
    u32 stamps = 0;        ///< of the active terrain
    std::string status;
};

struct ProfilerView {
    ProfilerPanel::FrameStats stats{};
    ProfilerPanel::FrameProfileData latest{};
    std::vector<f32> history; ///< CPU ms, oldest first
    u32 frames = 0;
    bool paused = false;
    f32 targetFps = 60.f;
    std::string summary;
};

struct TimelineLaneView {
    std::string label; ///< "SDF Object #0 position.x"
    std::vector<i32> keysMs;
};
struct TimelineView {
    i32 durationMs = 5000, playheadMs = 0;
    u32 keyCount = 0;
    bool playing = false;
    std::vector<TimelineLaneView> lanes;
    std::string status; ///< "2 lanes · 6 keys"
};

struct SequencerView {
    bool linked = true;
    i32 scrubMs = 0, rangeMs = 5000;
    u32 scrubPosts = 0;
    std::string preview; ///< "t=..ms sprite=(x,y) mount=.. bone=.. yaw=.. pitch=.. roll=.."
};

class EditorPanels {
public:
    explicit EditorPanels(EditorController& controller);
    ~EditorPanels();
    EditorPanels(const EditorPanels&) = delete;
    EditorPanels& operator=(const EditorPanels&) = delete;

    /// Bumped by every edit made through this object (and by profiler samples); shells re-read views when it changes.
    [[nodiscard]] u64 revision() const { return m_revision; }

    /// Runs `fn` with the host and the viewport camera under the scene lock: scene renderer hosts (SceneViewportHost)
    /// read the edited scene this way while the game thread may be ticking.
    void withScene(const std::function<void(EditorHost&, const ViewportPanel&)>& fn);

    // ---- Project Hub ----
    void setProjectsRoot(std::string directory);
    [[nodiscard]] ProjectHubView projectHub() const;
    bool hubOpen(const std::string& directory);
    bool hubNew(const std::string& directory, const std::string& name); ///< 3D + 2D + UI
    /// New Project wizard: dimension flags (kProjectEnable*) and module toggles.
    bool hubNew(const std::string& directory, const std::string& name, u32 dimensionFlags, const project::ModuleSettings& modules);

    // ---- Asset Browser ----
    [[nodiscard]] AssetBrowserView assets(); ///< roots at the open project (else the projects root) and rescans
    void assetsSetFilter(std::string_view filter);
    bool assetsActivate(const std::string& path); ///< folder: enter; scene: open (prompts through ShellServices)
    bool assetsUp();
    void assetsRescan();

    // ---- Material Editor ----
    [[nodiscard]] MaterialView materials();
    bool materialSelect(u32 id);
    u32 materialNew(); ///< copies the current material (undoable); returns its id
    bool materialSetRoughness(f32 v);
    bool materialSetMetallic(f32 v);
    bool materialSetShader(u32 fnId);
    bool materialSetUvScale(f32 v);
    bool materialSetBaseColorSrgb(f32 r, f32 g, f32 b);
    bool materialSetTexture(const std::string& path); ///< empty clears
    bool materialPickBaseColor();  ///< ShellServices::pickColour
    bool materialPickTexture();    ///< ShellServices::pickOpenFile
    bool materialAssignToSelection();
    /// 96x96 RGBA8 (R low byte) preview sphere of the current material (MaterialEditorPanel::renderPreview).
    bool materialPreview(u32 w, u32 h, u8* rgba) const;

    // ---- SDF Sculpt ----
    [[nodiscard]] SculptView sculpt();
    void setSculptMode(bool on);
    [[nodiscard]] bool sculptMode() const { return m_sculptMode; }
    void setBrushOp(u32 op);
    void setBrushShape(u32 shape);
    void setBrushMaterial(u32 id);
    void setBrushRadius(f32 v);
    void setBrushStrength(f32 v);
    void setBrushSpacing(f32 v);
    void setBrushAlpha(f32 v);
    void setBrushSymmetry(bool on);
    /// Left button in the viewport while sculpting: Down opens a "Sculpt stroke" undo macro and stamps, Move stamps,
    /// Up closes it. True when the event was consumed (sculpt mode on and a left-button event).
    bool sculptPointer(const ViewportPointerEvent& e);

    // ---- Profiler ----
    /// One sample per shell frame: the game tick time comes from the controller, `uiMs` is the shell's own frame.
    void profilerSample(f32 uiMs);
    [[nodiscard]] ProfilerView profiler() const;
    void profilerSetPaused(bool paused);
    void profilerClear();

    // ---- Timeline ----
    [[nodiscard]] TimelineView timeline() const;
    bool timelineKeySelection();          ///< keys position / rotation / scale of the primary at the playhead
    bool timelineDeleteKeysAtPlayhead();  ///< every key at the playhead (one undo step)
    void timelineSetPlayhead(i32 ms);     ///< applies the tracks to the scene (and scrubs a linked sequencer)
    void timelineSetDuration(i32 ms);
    void timelineSetPlaying(bool on);
    bool timelineAdvance(f32 dt);         ///< while playing: moves the playhead (loops); true when it moved

    // ---- Terrain ----
    [[nodiscard]] TerrainView terrain();
    void setTerrainSettings(u32 cells, f32 size, f32 height, u32 seed);
    void setTerrainBrush(u32 mode, f32 radius, f32 strength);
    void setTerrainSculpt(bool on);
    /// New Terrain entity at the origin from the settings (one undo step); selects it.
    EntityRef terrainCreate();
    /// One stamp at local (x, z) of the active terrain (one undo step); false without a terrain / full budget.
    bool terrainStampLocal(f32 x, f32 z);
    /// Removes every stamp of the active terrain (one undo step).
    bool terrainClear();
    /// Left button in the viewport while terrain sculpting: Down picks a terrain (world ray vs heightfield) and
    /// stamps, Move stamps every half radius, Up commits the stroke as one undo step. True when consumed.
    bool terrainPointer(const ViewportPointerEvent& e);

    // ---- Navigation ----
    [[nodiscard]] NavigationView navigation() const { return m_nav; }
    void setNavCellSize(f32 size) { m_nav.cellSize = size; ++m_revision; }
    void setNavHalfExtent(u32 cells) { m_nav.halfExtent = cells; ++m_revision; }
    void setNavShow(bool on) { m_nav.show = on; ++m_revision; }
    /// Bakes from the current scene (Qt NavmeshPanel::bake: collider world AABBs above y 0.05 block cells).
    bool navBake();
    /// A* between two world XZ points on the baked mesh; the waypoint count (0: no path).
    u32 navTestPath(f32 ax, f32 az, f32 bx, f32 bz);
    /// World segments of the navmesh overlay (x0 y0 z0 x1 y1 z1): grid outline, a cross per blocked cell, the path.
    [[nodiscard]] std::vector<f32> navSegments() const;

    // ---- Sequencer ----
    [[nodiscard]] SequencerView sequencer() const { return m_seq; }
    void sequencerSetLinked(bool on) { m_seq.linked = on; ++m_revision; }
    void sequencerScrub(i32 ms);

private:
    bool applyMaterial_(const MaterialEditState& s, const char* what);
    bool sculptAt_(f32 x, f32 y);

    EditorController& m_c;
    u64 m_revision = 1;
    NavigationView m_nav;
    TerrainView m_terrain;
    std::shared_ptr<void> m_strokeBefore; ///< ecs::Terrain at stroke start
    ecs::EntityID m_strokeEntity{};
    f32 m_strokeLast[2] = {0.f, 0.f};
    bool terrainPick_(f32 x, f32 y, bool onlyStroke, ecs::EntityID& out, f32 local[3]);
    std::shared_ptr<void> m_navMesh; ///< navigation::GridNavmesh (kept out of this header)
    std::vector<u8> m_navBlocked;
    std::vector<f32> m_navPath; ///< world xz pairs
    std::string m_projectsRoot;
    std::unique_ptr<class AssetBrowser> m_assets;
    std::string m_assetsRoot;
    u32 m_material = 0;
    std::unique_ptr<MaterialEditorPanel> m_matPanel;
    std::unique_ptr<SdfSculptPanel> m_sculpt;
    bool m_sculptMode = false, m_sculptDragging = false;
    std::unique_ptr<ProfilerPanel> m_profiler;
    bool m_timelinePlaying = false;
    f32 m_timelineAccum = 0.f;
    std::unique_ptr<class CinematicsSeqImport> m_seqImport;
    SequencerView m_seq;
};

} // namespace fuse::editor
