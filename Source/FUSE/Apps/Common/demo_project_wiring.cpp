#include "demo_project_wiring.hpp"

#include <fuse/dimension/world_handle.hpp>
#include <fuse/jobs/worker_count.hpp>
#include <fuse/project/t2d_module_bridge.hpp>
#include <fuse/project/t3d_datablock_resolve.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/scene/project_io.hpp>

#include <filesystem>
#include <fstream>

namespace fuse::demo::wiring {

namespace {

bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string swapExtension(const std::string& path, const char* extension) {
    const std::filesystem::path filePath(path);
    return (filePath.parent_path() / (filePath.stem().string() + extension)).lexically_normal().string();
}

std::string legacySourcePath(const std::string& fuselevelPath, const char* extension) {
    if (fuselevelPath.size() >= 10 && fuselevelPath.substr(fuselevelPath.size() - 10) == ".fuselevel") {
        return swapExtension(fuselevelPath, extension);
    }
    return fuselevelPath;
}

} // namespace

ProjectRuntimeContext prepareProjectRuntime(const project::LoadResult& projectLoad) {
    ProjectRuntimeContext context;
    if (projectLoad.status != project::LoadStatus::Ok) {
        return context;
    }

    jobs::setProjectWorkerCap(projectLoad.manifest.workerCap);
    context.vfs = project::mountProjectAssetRoots(projectLoad.manifest);
    context.ok = true;
    return context;
}

World3DLoadResult ensure3DWorldFromProject(const project::LoadResult& projectLoad, scene::Scene& scene) {
    World3DLoadResult result;
    if (projectLoad.status != project::LoadStatus::Ok) {
        result.note = "project load failed";
        return result;
    }
    if (projectLoad.manifest.defaultWorld3D.empty()) {
        result.note = "project missing defaultWorld3D";
        return result;
    }

    const std::string fuselevelPath = scene::resolveDefaultWorldPath(projectLoad.manifest);
    result.loadedPath = fuselevelPath;

    const std::string missionPath = legacySourcePath(fuselevelPath, ".mis");
    const bool needsConvert = !fileExists(fuselevelPath);
    const bool canRefreshFromMis = fileExists(missionPath) &&
                                   (!fileExists(fuselevelPath) ||
                                    std::filesystem::last_write_time(missionPath) >
                                        std::filesystem::last_write_time(fuselevelPath));
    if (needsConvert || canRefreshFromMis) {
        if (!fileExists(missionPath)) {
            result.note = "missing .fuselevel and legacy .mis: " + fuselevelPath;
            return result;
        }

        const project::ConvertResult converted =
            project::convertT3DMissionToFuselevel(missionPath, fuselevelPath);
        if (converted.status != project::ConvertStatus::Ok) {
            result.note = converted.note.empty() ? "T3D mission convert failed" : converted.note;
            return result;
        }
        result.entityCount = converted.entityCount;
        result.wiringStubCount = converted.wiringStubCount;
    }

    const scene::SerialiseResult loaded = scene::loadForProject(scene, projectLoad);
    if (loaded.status != scene::SerialiseStatus::Ok) {
        result.note = loaded.error.empty() ? "scene load failed" : loaded.error;
        return result;
    }

    result.entityCount = scene.entityCount();
    const project::T3DDatablockResolveResult bindings = project::resolveT3DBindingsFromScene(scene);
    result.datablockBindings = bindings.datablockCount;
    result.materialBindings = bindings.materialCount;

    const project::T3DMaterialVfsResolveResult materialVfs =
        project::resolveT3DMaterialVfsFromBindings(bindings);
    result.materialVfsResolved = materialVfs.resolvedCount;

    result.ok = true;
    result.note = "loaded 3D world (" + std::to_string(result.entityCount) + " entities)";
    return result;
}

World2DBridgeResult bridge2DWorldFromProject(const project::LoadResult& projectLoad,
                                             world2d::World2D& world) {
    World2DBridgeResult result;
    if (projectLoad.status != project::LoadStatus::Ok) {
        result.note = "project load failed";
        return result;
    }
    if (projectLoad.manifest.defaultWorld2D.empty()) {
        result.note = "project missing defaultWorld2D";
        return result;
    }

    const std::string fuselevelPath = scene::resolveDefaultWorld2DPath(projectLoad.manifest);
    const std::string modulePath = legacySourcePath(fuselevelPath, ".cs");

    if (!fileExists(fuselevelPath) && fileExists(modulePath)) {
        const project::ConvertResult converted =
            project::convertT2DModuleToFuselevel(modulePath, fuselevelPath);
        if (converted.status != project::ConvertStatus::Ok) {
            const project::T2DRuntimeBridgeResult bridged =
                project::bridgeT2DModuleToRuntime(world, modulePath);
            result.ok = bridged.ok;
            result.spriteCount = bridged.spriteCount;
            result.physicsBodyCount = bridged.physicsBodyCount;
            result.physicsEnabled = world.isPhysicsEnabled();
            result.note = bridged.note.empty() ? "T2D module runtime bridge" : bridged.note;
            return result;
        }
    }

    if (fileExists(modulePath) && world.readSnapshot().sprites().empty()) {
        const project::T2DRuntimeBridgeResult bridged = project::bridgeT2DModuleToRuntime(world, modulePath);
        if (bridged.ok && bridged.spriteCount > 0u) {
            result.ok = true;
            result.spriteCount = bridged.spriteCount;
            result.physicsBodyCount = bridged.physicsBodyCount;
            result.physicsEnabled = world.isPhysicsEnabled();
            result.note = "runtime bridge from " + modulePath;
            return result;
        }
    }

    world.setProjectWorldSource(projectLoad.manifest.projectRoot, projectLoad.manifest.defaultWorld2D);
    world.loadWorld(dimension::WorldHandle(2u, 1u));

    const auto& sprites = world.readSnapshot().sprites();
    result.spriteCount = static_cast<u32>(sprites.size());
    result.physicsEnabled = world.isPhysicsEnabled();
    result.ok = result.spriteCount > 0u || fileExists(fuselevelPath);
    result.note = result.ok ? "World2D loaded from project defaultWorld2D" : "no sprites after World2D load";
    return result;
}

} // namespace fuse::demo::wiring
