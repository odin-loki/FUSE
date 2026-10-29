// fuse_editor_panel_models: EditorPanels, the shell-independent models behind the ported panels (Project Hub, Asset
// Browser, Material Editor, SDF Sculpt, Profiler, Timeline, Sequencer), driven directly without any UI.
#include "scripted_services.hpp"

#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/editor_panel_models.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace fuse::editor;
using fuse::i32;
using fuse::u32;
using fuse::u64;
using fuse::u8;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

namespace fs = std::filesystem;

static void touch(const fs::path& p, const char* text = "{}") {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "fuse_editor_panel_models_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    EditorHost host;
    test::ScriptedServices sv;
    EditorController c(host, sv, {.settingsPath = (dir / "editor_settings.json").string()});
    c.refresh();
    EditorPanels p(c);
    u64 rev = p.revision();

    // ---- Project Hub ----
    const fs::path root = dir / "projects";
    fs::create_directories(root);
    p.setProjectsRoot(root.string());
    ProjectHubView hub = p.projectHub();
    CHECK(!hub.hasProject && hub.projects.empty() && hub.projectsRoot == root.string());
    CHECK(p.hubNew((root / "Alpha").string(), "Alpha"));
    CHECK(fs::exists(root / "Alpha" / "project.json"));
    hub = p.projectHub();
    CHECK(hub.hasProject && hub.name == "Alpha");
    CHECK(hub.projects.size() == 1u);
    CHECK(p.hubNew((root / "Beta").string(), "Beta"));
    hub = p.projectHub();
    CHECK(hub.projects.size() == 2u && hub.projects[0] < hub.projects[1]); // sorted
    CHECK(p.hubOpen((root / "Alpha").string()));
    CHECK(p.projectHub().name == "Alpha");
    CHECK(!p.hubOpen((dir / "missing").string()));
    CHECK(p.revision() > rev);
    rev = p.revision();

    // ---- Asset Browser (rooted at the open project) ----
    const fs::path proj = root / "Alpha";
    touch(proj / "level.fscene");
    touch(proj / "props" / "rock.fscene");
    touch(proj / "notes.txt", "x");
    AssetBrowserView av = p.assets();
    auto has = [](const AssetBrowserView& v, const std::string& name) {
        for (const AssetEntryView& e : v.entries)
            if (e.name == name) return true;
        return false;
    };
    CHECK(has(av, "props") && has(av, "level.fscene"));
    bool propsIsFolder = false;
    for (const AssetEntryView& e : av.entries)
        if (e.name == "props") propsIsFolder = e.folder;
    CHECK(propsIsFolder);
    p.assetsSetFilter("fscene");
    av = p.assets();
    CHECK(av.filter == "fscene" && has(av, "level.fscene") && !has(av, "notes.txt"));
    p.assetsSetFilter("");
    std::string propsPath;
    for (const AssetEntryView& e : p.assets().entries)
        if (e.name == "props") propsPath = e.path;
    CHECK(p.assetsActivate(propsPath));
    av = p.assets();
    CHECK(has(av, "rock.fscene") && !has(av, "level.fscene"));
    CHECK(p.assetsUp());
    CHECK(has(p.assets(), "level.fscene"));

    // ---- Material Editor ----
    MaterialView mv = p.materials();
    const std::size_t mats = mv.names.size();
    CHECK(mats >= 1u);
    const u32 mat = p.materialNew();
    mv = p.materials();
    CHECK(mv.names.size() == mats + 1 && mv.current == mat);
    CHECK(p.materialSetRoughness(0.25f));
    CHECK(p.materialSetMetallic(1.f));
    CHECK(p.materialSetShader(1));
    CHECK(p.materialSetBaseColorSrgb(1.f, 0.f, 0.f));
    mv = p.materials();
    CHECK(std::fabs(mv.state.roughness - 0.25f) < 1e-3f && mv.state.metallic > 0.99f && mv.shader == 1u);
    CHECK(mv.srgb[0] > 0.99f && mv.srgb[1] < 0.01f);
    CHECK(!mv.summary.empty());
    std::vector<u8> preview(32 * 32 * 4);
    CHECK(p.materialPreview(32, 32, preview.data()));
    std::printf("preview centre %u,%u,%u corner %u,%u,%u\n", preview[(16 * 32 + 16) * 4], preview[(16 * 32 + 16) * 4 + 1], preview[(16 * 32 + 16) * 4 + 2], preview[0], preview[1], preview[2]);
    CHECK(preview[(16 * 32 + 16) * 4 + 3] != 0); // a lit sphere covers the centre
    CHECK(p.materialSelect(0));
    CHECK(p.materials().current == 0u);
    CHECK(!p.materialSelect(9999));
    // assign to the selection (one undo step)
    CHECK(c.runEntityAction(ContextMenuAction::CreateCube));
    c.tick();
    c.refresh();
    CHECK(p.materialSelect(mat));
    CHECK(p.materials().hasSelection);
    CHECK(p.materialAssignToSelection());
    mv = p.materials();
    CHECK(mv.selectionHasMaterial && mv.selectionMaterial == mat);
    c.undo();
    c.tick();
    c.refresh();
    mv = p.materials();
    CHECK(!mv.selectionHasMaterial || mv.selectionMaterial != mat);

    // ---- SDF Sculpt ----
    CHECK(!p.sculpt().active);
    p.setSculptMode(true);
    CHECK(p.sculptMode() && p.sculpt().active);
    p.setBrushRadius(0.75f);
    p.setBrushStrength(0.5f);
    p.setBrushSpacing(0.3f);
    p.setBrushSymmetry(true);
    SculptView sc = p.sculpt();
    CHECK(std::fabs(sc.brush.radius - 0.75f) < 1e-4f && std::fabs(sc.brush.strength - 0.5f) < 1e-4f);
    CHECK(std::fabs(sc.spacing - 0.3f) < 1e-4f);
    CHECK(!sc.materials.empty() && !sc.summary.empty());
    p.setSculptMode(false);
    CHECK(!p.sculptMode());

    // ---- Profiler ----
    for (int i = 0; i < 12; ++i) p.profilerSample(1.f + float(i % 3));
    ProfilerView pv = p.profiler();
    CHECK(pv.frames >= 12u && pv.history.size() >= 12u && !pv.summary.empty());
    p.profilerSetPaused(true);
    p.profilerSample(5.f);
    CHECK(p.profiler().frames == pv.frames && p.profiler().paused);
    p.profilerSetPaused(false);
    p.profilerClear();
    CHECK(p.profiler().history.size() <= 1u);

    // ---- Timeline ----
    CHECK(p.timeline().keyCount == 0u);
    CHECK(p.timelineKeySelection());
    TimelineView tv = p.timeline();
    CHECK(tv.keyCount >= 3u && !tv.lanes.empty() && !tv.status.empty());
    p.timelineSetPlayhead(2000);
    CHECK(p.timeline().playheadMs == 2000);
    CHECK(p.timelineKeySelection());
    CHECK(p.timeline().keyCount >= 2 * tv.keyCount);
    const u32 keys = p.timeline().keyCount;
    CHECK(p.timelineDeleteKeysAtPlayhead());
    CHECK(p.timeline().keyCount < keys);
    p.timelineSetDuration(3000);
    CHECK(p.timeline().durationMs == 3000);
    CHECK(!p.timelineAdvance(0.1f)); // stopped
    p.timelineSetPlaying(true);
    const i32 ph = p.timeline().playheadMs;
    CHECK(p.timelineAdvance(0.25f));
    CHECK(p.timeline().playing && p.timeline().playheadMs != ph);
    for (int i = 0; i < 20; ++i) p.timelineAdvance(0.25f);
    CHECK(p.timeline().playheadMs >= 0 && p.timeline().playheadMs <= 3000); // loops
    p.timelineSetPlaying(false);

    // ---- Sequencer (linked to the timeline playhead) ----
    CHECK(p.sequencer().linked);
    p.sequencerScrub(400);
    CHECK(p.sequencer().scrubMs == 400 && !p.sequencer().preview.empty());
    p.timelineSetPlayhead(1250);
    CHECK(p.sequencer().scrubMs == 1250);
    p.sequencerSetLinked(false);
    p.timelineSetPlayhead(500);
    CHECK(p.sequencer().scrubMs == 1250);

    // ---- withScene: the scene renderer hook ----
    bool ran = false;
    p.withScene([&](EditorHost& h, const ViewportPanel&) { ran = &h == &host; });
    CHECK(ran);
    CHECK(p.revision() > rev);

    fs::remove_all(dir);
    if (g_fail) std::fprintf(stderr, "%d failure(s)\n", g_fail);
    else std::printf("fuse_editor_panel_models: OK\n");
    return g_fail ? 1 : 0;
}
