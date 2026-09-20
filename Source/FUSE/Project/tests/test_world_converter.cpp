#include <fuse/core/init.hpp>
#include <fuse/handle_table.hpp>
#include <fuse/io/asset.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/project/t2d_module_bridge.hpp>
#include <fuse/project/t3d_asset_vfs.hpp>
#include <fuse/types.hpp>
#include <fuse/project/t3d_datablock_resolve.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/scene/serialiser.hpp>
#include <fuse/scene/wire_stub.hpp>
#include <fuse/world2d/world_2d.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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
    const std::string moduleText =
        "module \"CompositeToy\";\n"
        "new SceneToy() {\n"
        "  new CompositeSprite(Composite) {\n"
        "    layer = 2;\n"
        "    new SpritePlayer(ChildA) {\n"
        "      position = \"1 2\";\n"
        "      sortPoint = 10;\n"
        "      physicsEnabled = true;\n"
        "    };\n"
        "  };\n"
        "};\n";
    const std::string module = writeTempFile("/tmp/fuse_t2d_deep_bridge.cs", moduleText);

    fuse::world2d::World2D world;
    const fuse::project::T2DRuntimeBridgeResult bridged =
        fuse::project::bridgeT2DModuleToRuntime(world, module);

    expectTrue(bridged.ok, "t2d deep runtime bridge ok");
    expectTrue(bridged.spriteCount >= 2u, "composite + child sprites bridged");
    expectTrue(world.isPhysicsEnabled(), "physics enabled when module requests it");
    expectTrue(bridged.physicsBodyCount >= 1u, "physics body count recorded");
    expectTrue(bridged.circleBodyCount >= 1u, "default physics shape is circle");
}

void testT2DPhysicsShapesCollisionLayers() {
    const std::string moduleText =
        R"(module "PhysicsToy";
new SceneToy() {
  new BoxSprite(Wall) {
    position = "0 0";
    physicsEnabled = true;
    shapeType = "box";
    size = "4 2";
    collisionLayer = 3;
    collisionMask = 5;
  };
  new CircleSprite(Ball) {
    position = "1 1";
    physicsEnabled = true;
    collisionRadius = 1.5;
    collisionLayer = 1;
  };
};)";
    const std::string module = writeTempFile("/tmp/fuse_t2d_physics_shapes.cs", moduleText);

    const fuse::project::T2DModuleExtract extract =
        fuse::project::extractT2DModuleFields(moduleText, module);
    expectTrue(extract.sceneNodes.size() >= 2u, "physics module extract produced nodes");

    bool foundBox = false;
    bool foundCircle = false;
    for (const fuse::project::T2DSceneNodeStub& node : extract.sceneNodes) {
        if (node.objectName == "Wall") {
            foundBox = true;
            expectTrue(node.physicsShape == fuse::project::T2DPhysicsShape::Box,
                       "box sprite maps to box physics shape");
            expectTrue(node.collisionLayer == 3, "collision layer parsed");
            expectTrue(node.collisionMask == 5u, "collision mask parsed");
        }
        if (node.objectName == "Ball") {
            foundCircle = true;
            expectTrue(node.physicsShape == fuse::project::T2DPhysicsShape::Circle,
                       "circle sprite maps to circle physics shape");
            expectTrue(node.physicsRadius > 1.f, "collision radius parsed");
        }
    }
    expectTrue(foundBox && foundCircle, "box and circle nodes extracted");

    fuse::world2d::World2D world;
    const fuse::project::T2DRuntimeBridgeResult bridged =
        fuse::project::populateWorld2DFromModuleExtract(world, extract);
    expectTrue(bridged.ok, "physics shapes bridge ok");
    expectTrue(bridged.physicsBodyCount == 2u, "two physics bodies bridged");
    expectTrue(bridged.boxBodyCount == 1u, "one box body");
    expectTrue(bridged.circleBodyCount == 1u, "one circle body");
    expectTrue(bridged.collisionLayerCount >= 1u, "collision layers recorded");
    expectTrue(world.physics().bodyCount() == 2u, "world physics bodies created");
}

