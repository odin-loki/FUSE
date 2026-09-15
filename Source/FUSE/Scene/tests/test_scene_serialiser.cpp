#include <fuse/core/init.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/scene/project_io.hpp>
#include <fuse/scene/scene.hpp>
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
    expectTrue(loaded.objectCount() == 2u, "object count round-trip");
    expectTrue(loaded.objectNames()[0] == "mesh_a", "first object name");
    expectTrue(loaded.objectNames()[1] == "mesh_b", "second object name");
    expectTrue(loaded.camera().matricesValid(), "camera matrices valid after load");
    expectTrue(loaded.camera().fovDeg == 90.f, "camera fov round-trip");
    expectTrue(loaded.camera().isActive, "camera active flag round-trip");
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
    testInvalidMagicRejected();
    testProjectSceneIo();
    testLoadForProjectFromLoadResult();
    fuse::core::shutdown();
    return g_failures;
}
