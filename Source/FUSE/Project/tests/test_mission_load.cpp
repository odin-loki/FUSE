#include <fuse/core/init.hpp>
#include <fuse/project/mission_load.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef FUSE_SOURCE_DIR
#define FUSE_SOURCE_DIR ""
#endif

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::string joinPath(const std::string& root, const std::string& relative) {
    if (root.empty()) {
        return relative;
    }
    const char last = root.back();
    if (last == '/' || last == '\\') {
        return root + relative;
    }
    return root + "/" + relative;
}

std::string resolveSourceDir(int argc, char** argv) {
    if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
        return argv[1];
    }
    return FUSE_SOURCE_DIR;
}

void testMissingMissionFile() {
    const fuse::project::MissionLoadResult result =
        fuse::project::loadMissionFile("__fuse_missing_mission__.mis");
    expectTrue(!result.ok, "missing mission file is not ok");
    expectTrue(result.objects.size() == 0u, "missing mission inserts no objects");
    expectTrue(!result.error.empty(), "missing mission reports an error");
}

void testGoldenMissionLoad(const std::string& sourceDir) {
    const std::string path =
        joinPath(sourceDir, "Samples/unification/demo_3d_empty/worlds/example.mis");
    fuse::project::MissionLoadResult result = fuse::project::loadMissionFile(path);

    expectTrue(result.ok, "golden .mis load ok");
    expectTrue(result.worldName == "ExampleLevel", "worldName is ExampleLevel");
    expectTrue(result.objects.size() >= 3u, "at least three sim objects loaded");
    expectTrue(result.handles.size() >= 3u, "handles stored for each sim object");
    expectTrue(!result.roots.empty(), "root handles recorded");
    expectTrue(result.roots.size() == 1u, "golden mission has one root");

    const fuse::project::MissionObject* exampleLevel = nullptr;
    const fuse::project::MissionObject* cameraGroup = nullptr;
    const fuse::project::MissionObject* floor = nullptr;
    const fuse::project::MissionObject* spawn = nullptr;
    bool allGetsWork = true;
    result.objects.forEachOccupied([&](fuse::Handle<fuse::project::MissionObject> handle) {
        const fuse::project::MissionObject* object = result.objects.get(handle);
        if (object == nullptr) {
            allGetsWork = false;
            return;
        }
        if (object->name == "ExampleLevel") {
            exampleLevel = object;
        }
        if (object->name == "CameraSpawnPoints") {
            cameraGroup = object;
        }
        if (object->name == "Floor") {
            floor = object;
        }
        if (object->name == "DefaultCameraSpawnSphere") {
            spawn = object;
        }
    });

    expectTrue(allGetsWork, "get(handle) works after load");
    expectTrue(exampleLevel != nullptr, "ExampleLevel found via handle walk");
    expectTrue(floor != nullptr, "Floor found via handle walk");
    expectTrue(spawn != nullptr, "DefaultCameraSpawnSphere found via handle walk");
    expectTrue(cameraGroup != nullptr, "CameraSpawnPoints found via handle walk");

    if (exampleLevel != nullptr && !result.roots.empty()) {
        expectTrue(!result.objects.valid(exampleLevel->parent), "ExampleLevel has no parent");
        const fuse::project::MissionObject* rootObject = result.objects.get(result.roots.front());
        expectTrue(rootObject != nullptr && rootObject->name == "ExampleLevel",
                   "root handle is ExampleLevel");
    }
    if (cameraGroup != nullptr) {
        const fuse::project::MissionObject* parent = result.objects.get(cameraGroup->parent);
        expectTrue(parent != nullptr && parent->name == "ExampleLevel",
                   "CameraSpawnPoints parent is ExampleLevel");
    }
    if (spawn != nullptr) {
        const fuse::project::MissionObject* parent = result.objects.get(spawn->parent);
        expectTrue(parent != nullptr && parent->name == "CameraSpawnPoints",
                   "DefaultCameraSpawnSphere parent is CameraSpawnPoints");
        expectTrue(spawn->datablockRef == "SpawnSphereMarker",
                   "DefaultCameraSpawnSphere datablockRef");
    }
    if (floor != nullptr) {
        const fuse::project::MissionObject* parent = result.objects.get(floor->parent);
        expectTrue(parent != nullptr && parent->name == "ExampleLevel",
                   "Floor parent is ExampleLevel");
    }

    expectTrue(!result.handles.empty(), "loaded handles available for UAF check");
    if (!result.handles.empty()) {
        const fuse::Handle<fuse::project::MissionObject> stale = result.handles.front();
        expectTrue(result.objects.get(stale) != nullptr, "handle resolves before remove");
        result.objects.remove(stale);
        expectTrue(result.objects.get(stale) == nullptr, "get after remove is null (generation check)");
        expectTrue(!result.objects.valid(stale), "removed handle is invalid");
    }
}

} // namespace

int main(int argc, char** argv) {
    fuse::core::initialize();

    testMissingMissionFile();
    testGoldenMissionLoad(resolveSourceDir(argc, argv));

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_mission_load_p4: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_mission_load_p4: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
