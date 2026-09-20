#include <fuse/core/init.hpp>
#include <fuse/scene/mission_load.hpp>
#include <fuse/scene/scene.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef FUSE_GOLDEN_MIS_PATH
#define FUSE_GOLDEN_MIS_PATH ""
#endif

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

const fuse::scene::MissionObject* findByName(
    fuse::scene::MissionLoadResult& result, const std::string& name) {
    const fuse::scene::MissionObject* found = nullptr;
    result.objects.forEachOccupied([&](fuse::Handle<fuse::scene::MissionObject> handle) {
        const fuse::scene::MissionObject* object = result.objects.get(handle);
        if (object != nullptr && object->objectName == name) {
            found = object;
        }
    });
    return found;
}

void populateFromMission(fuse::scene::Scene& scene, const fuse::scene::MissionLoadResult& mission) {
    if (!mission.worldName.empty()) {
        scene.setName(mission.worldName);
    }
    scene.clearEntities();
    mission.objects.forEachOccupied([&](fuse::Handle<fuse::scene::MissionObject> handle) {
        const fuse::scene::MissionObject* object = mission.objects.get(handle);
        if (object == nullptr) {
            return;
        }
        const std::string& name = object->objectName.empty() ? object->className : object->objectName;
        scene.addEntity(name);
    });
}

void testMissingMissionFile() {
    const fuse::scene::MissionLoadResult result =
        fuse::scene::loadMissionToHandleMap("__fuse_missing_mission__.mis");
    expectTrue(!result.ok, "missing mission file is not ok");
    expectTrue(result.objects.size() == 0u, "missing mission inserts no objects");
    expectTrue(!result.error.empty(), "missing mission reports an error");
}

void testGoldenMissionLoad() {
    fuse::scene::MissionLoadResult result =
        fuse::scene::loadMissionToHandleMap(FUSE_GOLDEN_MIS_PATH);

    expectTrue(result.ok, "golden .mis load ok");
    expectTrue(result.worldName == "ExampleLevel", "worldName is ExampleLevel");
    expectTrue(result.objects.size() >= 4u, "example.mis extracts four SimObject stubs");
    expectTrue(result.handles.size() == result.objects.size(), "one handle per inserted object");

    bool allGetsWork = true;
    bool allHandlesMatch = true;
    bool anyNamed = false;
    for (const fuse::Handle<fuse::scene::MissionObject>& handle : result.handles) {
        const fuse::scene::MissionObject* object = result.objects.get(handle);
        if (object == nullptr) {
            allGetsWork = false;
            continue;
        }
        if (!result.objects.valid(handle) || object->handle != handle) {
            allHandlesMatch = false;
        }
        if (!object->objectName.empty() || !object->className.empty()) {
            anyNamed = true;
        }
    }
    expectTrue(allGetsWork, "get(handle) is non-null for every loaded handle");
    expectTrue(allHandlesMatch, "stored Handle matches HandleMap slot");
    expectTrue(anyNamed, "objectName or className non-empty for at least one object");

    const fuse::scene::MissionObject* exampleLevel = findByName(result, "ExampleLevel");
    const fuse::scene::MissionObject* floor = findByName(result, "Floor");
    expectTrue(exampleLevel != nullptr, "ExampleLevel exists in HandleMap");
    expectTrue(floor != nullptr, "Floor exists in HandleMap");
    if (exampleLevel != nullptr) {
        expectTrue(exampleLevel->className == "Scene", "ExampleLevel class is Scene");
        expectTrue(result.objects.valid(exampleLevel->handle), "ExampleLevel handle is valid");
    }
    if (floor != nullptr) {
        expectTrue(floor->className == "GroundPlane", "Floor class is GroundPlane");
        expectTrue(result.objects.valid(floor->handle), "Floor handle is valid");
    }

    fuse::scene::MissionLoadResult sceneLoad =
        fuse::scene::loadMissionToHandleMap(FUSE_GOLDEN_MIS_PATH);
    expectTrue(sceneLoad.ok, "second parse of the same .mis is ok");

    fuse::scene::Scene scene;
    populateFromMission(scene, sceneLoad);
    expectTrue(scene.entityCount() == static_cast<fuse::u32>(result.handles.size()),
               "entityCount matches handle count when both parse the same file");
    expectTrue(scene.entityCount() == result.objects.size(),
               "entityCount matches HandleMap size when both parse the same file");

    bool allEntitiesPresent = true;
    for (fuse::u32 i = 0; i < scene.entityCount(); ++i) {
        if (scene.entityAt(i) == nullptr) {
            allEntitiesPresent = false;
        }
    }
    expectTrue(allEntitiesPresent, "entityAt(index) is non-null for every loaded entity");

    expectTrue(!result.handles.empty(), "loaded handles available for UAF check");
    if (!result.handles.empty()) {
        const fuse::Handle<fuse::scene::MissionObject> stale = result.handles.front();
        expectTrue(result.objects.get(stale) != nullptr, "handle resolves before remove");
        result.objects.remove(stale);
        expectTrue(result.objects.get(stale) == nullptr, "get after remove is null (generation check)");
        expectTrue(!result.objects.valid(stale), "removed handle is invalid");
    }
}

} // namespace

int main() {
    fuse::core::initialize();

    testMissingMissionFile();
    testGoldenMissionLoad();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_p4_golden_mis: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_p4_golden_mis: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
