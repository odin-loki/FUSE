#include <fuse/core/init.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/scene/project_io.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/scene/scene_snapshot.hpp>
#include <fuse/scene/serialiser.hpp>

#include <fuse/types.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::string tempDir() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "fuse_scene_b37_test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

void writeProjectJson(const std::string& projectDir) {
    const std::string json = R"({
  "schemaVersion": 1,
  "name": "scene_serial_test",
  "dimensions": { "enable3D": true, "enable2D": false, "enableUI": false },
  "modules": { "ai": false, "cinematics": false, "fx": false, "mechanics": false, "adventure": false },
  "defaultWorld3D": "worlds/test_level.fuselevel",
  "defaultWorld2D": ""
})";

    std::ofstream out(projectDir + "/project.json");
    out << json;
}

void testSceneRoundTrip() {
    const std::string path = tempDir() + "/roundtrip.fuselevel";

    fuse::scene::Scene scene("TestScene");
    scene.camera().setPosition(1.f, 2.f, 3.f);
    scene.camera().setOrientation(30.f, -5.f);
    scene.camera().fovDeg = 90.f;
    scene.camera().isActive = true;
    scene.addObjectName("mesh_a");
    scene.addObjectName("mesh_b");

    const fuse::scene::SerialiseResult saved = fuse::scene::SceneSerialiser::save(scene, path);
    expectTrue(saved.status == fuse::scene::SerialiseStatus::Ok, "scene save ok");

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult loadedResult = fuse::scene::SceneSerialiser::load(path, loaded);
    expectTrue(loadedResult.status == fuse::scene::SerialiseStatus::Ok, "scene load ok");
    expectTrue(loaded.name() == "TestScene", "scene name round-trip");
    expectTrue(loaded.entityCount() == 2u, "entity count round-trip");
    expectTrue(loaded.entities()[0].name == "mesh_a", "first entity name");
    expectTrue(loaded.entities()[1].name == "mesh_b", "second entity name");
    expectTrue(loaded.camera().matricesValid(), "camera matrices valid after load");
    expectTrue(loaded.camera().fovDeg == 90.f, "camera fov round-trip");
    expectTrue(loaded.camera().isActive, "camera active flag round-trip");
}

void testEntityTransformRoundTrip() {
    const std::string path = tempDir() + "/entity_transform.fuselevel";

    fuse::scene::SceneEntityTransform transformA{};
    transformA.positionX = 1.f;
    transformA.positionY = 2.f;
    transformA.positionZ = 3.f;
    transformA.rotationY = 0.70710677f;
    transformA.rotationW = 0.70710677f;
    transformA.scaleX = 2.f;
    transformA.scaleY = 2.f;
    transformA.scaleZ = 2.f;

    fuse::scene::SceneEntityTransform transformB{};
    transformB.positionX = -4.f;
    transformB.positionY = 0.5f;
    transformB.positionZ = 8.f;
    transformB.scaleZ = 0.5f;

    fuse::scene::Scene scene("EntityScene");
    scene.addEntity("prop_a", transformA);
    scene.addEntity("prop_b", transformB);

    expectTrue(fuse::scene::SceneSerialiser::save(scene, path).status == fuse::scene::SerialiseStatus::Ok,
               "entity scene save ok");

    fuse::scene::Scene loaded;
    expectTrue(fuse::scene::SceneSerialiser::load(path, loaded).status == fuse::scene::SerialiseStatus::Ok,
               "entity scene load ok");
    expectTrue(loaded.entityCount() == 2u, "entity transform count round-trip");
    expectTrue(loaded.entities()[0].transform.positionX == 1.f, "entity a position x");
    expectTrue(loaded.entities()[0].transform.scaleX == 2.f, "entity a scale x");
    expectTrue(loaded.entities()[1].transform.positionX == -4.f, "entity b position x");
    expectTrue(loaded.entities()[1].transform.scaleZ == 0.5f, "entity b scale z");
}

