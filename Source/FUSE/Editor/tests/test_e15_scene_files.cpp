// E15 (UNI-U6-FILE-1, MP-B6-QT-SCENE-FILES headless part) gates, CPU only:
//  - project manifest writer: writeManifestJson / saveToDirectory round trip through the existing
//    parser (all fields, JSON escapes), schemaVersion 1
//  - .fuselevel v3: scene + FECS component block; v1/v2 files still load (old and new API); a v3
//    file loads through the old scene-only API; corrupt / truncated v3 rejected without touching
//    the destination
//  - headless EditorHost: New/Open/Save/SaveAs scene commands through the CommandQueue; a scene with
//    Transform / Mesh / lights / Collider / RigidBody / Script components saves, reloads in a fresh
//    host with identical components, and re-saves byte-identically
//  - dirty flag: undo-stack edits and property edits set it, save clears it, load / new clear it;
//    save is refused while PIE runs
//  - NewProject / OpenProject commands: manifest + default 2D / 3D worlds, 2D scenes keep their
//    dimension across save / load
#include <fuse/editor/command_queue.hpp>
#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/undo_stack.hpp>

#include <fuse/core/temp_path.hpp>
#include <fuse/ecs/component_types.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/script.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/scene/serialiser.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

using fuse::f32;
using fuse::u32;
using fuse::u8;
using fuse::usize;
namespace ecs = fuse::ecs;
namespace editor = fuse::editor;
namespace project = fuse::project;
namespace scene = fuse::scene;
namespace fs = std::filesystem;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::vector<char> readBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

