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
        return "Samples/unification/demo_adventure_stub/worlds/outpost.mis";
    }
    if (demoName == "demo_ai_bt") {
        return "Samples/unification/demo_ai_bt/worlds/behavior_testbed.mis";
    }
    if (demoName == "demo_fx") {
        return "Samples/unification/demo_fx/worlds/afx_minimal.mis";
    }
    if (demoName == "demo_timeline") {
        return "Samples/unification/demo_timeline/worlds/verve_intro.mis";
    }
    return nullptr;
}

const char* goldenModulePathForDemo(const std::string& demoName) {
    if (demoName == "demo_2d_sprites") {
        return "Samples/unification/demo_2d_sprites/worlds/sprite_toy_stub.cs";
    }
    if (demoName == "demo_adventure_stub") {
        return "Samples/unification/demo_adventure_stub/worlds/interact_2d.cs";
    }
    if (demoName == "demo_ai_bt") {
        return "Samples/unification/demo_ai_bt/worlds/sprite_agent.cs";
    }
    if (demoName == "demo_fx") {
        return "Samples/unification/demo_fx/worlds/afx_sprite_stub.cs";
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

std::string goldenRootForRelative(const char* goldenRel) {
    if (goldenRel == nullptr) {
        return {};
    }

    const std::string path(goldenRel);
    if (path.rfind("Samples/unification/", 0) == 0) {
        return "Samples/unification";
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
        return "bundled stub (golden reference tree empty)";
    case SubmodulePathStatus::Absent:
        return "bundled stub (golden reference path absent in workspace)";
    case SubmodulePathStatus::Present:
        return "bundled stub (golden file missing despite reference tree)";
    }
    return "bundled stub (golden reference absent)";
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
        const std::string goldenRootRel = goldenRootForRelative(goldenRel);
        if (!goldenRootRel.empty()) {
            const std::string goldenRoot =
                (std::filesystem::path(repoRoot) / goldenRootRel).lexically_normal().string();
            result.goldenSubmoduleStatus = classifySubmoduleDirectory(goldenRoot);
        }

        const std::string golden = (std::filesystem::path(repoRoot) / goldenRel).lexically_normal().string();
        if (fileExists(golden)) {
            result.path = golden;
            result.origin = LegacySourceOrigin::GoldenSubmodule;
            result.note = "golden reference source";
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
