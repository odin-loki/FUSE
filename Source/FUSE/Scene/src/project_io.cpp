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
    return path.lexically_normal().generic_string(); // "/" separators on Windows too
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

std::string resolveDefaultWorld2DPath(const project::ProjectManifest& manifest) {
    return joinPath(manifest.projectRoot, manifest.defaultWorld2D);
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

namespace {

const std::string& defaultWorldRel(const project::ProjectManifest& manifest, SceneDimension dimension) {
    return dimension == SceneDimension::World2D ? manifest.defaultWorld2D : manifest.defaultWorld3D;
}

std::string defaultWorldPath(const project::ProjectManifest& manifest, SceneDimension dimension) {
    return dimension == SceneDimension::World2D ? resolveDefaultWorld2DPath(manifest) : resolveDefaultWorldPath(manifest);
}

SerialiseResult missingWorld(SceneDimension dimension) {
    SerialiseResult result;
    result.status = SerialiseStatus::IoError;
    result.error = dimension == SceneDimension::World2D ? "project manifest missing defaultWorld2D"
                                                         : "project manifest missing defaultWorld3D";
    return result;
}

} // namespace

SerialiseResult saveWorldForProject(const Scene& scene, const ecs::Registry& registry,
                                    const project::ProjectManifest& manifest, SceneDimension dimension) {
    if (defaultWorldRel(manifest, dimension).empty()) {
        return missingWorld(dimension);
    }
    const std::string worldPath = defaultWorldPath(manifest, dimension);
    if (!ensureParentDirectory(worldPath)) {
        SerialiseResult result;
        result.status = SerialiseStatus::IoError;
        result.error = "unable to create parent directory for: " + worldPath;
        return result;
    }
    return SceneSerialiser::saveWithRegistry(scene, registry, worldPath, dimension);
}

SerialiseResult loadWorldForProject(Scene& scene, ecs::Registry& registry, const project::ProjectManifest& manifest,
                                    SceneDimension dimension, SceneFileInfo* info) {
    if (defaultWorldRel(manifest, dimension).empty()) {
        return missingWorld(dimension);
    }
    return SceneSerialiser::loadWithRegistry(defaultWorldPath(manifest, dimension), scene, registry, info);
}

} // namespace fuse::scene
