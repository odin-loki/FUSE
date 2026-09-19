#include <fuse/core/init.hpp>
#include <fuse/ecs/components/mesh.hpp>
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
        "/tmp/fuse_wire_bind.mis",
        "new Scene(ExampleLevel) {\n"
        "   new GroundPlane() {\n"
        "      MaterialAsset = \"Prototyping:FloorGray\";\n"
        "   };\n"
        "   new SpawnSphere(DefaultCameraSpawnSphere) {\n"
        "      dataBlock = \"SpawnSphereMarker\";\n"
        "   };\n"
        "};\n");
    const std::string fuselevel = "/tmp/fuse_wire_bind.fuselevel";
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
    testApplyWireBindingsToEcs();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_scene_wire_runtime_bind_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_scene_wire_runtime_bind_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