void testT3DMaterialVfsMountAndResolve() {
    const std::filesystem::path projectRoot = std::filesystem::path("/tmp/fuse_vfs_project");
    const std::filesystem::path materialPath =
        projectRoot / "data" / "materials" / "Prototyping" / "FloorGray.mat";
    std::filesystem::create_directories(materialPath.parent_path());
    writeTempFile(materialPath.string(), "stub material");

    fuse::project::ProjectManifest manifest{};
    manifest.projectRoot = projectRoot.string();
    const fuse::project::ProjectVfsMountResult mount =
        fuse::project::mountProjectAssetRoots(manifest);
    expectTrue(mount.mountsAdded >= 1u, "project vfs mounts added");
    expectTrue(mount.t3dMount, "t3d mount registered");

    const std::string virtualPath =
        fuse::project::materialAssetToVirtualPath("Prototyping:FloorGray");
    expectTrue(virtualPath == "/t3d/materials/Prototyping/FloorGray.mat",
               "material ref maps to virtual path");

    std::string resolvedPhysical;
    expectTrue(fuse::io::VirtualFileSystem::instance().resolve(virtualPath, resolvedPhysical),
               "mounted vfs resolves material path");

    const std::string mission = writeTempFile(
        "/tmp/fuse_vfs_mission.mis",
        "new Scene(ExampleLevel) {\n"
        "   new GroundPlane(Floor) {\n"
        "      MaterialAsset = \"Prototyping:FloorGray\";\n"
        "   };\n"
        "};\n");
    const std::string missionText = [&]() {
        std::ifstream in(mission);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }();

    const fuse::project::T3DMissionExtract extract =
        fuse::project::extractT3DMissionFields(missionText);
    const fuse::project::T3DMaterialVfsResolveResult resolved =
        fuse::project::resolveT3DMaterialVfsPaths(extract);
    expectTrue(resolved.materialCount >= 1u, "material vfs resolve counted refs");
    expectTrue(resolved.resolvedCount >= 1u, "material vfs path resolved on disk");
}

void testT3DMaterialVfsAsyncLoad() {
    const std::filesystem::path projectRoot = std::filesystem::path("/tmp/fuse_vfs_project");
    const std::filesystem::path materialPath =
        projectRoot / "data" / "materials" / "Prototyping" / "FloorGray.mat";
    std::filesystem::create_directories(materialPath.parent_path());
    writeTempFile(materialPath.string(), "async stub material");

    const std::string missionText =
        "new Scene(ExampleLevel) {\n"
        "   new GroundPlane(Floor) {\n"
        "      MaterialAsset = \"Prototyping:FloorGray\";\n"
        "   };\n"
        "};\n";
    const fuse::project::T3DMissionExtract extract =
        fuse::project::extractT3DMissionFields(missionText);

    const fuse::project::T3DMaterialVfsAsyncLoadResult submitted =
        fuse::project::submitT3DMaterialLoadsAsync(extract);
    expectTrue(submitted.submittedCount >= 1u, "async material vfs load submitted");

    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();
    for (fuse::u32 spinGuard = 0u;
         spinGuard < 1'000'000u && vfs.completedLoadCount() < submitted.submittedCount; ++spinGuard) {
        std::this_thread::yield();
    }
    expectTrue(vfs.completedLoadCount() >= submitted.submittedCount,
               "async material vfs load completes on I/O lane");

    const std::string virtualPath =
        fuse::project::materialAssetToVirtualPath("Prototyping:FloorGray");
    const std::string cookOutput = fuse::project::materialVirtualPathToCookOutput(virtualPath);
    expectTrue(cookOutput == "cooked/materials/Prototyping/FloorGray.fusetex",
               "material virtual path maps to cooked output");

    fuse::HandleTable<fuse::io::Asset> table;
    fuse::project::CookCache cache;
    const fuse::project::T3DMaterialCookCacheResult drained =
        fuse::project::drainT3DMaterialLoads(table, &cache);
    expectTrue(drained.drainedCount >= 1u, "async material vfs load drained to handle table");
    expectTrue(drained.cookCacheStores >= 1u, "async material vfs load stored cook-cache entry");
    expectTrue(fuse::io::VirtualFileSystem::instance().completedLoadCount() == 0u,
               "drain clears completed vfs loads");

    std::string resolvedPhysical;
    expectTrue(fuse::io::VirtualFileSystem::instance().resolve(virtualPath, resolvedPhysical),
               "mounted vfs resolves material path for cook-cache key");
    const fuse::u64 cacheKey = fuse::project::materialCookCacheKey(resolvedPhysical);
    fuse::project::CookCacheEntry cached;
    expectTrue(cache.lookup(cacheKey, &cached) == fuse::project::CookCacheLookup::Hit,
               "material cook-cache entry is retrievable");
    expectTrue(cached.output_path == cookOutput, "material cook-cache stores cooked output path");

    const fuse::project::T3DMaterialVfsAsyncLoadResult cachedSubmit =
        fuse::project::submitT3DMaterialLoadsAsync(extract, &cache);
    expectTrue(cachedSubmit.cookCacheHits >= 1u, "cached material submit skips I/O on cook-cache hit");
    expectTrue(cachedSubmit.submittedCount == 0u, "cached material submit does not enqueue vfs reads");
}

