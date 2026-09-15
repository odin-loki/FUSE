#include <fuse/scene/project_io.hpp>

#include <fuse/scene/serialiser.hpp>

#include <filesystem>
#include <system_error>

namespace fuse::scene {

namespace {

std::string joinPath(const std::string& root, const std::string& relative) {
    if (root.empty()) {
        return relative;
    }

    std::filesystem::path path(root);
    path /= relative;
    return path.lexically_normal().string();
}

bool ensureParentDirectory(const std::string& filePath) {
    const std::filesystem::path parent = std::filesystem::path(filePath).parent_path();
    if (parent.empty()) {
        return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    return !ec;
}

} // namespace

std::string resolveDefaultWorldPath(const project::ProjectManifest& manifest) {
    return joinPath(manifest.projectRoot, manifest.defaultWorld3D);
}

SerialiseResult saveForProject(const Scene& scene, const project::ProjectManifest& manifest) {
    SerialiseResult result;

    if (manifest.defaultWorld3D.empty()) {
        result.status = SerialiseStatus::IoError;
        result.error = "project manifest missing defaultWorld3D";
        return result;
    }

    const std::string worldPath = resolveDefaultWorldPath(manifest);
    if (!ensureParentDirectory(worldPath)) {
        result.status = SerialiseStatus::IoError;
        result.error = "unable to create parent directory for: " + worldPath;
        return result;
    }

    return SceneSerialiser::save(scene, worldPath);
}

SerialiseResult loadForProject(Scene& scene, const project::ProjectManifest& manifest) {
    if (manifest.defaultWorld3D.empty()) {
        SerialiseResult result;
        result.status = SerialiseStatus::IoError;
        result.error = "project manifest missing defaultWorld3D";
        return result;
    }

    return SceneSerialiser::load(resolveDefaultWorldPath(manifest), scene);
}

SerialiseResult loadForProject(Scene& scene, const project::LoadResult& projectLoad) {
    if (projectLoad.status != project::LoadStatus::Ok) {
        SerialiseResult result;
        result.status = SerialiseStatus::IoError;
        result.error = "project load failed: " + projectLoad.error;
        return result;
    }

    return loadForProject(scene, projectLoad.manifest);
}

} // namespace fuse::scene