template <typename T>
bool sameBytes(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

// ---- manifest writer ------------------------------------------------------------------------------

void testManifestWriterRoundTrip(const fs::path& dir) {
    project::ProjectManifest manifest;
    manifest.schemaVersion = 1;
    manifest.name = "E15 \"quoted\" \\ back\tslash";
    manifest.dimensions.enable3D = false;
    manifest.dimensions.enable2D = true;
    manifest.dimensions.enableUI = false;
    manifest.modules.ai = true;
    manifest.modules.cinematics = false;
    manifest.modules.fx = true;
    manifest.modules.mechanics = true;
    manifest.modules.adventure = false;
    manifest.defaultWorld3D = "worlds/w3d.fuselevel";
    manifest.defaultWorld2D = "worlds/sub dir/w2d.fuselevel";
    manifest.workerCap = 7;

    const std::string json = project::writeManifestJson(manifest);
    expectTrue(json.find("\"schemaVersion\": 1") != std::string::npos, "manifest writer: schemaVersion 1");
    const project::LoadResult parsed = project::parseManifest(json, "/root");
    expectTrue(parsed.status == project::LoadStatus::Ok, "manifest writer: text parses");
    const project::ProjectManifest& m = parsed.manifest;
    expectTrue(m.schemaVersion == 1u && m.name == manifest.name && m.projectRoot == "/root",
               "manifest writer: schema, escaped name and root round trip");
    expectTrue(!m.dimensions.enable3D && m.dimensions.enable2D && !m.dimensions.enableUI,
               "manifest writer: dimensions round trip");
    expectTrue(m.modules.ai && !m.modules.cinematics && m.modules.fx && m.modules.mechanics && !m.modules.adventure,
               "manifest writer: modules round trip");
    expectTrue(m.defaultWorld3D == manifest.defaultWorld3D && m.defaultWorld2D == manifest.defaultWorld2D,
               "manifest writer: default worlds round trip");
    expectTrue(m.workerCap == 7u, "manifest writer: workerCap round trip");

    const project::SaveResult saved = project::saveToDirectory(manifest, (dir / "proj").string());
    expectTrue(saved.ok, "manifest writer: saveToDirectory");
    const project::LoadResult loaded = project::loadFromDirectory((dir / "proj").string());
    expectTrue(loaded.status == project::LoadStatus::Ok && loaded.manifest.name == manifest.name &&
                   loaded.manifest.workerCap == 7u && loaded.manifest.defaultWorld2D == manifest.defaultWorld2D &&
                   loaded.manifest.projectRoot == (dir / "proj").string(),
               "manifest writer: file round trip through loadFromDirectory");
    // Re-writing the loaded manifest reproduces the same text (stable writer).
    expectTrue(project::writeManifestJson(loaded.manifest) == json, "manifest writer: stable output");
    // Overwrite in place.
    manifest.workerCap = 3;
    expectTrue(project::saveToDirectory(manifest, (dir / "proj").string()).ok &&
                   project::loadFromDirectory((dir / "proj").string()).manifest.workerCap == 3u,
               "manifest writer: overwrite existing project.json");
    expectTrue(project::unescapeJsonString("\\u00e9\\n") == "\xC3\xA9\n", "json unescape: \\u escapes as UTF-8");
}

// ---- scene serialiser v3 --------------------------------------------------------------------------

struct Ids {
    ecs::EntityID root;
    ecs::EntityID lamp;
    ecs::EntityID crate;
    ecs::EntityID scripted;
};

Ids populate(ecs::Registry& registry) {
    Ids ids{};
    ids.root = registry.create();
    ecs::Transform rootT{};
    rootT.position = {1.f, 2.f, 3.f, 1.f};
    rootT.rotation = {0.f, 0.3826834f, 0.f, 0.9238795f};
    rootT.scale = {2.f, 2.f, 2.f, 0.f};
    registry.add(ids.root, rootT);
    ecs::Mesh mesh{};
    mesh.index_count = 36;
    mesh.material_id = 5;
    mesh.aabb_min = {-1.f, -1.f, -1.f, 0.f};
    mesh.aabb_max = {1.f, 1.f, 1.f, 0.f};
    mesh.cast_shadow = false;
    registry.add(ids.root, mesh);

    ids.lamp = registry.create();
    ecs::Transform lampT{};
    lampT.position = {0.f, 5.f, 0.f, 1.f};
    lampT.parent = ids.root;
    registry.add(ids.lamp, lampT);
    ecs::PointLight point{};
    point.color = {1.f, 0.5f, 0.25f, 0.f};
    point.intensity = 4.f;
    point.radius = 12.f;
    registry.add(ids.lamp, point);
    ecs::SpotLight spot{};
    spot.inner_cone_deg = 10.f;
    registry.add(ids.lamp, spot);

    ids.crate = registry.create();
    ecs::Transform crateT{};
    crateT.position = {4.f, 1.f, -2.f, 1.f};
    registry.add(ids.crate, crateT);
    ecs::Collider collider{};
    collider.shape = ecs::Collider::Box;
    collider.params = {0.5f, 0.5f, 0.5f, 0.f};
    collider.is_trigger = true;
    collider.layer = 4u;
    registry.add(ids.crate, collider);
    ecs::RigidBody body{};
    body.mass = 3.f;
    body.inv_mass = 1.f / 3.f;
    body.velocity = {0.f, -1.f, 0.f, 0.f};
    registry.add(ids.crate, body);

    ids.scripted = registry.create();
    registry.add(ids.scripted, ecs::Transform{});
    ecs::Script script{};
    (void)script.set_path("scripts/door.lua");
    (void)script.set_number("speed", 2.5);
    (void)script.set_vec3("axis", 0.f, 1.f, 0.f);
    (void)script.set_string("label", "north gate");
    registry.add(ids.scripted, script);

    // A destroyed slot: generations + free list must survive.
    const ecs::EntityID dead = registry.create();
    registry.add(dead, ecs::Transform{});
    registry.destroy_entity(dead);

    ecs::DirectionalLight sun{};
    sun.intensity = 3.f;
    const ecs::EntityID sunId = registry.create();
    registry.add(sunId, sun);
    return ids;
}

void fillScene(scene::Scene& s) {
    s.setName("E15 level");
    s.camera().positionX = 3.f;
    s.camera().fovDeg = 70.f;
    scene::SceneEntityTransform t{};
    t.positionX = 1.f;
    s.addEntity("root", t);
    t.positionY = 5.f;
    s.addEntity("lamp", t, 0);
    s.addEntity("crate");
}

void testSerialiserV3(const fs::path& dir) {
    ecs::register_builtin_components();
    ecs::Registry registry;
    registry.init();
    const Ids ids = populate(registry);
    scene::Scene s;
    fillScene(s);

    const std::string v3 = (dir / "level_v3.fuselevel").string();
    expectTrue(scene::SceneSerialiser::saveWithRegistry(s, registry, v3, scene::SceneDimension::World2D).status ==
                   scene::SerialiseStatus::Ok,
               "v3: saveWithRegistry");

    scene::Scene loaded;
    ecs::Registry loadedRegistry;
    scene::SceneFileInfo info;
    const scene::SerialiseResult r = scene::SceneSerialiser::loadWithRegistry(v3, loaded, loadedRegistry, &info);
    expectTrue(r.status == scene::SerialiseStatus::Ok, "v3: loadWithRegistry");
    expectTrue(info.version == 3u && info.hasEcsBlock && info.dimension == scene::SceneDimension::World2D,
               "v3: file info (version, ECS block, 2D)");
    expectTrue(loaded.name() == "E15 level" && loaded.entityCount() == 3u && loaded.entities()[1].parentIndex == 0 &&
                   loaded.camera().fovDeg == 70.f,
               "v3: scene part (name, entities, hierarchy, camera)");
    expectTrue(loadedRegistry.count() == registry.count(), "v3: ECS entity count");
    expectTrue(loadedRegistry.alive(ids.root) && loadedRegistry.alive(ids.scripted), "v3: exact entity ids");
    const ecs::Transform* lampT = loadedRegistry.get<ecs::Transform>(ids.lamp);
    expectTrue(lampT != nullptr && lampT->parent == ids.root && lampT->position.y == 5.f, "v3: Transform + parent link");
    const ecs::Mesh* mesh = loadedRegistry.get<ecs::Mesh>(ids.root);
    expectTrue(mesh != nullptr && sameBytes(*mesh, *registry.get<ecs::Mesh>(ids.root)), "v3: Mesh bytes");
    expectTrue(sameBytes(*loadedRegistry.get<ecs::PointLight>(ids.lamp), *registry.get<ecs::PointLight>(ids.lamp)) &&
                   sameBytes(*loadedRegistry.get<ecs::SpotLight>(ids.lamp), *registry.get<ecs::SpotLight>(ids.lamp)),
               "v3: light bytes");
    expectTrue(sameBytes(*loadedRegistry.get<ecs::Collider>(ids.crate), *registry.get<ecs::Collider>(ids.crate)) &&
                   sameBytes(*loadedRegistry.get<ecs::RigidBody>(ids.crate), *registry.get<ecs::RigidBody>(ids.crate)),
               "v3: Collider + RigidBody bytes");
    const ecs::Script* script = loadedRegistry.get<ecs::Script>(ids.scripted);
    expectTrue(script != nullptr && script->path() == "scripts/door.lua" && script->property_count == 3u &&
                   script->find_property("speed")->number == 2.5 &&
                   script->find_property("label")->string_value() == "north gate",
               "v3: Script path + properties");

    // Re-save is byte-identical.
    const std::string again = (dir / "level_v3_again.fuselevel").string();
    expectTrue(scene::SceneSerialiser::saveWithRegistry(loaded, loadedRegistry, again, info.dimension).status ==
                   scene::SerialiseStatus::Ok,
               "v3: re-save");
    expectTrue(readBytes(v3) == readBytes(again), "v3: re-save is byte-identical");

    // The scene-only API reads v3 (ECS block skipped).
    scene::Scene sceneOnly;
    expectTrue(scene::SceneSerialiser::load(v3, sceneOnly).status == scene::SerialiseStatus::Ok &&
                   sceneOnly.entityCount() == 3u && sceneOnly.name() == "E15 level",
               "v3: SceneSerialiser::load (scene-only) reads v3");

    // v2 file (old writer) loads through the registry API: Transform-only entities, parents mapped.
    const std::string v2 = (dir / "level_v2.fuselevel").string();
    expectTrue(scene::SceneSerialiser::save(s, v2).status == scene::SerialiseStatus::Ok, "v2: old writer");
    scene::Scene v2Scene;
    ecs::Registry v2Registry;
    scene::SceneFileInfo v2Info;
    expectTrue(scene::SceneSerialiser::loadWithRegistry(v2, v2Scene, v2Registry, &v2Info).status ==
                   scene::SerialiseStatus::Ok,
               "v2: loadWithRegistry");
    expectTrue(v2Info.version == 2u && !v2Info.hasEcsBlock && v2Info.dimension == scene::SceneDimension::World3D,
               "v2: file info");
    expectTrue(v2Scene.entityCount() == 3u && v2Registry.count() == 3u, "v2: one ECS entity per scene entity");
    u32 withParent = 0;
    f32 lampY = 0.f;
    v2Registry.each<ecs::Transform>([&](ecs::EntityID, const ecs::Transform& t) {
        if (t.parent.valid()) {
            ++withParent;
            lampY = t.position.y;
        }
    });
    expectTrue(withParent == 1u && lampY == 5.f, "v2: transform + parent mapped into the registry");

    // Corrupt v3: truncated ECS block -> error, destination untouched.
    std::vector<char> bytes = readBytes(v3);
    const std::string cut = (dir / "level_cut.fuselevel").string();
    {
        std::ofstream out(cut, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size() - 16u));
    }
    scene::Scene keep("keep");
    ecs::Registry keepRegistry;
    keepRegistry.init();
    const ecs::EntityID marker = keepRegistry.create();
    expectTrue(scene::SceneSerialiser::loadWithRegistry(cut, keep, keepRegistry).status != scene::SerialiseStatus::Ok,
               "v3: truncated file rejected");
    expectTrue(keep.name() == "keep" && keepRegistry.alive(marker) && keepRegistry.count() == 1u,
               "v3: failed load leaves scene + registry untouched");
    // Corrupt FECS payload (bad magic inside the block).
    const std::string needle = "FECS";
    auto it = std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end());
    expectTrue(it != bytes.end(), "v3: FECS block present in the file");
    if (it != bytes.end()) {
        *it = 'X';
        const std::string bad = (dir / "level_badecs.fuselevel").string();
        std::ofstream out(bad, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        expectTrue(scene::SceneSerialiser::loadWithRegistry(bad, keep, keepRegistry).status != scene::SerialiseStatus::Ok &&
                       keepRegistry.alive(marker),
                   "v3: corrupt ECS block rejected");
    }
}

