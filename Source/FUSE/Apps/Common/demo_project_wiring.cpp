#include "demo_project_wiring.hpp"

#include <fuse/dimension/world_handle.hpp>
#include <fuse/jobs/worker_count.hpp>
#include <fuse/project/parity_legacy_sources.hpp>
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

    const project::Ensure3DWorldResult prepared = project::ensureDefault3DWorldReady(projectLoad);
    result.loadedPath = prepared.loadedPath;
    result.entityCount = prepared.entityCount;
    result.wiringStubCount = prepared.wiringStubCount;
    if (!prepared.ok) {
        result.note = prepared.note;
        return result;
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
    if (prepared.sourceOrigin == project::LegacySourceOrigin::GoldenSubmodule) {
        result.note = "loaded 3D world from golden submodule (" + std::to_string(result.entityCount) +
                      " entities)";
    } else {
        result.note = "loaded 3D world (" + std::to_string(result.entityCount) + " entities)";
    }
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
    const project::LegacySourceResolution moduleSource =
        project::resolveParityLegacySource(projectLoad.manifest, fuselevelPath, ".cs");
    const std::string& modulePath = moduleSource.path;

    if (!fileExists(fuselevelPath) && moduleSource.origin != project::LegacySourceOrigin::Missing) {
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

    if (moduleSource.origin != project::LegacySourceOrigin::Missing && world.readSnapshot().sprites().empty()) {
        const project::T2DRuntimeBridgeResult bridged = project::bridgeT2DModuleToRuntime(world, modulePath);
        if (bridged.ok && bridged.spriteCount > 0u) {
            result.ok = true;
            result.spriteCount = bridged.spriteCount;
            result.physicsBodyCount = bridged.physicsBodyCount;
            result.physicsEnabled = world.isPhysicsEnabled();
            if (moduleSource.origin == project::LegacySourceOrigin::GoldenSubmodule) {
                result.note = "runtime bridge from golden SpriteToy submodule";
            } else {
                result.note = "runtime bridge from " + modulePath;
            }
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