void testVfsAssetPathRemap() {
    expectTrue(fuse::project::remapLegacyAssetPath("data/materials/FloorGray.mat") ==
                   "/t3d/materials/FloorGray.mat",
               "data/ prefix remaps to /t3d/");
    expectTrue(fuse::project::remapLegacyAssetPath("game/textures/albedo.png") ==
                   "/game/textures/albedo.png",
               "game/ prefix remaps to /game/");
    expectTrue(fuse::project::shaderAssetToVirtualPath("Common:ScreenSpace") ==
                   "/t3d/shaders/Common/ScreenSpace.cs",
               "shader ref maps to virtual path");
    expectTrue(fuse::project::shaderVirtualPathToCookOutput("/t3d/shaders/Common/ScreenSpace.cs") ==
                   "cooked/shaders/Common/ScreenSpace.fuseshader",
               "shader virtual path maps to cook output");
    expectTrue(fuse::project::materialVirtualPathToRefName("/t3d/materials/Prototyping/FloorGray.mat") ==
                   "Prototyping:FloorGray",
               "material virtual path reverse-maps to wire ref");
    expectTrue(fuse::project::shaderVirtualPathToRefName("/t3d/shaders/Common/ScreenSpace.cs") ==
                   "Common:ScreenSpace",
               "shader virtual path reverse-maps to wire ref");

    fuse::project::T3DDatablockResolveResult bindings;
    bindings.bindings.push_back({"Floor", "Prototyping:FloorGray", "material", 1u});
    bindings.bindings.push_back({"Post", "Common:ScreenSpace", "shader", 2u});
    expectTrue(fuse::project::countRemappedAssetVfsPaths(bindings) == 2u,
               "remapped asset vfs path count includes material and shader bindings");
}

void testT3DMissionShaderDataExtract() {
    const std::string missionText =
        "new Scene(ShaderLevel) {\n"
        "   new PostEffect(Post) {\n"
        "      ShaderData = \"Common:ScreenSpace\";\n"
        "   };\n"
        "};\n";

    const fuse::project::T3DMissionExtract extract =
        fuse::project::extractT3DMissionFields(missionText);
    expectTrue(extract.shaders.size() >= 1u, "mission extract records ShaderData refs");
    expectTrue(extract.simObjects.size() >= 2u, "mission extract records shader owner object");
    bool foundShaderOwner = false;
    for (const fuse::project::T3DSimObjectStub& object : extract.simObjects) {
        if (object.objectName == "Post" && object.shaderAsset == "Common:ScreenSpace") {
            foundShaderOwner = true;
            break;
        }
    }
    expectTrue(foundShaderOwner, "shader owner object stores ShaderData ref");

    const fuse::project::T3DDatablockResolveResult resolved =
        fuse::project::resolveT3DMissionBindings(extract);
    expectTrue(resolved.shaderCount >= 1u, "mission resolve linked shader bindings");
}

