// UNI-U7-WORLD-1 / UNI-U7-MIS-1 gates (CPU):
//   demo level   Samples/unification/demo_3d_empty/worlds/example.mis -> .fuselevel v3 -> World3D: entity count,
//                transforms (Z-up -> Y-up), colliders, mirrors, archetype counts, zero unexpected stubs.
//   physics      the level's GroundPlane collider is the physics floor (a dropped ball rests on it); no
//                per-object default spheres for level objects.
//   round trip   World3D::saveWorld after a load reproduces the file byte for byte; reload matches; the world
//                saves ECS state changes (a moved entity) back into the scene table.
//   ExampleLevel Templates/BaseGame ExampleLevel.mis: per-archetype component counts (LevelInfo, SkyBox, Sun,
//                GroundPlane, Skylight, SimGroup, SpawnSphere), Sun orientation from azimuth / elevation.
//   World2D      saveWorld2DToFuselevel -> populateWorld2DFromFuselevel -> save: identical bytes; physics settings
//                restored from Collider / RigidBody components.

#include <fuse/core/init.hpp>
#include <fuse/core/temp_path.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/environment.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/spawn_marker.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/scene/serialiser.hpp>
#include <fuse/world2d/fuselevel_bridge.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifndef FUSE_SOURCE_DIR
#define FUSE_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
namespace w3 = fuse::world3d;
using fuse::u32;

int g_failures = 0;

