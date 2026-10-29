// fuse_editor_controller_api: the Qt-free EditorController (Q1) without any shell: action table, title format, entity
// create / undo / redo through the unified journal, selection, reparent (cycle-safe), inspector edits, transport,
// console, file save / open round trip with the unsaved-changes prompt, recent files.
#include "scripted_services.hpp"

#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/editor_theme_tokens.hpp>

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace fuse::editor;
using fuse::u32;
using fuse::u64;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    // action table
    CHECK(editorActions().size() == std::size_t(ActionId::Count));
    CHECK(findAction("fuseActionNewScene") && findAction("fuseActionNewScene")->id == ActionId::FileNewScene);
    CHECK(findAction("edit.undo") && findAction("edit.undo")->id == ActionId::EditUndo);
    CHECK(themeHex("WindowBg").size() == 7);

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "fuse_editor_controller_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    EditorHost host;
    test::ScriptedServices sv;
    EditorController c(host, sv, {.settingsPath = (dir / "editor_settings.json").string()});
    c.refresh();
    const EditorViewState& v = c.viewState();
    CHECK(v.title == "EditorHostScene - FUSE Editor");
    CHECK(!v.dirty && !v.canUndo);
    const u32 base = v.entityCount;

    // create + undo / redo through the journal
    CHECK(c.runEntityAction(ContextMenuAction::CreateSdfSphere));
    c.tick();
    c.refresh();
    CHECK(v.entityCount == base + 1);
    CHECK(v.dirty && v.canUndo);
    CHECK(v.title == "EditorHostScene* - FUSE Editor");
    const EntityRef sphere = c.lastCreated();
    CHECK(sphere.valid());
    CHECK(v.primary == sphere);
    CHECK(!v.inspector.empty());
    c.undo();
    c.tick();
    c.refresh();
    CHECK(v.entityCount == base);
    c.redo();
    c.tick();
    c.refresh();
    CHECK(v.entityCount == base + 1);

    // second entity, reparent (and the cycle is refused)
    CHECK(c.runEntityAction(ContextMenuAction::CreateEmpty));
    c.tick();
    const EntityRef empty = c.lastCreated();
    const EntityRef sphereNow = v.hierarchy.empty() ? EntityRef{} : sphere;
    (void)sphereNow;
    c.refresh();
    EntityRef sph{};
    for (const HierarchyRowView& r : v.hierarchy)
        if (r.label.find("SDF") != std::string::npos) sph = r.entity;
    CHECK(sph.valid());
    const EntityRef one[] = {sph};
    CHECK(c.reparent(one, empty) == 1);
    c.tick();
    c.refresh();
    bool nested = false;
    for (const HierarchyRowView& r : v.hierarchy)
        if (r.entity == sph) nested = r.parent == empty && r.depth == 1;
    CHECK(nested);
    const EntityRef two[] = {empty};
    CHECK(c.reparent(two, sph) == 0); // cycle

    // inspector edit on the transform
    c.selectOnly(sph);
    c.refresh();
    c.setProperty(sph, "transform.position", "1,2,3");
    c.tick();
    c.refresh();
    bool moved = false;
    for (const InspectorSectionView& s : v.inspector)
        for (const InspectorFieldView& f : s.fields)
            if (f.propertyName == "transform.position" && f.value.rfind("1,2,3", 0) == 0) moved = true;
    CHECK(moved);

    // transport
    CHECK(c.actionEnabled(ActionId::PlayPlay) && !c.actionEnabled(ActionId::PlayStop));
    CHECK(c.trigger(ActionId::PlayPlay));
    c.tick(3);
    c.refresh();
    CHECK(v.transport == TransportState::Playing);
    CHECK(v.title.find("[Playing]") != std::string::npos);
    CHECK(c.trigger(ActionId::PlayStop));
    c.tick();
    c.refresh();
    CHECK(v.transport == TransportState::Stopped);

    // console
    const u64 cv = c.consoleVersion();
    c.submitConsole("help");
    c.tick();
    CHECK(c.consoleVersion() != cv);
    CHECK(!c.console().lines().empty());

    // save as -> clean, then new scene (clean: no prompt), open again
    const std::string path = (dir / "level.fscene").string();
    sv.files.push_back(path);
    CHECK(c.fileSaveAs());
    c.refresh();
    CHECK(!v.dirty);
    CHECK(v.sceneName == "level");
    CHECK(!v.recentFiles.empty());
    CHECK(c.fileNew());
    c.refresh();
    CHECK(v.entityCount == base);
    // dirty + open: Cancel keeps the scene, Discard proceeds
    CHECK(c.runEntityAction(ContextMenuAction::CreateEmpty));
    c.tick();
    sv.answers.push_back(PromptAnswer::Cancel);
    CHECK(!c.fileOpenRecent(0));
    sv.answers.push_back(PromptAnswer::Discard);
    CHECK(c.fileOpenRecent(0));
    c.refresh();
    CHECK(v.entityCount == base + 2);
    CHECK(!v.dirty);

    // recent files persisted
    {
        EditorHost h2;
        test::ScriptedServices s2;
        EditorController c2(h2, s2, {.settingsPath = (dir / "editor_settings.json").string()});
        c2.refresh();
        CHECK(!c2.viewState().recentFiles.empty());
    }

    // threaded game loop smoke
    c.startGameLoop(120.f);
    CHECK(c.trigger(ActionId::PlayPlay));
    for (int i = 0; i < 20; ++i) { c.refresh(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    c.stopGameLoop();
    c.refresh();
    CHECK(v.gameTicks > 0);

    if (g_fail) { std::fprintf(stderr, "fuse_editor_controller_api: %d failure(s)\n", g_fail); return 1; }
    std::printf("fuse_editor_controller_api: OK (%u entities, title \"%s\")\n", v.entityCount, v.title.c_str());
    return 0;
}