// ---- EditorHost file commands ---------------------------------------------------------------------

void tick(editor::EditorHost& host, int frames = 1) {
    for (int i = 0; i < frames; ++i) {
        host.gameTick();
    }
}

void testEditorHostSaveLoad(const fs::path& dir) {
    const std::string levelA = (dir / "editor" / "level.fuselevel").string();
    const std::string levelB = (dir / "editor" / "level_resaved.fuselevel").string();
    Ids ids{};
    {
        editor::EditorHost host;
        host.postFromUi(editor::makeNewSceneCommand(false, "Editor E15"));
        tick(host);
        expectTrue(host.lastFileResult().ok && host.lastFileResult().command == editor::CommandKind::NewScene,
                   "host: NewScene command");
        expectTrue(!host.isSceneDirty(), "host: new scene is clean");
        ids = populate(host.editorScene().registry());
        fillScene(host.runtimeScene());

        host.postFromUi(editor::makeSaveSceneCommand());
        tick(host);
        expectTrue(!host.lastFileResult().ok, "host: Save without a path fails (needs Save As)");

        host.postFromUi(editor::makeSaveSceneAsCommand(levelA));
        tick(host);
        expectTrue(host.lastFileResult().ok && host.currentScenePath() == levelA && host.sceneSaveCount() == 1u,
                   "host: SaveSceneAs command writes the level");
        std::vector<fuse::editor::EditorConsoleLine> out;
        host.drainConsoleOutput(out);
        expectTrue(!out.empty() && out.back().text.find("saved scene") != std::string::npos,
                   "host: save reported on the console queue");
    }
    {
        editor::EditorHost host;
        host.postFromUi(editor::makeOpenSceneCommand(levelA));
        tick(host);
        expectTrue(host.lastFileResult().ok && host.sceneLoadCount() == 1u, "host: OpenScene command");
        const ecs::Registry& registry = host.editorScene().registry();
        expectTrue(registry.alive(ids.root) && registry.alive(ids.lamp) && registry.alive(ids.crate) &&
                       registry.alive(ids.scripted),
                   "host: entities restored with their ids");
        expectTrue(registry.get<ecs::Mesh>(ids.root) != nullptr && registry.get<ecs::Mesh>(ids.root)->material_id == 5u,
                   "host: Mesh restored");
        expectTrue(registry.get<ecs::PointLight>(ids.lamp) != nullptr &&
                       registry.get<ecs::PointLight>(ids.lamp)->intensity == 4.f,
                   "host: Light restored");
        expectTrue(registry.get<ecs::Collider>(ids.crate) != nullptr && registry.get<ecs::Collider>(ids.crate)->is_trigger &&
                       registry.get<ecs::RigidBody>(ids.crate) != nullptr &&
                       registry.get<ecs::RigidBody>(ids.crate)->mass == 3.f,
                   "host: Collider + RigidBody restored");
        expectTrue(registry.get<ecs::Script>(ids.scripted) != nullptr &&
                       registry.get<ecs::Script>(ids.scripted)->path() == "scripts/door.lua",
                   "host: Script restored");
        expectTrue(registry.get<ecs::Transform>(ids.lamp)->parent == ids.root, "host: Transform hierarchy restored");
        // (The runtime scene's entity table mirrors the ECS Transform entities in the editor, see
        // RuntimeViewportHook::mirrorEditorEntities_; name + camera come from the file.)
        expectTrue(host.runtimeScene().name() == "E15 level" && host.runtimeScene().camera().fovDeg == 70.f,
                   "host: runtime scene restored");
        expectTrue(!host.isSceneDirty(), "host: loaded scene is clean");

        host.postFromUi(editor::makeSaveSceneAsCommand(levelB));
        tick(host);
        expectTrue(host.lastFileResult().ok, "host: re-save");
        const std::vector<char> a = readBytes(levelA);
        const std::vector<char> b = readBytes(levelB);
        expectTrue(!a.empty() && a == b, "host: save -> load -> save is byte-identical");

        // Dirty tracking: an undoable delete dirties, save clears.
        editor::EditorCommand del;
        del.kind = editor::CommandKind::DeleteObject;
        del.target = fuse::Handle<fuse::Object>(ids.crate.index, ids.crate.generation);
        host.postFromUi(del);
        tick(host);
        expectTrue(!host.editorScene().registry().alive(ids.crate), "host: delete applied");
        expectTrue(host.undoStack().isDirty() && host.isSceneDirty(), "host: undoable edit sets the dirty flag");
        host.postFromUi(editor::makeSaveSceneCommand());
        tick(host);
        expectTrue(host.lastFileResult().ok && host.lastFileResult().command == editor::CommandKind::SaveScene,
                   "host: SaveScene to the current path");
        expectTrue(!host.isSceneDirty() && !host.undoStack().isDirty(), "host: save clears the dirty flag");
        // Undo past the saved baseline dirties again.
        host.postFromUi(editor::makeTransportCommand(editor::CommandKind::Undo));
        tick(host);
        expectTrue(host.isSceneDirty() && host.editorScene().registry().alive(ids.crate),
                   "host: undo after save is dirty again");

        // Property edit (CommandStack) dirties; save clears.
        host.postFromUi(editor::makeSaveSceneCommand());
        tick(host);
        editor::EditorCommand move;
        move.kind = editor::CommandKind::SetProperty;
        move.target = fuse::Handle<fuse::Object>(ids.root.index, ids.root.generation);
        move.propertyName = "transform.position";
        move.propertyValue = "9 8 7";
        host.postFromUi(move);
        tick(host);
        expectTrue(host.editorScene().registry().get<ecs::Transform>(ids.root)->position.x == 9.f,
                   "host: property edit applied");
        expectTrue(host.isSceneDirty(), "host: property edit sets the dirty flag");
        host.postFromUi(editor::makeSaveSceneCommand());
        tick(host);
        expectTrue(!host.isSceneDirty(), "host: save clears the property-edit dirty flag");

        // Save refused during PIE; the edit scene is saved after Stop.
        host.postFromUi(editor::makeTransportCommand(editor::CommandKind::StartPlay));
        tick(host);
        host.postFromUi(editor::makeSaveSceneCommand());
        tick(host);
        expectTrue(!host.lastFileResult().ok, "host: save refused while playing");
        host.postFromUi(editor::makeTransportCommand(editor::CommandKind::StopPlay));
        host.postFromUi(editor::makeSaveSceneCommand());
        tick(host);
        expectTrue(host.lastFileResult().ok, "host: save after stop");

        // Opening a missing file fails and keeps the open scene.
        host.postFromUi(editor::makeOpenSceneCommand((dir / "missing.fuselevel").string()));
        tick(host);
        expectTrue(!host.lastFileResult().ok && host.editorScene().registry().alive(ids.root) &&
                       host.currentScenePath() == levelB,
                   "host: failed open keeps the current scene");

        // v2 file opens in the editor (Transform-only entities).
        scene::Scene v2;
        fillScene(v2);
        const std::string v2Path = (dir / "editor" / "legacy_v2.fuselevel").string();
        expectTrue(scene::SceneSerialiser::save(v2, v2Path).status == scene::SerialiseStatus::Ok, "host: write v2 file");
        host.postFromUi(editor::makeOpenSceneCommand(v2Path));
        tick(host);
        expectTrue(host.lastFileResult().ok && host.editorScene().registry().count() == 3u &&
                       host.runtimeScene().entityCount() == 3u && !host.isSceneDirty(),
                   "host: v2 file still loads in the editor");
        // ... and upgrades to v3 on save.
        host.postFromUi(editor::makeSaveSceneCommand());
        tick(host);
        scene::Scene reread;
        ecs::Registry rereadRegistry;
        scene::SceneFileInfo info;
        expectTrue(scene::SceneSerialiser::loadWithRegistry(v2Path, reread, rereadRegistry, &info).status ==
                           scene::SerialiseStatus::Ok &&
                       info.version == 3u && rereadRegistry.count() == 3u,
                   "host: re-saving a v2 level writes v3");
    }
}

