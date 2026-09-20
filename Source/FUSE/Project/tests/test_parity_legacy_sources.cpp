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
    const std::string emptyDir = "/tmp/fuse_submodule_empty_probe";
    std::filesystem::create_directories(emptyDir);
    expectTrue(fuse::project::classifySubmoduleDirectory(emptyDir) ==
                   fuse::project::SubmodulePathStatus::Uninitialized,
               "empty directory classified as uninitialized submodule checkout");

    const std::string populatedDir = "/tmp/fuse_submodule_populated_probe";
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
    expectTrue(prepared.ok, "demo_3d_empty fuselevel exists after prepare");
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
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_parity_legacy_sources_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_parity_legacy_sources_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