void testT3DMissionShaderVfsAsyncLoad() {
    const std::filesystem::path projectRoot = std::filesystem::path("/tmp/fuse_vfs_project");
    const std::filesystem::path shaderPath =
        projectRoot / "data" / "shaders" / "Common" / "ScreenSpace.cs";
    std::filesystem::create_directories(shaderPath.parent_path());
    writeTempFile(shaderPath.string(), "void main() {}\n");

    fuse::project::ProjectManifest manifest{};
    manifest.projectRoot = projectRoot.string();
    fuse::project::mountProjectAssetRoots(manifest);

    const std::string missionText =
        "new Scene(ShaderLevel) {\n"
        "   new PostEffect(Post) {\n"
        "      ShaderData = \"Common:ScreenSpace\";\n"
        "   };\n"
        "};\n";
    const fuse::project::T3DMissionExtract extract =
        fuse::project::extractT3DMissionFields(missionText);

    const fuse::project::T3DShaderVfsResolveResult resolved =
        fuse::project::resolveT3DShaderVfsPaths(extract);
    expectTrue(resolved.shaderCount >= 1u, "mission shader vfs resolve counted refs");
    expectTrue(resolved.resolvedCount >= 1u, "mission shader vfs path resolved on disk");

    const fuse::project::T3DShaderVfsAsyncLoadResult submitted =
        fuse::project::submitT3DShaderLoadsAsync(extract);
    expectTrue(submitted.submittedCount >= 1u, "mission async shader vfs load submitted");

    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();
    for (fuse::u32 spinGuard = 0u;
         spinGuard < 1'000'000u && vfs.completedLoadCount() < submitted.submittedCount; ++spinGuard) {
        std::this_thread::yield();
    }

    fuse::HandleTable<fuse::io::Asset> table;
    fuse::project::drainT3DShaderLoads(table, nullptr);
}

void testT3DShaderVfsAsyncLoad() {
    const std::filesystem::path projectRoot = std::filesystem::path("/tmp/fuse_vfs_project");
    const std::filesystem::path shaderPath =
        projectRoot / "data" / "shaders" / "Common" / "ScreenSpace.cs";
    std::filesystem::create_directories(shaderPath.parent_path());
    writeTempFile(shaderPath.string(), "void main() {}\n");

    fuse::project::ProjectManifest manifest{};
    manifest.projectRoot = projectRoot.string();
    fuse::project::mountProjectAssetRoots(manifest);

    fuse::project::T3DDatablockResolveResult bindings;
    bindings.bindings.push_back({"Post", "Common:ScreenSpace", "shader", 2u});

    const fuse::project::T3DShaderVfsResolveResult resolved =
        fuse::project::resolveT3DShaderVfsFromBindings(bindings);
    expectTrue(resolved.shaderCount == 1u, "shader vfs resolve counted refs");
    expectTrue(resolved.resolvedCount == 1u, "shader vfs path resolved on disk");

    const fuse::project::T3DShaderVfsAsyncLoadResult submitted =
        fuse::project::submitT3DShaderLoadsAsync(bindings);
    expectTrue(submitted.submittedCount >= 1u, "async shader vfs load submitted");

    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();
    for (fuse::u32 spinGuard = 0u;
         spinGuard < 1'000'000u && vfs.completedLoadCount() < submitted.submittedCount; ++spinGuard) {
        std::this_thread::yield();
    }
    expectTrue(vfs.completedLoadCount() >= submitted.submittedCount,
               "async shader vfs load completes on I/O lane");

    const std::string virtualPath = fuse::project::shaderAssetToVirtualPath("Common:ScreenSpace");
    const std::string cookOutput = fuse::project::shaderVirtualPathToCookOutput(virtualPath);
    expectTrue(cookOutput == "cooked/shaders/Common/ScreenSpace.fuseshader",
               "shader virtual path maps to cooked output");

    fuse::HandleTable<fuse::io::Asset> table;
    fuse::project::CookCache cache;
    const fuse::project::T3DShaderCookCacheResult drained =
        fuse::project::drainT3DShaderLoads(table, &cache);
    expectTrue(drained.drainedCount >= 1u, "async shader vfs load drained to handle table");
    expectTrue(drained.cookCacheStores >= 1u, "async shader vfs load stored cook-cache entry");
    expectTrue(vfs.completedLoadCount() == 0u, "shader drain clears completed vfs loads");

    std::string resolvedPhysical;
    expectTrue(vfs.resolve(virtualPath, resolvedPhysical),
               "mounted vfs resolves shader path for cook-cache key");
    const fuse::u64 cacheKey = fuse::project::shaderCookCacheKey(resolvedPhysical);
    fuse::project::CookCacheEntry cached;
    expectTrue(cache.lookup(cacheKey, &cached) == fuse::project::CookCacheLookup::Hit,
               "shader cook-cache entry is retrievable");
    expectTrue(cached.output_path == cookOutput, "shader cook-cache stores cooked output path");

    const fuse::project::T3DShaderVfsAsyncLoadResult cachedSubmit =
        fuse::project::submitT3DShaderLoadsAsync(bindings, &cache);
    expectTrue(cachedSubmit.cookCacheHits >= 1u, "cached shader submit skips I/O on cook-cache hit");
    expectTrue(cachedSubmit.submittedCount == 0u, "cached shader submit does not enqueue vfs reads");
}

