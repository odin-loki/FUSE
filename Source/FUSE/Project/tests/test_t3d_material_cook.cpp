// UNI-U7-MAT-1 / UNI-U7-MIS-1 gates (CPU):
//   materials  Templates/**/materials.tscript -> .fusemat with resolved, strictly cooked texture refs (count +
//              spot check through the runtime readers); MaterialAsset TAML (Prototyping:FloorGray) resolves its
//              ImageAsset; inheritance / specularPower / unresolved maps; BC5 normals; the VFS drain path emits
//              .fusemat (no placeholder texture).
//   archetypes every archetype of the converter table on an inline mission (PointLight, SpotLight, TSStatic,
//              ScatterSky, unknown class -> archetype stub) with component values read back from the v3 file.

#include <fuse/asset/cooked_material.hpp>
#include <fuse/asset/cooked_texture.hpp>
#include <fuse/core/init.hpp>
#include <fuse/core/temp_path.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/environment.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/spawn_marker.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/project/t3d_material_parse.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/scene/serialiser.hpp>
#include <fuse/scene/wire_stub.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifndef FUSE_SOURCE_DIR
#define FUSE_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
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

std::string writeFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
    return path.generic_string();
}

const fs::path kSource(FUSE_SOURCE_DIR);

void testTemplatesMaterialsTscript() {
    // Every Templates/**/materials.tscript.
    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(kSource / "Templates")) {
        if (entry.is_regular_file() && entry.path().filename() == "materials.tscript") {
            files.push_back(entry.path());
        }
    }
    expectTrue(!files.empty(), "Templates contains materials.tscript files");

    const fs::path out = fuse::test::makeUniqueTempDir("fuse_t3d_materials");
    fuse::project::T3DMaterialCookOptions options;
    options.outputDir = out.generic_string();
    options.useVfs = false;
    u32 materials = 0;
    u32 written = 0;
    u32 resolved = 0;
    u32 referenced = 0;
    std::set<std::string> cookedIds;
    for (const fs::path& file : files) {
        const fuse::project::T3DMaterialCookResult cooked = fuse::project::cookT3DMaterialFile(file.string(), options);
        materials += cooked.materialCount;
        written += cooked.fusematWritten;
        resolved += cooked.mapsResolved;
        referenced += cooked.mapsReferenced;
        expectTrue(cooked.textureCookFailures == 0u, "strict texture cooks succeed for " + file.string());
        for (const auto& entry : cooked.entries) {
            if (!entry.albedoCookId.empty()) {
                cookedIds.insert(entry.albedoCookId);
            }
        }
    }
    std::printf("materials.tscript: %zu file(s), %u material(s), %u .fusemat, %u/%u maps resolved, %zu textures\n",
                files.size(), materials, written, resolved, referenced, cookedIds.size());
    // Verve's VervePathTutorial/materials.tscript: 9 singleton Materials over 4 PNGs (non-empty diffuseMap[0] each).
    expectTrue(materials >= 9u, "every singleton Material parsed");
    expectTrue(written == materials, "one .fusemat per material");
    expectTrue(referenced >= 9u && resolved == referenced, "every diffuse map reference resolves to a source image");
    expectTrue(cookedIds.size() >= 4u, "distinct textures cooked once each");

    // Spot check: Door_PILLAR128X128G -> albedo PILLAR128X128G (BC7 sRGB), no normal map.
    fuse::asset::CookedMaterial material;
    std::string error;
    expectTrue(fuse::asset::read_cooked_material_file((out / "Door_PILLAR128X128G.fusemat").string(), material, &error),
               "Door_PILLAR128X128G.fusemat reads back: " + error);
    expectTrue(material.name == "Door_PILLAR128X128G", "fusemat name");
    expectTrue(material.textures.albedo == "PILLAR128X128G", "albedo slot = cooked texture id");
    expectTrue(material.textures.normal.empty(), "empty normalMap -> no normal slot");
    expectTrue(nearly(material.roughness, std::sqrt(std::sqrt(2.f / 10.f)), 1e-5f), "specularPower 8 -> roughness");
    fuse::asset::CookedTexture texture;
    expectTrue(fuse::asset::read_cooked_texture_file((out / "textures" / "PILLAR128X128G.fusetex").string(), texture, &error),
               "PILLAR128X128G.fusetex parses: " + error);
    expectTrue(texture.compression == "BC7" && texture.width == 128u && texture.height == 128u && texture.srgb &&
                   texture.levels.size() == 8u,
               "albedo cooked BC7 sRGB 128x128 with a full mip chain");
    // No placeholder: no texture file for a map that was never referenced.
    expectTrue(!fs::exists(out / "Door_PILLAR128X128G.fusetex"), "the material text is not cooked as a texture");
}