void testProjectCommands(const fs::path& dir) {
    const std::string projectDir = (dir / "NewGame").string();
    editor::EditorHost host;
    host.postFromUi(editor::makeNewProjectCommand(projectDir, "New Game",
                                                  editor::kProjectEnable2D | editor::kProjectEnableUI));
    tick(host);
    expectTrue(host.lastFileResult().ok && host.lastFileResult().command == editor::CommandKind::NewProject,
               "project: NewProject command");
    expectTrue(host.hasProject() && host.projectManifest().name == "New Game" &&
                   !host.projectManifest().dimensions.enable3D && host.projectManifest().dimensions.enable2D,
               "project: manifest in effect");
    expectTrue(host.currentSceneDimension() == scene::SceneDimension::World2D &&
                   host.currentScenePath() == (fs::path(projectDir) / "worlds/main2d.fuselevel").lexically_normal().generic_string(),
               "project: 2D-only project opens its 2D default world");
    const project::LoadResult manifest = project::loadFromDirectory(projectDir);
    expectTrue(manifest.status == project::LoadStatus::Ok && manifest.manifest.defaultWorld2D == "worlds/main2d.fuselevel" &&
                   manifest.manifest.defaultWorld3D.empty(),
               "project: project.json written by the manifest writer");

    // Edit the 2D world, save, reopen the project: the entity and the 2D dimension survive.
    ecs::Registry& registry = host.editorScene().registry();
    const ecs::EntityID sprite = registry.create();
    ecs::Transform t{};
    t.position = {3.f, 4.f, 0.f, 1.f};
    registry.add(sprite, t);
    host.runtimeScene().addEntity("sprite");
    host.postFromUi(editor::makeSaveSceneCommand());
    tick(host);
    expectTrue(host.lastFileResult().ok, "project: save the 2D world");

    host.postFromUi(editor::makeNewProjectCommand(projectDir, "again"));
    tick(host);
    expectTrue(!host.lastFileResult().ok, "project: NewProject refuses to overwrite an existing project");

    editor::EditorHost other;
    other.postFromUi(editor::makeOpenProjectCommand(projectDir));
    tick(other);
    expectTrue(other.lastFileResult().ok && other.lastFileResult().command == editor::CommandKind::OpenProject,
               "project: OpenProject command");
    expectTrue(other.currentSceneDimension() == scene::SceneDimension::World2D &&
                   other.editorScene().registry().alive(sprite) &&
                   other.editorScene().registry().get<ecs::Transform>(sprite)->position.y == 4.f &&
                   other.runtimeScene().entityCount() == 1u,
               "project: reopened 2D world has the saved entity");
    expectTrue(other.loadedProject() == "New Game", "project: loaded project label");

    // 3D + 2D project opens the 3D world.
    editor::EditorHost both;
    both.postFromUi(editor::makeNewProjectCommand((dir / "Both").string(), "Both"));
    tick(both);
    expectTrue(both.lastFileResult().ok && both.currentSceneDimension() == scene::SceneDimension::World3D &&
                   fs::exists(dir / "Both" / "worlds" / "main2d.fuselevel") &&
                   fs::exists(dir / "Both" / "worlds" / "main3d.fuselevel"),
               "project: 3D+2D project writes both worlds and opens the 3D one");
    both.postFromUi(editor::makeOpenProjectCommand((dir / "nothing_here").string()));
    tick(both);
    expectTrue(!both.lastFileResult().ok && both.hasProject() && both.projectManifest().name == "Both",
               "project: opening a missing project fails and keeps the current one");

    // NewScene 2D -> SaveAs -> reopen keeps 2D.
    both.postFromUi(editor::makeNewSceneCommand(true, "level2d"));
    both.postFromUi(editor::makeSaveSceneAsCommand((dir / "Both" / "worlds" / "extra2d.fuselevel").string()));
    both.postFromUi(editor::makeNewSceneCommand(false));
    both.postFromUi(editor::makeOpenSceneCommand((dir / "Both" / "worlds" / "extra2d.fuselevel").string()));
    tick(both);
    expectTrue(both.lastFileResult().ok && both.currentSceneDimension() == scene::SceneDimension::World2D &&
                   both.runtimeScene().name() == "level2d",
               "project: 2D scene dimension survives save / load");
}

} // namespace

int main() {
    const fs::path dir = fuse::test::makeUniqueTempDir("fuse_e15_scene_files");
    testManifestWriterRoundTrip(dir);
    testSerialiserV3(dir);
    testEditorHostSaveLoad(dir);
    testProjectCommands(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_editor_e15_scene_files: all checks passed\n");
    return 0;
}
