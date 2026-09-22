#include <fuse/core/init.hpp>
#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/project/importer_extract.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/project/parity_legacy_sources.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/scene/mission_load.hpp>
#include <fuse/scene/project_io.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/scene/serialiser.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifndef FUSE_SOURCE_DIR
#define FUSE_SOURCE_DIR ""
#endif

namespace {

int g_failures = 0;

void expectTrue(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

std::string demoMsg(const char* demo, const char* text) {
    return std::string(demo) + ": " + text;
}

std::filesystem::path findRepoRoot() {
    const std::filesystem::path relative =
        std::filesystem::path("Samples") / "unification" / "demo_3d_empty" / "project.json";

    if (!std::string(FUSE_SOURCE_DIR).empty()) {
        const std::filesystem::path fromMacro = std::filesystem::path(FUSE_SOURCE_DIR);
        if (std::filesystem::exists(fromMacro / relative) ||
            std::filesystem::exists(fromMacro / "Samples" / "unification")) {
            return fromMacro;
        }
    }

    std::filesystem::path dir = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
        if (std::filesystem::exists(dir / relative) ||
            std::filesystem::exists(dir / "Samples" / "unification")) {
            return dir;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) {
            break;
        }
        dir = dir.parent_path();
    }

    return {};
}

bool fileExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string readFileToString(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::filesystem::path withExtension(std::filesystem::path path, const char* extension) {
    path.replace_extension(extension);
    return path;
}

std::filesystem::path findFirstWorldFile(const std::filesystem::path& worldsDir, const char* extension) {
    std::error_code ec;
    if (!std::filesystem::exists(worldsDir, ec)) {
        return {};
    }

    for (const auto& entry : std::filesystem::directory_iterator(worldsDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (entry.path().extension() == extension) {
            return entry.path();
        }
    }
    return {};
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

void populateFromT2D(fuse::scene::Scene& scene, const fuse::project::T2DModuleExtract& extract) {
    if (!extract.moduleName.empty()) {
        scene.setName(extract.moduleName);
    }
    scene.clearEntities();
    for (const fuse::project::T2DSceneNodeStub& node : extract.sceneNodes) {
        const std::string& name = node.objectName.empty() ? node.className : node.objectName;
        scene.addEntity(name);
    }
}

bool loadWorldIntoScene(const fuse::project::LoadResult& project, fuse::scene::Scene& scene,
                        std::string& via) {
    via.clear();

    if (!project.manifest.defaultWorld3D.empty()) {
        const std::string fuselevelPath = fuse::scene::resolveDefaultWorldPath(project.manifest);
        std::filesystem::path missionPath = withExtension(fuselevelPath, ".mis");
        if (!fileExists(missionPath)) {
            missionPath = findFirstWorldFile(std::filesystem::path(project.manifest.projectRoot) / "worlds",
                                             ".mis");
        }

        // Production converter (wire stubs included) so the cooked sample matches what demos load.
        (void)fuse::project::ensureDefault3DWorldReady(project);

        const fuse::scene::SerialiseResult loaded = fuse::scene::loadForProject(scene, project);
        if (loaded.status == fuse::scene::SerialiseStatus::Ok && scene.entityCount() > 0u) {
            via = "loadForProject";
            return true;
        }

        if (fileExists(missionPath)) {
            const fuse::scene::MissionLoadResult mission =
                fuse::scene::loadMissionToHandleMap(missionPath.string());
            if (mission.ok) {
                populateFromMission(scene, mission);
                via = "loadMissionToHandleMap";
                return scene.entityCount() > 0u;
            }
        }
    }

    if (!project.manifest.defaultWorld2D.empty()) {
        const std::string fuselevelPath = fuse::scene::resolveDefaultWorld2DPath(project.manifest);
        std::filesystem::path modulePath = withExtension(fuselevelPath, ".cs");
        if (!fileExists(modulePath)) {
            modulePath = findFirstWorldFile(std::filesystem::path(project.manifest.projectRoot) / "worlds",
                                            ".cs");
        }

        (void)fuse::project::ensureDefault2DWorldReady(project);

        if (fileExists(fuselevelPath)) {
            const fuse::scene::SerialiseResult loaded =
                fuse::scene::SceneSerialiser::load(fuselevelPath, scene);
            if (loaded.status == fuse::scene::SerialiseStatus::Ok && scene.entityCount() > 0u) {
                via = "SceneSerialiser::load";
                return true;
            }
        }

        if (fileExists(modulePath)) {
            const std::string text = readFileToString(modulePath);
            const fuse::project::T2DModuleExtract extract =
                fuse::project::extractT2DModuleFields(text, modulePath.string());
            populateFromT2D(scene, extract);
            via = "extractT2DModuleFields";
            return scene.entityCount() > 0u;
        }
    }

    return false;
}

bool shipsMission(const std::filesystem::path& demoDir, const fuse::project::LoadResult& project) {
    if (!project.manifest.defaultWorld3D.empty()) {
        const std::filesystem::path fuselevel = fuse::scene::resolveDefaultWorldPath(project.manifest);
        if (fileExists(fuselevel) || fileExists(withExtension(fuselevel, ".mis"))) {
            return true;
        }
    }
    if (!project.manifest.defaultWorld2D.empty()) {
        const std::filesystem::path fuselevel = fuse::scene::resolveDefaultWorld2DPath(project.manifest);
        if (fileExists(fuselevel) || fileExists(withExtension(fuselevel, ".cs"))) {
            return true;
        }
    }

    const std::filesystem::path worlds = demoDir / "worlds";
    return fileExists(findFirstWorldFile(worlds, ".mis")) || fileExists(findFirstWorldFile(worlds, ".cs"));
}

bool findEntityByName(const fuse::scene::Scene& scene, const char* name) {
    for (fuse::u32 i = 0; i < scene.entityCount(); ++i) {
        const fuse::scene::SceneEntity* entity = scene.entityAt(i);
        if (entity != nullptr && entity->name == name) {
            return true;
        }
    }
    return false;
}

void assertHandlesAndRoundTrip(const char* demo, fuse::scene::Scene& scene) {
    const fuse::u32 count = scene.entityCount();
    bool allPresent = true;
    bool anyNamed = false;
    for (fuse::u32 i = 0; i < count; ++i) {
        expectTrue(scene.entityAt(i) != nullptr, demoMsg(demo, "entityAt(i) is non-null"));
        const fuse::Handle<fuse::Object> handle(i, 1u);
        const fuse::scene::SceneEntity* resolved = scene.entityAt(handle.index());
        if (resolved == nullptr) {
            allPresent = false;
            continue;
        }
        if (!resolved->name.empty()) {
            anyNamed = true;
        }
    }
    expectTrue(allPresent, demoMsg(demo, "Handle<Object>(index, 1) resolves via entityAt"));
    if (count > 0u) {
        expectTrue(anyNamed, demoMsg(demo, "at least one entity name is non-empty"));
    }

    const std::filesystem::path roundTripPath =
        std::filesystem::temp_directory_path() / (std::string("fuse_p7_parity_") + demo + ".fuselevel");
    const fuse::scene::SerialiseResult saved =
        fuse::scene::SceneSerialiser::save(scene, roundTripPath.string());
    expectTrue(saved.status == fuse::scene::SerialiseStatus::Ok, demoMsg(demo, "SceneSerialiser::save ok"));

    fuse::scene::Scene reloaded;
    const fuse::scene::SerialiseResult loaded =
        fuse::scene::SceneSerialiser::load(roundTripPath.string(), reloaded);
    expectTrue(loaded.status == fuse::scene::SerialiseStatus::Ok, demoMsg(demo, "SceneSerialiser::load ok"));
    expectTrue(reloaded.entityCount() == count, demoMsg(demo, "round-trip entity count matches"));
}

struct DemoSpec {
    const char* name;
    bool required;
    const char* requiredEntity;       ///< Expected when the bundled sample mission is loaded
    const char* goldenRequiredEntity; ///< Expected when the golden in-tree/submodule mission wins
};

void testDemo(const std::filesystem::path& repoRoot, const DemoSpec& spec) {
    const std::filesystem::path demoDir =
        repoRoot / "Samples" / "unification" / spec.name;
    if (!fileExists(demoDir / "project.json")) {
        if (spec.required) {
            expectTrue(false, demoMsg(spec.name, "required sample directory with project.json is missing"));
        } else {
            std::printf("p7 parity %s: skipped (missing)\n", spec.name);
        }
        return;
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(demoDir.string());
    expectTrue(project.status == fuse::project::LoadStatus::Ok, demoMsg(spec.name, "loadFromDirectory ok"));
    if (project.status != fuse::project::LoadStatus::Ok) {
        if (!project.error.empty()) {
            std::fprintf(stderr, "FAIL: %s loadFromDirectory: %s\n", spec.name, project.error.c_str());
        }
        return;
    }

    const bool expectEntities = shipsMission(demoDir, project);
    fuse::scene::Scene scene;
    std::string via;
    const bool loaded = loadWorldIntoScene(project, scene, via);

    if (expectEntities) {
        expectTrue(loaded, demoMsg(spec.name, "world loaded via loadForProject or P4/.cs helper"));
        expectTrue(scene.entityCount() > 0u, demoMsg(spec.name, "entityCount() > 0"));
    }

    if (!loaded) {
        std::printf("p7 parity %s: project.json ok, no world to load\n", spec.name);
        return;
    }

    // resolveParityLegacySource prefers golden missions over the bundled sample copy,
    // so the entity to expect depends on which source was converted.
    const char* requiredEntity = spec.requiredEntity;
    if (!project.manifest.defaultWorld3D.empty()) {
        const fuse::project::LegacySourceResolution source = fuse::project::resolveParityLegacySource(
            project.manifest, fuse::scene::resolveDefaultWorldPath(project.manifest), ".mis");
        if (source.origin == fuse::project::LegacySourceOrigin::GoldenSubmodule) {
            requiredEntity = spec.goldenRequiredEntity;
        }
    }
    if (requiredEntity != nullptr) {
        expectTrue(findEntityByName(scene, requiredEntity),
                   demoMsg(spec.name, (std::string("required entity present: ") + requiredEntity).c_str()));
    }

    assertHandlesAndRoundTrip(spec.name, scene);
    std::printf("p7 parity %s: %u entities via %s\n", spec.name, scene.entityCount(), via.c_str());
}

void testParitySuite(const std::filesystem::path& repoRoot) {
    expectTrue(!repoRoot.empty(), "repo root located via FUSE_SOURCE_DIR");
    if (repoRoot.empty()) {
        return;
    }

    const DemoSpec demos[] = {
        {"demo_3d_empty", true, "Floor", "GroundPlane"},
        {"demo_2d_sprites", true, nullptr, nullptr},
        {"demo_ai_bt", false, nullptr, nullptr},
        {"demo_timeline", false, nullptr, nullptr},
        {"demo_fx", false, nullptr, nullptr},
        {"demo_adventure_stub", false, nullptr, nullptr},
    };

    for (const DemoSpec& spec : demos) {
        testDemo(repoRoot, spec);
    }
}

} // namespace

int main() {
    fuse::core::initialize();

    testParitySuite(findRepoRoot());

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_p7_parity_scenes: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_p7_parity_scenes: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
