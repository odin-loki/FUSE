#include <fuse/core/init.hpp>
#include <fuse/project/t2d_module_bridge.hpp>
#include <fuse/project/t3d_datablock_resolve.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/scene/serialiser.hpp>
#include <fuse/world2d/world_2d.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
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
    const std::string module = writeTempFile(
        "/tmp/fuse_convert_module.cs",
        R"(module "SpriteToy";
new SceneToy() {
  new SpritePlayer(Player) {
    position = "0 0";
  };
};)");
    const std::string output = "/tmp/fuse_convert_module.fuselevel";

    const fuse::project::ConvertResult result =
        fuse::project::convertT2DModuleToFuselevel(module, output);

    expectTrue(result.status == fuse::project::ConvertStatus::Ok, "module convert ok");
    expectTrue(result.entityCount >= 2u, "module convert produced toybox scene nodes");

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult loadResult = fuse::scene::SceneSerialiser::load(output, loaded);
    expectTrue(loadResult.status == fuse::scene::SerialiseStatus::Ok, "converted module fuselevel loads");
    expectTrue(loaded.name() == "SpriteToy", "module scene name preserved");

    bool foundChildWithParent = false;
    for (const fuse::scene::SceneEntity& entity : loaded.entities()) {
        if (entity.name == "Player" && entity.parentIndex >= 0) {
            foundChildWithParent = true;
        }
    }
    expectTrue(foundChildWithParent, "t2d toybox hierarchy preserved in fuselevel");
}

void testConvertT3DDatablockWiringStubs() {
    const std::string mission = writeTempFile(
        "/tmp/fuse_convert_wiring.mis",
        "new Scene(ExampleLevel) {\n"
        "   new GroundPlane() {\n"
        "      MaterialAsset = \"Prototyping:FloorGray\";\n"
        "   };\n"
        "   new SpawnSphere(DefaultCameraSpawnSphere) {\n"
        "      dataBlock = \"SpawnSphereMarker\";\n"
        "   };\n"
        "};\n");

    const std::string output = "/tmp/fuse_convert_wiring.fuselevel";
    const fuse::project::ConvertResult result =
        fuse::project::convertT3DMissionToFuselevel(mission, output);

    expectTrue(result.status == fuse::project::ConvertStatus::Ok, "wiring mission convert ok");
    expectTrue(result.wiringStubCount >= 2u, "datablock/material wiring stubs emitted");

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult loadResult = fuse::scene::SceneSerialiser::load(output, loaded);
    expectTrue(loadResult.status == fuse::scene::SerialiseStatus::Ok, "wiring fuselevel loads");

    fuse::u32 wireCount = 0;
    for (const fuse::scene::SceneEntity& entity : loaded.entities()) {
        if (entity.name.rfind("__fuse.wire|", 0) == 0) {
            ++wireCount;
        }
    }
    expectTrue(wireCount >= 2u, "wire stub entities round-trip in fuselevel");
}

void testConvertT2DModuleHierarchy() {
    const std::string module = writeTempFile(
        "/tmp/fuse_convert_t2d_hierarchy.cs",
        "module \"SpriteToy\";\n"
        "new SceneToy() {\n"
        "  new SpritePlayer(Player) {\n"
        "    position = \"10 20\";\n"
        "  };\n"
        "};\n");
    const std::string output = "/tmp/fuse_convert_t2d_hierarchy.fuselevel";

    const fuse::project::ConvertResult result =
        fuse::project::convertT2DModuleToFuselevel(module, output);

    expectTrue(result.status == fuse::project::ConvertStatus::Ok, "t2d hierarchy convert ok");
    expectTrue(result.entityCount >= 2u, "t2d hierarchy produced nested entities");

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult loadResult = fuse::scene::SceneSerialiser::load(output, loaded);
    expectTrue(loadResult.status == fuse::scene::SerialiseStatus::Ok, "t2d hierarchy fuselevel loads");

    bool foundChildWithParent = false;
    for (const fuse::scene::SceneEntity& entity : loaded.entities()) {
        if (entity.name == "Player" && entity.parentIndex >= 0) {
            foundChildWithParent = true;
            expectTrue(loaded.entities()[static_cast<std::size_t>(entity.parentIndex)].name == "SceneToy",
                       "sprite player parent is SceneToy");
        }
    }
    expectTrue(foundChildWithParent, "t2d hierarchy parent indices preserved");
}

