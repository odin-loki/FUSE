#include <fuse/core/init.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/scene/serialiser.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

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

void testConvertT3DMissionHierarchy() {
    const std::string mission = writeTempFile(
        "/tmp/fuse_convert_hierarchy.mis",
        "new Scene(ExampleLevel) {\n"
        "   new SimGroup(CameraSpawnPoints) {\n"
        "      new SpawnSphere(DefaultCameraSpawnSphere) {\n"
        "         position = \"0 0 10\";\n"
        "      };\n"
        "   };\n"
        "   new GroundPlane() {\n"
        "      position = \"0 0 0\";\n"
        "   };\n"
        "};\n");

    const std::string output = "/tmp/fuse_convert_hierarchy.fuselevel";
    const fuse::project::ConvertResult result =
        fuse::project::convertT3DMissionToFuselevel(mission, output);

    expectTrue(result.status == fuse::project::ConvertStatus::Ok, "hierarchy mission convert ok");
    expectTrue(result.entityCount >= 2u, "hierarchy mission produced nested entities");

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult loadResult = fuse::scene::SceneSerialiser::load(output, loaded);
    expectTrue(loadResult.status == fuse::scene::SerialiseStatus::Ok, "hierarchy fuselevel loads");

    bool foundChildWithParent = false;
    for (const fuse::scene::SceneEntity& entity : loaded.entities()) {
        if (entity.name == "DefaultCameraSpawnSphere" && entity.parentIndex >= 0) {
            foundChildWithParent = true;
            expectTrue(loaded.entities()[static_cast<std::size_t>(entity.parentIndex)].name ==
                           "CameraSpawnPoints",
                       "spawn sphere parent is SimGroup");
        }
    }
    expectTrue(foundChildWithParent, "hierarchy parent indices preserved in fuselevel v2");
}

void testConvertT3DMissionToFuselevel() {
    const std::string mission = writeTempFile(
        "/tmp/fuse_convert_mission.mis",
        "new Scene(ExampleLevel) {\n"
        "   new GroundPlane() {\n"
        "      position = \"1 2 3\";\n"
        "      rotation = \"0 0 0 1\";\n"
        "      scale = \"2 2 2\";\n"
        "   };\n"
        "};\n");

    const std::string output = "/tmp/fuse_convert_mission.fuselevel";
    const fuse::project::ConvertResult result =
        fuse::project::convertT3DMissionToFuselevel(mission, output);

    expectTrue(result.status == fuse::project::ConvertStatus::Ok, "mission convert ok");
    expectTrue(result.entityCount >= 1u, "mission convert produced entities");

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult loadResult = fuse::scene::SceneSerialiser::load(output, loaded);
    expectTrue(loadResult.status == fuse::scene::SerialiseStatus::Ok, "converted fuselevel loads");
    expectTrue(loaded.name() == "ExampleLevel", "scene name preserved");
    expectTrue(loaded.entityCount() >= 1u, "loaded scene has entities");
}

void testConvertT2DModuleToFuselevel() {
    const std::string module = writeTempFile("/tmp/fuse_convert_module.cs", "module \"SpriteToy\";\n");
    const std::string output = "/tmp/fuse_convert_module.fuselevel";

    const fuse::project::ConvertResult result =
        fuse::project::convertT2DModuleToFuselevel(module, output);

    expectTrue(result.status == fuse::project::ConvertStatus::Ok, "module convert ok");
    expectTrue(result.entityCount == 1u, "module convert produced root entity");

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult loadResult = fuse::scene::SceneSerialiser::load(output, loaded);
    expectTrue(loadResult.status == fuse::scene::SerialiseStatus::Ok, "converted module fuselevel loads");
    expectTrue(loaded.name() == "SpriteToy", "module scene name preserved");
}

} // namespace

int main() {
    fuse::core::initialize();
    testConvertT3DMissionHierarchy();
    testConvertT3DMissionToFuselevel();
    testConvertT2DModuleToFuselevel();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_world_converter_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_world_converter_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