void testMaterialAssetTaml() {
    const fs::path prototyping = kSource / "Templates/BaseGame/game/data/Prototyping";
    const fs::path out = fuse::test::makeUniqueTempDir("fuse_t3d_material_taml");
    fuse::project::T3DMaterialCookOptions options;
    options.outputDir = out.generic_string();
    options.assetSearchRoots = {prototyping.generic_string()};
    options.useVfs = false;
    const fuse::project::T3DMaterialCookResult cooked =
        fuse::project::cookT3DMaterialFile((prototyping / "Materials/FloorGray.asset.taml").string(), options);
    expectTrue(cooked.materialCount == 1u && cooked.fusematWritten == 1u, "FloorGray MaterialAsset -> one .fusemat");
    expectTrue(cooked.mapsResolved == 1u && cooked.texturesCooked == 1u, "DiffuseMapAsset resolves through its ImageAsset");
    expectTrue(!cooked.entries.empty() && cooked.entries[0].albedoSource.find("FloorGray.png") != std::string::npos,
               "ImageAsset imageFile -> FloorGray.png");
    fuse::asset::CookedMaterial material;
    expectTrue(fuse::asset::read_cooked_material_file((out / "FloorGray.fusemat").string(), material),
               "FloorGray.fusemat reads back");
    expectTrue(material.textures.albedo == "FloorGray", "FloorGray albedo slot");
}

void testInlineScript() {
    const fs::path dir = fuse::test::makeUniqueTempDir("fuse_t3d_material_inline");
    // Real image sources (copied) so the strict cook has something to decode.
    fs::copy_file(kSource / "Templates/BaseGame/game/data/Prototyping/Materials/FloorGray.png", dir / "rock.png");
    fs::copy_file(kSource / "Templates/BaseGame/game/data/Prototyping/Materials/FloorGray.png", dir / "rock_n.png");
    writeFile(dir / "not_an_image.png", "this is text, not a PNG");
    const std::string script = writeFile(dir / "rocks.tscript", R"(
// comment with singleton Material(Fake) { diffuseMap[0] = "x"; };
/* block comment
   new Material(AlsoFake) { }; */
singleton Material(Rock)
{
   mapTo = "rock_mesh";
   diffuseMap[0] = "rock";
   normalMap[0] = "./rock_n.png";
   diffuseColor[0] = "1 1 1 1";
   ROUGHNESS[0] = "0.7";
   metalness[0] = 0.25;
   diffuseMap[1] = "ignored_stage1";
   translucent = true;
   alphaRef = 128;
};
singleton Material(MossyRock : Rock)
{
   mapTo = "mossy";
   diffuseColor[0] = "0.2 0.6 0.2 1";
};
new Material(Broken) { diffuseMap[0] = "not_an_image"; };
new Material(Missing) { diffuseMap[0] = "does/not/exist"; specularPower[0] = 32; };
new SimObject(Other) { diffuseMap[0] = "rock"; };
)");
    const fuse::project::T3DMaterialParseResult parsed = fuse::project::parseT3DMaterialFile(script);
    expectTrue(parsed.materials.size() == 4u, "4 Material blocks (comments and other classes skipped)");
    if (parsed.materials.size() == 4u) {
        const fuse::project::T3DMaterialDef& rock = parsed.materials[0];
        expectTrue(rock.name == "Rock" && rock.mapTo == "rock_mesh" && rock.diffuseMap == "rock" &&
                       rock.normalMap == "./rock_n.png",
                   "Rock fields");
        expectTrue(rock.hasRoughness && nearly(rock.roughness, 0.7f) && rock.hasMetalness && nearly(rock.metalness, 0.25f),
                   "keys are case-insensitive; bare numbers parse");
        expectTrue(rock.translucent && nearly(rock.alphaRef, 128.f / 255.f), "alpha settings parsed");
        const fuse::project::T3DMaterialDef& mossy = parsed.materials[1];
        expectTrue(mossy.parent == "Rock" && mossy.diffuseMap == "rock" && mossy.mapTo == "mossy" &&
                       nearly(mossy.diffuseColor[1], 0.6f),
                   "Material(Child : Parent) inherits and overrides");
    }

    fuse::project::T3DMaterialCookOptions options;
    options.outputDir = (dir / "cooked").generic_string();
    options.useVfs = false;
    const fuse::project::T3DMaterialCookResult cooked = fuse::project::cookT3DMaterials(parsed.materials, options);
    expectTrue(cooked.fusematWritten == 4u, "every material emits a .fusemat (untextured when maps fail)");
    expectTrue(cooked.mapsReferenced == 6u, "6 map references (Rock 2, Mossy 2, Broken 1, Missing 1)");
    expectTrue(cooked.mapsUnresolved == 1u, "the missing path is unresolved");
    expectTrue(cooked.textureCookFailures == 1u, "the undecodable image is refused by the strict cook");
    expectTrue(cooked.texturesCooked == 2u && cooked.textureReuses == 2u, "rock albedo + normal cooked once, reused");

    fuse::asset::CookedMaterial rock;
    expectTrue(fuse::asset::read_cooked_material_file((dir / "cooked" / "Rock.fusemat").string(), rock), "Rock.fusemat");
    expectTrue(rock.textures.albedo == "rock" && rock.textures.normal == "rock_n_n", "albedo + normal slots");
    expectTrue(nearly(rock.roughness, 0.7f) && nearly(rock.metallic, 0.25f), "roughness / metalness carried");
    fuse::asset::CookedTexture normal;
    expectTrue(fuse::asset::read_cooked_texture_file((dir / "cooked" / "textures" / "rock_n_n.fusetex").string(), normal),
               "normal map cooked");
    expectTrue(normal.compression == "BC5" && normal.normal_map && !normal.srgb, "normal map -> BC5 linear");
    fuse::asset::CookedMaterial broken;
    expectTrue(fuse::asset::read_cooked_material_file((dir / "cooked" / "Broken.fusemat").string(), broken) &&
                   broken.textures.albedo.empty(),
               "refused texture leaves the slot empty (no placeholder)");
    expectTrue(!fs::exists(dir / "cooked" / "textures" / "not_an_image.fusetex"), "no placeholder .fusetex written");
    fuse::asset::CookedMaterial missing;
    expectTrue(fuse::asset::read_cooked_material_file((dir / "cooked" / "Missing.fusemat").string(), missing) &&
                   nearly(missing.roughness, std::sqrt(std::sqrt(2.f / 34.f)), 1e-5f),
               "specularPower 32 -> roughness");

    // Writer <-> reader agreement on every field.
    fuse::asset::CookedMaterial full = fuse::project::t3dMaterialToCooked(parsed.materials[0], "a", "b");
    full.layers.push_back(fuse::asset::CookedMaterialLayer{});
    full.layers.back().name = "wet";
    full.layers.back().mode = 1u;
    full.layers.back().albedo[0] = 0.5f;
    full.layers.back().albedo[1] = 0.5f;
    full.layers.back().albedo[2] = 0.5f;
    full.procedural_params = {1.f, 2.f};
    const std::vector<fuse::u8> bytes = fuse::project::encodeCookedMaterial(full);
    fuse::asset::CookedMaterial back;
    std::string error;
    expectTrue(fuse::asset::read_cooked_material(bytes.data(), bytes.size(), back, &error) && back == full,
               "encodeCookedMaterial round-trips through read_cooked_material: " + error);
}