void testConvertT2DAnimatedSpriteWiringStubs() {
    const std::string module = writeTempFile(
        "/tmp/fuse_convert_animated.cs",
        R"(module "SpriteToy";
new SceneToy() {
  new SpritePlayer(Hero) {
    position = "0 0";
    imageMap = "HeroSheet.png";
    animationName = "Walk";
    frameCount = 8;
    animationFPS = 12;
  };
};)");
    const std::string output = "/tmp/fuse_convert_animated.fuselevel";

    const fuse::project::ConvertResult result =
        fuse::project::convertT2DModuleToFuselevel(module, output);

    expectTrue(result.status == fuse::project::ConvertStatus::Ok, "animated module convert ok");
    expectTrue(result.wiringStubCount >= 1u, "animated-sprite wiring stub emitted");

    fuse::scene::Scene loaded;
    const fuse::scene::SerialiseResult loadResult = fuse::scene::SceneSerialiser::load(output, loaded);
    expectTrue(loadResult.status == fuse::scene::SerialiseStatus::Ok, "animated fuselevel loads");

    bool foundAnimatedWire = false;
    for (const fuse::scene::SceneEntity& entity : loaded.entities()) {
        const fuse::scene::WireStubRef wire = fuse::scene::parseWireStubEntityName(entity.name);
        if (wire.valid && wire.kind == "animated_sprite" && wire.owner == "Hero") {
            foundAnimatedWire = true;
            expectTrue(wire.value.find("HeroSheet.png") != std::string::npos,
                       "animated wire value includes imageMap");
        }
    }
    expectTrue(foundAnimatedWire, "animated-sprite wire stub round-trips in fuselevel");

    fuse::world2d::World2D world;
    const fuse::project::T2DRuntimeBridgeResult bridged =
        fuse::project::bridgeT2DModuleToRuntime(world, module);
    expectTrue(bridged.ok, "animated module runtime bridge ok");
    expectTrue(bridged.animatedSpriteCount >= 1u, "animated sprite metadata counted in bridge");
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
    testT2DPhysicsShapesCollisionLayers();
    testConvertT2DAnimatedSpriteWiringStubs();
    testT3DMaterialVfsMountAndResolve();
    testT3DMaterialVfsAsyncLoad();
    testVfsAssetPathRemap();
    testT3DMissionShaderDataExtract();
    testT3DMissionShaderVfsAsyncLoad();
    testT3DShaderVfsAsyncLoad();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_world_converter_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_world_converter_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
