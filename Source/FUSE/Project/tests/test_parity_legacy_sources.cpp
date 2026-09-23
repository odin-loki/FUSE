#include <fuse/core/temp_path.hpp>
#include <fuse/core/init.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/project/parity_legacy_sources.hpp>
#include <fuse/project/world_converter.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

void testRepositoryRootFromParityDemo() {
    const std::string root =
        fuse::project::findRepositoryRoot("Samples/unification/demo_2d_sprites");
    expectTrue(!root.empty(), "repository root resolved from parity demo path");
    expectTrue(root.find("Samples") == std::string::npos, "repository root is not the demo folder");
}

void testSubmoduleDirectoryClassification() {
    const std::string emptyDir = fuse::test::tempPath("fuse_submodule_empty_probe");
    std::filesystem::create_directories(emptyDir);
    expectTrue(fuse::project::classifySubmoduleDirectory(emptyDir) ==
                   fuse::project::SubmodulePathStatus::Uninitialized,
               "empty directory classified as uninitialized submodule checkout");

    const std::string populatedDir = fuse::test::tempPath("fuse_submodule_populated_probe");
    std::filesystem::create_directories(populatedDir);
    writeTempFile(populatedDir + "/main.cs", "module \"Probe\";");
    expectTrue(fuse::project::classifySubmoduleDirectory(populatedDir) ==
                   fuse::project::SubmodulePathStatus::Present,
               "populated directory classified as present submodule checkout");
}

void testGoldenSubmoduleStatusOnSpriteToyFallback() {
    const fuse::project::LoadResult project =
        fuse::project::loadFromDirectory("Samples/unification/demo_2d_sprites");
    expectTrue(project.status == fuse::project::LoadStatus::Ok, "demo_2d_sprites loads");

    const std::string fuselevelPath =
        project.manifest.projectRoot + "/worlds/sprite_toy_stub.fuselevel";
    const fuse::project::LegacySourceResolution source =
        fuse::project::resolveParityLegacySource(project.manifest, fuselevelPath, ".cs");
    expectTrue(source.origin == fuse::project::LegacySourceOrigin::Bundled,
               "SpriteToy falls back to bundled stub when submodule absent");
    expectTrue(source.goldenSubmoduleStatus != fuse::project::SubmodulePathStatus::Present,
               "SpriteToy golden submodule status recorded when golden file missing");
    expectTrue(!source.note.empty(), "SpriteToy fallback note explains golden-path status");
}

void testBundledSpriteToyFallback() {
    const fuse::project::LoadResult project =
        fuse::project::loadFromDirectory("Samples/unification/demo_2d_sprites");
    expectTrue(project.status == fuse::project::LoadStatus::Ok, "demo_2d_sprites loads");

    const std::string fuselevelPath =
        project.manifest.projectRoot + "/worlds/sprite_toy_stub.fuselevel";
    const fuse::project::LegacySourceResolution source =
        fuse::project::resolveParityLegacySource(project.manifest, fuselevelPath, ".cs");
    expectTrue(source.origin == fuse::project::LegacySourceOrigin::Bundled,
               "SpriteToy falls back to bundled stub when submodule absent");
    expectTrue(!source.path.empty(), "bundled SpriteToy stub path resolved");
}

void testBundledOutpostFallback() {
    const fuse::project::LoadResult project =
        fuse::project::loadFromDirectory("Samples/unification/demo_adventure_stub");
    expectTrue(project.status == fuse::project::LoadStatus::Ok, "demo_adventure_stub loads");

    const std::string fuselevelPath = project.manifest.projectRoot + "/worlds/outpost.fuselevel";
    const fuse::project::LegacySourceResolution source =
        fuse::project::resolveParityLegacySource(project.manifest, fuselevelPath, ".mis");
    expectTrue(source.origin == fuse::project::LegacySourceOrigin::Bundled,
               "Outpost falls back to bundled stub when submodule absent");
}

void testEnsure3DWorldFromBundledMis() {
    const fuse::project::LoadResult project =
        fuse::project::loadFromDirectory("Samples/unification/demo_3d_empty");
    expectTrue(project.status == fuse::project::LoadStatus::Ok, "demo_3d_empty loads");

    const fuse::project::Ensure3DWorldResult prepared =
        fuse::project::ensureDefault3DWorldReady(project);
    expectTrue(prepared.ok, "demo_3d_empty 3D world prepared from bundled/golden source");
    expectTrue(prepared.entityCount >= 2u, "demo_3d_empty fuselevel has mission entities");
}