void testT3DDatablockResolveFromMission() {
    const std::string mission = writeTempFile(
        "/tmp/fuse_t3d_resolve.mis",
        "new Scene(ExampleLevel) {\n"
        "   new GroundPlane(Floor) {\n"
        "      MaterialAsset = \"Prototyping:FloorGray\";\n"
        "   };\n"
        "   new SpawnSphere(DefaultCameraSpawnSphere) {\n"
        "      dataBlock = \"SpawnSphereMarker\";\n"
        "   };\n"
        "};\n");

    const std::string text = [&]() {
        std::ifstream in(mission);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }();

    const fuse::project::T3DMissionExtract extract = fuse::project::extractT3DMissionFields(text);
    const fuse::project::T3DDatablockResolveResult resolved =
        fuse::project::resolveT3DMissionBindings(extract);

    expectTrue(resolved.ownerLinked >= 2u, "mission resolve linked owners");
    expectTrue(resolved.datablockCount >= 1u, "datablock resolved");
    expectTrue(resolved.materialCount >= 1u, "material resolved");
    expectTrue(resolved.bindings[0].resolvedId != 0u, "resolved id hashed");
}

void testT3DDatablockResolveFromScene() {
    const std::string mission = writeTempFile(
        "/tmp/fuse_t3d_resolve_scene.mis",
        "new Scene(ExampleLevel) {\n"
        "   new SpawnSphere(DefaultCameraSpawnSphere) {\n"
        "      dataBlock = \"SpawnSphereMarker\";\n"
        "   };\n"
        "};\n");
    const std::string output = "/tmp/fuse_t3d_resolve_scene.fuselevel";
    const fuse::project::ConvertResult converted =
        fuse::project::convertT3DMissionToFuselevel(mission, output);
    expectTrue(converted.status == fuse::project::ConvertStatus::Ok, "mission converts for resolve");

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult loadResult = fuse::scene::SceneSerialiser::load(output, loaded);
    expectTrue(loadResult.status == fuse::scene::SerialiseStatus::Ok, "fuselevel loads for resolve");

    const fuse::project::T3DDatablockResolveResult resolved =
        fuse::project::resolveT3DBindingsFromScene(loaded);
    expectTrue(resolved.datablockCount >= 1u, "scene wire resolve datablock");
}

void testT2DModuleRuntimeBridgeLayersPhysicsComposite() {
    const std::string module = writeTempFile(
        "/tmp/fuse_t2d_deep_bridge.cs",
        R"(module "CompositeToy";
new SceneToy() {
  new CompositeSprite(Composite) {
    layer = 2;
    new SpritePlayer(ChildA) {
      position = "1 2";
      sortPoint = 10;
      physicsEnabled = true;
    };
  };
};)");

    fuse::world2d::World2D world;
    const fuse::project::T2DRuntimeBridgeResult bridged =
        fuse::project::bridgeT2DModuleToRuntime(world, module);

    expectTrue(bridged.ok, "t2d deep runtime bridge ok");
    expectTrue(bridged.spriteCount >= 2u, "composite + child sprites bridged");
    expectTrue(world.isPhysicsEnabled(), "physics enabled when module requests it");
}

void testT2DModuleRuntimeBridge() {
    const std::string module = writeTempFile(
        "/tmp/fuse_t2d_bridge.cs",
        "module \"SpriteToy\";\n"
        "new SceneToy() {\n"
        "  new SpritePlayer(Player) { position = \"4 8\"; };\n"
        "};\n");

    fuse::world2d::World2D world;
    const fuse::project::T2DRuntimeBridgeResult bridged =
        fuse::project::bridgeT2DModuleToRuntime(world, module);

    expectTrue(bridged.ok, "t2d runtime bridge ok");
    expectTrue(bridged.spriteCount >= 1u, "t2d runtime bridge populated sprites");
}

} // namespace

int main() {
    fuse::core::initialize();
    testConvertT3DMissionHierarchy();
    testConvertT3DMissionToFuselevel();
    testConvertT2DModuleToFuselevel();
    testConvertT2DModuleHierarchy();
    testConvertT3DDatablockWiringStubs();
    testT3DDatablockResolveFromMission();
    testT3DDatablockResolveFromScene();
    testT2DModuleRuntimeBridge();
    testT2DModuleRuntimeBridgeLayersPhysicsComposite();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_world_converter_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_world_converter_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
