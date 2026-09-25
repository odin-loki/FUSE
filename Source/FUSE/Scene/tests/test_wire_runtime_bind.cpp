#include <fuse/core/temp_path.hpp>
#include <fuse/core/init.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/spawn_marker.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/scene/serialiser.hpp>
#include <fuse/scene/wire_runtime_bind.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::string writeTempFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary);
    out << contents;
    return path;
}

void testLegacyTableFromConvertedMission() {
    const std::string mission = writeTempFile(
        fuse::test::tempPath("fuse_wire_bind.mis"),
        "new Scene(ExampleLevel) {\n"
        "   new GroundPlane() {\n"
        "      MaterialAsset = \"Prototyping:FloorGray\";\n"
        "   };\n"
        "   new SpawnSphere(DefaultCameraSpawnSphere) {\n"
        "      dataBlock = \"SpawnSphereMarker\";\n"
        "   };\n"
        "};\n");
    const std::string fuselevel = fuse::test::tempPath("fuse_wire_bind.fuselevel");
    const fuse::project::ConvertResult converted =
        fuse::project::convertT3DMissionToFuselevel(mission, fuselevel);
    expectTrue(converted.status == fuse::project::ConvertStatus::Ok, "mission converts for wire bind");

    fuse::scene::Scene scene;
    const fuse::scene::SerialiseResult loaded = fuse::scene::SceneSerialiser::load(fuselevel, scene);
    expectTrue(loaded.status == fuse::scene::SerialiseStatus::Ok, "fuselevel loads for wire bind");

    fuse::scene::LegacyDatablockTable table;
    const fuse::scene::WireRuntimeBindResult populated =
        fuse::scene::populateLegacyTableFromScene(scene, table);
    expectTrue(populated.wireStubCount >= 2u, "wire stubs counted");
    expectTrue(populated.datablockEntries >= 1u, "datablock entry populated");
    expectTrue(populated.materialEntries >= 1u, "material entry populated");

    fuse::scene::LegacyDatablockEntry material{};
    expectTrue(table.findMaterialByOwner("GroundPlane", &material), "material owner lookup");
    expectTrue(material.refName.find("FloorGray") != std::string::npos, "material ref preserved");

    fuse::scene::LegacyDatablockEntry datablock{};
    expectTrue(table.findDatablockByOwner("DefaultCameraSpawnSphere", &datablock) ||
                   table.findDatablockByOwner("SpawnSphere", &datablock),
               "datablock owner lookup");
}

void testApplyDatablockSpawnBinding() {
    fuse::ecs::Registry registry;
    registry.init();

    const fuse::ecs::EntityID spawn = registry.create();
    registry.add<fuse::ecs::Transform>(spawn);

    std::unordered_map<std::string, fuse::ecs::EntityID> entitiesByName;
    entitiesByName.emplace("DefaultCameraSpawnSphere", spawn);

    fuse::scene::LegacyDatablockTable table;
    table.datablocks.push_back({"DefaultCameraSpawnSphere", "SpawnSphereMarker"});

    const fuse::scene::WireRuntimeBindResult applied =
        fuse::scene::applyWireBindingsToEcs(registry, entitiesByName, table);
    expectTrue(applied.ecsSpawnApplied == 1u, "datablock wire applied to ECS spawn marker");
    expectTrue(applied.ecsDatablockResolved == 1u, "datablock resolved count matches spawn bind");

    const fuse::ecs::SpawnMarker* marker = registry.get<fuse::ecs::SpawnMarker>(spawn);
    expectTrue(marker != nullptr, "spawn marker component readable");
    expectTrue(marker->datablock_id == fuse::scene::hashWireRefName("SpawnSphereMarker"),
               "spawn marker datablock id hashed from wire ref");
    expectTrue(marker->active, "spawn marker active by default");
}

void testApplyWireBindingsFromScene() {
    const std::string mission = writeTempFile(
        fuse::test::tempPath("fuse_wire_bind_scene.mis"),
        "new Scene(ExampleLevel) {\n"
        "   new SpawnSphere(DefaultCameraSpawnSphere) {\n"
        "      dataBlock = \"SpawnSphereMarker\";\n"
        "   };\n"
        "};\n");
    const std::string fuselevel = fuse::test::tempPath("fuse_wire_bind_scene.fuselevel");
    const fuse::project::ConvertResult converted =
        fuse::project::convertT3DMissionToFuselevel(mission, fuselevel);
    expectTrue(converted.status == fuse::project::ConvertStatus::Ok, "mission converts for scene bind");

    fuse::scene::Scene scene;
    const fuse::scene::SerialiseResult loaded = fuse::scene::SceneSerialiser::load(fuselevel, scene);
    expectTrue(loaded.status == fuse::scene::SerialiseStatus::Ok, "fuselevel loads for scene bind");

    fuse::ecs::Registry registry;
    registry.init();
    const fuse::scene::WireRuntimeBindResult applied =
        fuse::scene::applyWireBindingsFromScene(registry, scene);
    expectTrue(applied.ecsSpawnApplied >= 1u, "scene bind applies spawn marker");
    expectTrue(applied.datablockEntries >= 1u, "scene bind retains datablock entries");
}

void testApplyWireBindingsToEcs() {
    fuse::ecs::Registry registry;
    registry.init();

    const fuse::ecs::EntityID ground = registry.create();
    registry.add<fuse::ecs::Transform>(ground);
    registry.add<fuse::ecs::Mesh>(ground);

    std::unordered_map<std::string, fuse::ecs::EntityID> entitiesByName;
    entitiesByName.emplace("GroundPlane", ground);

    fuse::scene::LegacyDatablockTable table;
    table.materials.push_back({"GroundPlane", "Prototyping:FloorGray"});

    const fuse::scene::WireRuntimeBindResult applied =
        fuse::scene::applyWireBindingsToEcs(registry, entitiesByName, table);
    expectTrue(applied.ecsMaterialApplied == 1u, "material wire applied to ECS mesh");

    const fuse::ecs::Mesh* mesh = registry.get<fuse::ecs::Mesh>(ground);
    expectTrue(mesh != nullptr, "mesh component readable");
    expectTrue(mesh->material_id == fuse::scene::hashWireRefName("Prototyping:FloorGray"),
               "material id hashed from wire ref");
}

} // namespace

int main() {
    fuse::core::initialize();
    testLegacyTableFromConvertedMission();
    testApplyDatablockSpawnBinding();
    testApplyWireBindingsFromScene();
    testApplyWireBindingsToEcs();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_scene_wire_runtime_bind_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_scene_wire_runtime_bind_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