void testSceneSnapshotRoundTrip() {
    fuse::scene::Scene scene("SnapshotScene");
    scene.camera().setPosition(0.f, 1.f, 5.f);
    scene.camera().update();

    fuse::scene::SceneEntityTransform transform{};
    transform.positionX = 10.f;
    transform.positionY = -2.f;
    transform.positionZ = 0.25f;
    scene.addEntity("crate", transform);

    const fuse::scene::SceneSnapshot snapshot = scene.captureSnapshot();

    scene.setName("Mutated");
    scene.clearEntities();
    scene.camera().setPosition(99.f, 99.f, 99.f);

    snapshot.apply(scene);
    expectTrue(scene.name() == "SnapshotScene", "snapshot restores scene name");
    expectTrue(scene.entityCount() == 1u, "snapshot restores entity count");
    expectTrue(scene.entities()[0].name == "crate", "snapshot restores entity name");
    expectTrue(scene.entities()[0].transform.positionX == 10.f, "snapshot restores transform");
    expectTrue(scene.camera().positionZ == 5.f, "snapshot restores camera");
}

void testInvalidMagicRejected() {
    const std::string path = tempDir() + "/bad_magic.fuselevel";
    std::vector<fuse::u8> header(64u, 0);
    const fuse::u32 badMagic = 0xBAD0BAD0u;
    std::memcpy(header.data(), &badMagic, sizeof(badMagic));
    header[24] = 'C';

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(header.data()),
                  static_cast<std::streamsize>(header.size()));
        out.flush();
        expectTrue(out.good(), "bad magic fixture written");
    }
    expectTrue(std::filesystem::file_size(path) >= header.size(), "bad magic fixture size");

    fuse::scene::Scene scene;
    const fuse::scene::SerialiseResult result = fuse::scene::SceneSerialiser::load(path, scene);
    expectTrue(result.status == fuse::scene::SerialiseStatus::InvalidMagic, "invalid magic rejected");
}

void testProjectSceneIo() {
    const std::string projectDir = tempDir() + "/project_io";
    std::filesystem::create_directories(projectDir);
    writeProjectJson(projectDir);

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectDir);
    expectTrue(project.status == fuse::project::LoadStatus::Ok, "project.json loads");

    const std::string expectedWorldPath = fuse::scene::resolveDefaultWorldPath(project.manifest);
    expectTrue(expectedWorldPath.find("worlds/test_level.fuselevel") != std::string::npos,
               "default world path resolved");

    fuse::scene::Scene scene("ProjectScene");
    scene.camera().setPosition(0.f, 5.f, 20.f);
    scene.camera().update();
    scene.addObjectName("root_mesh");

    const fuse::scene::SerialiseResult saved = fuse::scene::saveForProject(scene, project.manifest);
    expectTrue(saved.status == fuse::scene::SerialiseStatus::Ok, "saveForProject ok");
    expectTrue(std::filesystem::exists(expectedWorldPath), "world file written beside project");

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult loadedResult = fuse::scene::loadForProject(loaded, project);
    expectTrue(loadedResult.status == fuse::scene::SerialiseStatus::Ok, "loadForProject ok");
    expectTrue(loaded.name() == "ProjectScene", "project scene name round-trip");
    expectTrue(loaded.objectCount() == 1u, "project scene object count");
    expectTrue(loaded.camera().positionZ == 20.f, "project scene camera z round-trip");
}

void testLoadForProjectFromLoadResult() {
    const std::string projectDir = tempDir() + "/project_load_result";
    std::filesystem::create_directories(projectDir);
    writeProjectJson(projectDir);

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectDir);

    fuse::scene::Scene scene("ViaLoadResult");
    scene.addObjectName("prop");
    expectTrue(fuse::scene::saveForProject(scene, project.manifest).status ==
                   fuse::scene::SerialiseStatus::Ok,
               "seed project world");

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult result = fuse::scene::loadForProject(loaded, project);
    expectTrue(result.status == fuse::scene::SerialiseStatus::Ok, "loadForProject(LoadResult)");
    expectTrue(loaded.name() == "ViaLoadResult", "loaded scene via LoadResult wrapper");
}

} // namespace

int test_scene_serialiser_main() {
    fuse::core::initialize();
    testSceneRoundTrip();
    testEntityTransformRoundTrip();
    testSceneSnapshotRoundTrip();
    testInvalidMagicRejected();
    testProjectSceneIo();
    testLoadForProjectFromLoadResult();
    fuse::core::shutdown();
    return g_failures;
}