template <typename T>
u32 countComponent(fuse::ecs::Registry& registry) {
    u32 n = 0;
    registry.each<T>([&](fuse::ecs::EntityID, T&) { ++n; });
    return n;
}

void testMissionArchetypes() {
    const fs::path dir = fuse::test::makeUniqueTempDir("fuse_mis_archetypes");
    const std::string mission = writeFile(dir / "arch.mis", R"(//--- OBJECT WRITE BEGIN ---
new Scene(ArchLevel) {
   new LevelInfo(theLevelInfo) {
      fogColor = "1 1 1 1";
      fogDensity = "0.01";
      visibleDistance = "750";
      nearClip = "0.2";
      canvasClearColor = "255 0 0 255";
   };
   new ScatterSky(sky) {
      skyBrightness = "30";
      exposure = "2";
   };
   new SimGroup(Lights) {
      new PointLight(lamp) {
         position = "1 2 3";
         radius = "12";
         brightness = "3";
         color = "1 0 0 1";
      };
      new SpotLight(spot) {
         position = "0 0 5";
         rotation = "1 0 0 90";
         range = "20";
         innerAngle = "30";
         outerAngle = "60";
      };
   };
   new TSStatic(rock) {
      shapeName = "art/shapes/rocks/rock1.dts";
      position = "4 5 6";
      scale = "1 2 3";
   };
   new Trigger(zone) {
      position = "0 0 0";
   };
};
//--- OBJECT WRITE END ---
)");
    const std::string output = (dir / "arch.fuselevel").generic_string();
    const fuse::project::ConvertResult converted = fuse::project::convertT3DMissionToFuselevel(mission, output);
    expectTrue(converted.status == fuse::project::ConvertStatus::Ok, "archetype mission converts");
    const fuse::project::MissionArchetypeCounts& c = converted.components;
    expectTrue(c.fogs == 1u && c.atmospheres == 1u && c.groups == 1u && c.pointLights == 1u && c.spotLights == 1u &&
                   c.meshes == 1u && c.unhandled == 1u && c.directionalLights == 0u,
               "archetype counts");
    expectTrue(converted.unexpectedStubCount == 1u && converted.unhandledClasses.size() == 1u &&
                   converted.unhandledClasses[0] == "Trigger",
               "the unknown class is reported with an archetype stub");

    fuse::scene::Scene scene;
    fuse::ecs::Registry registry;
    fuse::scene::SceneFileInfo info;
    expectTrue(fuse::scene::SceneSerialiser::loadWithRegistry(output, scene, registry, &info).status ==
                   fuse::scene::SerialiseStatus::Ok,
               "v3 file loads");
    expectTrue(info.version == 3u && info.hasEcsBlock && info.ecsEntityCount == scene.entityCount(),
               "one ECS entity per scene entity");
    bool archetypeStub = false;
    for (const fuse::scene::SceneEntity& entity : scene.entities()) {
        const fuse::scene::WireStubRef wire = fuse::scene::parseWireStubEntityName(entity.name);
        if (wire.valid && wire.kind == "archetype" && wire.owner == "zone" && wire.value == "Trigger") {
            archetypeStub = true;
        }
    }
    expectTrue(archetypeStub, "__fuse.wire|archetype|zone|Trigger in the scene table");

    registry.each<fuse::ecs::PointLight, fuse::ecs::Transform>(
        [&](fuse::ecs::EntityID, fuse::ecs::PointLight& light, fuse::ecs::Transform& t) {
            expectTrue(nearly(light.radius, 12.f) && nearly(light.intensity, 3.f) && nearly(light.color.x, 1.f) &&
                           nearly(light.color.y, 0.f),
                       "PointLight radius / brightness / colour");
            // Torque (1, 2, 3) -> FUSE (1, 3, -2); parent group is identity.
            expectTrue(nearly(t.local_to_world.data[12], 1.f) && nearly(t.local_to_world.data[13], 3.f) &&
                           nearly(t.local_to_world.data[14], -2.f),
                       "Z-up -> Y-up position");
        });
    registry.each<fuse::ecs::SpotLight, fuse::ecs::Transform>(
        [&](fuse::ecs::EntityID, fuse::ecs::SpotLight& light, fuse::ecs::Transform& t) {
            expectTrue(nearly(light.inner_cone_deg, 15.f) && nearly(light.outer_cone_deg, 30.f) && nearly(light.radius, 20.f),
                       "SpotLight half angles / range");
            // T3D rotation "1 0 0 90" about Torque +X -> FUSE +X, 90 degrees.
            expectTrue(nearly(t.rotation.x, std::sqrt(0.5f)) && nearly(t.rotation.w, std::sqrt(0.5f)),
                       "axis-angle rotation -> quaternion");
        });
    registry.each<fuse::ecs::Mesh, fuse::ecs::MeshAssets, fuse::ecs::Transform>(
        [&](fuse::ecs::EntityID, fuse::ecs::Mesh& mesh, fuse::ecs::MeshAssets& assets, fuse::ecs::Transform& t) {
            expectTrue(!mesh.vertex_buffer.isValid(), "TSStatic mesh waits for the shape cook (no vertex buffer)");
            expectTrue(assets.mesh == fuse::asset::asset_id_of("game:/cooked/art/shapes/rocks/rock1.fusemesh"),
                       "TSStatic keeps the shape asset id");
            expectTrue(nearly(t.scale.x, 1.f) && nearly(t.scale.y, 3.f) && nearly(t.scale.z, 2.f), "scale y / z swap");
        });
    registry.each<fuse::ecs::SkyAtmosphere>([&](fuse::ecs::EntityID, fuse::ecs::SkyAtmosphere& sky) {
        expectTrue(sky.mode == fuse::ecs::SkyAtmosphere::Scattering && nearly(sky.sky_brightness, 30.f) &&
                       nearly(sky.exposure, 2.f),
                   "ScatterSky settings");
    });
    registry.each<fuse::ecs::EnvironmentFog>([&](fuse::ecs::EntityID, fuse::ecs::EnvironmentFog& fog) {
        expectTrue(nearly(fog.density, 0.01f) && nearly(fog.visible_distance, 750.f) && nearly(fog.near_clip, 0.2f) &&
                       nearly(fog.clear_color.x, 1.f) && nearly(fog.clear_color.y, 0.f),
                   "LevelInfo fog / clip / clear colour (ColorI 0..255)");
    });
    expectTrue(countComponent<fuse::ecs::Collider>(registry) == 0u, "no collider invented for TSStatic");
}

} // namespace

int main() {
    fuse::core::initialize();
    testTemplatesMaterialsTscript();
    testMaterialAssetTaml();
    testInlineScript();
    testMissionArchetypes();
    fuse::core::shutdown();
    if (g_failures == 0) {
        std::printf("fuse_t3d_material_mission_gates: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "fuse_t3d_material_mission_gates: %d failure(s)\n", g_failures);
    return 1;
}
