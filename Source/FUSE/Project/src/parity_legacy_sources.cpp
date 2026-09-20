#include <fuse/project/parity_legacy_sources.hpp>

#include <cstring>
#include <filesystem>

namespace fuse::project {

namespace {

bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string swapExtension(const std::string& path, const char* extension) {
    const std::filesystem::path filePath(path);
    return (filePath.parent_path() / (filePath.stem().string() + extension)).lexically_normal().string();
}

std::string joinProjectPath(const std::string& root, const std::string& relative) {
    if (root.empty()) {
        return relative;
    }
    return (std::filesystem::path(root) / relative).lexically_normal().string();
}

std::string bundledLegacyPath(const std::string& fuselevelPath, const char* extension) {
    if (fuselevelPath.size() >= 10 && fuselevelPath.substr(fuselevelPath.size() - 10) == ".fuselevel") {
        return swapExtension(fuselevelPath, extension);
    }
    return fuselevelPath;
}

const char* goldenMissionPathForDemo(const std::string& demoName) {
    if (demoName == "demo_3d_empty") {
        return "Templates/BaseGame/game/data/ExampleModule/levels/ExampleLevel.mis";
    }
    if (demoName == "demo_adventure_stub") {
        return "third_party/addons/3DAAK/Templates/Full/game/levels/Outpost.mis";
    }
    if (demoName == "demo_ai_bt") {
        return "third_party/addons/BadBehaviour/Templates/Full/game/levels/BehaviorTestbed.mis";
    }
    if (demoName == "demo_fx") {
        return "third_party/addons/AFX-Template/game/levels/AFXDemo_Minimal.mis";
    }
    if (demoName == "demo_timeline") {
        return "third_party/addons/Verve/Templates/Full/game/levels/default.mis";
    }
    return nullptr;
}

const char* goldenModulePathForDemo(const std::string& demoName) {
    if (demoName == "demo_2d_sprites") {
        return "third_party/Torque2D/toybox/SpriteToy/1/main.cs";
    }
    if (demoName == "demo_adventure_stub") {
        return nullptr;
    }
    if (demoName == "demo_ai_bt") {
        return "third_party/Torque2D/toybox/SpriteToy/1/main.cs";
    }
    if (demoName == "demo_fx") {
        return "third_party/Torque2D/toybox/SpriteToy/1/main.cs";
    }
    return nullptr;
}

const char* goldenPathForDemo(const std::string& demoName, const char* extension) {
    if (extension != nullptr && std::strcmp(extension, ".cs") == 0) {
        return goldenModulePathForDemo(demoName);
    }
    if (extension != nullptr && std::strcmp(extension, ".mis") == 0) {
        return goldenMissionPathForDemo(demoName);
    }
    return nullptr;
}

} // namespace

std::string findRepositoryRoot(const std::string& startPath) {
    if (startPath.empty()) {
        return {};
    }

    std::error_code ec;
    std::filesystem::path cursor = std::filesystem::absolute(startPath, ec).lexically_normal();
    if (ec || cursor.empty()) {
        cursor = std::filesystem::current_path(ec) / startPath;
        cursor = cursor.lexically_normal();
    }
    while (!cursor.empty()) {
        const std::filesystem::path cmakeLists = cursor / "CMakeLists.txt";
        const std::filesystem::path samples = cursor / "Samples" / "unification";
        if (std::filesystem::exists(cmakeLists, ec) && std::filesystem::exists(samples, ec)) {
            return cursor.lexically_normal().string();
        }
        const std::filesystem::path parent = cursor.parent_path();
        if (parent == cursor) {
            break;
        }
        cursor = parent;
    }

    return {};
}

LegacySourceResolution resolveParityLegacySource(const ProjectManifest& manifest,
                                                 const std::string& fuselevelPath,
                                                 const char* extension) {
    LegacySourceResolution result;
    const std::string bundled = bundledLegacyPath(fuselevelPath, extension);

    const char* goldenRel = goldenPathForDemo(manifest.name, extension);
    if (goldenRel != nullptr) {
        const std::string repoRoot = findRepositoryRoot(manifest.projectRoot);
        if (!repoRoot.empty()) {
            const std::string golden = (std::filesystem::path(repoRoot) / goldenRel).lexically_normal().string();
            if (fileExists(golden)) {
                result.path = golden;
                result.origin = LegacySourceOrigin::GoldenSubmodule;
                result.note = "golden submodule source";
                return result;
            }
        }
    }

    if (fileExists(bundled)) {
        result.path = bundled;
        result.origin = LegacySourceOrigin::Bundled;
        result.note = goldenRel != nullptr ? "bundled stub (golden submodule absent)"
                                           : "bundled legacy source";
        return result;
    }

    result.path = bundled;
    result.origin = LegacySourceOrigin::Missing;
    result.note = "no bundled or golden legacy source";
    return result;
}

} // namespace fuse::project
