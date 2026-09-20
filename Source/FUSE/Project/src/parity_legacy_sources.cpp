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
        return "third_party/addons/3DAAK/Templates/Full/game/data/interact.cs";
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

std::string submoduleRootForGoldenRelative(const char* goldenRel) {
    if (goldenRel == nullptr) {
        return {};
    }

    const std::string path(goldenRel);
    if (path.rfind("third_party/Torque2D/", 0) == 0) {
        return "third_party/Torque2D";
    }
    if (path.rfind("third_party/addons/", 0) == 0) {
        const std::size_t slash = path.find('/', 19);
        if (slash != std::string::npos) {
            return path.substr(0, slash);
        }
        return "third_party/addons";
    }
    if (path.rfind("Templates/", 0) == 0) {
        return "Templates";
    }
    return {};
}

std::string bundledFallbackNote(const char* goldenRel, SubmodulePathStatus status) {
    if (goldenRel == nullptr) {
        return "bundled legacy source";
    }

    switch (status) {
    case SubmodulePathStatus::Uninitialized:
        return "bundled stub (golden submodule checkout empty — no network init attempted)";
    case SubmodulePathStatus::Absent:
        return "bundled stub (golden submodule path absent in workspace)";
    case SubmodulePathStatus::Present:
        return "bundled stub (golden file missing despite initialized submodule tree)";
    }
    return "bundled stub (golden submodule absent)";
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

SubmodulePathStatus classifySubmoduleDirectory(const std::string& directoryPath) {
    if (directoryPath.empty()) {
        return SubmodulePathStatus::Absent;
    }

    std::error_code ec;
    if (!std::filesystem::exists(directoryPath, ec)) {
        return SubmodulePathStatus::Absent;
    }

    if (!std::filesystem::is_directory(directoryPath, ec)) {
        return SubmodulePathStatus::Present;
    }

    bool hasNonGitEntry = false;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directoryPath, ec)) {
        if (ec) {
            break;
        }

        const std::string leaf = entry.path().filename().string();
        if (leaf == ".git") {
            continue;
        }
        hasNonGitEntry = true;
        break;
    }

    if (hasNonGitEntry) {
        return SubmodulePathStatus::Present;
    }

    const std::filesystem::path gitMarker = std::filesystem::path(directoryPath) / ".git";
    if (std::filesystem::exists(gitMarker, ec) && std::filesystem::is_regular_file(gitMarker, ec)) {
        return SubmodulePathStatus::Uninitialized;
    }

    return SubmodulePathStatus::Uninitialized;
}

LegacySourceResolution resolveParityLegacySource(const ProjectManifest& manifest,
                                                 const std::string& fuselevelPath,
                                                 const char* extension) {
    LegacySourceResolution result;
    const std::string bundled = bundledLegacyPath(fuselevelPath, extension);

    const char* goldenRel = goldenPathForDemo(manifest.name, extension);
    const std::string repoRoot = findRepositoryRoot(manifest.projectRoot);

    if (goldenRel != nullptr && !repoRoot.empty()) {
        const std::string submoduleRootRel = submoduleRootForGoldenRelative(goldenRel);
        if (!submoduleRootRel.empty()) {
            const std::string submoduleRoot =
                (std::filesystem::path(repoRoot) / submoduleRootRel).lexically_normal().string();
            result.goldenSubmoduleStatus = classifySubmoduleDirectory(submoduleRoot);
        }

        const std::string golden = (std::filesystem::path(repoRoot) / goldenRel).lexically_normal().string();
        if (fileExists(golden)) {
            result.path = golden;
            result.origin = LegacySourceOrigin::GoldenSubmodule;
            result.note = "golden submodule source";
            return result;
        }
    }

    if (fileExists(bundled)) {
        result.path = bundled;
        result.origin = LegacySourceOrigin::Bundled;
        result.note = bundledFallbackNote(goldenRel, result.goldenSubmoduleStatus);
        return result;
    }

    result.path = bundled;
    result.origin = LegacySourceOrigin::Missing;
    result.note = "no bundled or golden legacy source";
    return result;
}

} // namespace fuse::project