void expectTrue(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

bool nearly(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

std::vector<char> readBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

const fs::path kSource(FUSE_SOURCE_DIR);

fuse::SceneObject3D* findObject(w3::World3D& world, const std::string& name) {
    // Depth-first over the root's scene children.
    std::vector<fuse::Object*> stack(world.root()->children().begin(), world.root()->children().end());
    while (!stack.empty()) {
        fuse::Object* node = stack.back();
        stack.pop_back();
        if (node->name() == name) {
            return fuse::asSceneObject3D(node);
        }
        stack.insert(stack.end(), node->children().begin(), node->children().end());
    }
    return nullptr;
}

void tick(w3::World3D& world, u32 frames) {
    fuse::frame::FrameCtx ctx;
    for (u32 i = 0; i < frames; ++i) {
        ctx.dt = 1.f / 60.f;
        ctx.frameIndex = i;
        world.tick(ctx);
    }
}

void testDemoLevel(const fs::path& dir) {
    const std::string mission = (kSource / "Samples/unification/demo_3d_empty/worlds/example.mis").string();
    const std::string level = (dir / "example.fuselevel").generic_string();
    const fuse::project::ConvertResult converted = fuse::project::convertT3DMissionToFuselevel(mission, level);
    expectTrue(converted.status == fuse::project::ConvertStatus::Ok, "example.mis converts");
    // CameraSpawnPoints, DefaultCameraSpawnSphere, Floor + datablock / material stubs.
    expectTrue(converted.entityCount == 5u && converted.ecsEntityCount == 5u, "5 scene entities, 5 ECS entities");
    const fuse::project::MissionArchetypeCounts& c = converted.components;
    expectTrue(c.groups == 1u && c.spawnMarkers == 1u && c.meshes == 1u && c.staticColliders == 1u && c.unhandled == 0u,
               "example.mis archetype counts (1 group, 1 spawn marker, 1 plane mesh + collider)");
    expectTrue(converted.unexpectedStubCount == 0u && converted.wiringStubCount == 2u,
               "example.mis: zero unexpected stubs (2 legacy datablock / material stubs)");
    expectTrue(converted.materialSlots.size() == 1u && converted.materialSlots[0] == "Prototyping:FloorGray",
               "one material slot");

    w3::World3D world;
    const w3::World3DLevelLoadResult loaded = world.loadWorldFromFuselevel(level);
    expectTrue(loaded.ok && loaded.fileVersion == 3u && loaded.hasEcsBlock, "World3D loads the v3 level");
    expectTrue(loaded.sceneEntities == 5u && loaded.wireStubs == 2u && loaded.objects == 3u && loaded.ecsEntities == 5u,
               "entity count: 3 mirrored objects + 2 stubs, 5 ECS entities");
    expectTrue(loaded.meshes == 1u && loaded.colliders == 1u && loaded.rigidBodies == 1u && loaded.spawnMarkers == 1u &&
                   loaded.physicsLinked == 1u,
               "components: plane mesh, plane collider + static body, spawn marker");
    expectTrue(world.objectCount() == 3u, "3 World3D objects");
    expectTrue(world.materials().size() >= 1u, "default material row for the level's material slot");

    fuse::SceneObject3D* spawn = findObject(world, "DefaultCameraSpawnSphere");
    fuse::SceneObject3D* floor = findObject(world, "Floor");
    fuse::SceneObject3D* group = findObject(world, "CameraSpawnPoints");
    expectTrue(spawn != nullptr && floor != nullptr && group != nullptr, "mirrors named after the mission objects");
    if (spawn != nullptr && group != nullptr) {
        expectTrue(spawn->parent() == group, "hierarchy: spawn sphere under its SimGroup");
        const fuse::math::Vec3 p = spawn->worldTranslation();
        // Torque "0 0 10" (Z-up) -> FUSE (0, 10, 0).
        expectTrue(nearly(p.x, 0.f) && nearly(p.y, 10.f) && nearly(p.z, 0.f), "spawn sphere transform (Y-up)");
    }
    float spawnPoint[3] = {0.f, 0.f, 0.f};
    expectTrue(world.findSpawnPoint(spawnPoint) && nearly(spawnPoint[1], 10.f), "findSpawnPoint = spawn sphere");

    fuse::ecs::Registry& registry = world.registry();
    registry.each<fuse::ecs::Collider, fuse::ecs::RigidBody, fuse::ecs::Mesh>(
        [&](fuse::ecs::EntityID, fuse::ecs::Collider& collider, fuse::ecs::RigidBody& body, fuse::ecs::Mesh& mesh) {
            expectTrue(collider.shape == fuse::ecs::Collider::Plane && nearly(collider.params.y, 1.f) &&
                           nearly(collider.scalar, 0.f),
                       "GroundPlane collider: y-up plane through the origin");
            expectTrue(body.is_static && body.inv_mass == 0.f, "GroundPlane body is static");
            expectTrue(mesh.vertex_buffer.isValid() &&
                           mesh.vertex_buffer.index() == static_cast<u32>(w3::BuiltinMesh::Plane) && mesh.material_id == 0u,
                       "GroundPlane mesh = builtin plane, material slot 0");
        });

    // Round trip: save right after the load reproduces the converter's bytes; reload + save again too.
    const std::string saved = (dir / "example_saved.fuselevel").generic_string();
    std::string error;
    expectTrue(world.saveWorld(saved, &error), "saveWorld: " + error);
    expectTrue(readBytes(saved) == readBytes(level), "load -> save reproduces the level byte for byte");
    w3::World3D reloaded;
    const w3::World3DLevelLoadResult again = reloaded.loadWorldFromFuselevel(saved);
    expectTrue(again.ok && again.objects == loaded.objects && again.ecsEntities == loaded.ecsEntities &&
                   again.colliders == loaded.colliders && again.meshes == loaded.meshes,
               "reloaded level matches");
    const std::string savedAgain = (dir / "example_saved2.fuselevel").generic_string();
    expectTrue(reloaded.saveWorld(savedAgain) && readBytes(savedAgain) == readBytes(saved), "save / load / save stable");

    // Physics: the level's collider is the floor; level objects get no default spheres.
    world.setPhysicsEnabled(true);
    auto ball = std::make_unique<fuse::SceneObject3D>("ball");
    ball->setPhysicsShape(fuse::PhysicsShape2D::Circle);
    ball->setPhysicsRadius(0.5f);
    ball->setPosition3D(3.f, 3.f, 3.f);
    world.addObject(ball.get());
    tick(world, 180);
    expectTrue(world.physics().bodyCount() == 2u, "bodies = level plane + the dropped ball (no default spheres)");
    const fuse::math::Vec3 rest = ball->worldTranslation();
    std::printf("ball rests at y=%.3f\n", static_cast<double>(rest.y));
    expectTrue(rest.y > 0.3f && rest.y < 0.7f, "ball rests on the converted GroundPlane collider");
    if (floor != nullptr) {
        const fuse::math::Vec3 f = floor->worldTranslation();
        expectTrue(nearly(f.y, 0.f), "static floor mirror did not move");
    }

    // ECS edits are saved into the scene table (move the spawn marker entity).
    registry.each<fuse::ecs::SpawnMarker, fuse::ecs::Transform>(
        [&](fuse::ecs::EntityID, fuse::ecs::SpawnMarker&, fuse::ecs::Transform& t) { t.position.x = 7.f; });
    const std::string moved = (dir / "example_moved.fuselevel").generic_string();
    expectTrue(world.saveWorld(moved), "save after edit");
    fuse::scene::Scene movedScene;
    expectTrue(fuse::scene::SceneSerialiser::load(moved, movedScene).status == fuse::scene::SerialiseStatus::Ok,
               "edited level loads");
    bool movedFound = false;
    for (const fuse::scene::SceneEntity& entity : movedScene.entities()) {
        if (entity.name == "DefaultCameraSpawnSphere") {
            movedFound = nearly(entity.transform.positionX, 7.f);
        }
    }
    expectTrue(movedFound, "scene-table transform follows the ECS Transform on save");
    world.removeObject(ball.get());
}

void testExampleLevel(const fs::path& dir) {
    const std::string mission = (kSource / "Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis").string();
    const std::string level = (dir / "ExampleLevel.fuselevel").generic_string();
    const fuse::project::ConvertResult converted = fuse::project::convertT3DMissionToFuselevel(mission, level);
    expectTrue(converted.status == fuse::project::ConvertStatus::Ok, "ExampleLevel.mis converts");
    const fuse::project::MissionArchetypeCounts& c = converted.components;
    std::printf("ExampleLevel: groups=%u sun=%u point=%u spot=%u ambient=%u sky=%u fog=%u meshes=%u colliders=%u "
                "spawn=%u unhandled=%u stubs=%u (unexpected %u)\n",
                c.groups, c.directionalLights, c.pointLights, c.spotLights, c.ambientLights, c.atmospheres, c.fogs,
                c.meshes, c.staticColliders, c.spawnMarkers, c.unhandled, converted.wiringStubCount,
                converted.unexpectedStubCount);
    expectTrue(c.fogs == 1u, "LevelInfo -> EnvironmentFog");
    expectTrue(c.atmospheres == 1u, "SkyBox -> SkyAtmosphere");
    expectTrue(c.directionalLights == 1u, "Sun -> DirectionalLight");
    expectTrue(c.meshes == 1u && c.staticColliders == 1u, "GroundPlane -> plane mesh + static collider");
    expectTrue(c.ambientLights == 1u, "Skylight -> AmbientLight");
    expectTrue(c.groups == 1u && c.spawnMarkers == 1u, "SimGroup + SpawnSphere");
    expectTrue(c.pointLights == 0u && c.spotLights == 0u && c.unhandled == 0u, "nothing else");
    expectTrue(converted.unexpectedStubCount == 0u, "ExampleLevel.mis: zero unexpected wiring stubs");
    // LevelInfo, SkyBox, Sun, GroundPlane, Skylight, SimGroup, SpawnSphere + SkyBox / GroundPlane material stubs +
    // SpawnSphere datablock stub.
    expectTrue(converted.entityCount == 10u && converted.wiringStubCount == 3u, "ExampleLevel entity / stub count");

    w3::World3D world;
    const w3::World3DLevelLoadResult loaded = world.loadWorldFromFuselevel(level);
    expectTrue(loaded.ok && loaded.objects == 7u && loaded.directionalLights == 1u && loaded.meshes == 1u &&
                   loaded.colliders == 1u && loaded.spawnMarkers == 1u,
               "ExampleLevel loads into World3D");
    fuse::ecs::Registry& registry = world.registry();
    u32 ambient = 0;
    u32 fog = 0;
    u32 sky = 0;
    registry.each<fuse::ecs::AmbientLight>([&](fuse::ecs::EntityID, fuse::ecs::AmbientLight& a) {
        ++ambient;
        // Sun ambient "0.337255 0.533333 0.619608" (sRGB) -> linear.
        expectTrue(a.use_ddgi == 1u && nearly(a.color.x, 0.0930f, 2e-3f) && nearly(a.color.z, 0.3423f, 2e-3f),
                   "Skylight ambient = Sun ambient (linear), DDGI on");
    });
    registry.each<fuse::ecs::EnvironmentFog>([&](fuse::ecs::EntityID, fuse::ecs::EnvironmentFog& f) {
        ++fog;
        expectTrue(nearly(f.density_offset, 700.f), "LevelInfo fogDensityOffset");
    });
    registry.each<fuse::ecs::SkyAtmosphere>([&](fuse::ecs::EntityID, fuse::ecs::SkyAtmosphere& s) {
        ++sky;
        expectTrue(s.mode == fuse::ecs::SkyAtmosphere::Cubemap &&
                       s.sky_material == fuse::project::t3dMaterialRefAssetId("Core_Rendering:BlankSkyMat"),
                   "SkyBox cubemap material id");
    });
    expectTrue(ambient == 1u && fog == 1u && sky == 1u, "environment components present after load");
    registry.each<fuse::ecs::DirectionalLight, fuse::ecs::Transform>(
        [&](fuse::ecs::EntityID, fuse::ecs::DirectionalLight& light, fuse::ecs::Transform& t) {
            // azimuth 230.396, elevation 45: toSun (Torque) = (sin az cos el, cos az cos el, sin el) -> FUSE (x, z, -y).
            const float az = 230.396f * 3.14159265f / 180.f;
            const float el = 45.f * 3.14159265f / 180.f;
            const float expect[3] = {std::sin(az) * std::cos(el), std::sin(el), -std::cos(az) * std::cos(el)};
            expectTrue(nearly(t.local_to_world.data[8], expect[0], 1e-3f) &&
                           nearly(t.local_to_world.data[9], expect[1], 1e-3f) &&
                           nearly(t.local_to_world.data[10], expect[2], 1e-3f),
                       "Sun local +Z points at the sun (azimuth / elevation)");
            expectTrue(light.intensity == 1.f && light.color.x > light.color.z, "Sun colour / brightness");
        });
    const std::string saved = (dir / "ExampleLevel_saved.fuselevel").generic_string();
    expectTrue(world.saveWorld(saved) && readBytes(saved) == readBytes(level), "ExampleLevel save round trip");
}

void testLegacyLevel(const fs::path& dir) {
    // v2 file (no ECS block): entities rebuilt as Transform-only, mirrored, no bodies.
    fuse::scene::Scene scene("legacy");
    fuse::scene::SceneEntityTransform t{};
    t.positionX = 1.f;
    scene.addEntity("root", t);
    t.positionX = 2.f;
    scene.addEntity("child", t, 0);
    const std::string path = (dir / "legacy.fuselevel").generic_string();
    expectTrue(fuse::scene::SceneSerialiser::save(scene, path).status == fuse::scene::SerialiseStatus::Ok, "v2 save");
    w3::World3D world;
    const w3::World3DLevelLoadResult loaded = world.loadWorldFromFuselevel(path);
    expectTrue(loaded.ok && loaded.fileVersion == 2u && !loaded.hasEcsBlock && loaded.objects == 2u &&
                   loaded.physicsLinked == 0u,
               "v2 level loads (Transform-only)");
    fuse::SceneObject3D* child = findObject(world, "child");
    expectTrue(child != nullptr && nearly(child->worldTranslation().x, 3.f), "v2 hierarchy composes transforms");
    // A missing file leaves the loaded level alone.
    const w3::World3DLevelLoadResult failed = world.loadWorldFromFuselevel((dir / "missing.fuselevel").generic_string());
    expectTrue(!failed.ok && world.objectCount() == 2u, "failed load keeps the current level");
}

void testWorld2DRoundTrip(const fs::path& dir) {
    const std::string module = (dir / "toy.cs").generic_string();
    {
        std::ofstream out(module, std::ios::binary);
        out << "module \"Toy\";\n"
               "new SceneToy() {\n"
               "  new SpritePlayer(Player) {\n"
               "    position = \"1 2\";\n"
               "    new Sprite(Hat) {\n"
               "      position = \"0 1\";\n"
               "    };\n"
               "  };\n"
               "  new AnimatedSprite(Coin) {\n"
               "    position = \"4 0\";\n"
               "    imageMap = \"CoinSheet.png\";\n"
               "    animationName = \"spin\";\n"
               "    frameCount = 8;\n"
               "  };\n"
               "};\n";
    }
    const std::string converted = (dir / "toy.fuselevel").generic_string();
    const fuse::project::ConvertResult result = fuse::project::convertT2DModuleToFuselevel(module, converted);
    expectTrue(result.status == fuse::project::ConvertStatus::Ok, "T2D module converts");

    fuse::world2d::World2D world;
    const fuse::world2d::FuselevelLoadResult first = world.loadWorldFromFuselevel(converted);
    expectTrue(first.ok && first.entityCount >= 3u, "World2D loads the converted module");
    // Give one sprite physics settings, then save (v3).
    fuse::SceneObject2D* player = nullptr;
    std::vector<fuse::Object*> stack(world.root()->children().begin(), world.root()->children().end());
    while (!stack.empty()) {
        fuse::Object* node = stack.back();
        stack.pop_back();
        if (node->name() == "Player") {
            player = fuse::asSceneObject2D(node);
        }
        stack.insert(stack.end(), node->children().begin(), node->children().end());
    }
    expectTrue(player != nullptr, "Player sprite loaded");
    if (player != nullptr) {
        player->setPhysicsEnabled(true);
        player->setPhysicsShape(fuse::PhysicsShape2D::Box);
        player->setBoxHalfWidth(0.75f);
        player->setBoxHalfHeight(1.25f);
        player->setCollisionLayer(3);
        player->setCollisionMask(5u);
    }
    const std::string a = (dir / "toy_a.fuselevel").generic_string();
    const fuse::world2d::FuselevelSaveResult savedA = fuse::world2d::saveWorld2DToFuselevel(world, a);
    expectTrue(savedA.ok && savedA.physicsBodies == 1u && savedA.entityCount == first.entityCount + first.wireStubCount,
               "World2D saves sprites + wire stubs (v3)");

    fuse::world2d::World2D reloaded;
    const fuse::world2d::FuselevelLoadResult second = reloaded.loadWorldFromFuselevel(a);
    expectTrue(second.ok && second.fileVersion == 3u && second.hasEcsBlock && second.physicsBodies == 1u &&
                   second.entityCount == first.entityCount && second.wireStubCount == first.wireStubCount,
               "World2D reload: same sprites, stubs and physics");
    expectTrue(reloaded.isPhysicsEnabled() && reloaded.physics().bodyCount() >= 1u, "physics bodies from components");
    const std::string b = (dir / "toy_b.fuselevel").generic_string();
    expectTrue(fuse::world2d::saveWorld2DToFuselevel(reloaded, b).ok && readBytes(a) == readBytes(b),
               "World2D save / load / save identical");
    fuse::scene::Scene scene;
    fuse::ecs::Registry registry;
    fuse::scene::SceneFileInfo info;
    expectTrue(fuse::scene::SceneSerialiser::loadWithRegistry(b, scene, registry, &info).status ==
                       fuse::scene::SerialiseStatus::Ok &&
                   info.dimension == fuse::scene::SceneDimension::World2D,
               "World2D file is tagged 2D");
    u32 boxes = 0;
    registry.each<fuse::ecs::Collider>([&](fuse::ecs::EntityID, fuse::ecs::Collider& c) {
        boxes += (c.shape == fuse::ecs::Collider::Box && nearly(c.params.x, 0.75f) && nearly(c.params.y, 1.25f) &&
                  c.layer == 3u && c.mask == 5u)
                     ? 1u
                     : 0u;
    });
    expectTrue(boxes == 1u, "Box collider with half extents / layer / mask");
}

} // namespace

int main() {
    fuse::core::initialize();
    const fs::path dir = fuse::test::makeUniqueTempDir("fuse_world3d_level");
    testDemoLevel(dir);
    testExampleLevel(dir);
    testLegacyLevel(dir);
    testWorld2DRoundTrip(dir);
    fuse::core::shutdown();
    if (g_failures == 0) {
        std::printf("fuse_world3d_level_gates: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "fuse_world3d_level_gates: %d failure(s)\n", g_failures);
    return 1;
}