void testBundledAfxMinimalFallback() {
    const fuse::project::LoadResult project =
        fuse::project::loadFromDirectory("Samples/unification/demo_fx");
    expectTrue(project.status == fuse::project::LoadStatus::Ok, "demo_fx loads");

    const std::string fuselevelPath = project.manifest.projectRoot + "/worlds/afx_minimal.fuselevel";
    const fuse::project::LegacySourceResolution source =
        fuse::project::resolveParityLegacySource(project.manifest, fuselevelPath, ".mis");
    expectTrue(source.origin == fuse::project::LegacySourceOrigin::Bundled,
               "AFX minimal falls back to bundled stub when submodule absent");
    expectTrue(!source.note.empty(), "AFX minimal fallback note recorded");
}

void testBundledBehaviorTestbedFallback() {
    const fuse::project::LoadResult project =
        fuse::project::loadFromDirectory("Samples/unification/demo_ai_bt");
    expectTrue(project.status == fuse::project::LoadStatus::Ok, "demo_ai_bt loads");

    const std::string fuselevelPath =
        project.manifest.projectRoot + "/worlds/behavior_testbed.fuselevel";
    const fuse::project::LegacySourceResolution source =
        fuse::project::resolveParityLegacySource(project.manifest, fuselevelPath, ".mis");
    expectTrue(source.origin == fuse::project::LegacySourceOrigin::Bundled,
               "BehaviorTestbed falls back to bundled stub when submodule absent");
}

void testBundledVerveIntroFallback() {
    const fuse::project::LoadResult project =
        fuse::project::loadFromDirectory("Samples/unification/demo_timeline");
    expectTrue(project.status == fuse::project::LoadStatus::Ok, "demo_timeline loads");

    const std::string fuselevelPath = project.manifest.projectRoot + "/worlds/verve_intro.fuselevel";
    const fuse::project::LegacySourceResolution source =
        fuse::project::resolveParityLegacySource(project.manifest, fuselevelPath, ".mis");
    expectTrue(source.origin == fuse::project::LegacySourceOrigin::Bundled,
               "Verve intro falls back to bundled stub when submodule absent");
}

void testEnsure2DWorldFromBundledCs() {
    const fuse::project::LoadResult project =
        fuse::project::loadFromDirectory("Samples/unification/demo_2d_sprites");
    expectTrue(project.status == fuse::project::LoadStatus::Ok, "demo_2d_sprites loads");

    const fuse::project::Ensure2DWorldResult prepared =
        fuse::project::ensureDefault2DWorldReady(project);
    expectTrue(prepared.ok, "demo_2d_sprites 2D world prepared from bundled/golden source");
    expectTrue(prepared.entityCount >= 3u, "SpriteToy stub produces sprite entities");
    expectTrue(prepared.wiringStubCount >= 1u, "SpriteToy stub emits animated-sprite wiring stubs");
}

void testEnsure2DWorldFromAfxSpriteStub() {
    const fuse::project::LoadResult project =
        fuse::project::loadFromDirectory("Samples/unification/demo_fx");
    expectTrue(project.status == fuse::project::LoadStatus::Ok, "demo_fx loads");

    const fuse::project::Ensure2DWorldResult prepared =
        fuse::project::ensureDefault2DWorldReady(project);
    expectTrue(prepared.ok, "demo_fx defaultWorld2D prepared from bundled sprite stub");
}

} // namespace

int main() {
    fuse::core::initialize();
    testRepositoryRootFromParityDemo();
    testSubmoduleDirectoryClassification();
    testGoldenSubmoduleStatusOnSpriteToyFallback();
    testBundledSpriteToyFallback();
    testBundledOutpostFallback();
    testEnsure3DWorldFromBundledMis();
    testBundledAfxMinimalFallback();
    testBundledBehaviorTestbedFallback();
    testBundledVerveIntroFallback();
    testEnsure2DWorldFromBundledCs();
    testEnsure2DWorldFromAfxSpriteStub();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_parity_legacy_sources_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_parity_legacy_sources_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
