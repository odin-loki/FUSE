#include "demo_check.hpp"
#include "demo_project_wiring.hpp"

#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/world3d/world_3d.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#include <fuse/renderer/vk/instance.hpp>
#endif

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_3d_empty: 3D dimension path + U7 mission convert/load");

    std::string projectPath = "Samples/unification/demo_3d_empty";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.dimensions.enable3D, "project enables 3D");

    const fuse::demo::wiring::ProjectRuntimeContext runtime =
        fuse::demo::wiring::prepareProjectRuntime(project);
    fuse::demo::check(runtime.ok, "project VFS mounts");
    fuse::demo::check(runtime.vfs.mountsAdded >= 1u, "at least one project asset root mounted");

    fuse::scene::Scene runtimeScene;
    const fuse::demo::wiring::World3DLoadResult worldLoad =
        fuse::demo::wiring::ensure3DWorldFromProject(project, runtimeScene);
    fuse::demo::check(worldLoad.ok, "bundled ExampleLevel.mis converts and loads");
    fuse::demo::check(worldLoad.entityCount >= 2u, "converted scene has mission entities");
    fuse::demo::check(worldLoad.wiringStubCount >= 1u || worldLoad.datablockBindings >= 1u ||
                          worldLoad.materialBindings >= 1u,
                      "mission wiring stubs or resolved bindings preserved");

    // UNI-U7-WORLD-1: the converted mission (worlds/example.fuselevel, v3 components) is the renderable scene:
    // World3D instantiates its ECS block (GroundPlane -> plane mesh + static plane collider, SpawnSphere ->
    // SpawnMarker, ...) and draws it through the E03 path. No hand-made proxies.
    fuse::world3d::World3D world3D;
    fuse::world3d::World3DLevelLoadResult level = world3D.loadWorldFromFuselevel(worldLoad.loadedPath);
    if (level.ok && !level.hasEcsBlock) {
        // A generated (git-ignored) level cached by a pre-v3 converter has no components: convert it again. User
        // levels are never overwritten this way: ensure3DWorldFromProject only reconverts a missing / older file.
        fuse::log::info("demo_3d_empty: %s predates .fuselevel v3; reconverting", worldLoad.loadedPath.c_str());
        std::error_code removeEc;
        std::filesystem::remove(worldLoad.loadedPath, removeEc);
        fuse::scene::Scene refreshed;
        const fuse::demo::wiring::World3DLoadResult again =
            fuse::demo::wiring::ensure3DWorldFromProject(project, refreshed);
        fuse::demo::check(again.ok, "stale level reconverted");
        level = world3D.loadWorldFromFuselevel(again.loadedPath);
    }
    fuse::demo::check(level.ok, "converted level loads into World3D");
    fuse::demo::check(level.hasEcsBlock && level.fileVersion == 3u, "converted level is .fuselevel v3 with components");
    fuse::demo::check(level.meshes >= 1u && level.colliders >= 1u, "GroundPlane -> mesh + collider components");
    fuse::demo::check(level.spawnMarkers >= 1u, "SpawnSphere -> SpawnMarker component");
    fuse::demo::check(level.objects >= 2u, "level entities mirrored as World3D objects");
    world3D.setPhysicsEnabled(true);

    if (level.directionalLights == 0u) {
        // The bundled mission has no Sun: light it with a default sun (the renderer needs one to shade).
        const float toSun[3] = {0.45f, 0.8f, 0.4f};
        const float sunColor[3] = {1.f, 0.95f, 0.85f};
        (void)world3D.spawnDirectionalLight(toSun, sunColor, 2.5f);
        fuse::log::info("demo_3d_empty: level has no Sun; default directional light added");
    }
    // Camera at the level's spawn point, looking down at the ground ahead of it.
    float spawn[3] = {0.f, 10.f, 0.f};
    fuse::demo::check(world3D.findSpawnPoint(spawn), "spawn point found in the level");
    fuse::world3d::RenderCamera3D camera{};
    for (int a = 0; a < 3; ++a) {
        camera.eye[a] = spawn[a];
    }
    camera.target[0] = spawn[0];
    camera.target[1] = 0.f;
    camera.target[2] = spawn[2] - spawn[1];
    camera.valid = true;
    world3D.setCamera(camera);

    // GPU path when the build has the Vulkan RHI and a device exists; the PlaceholderRenderer otherwise.
    fuse::hybrid::HybridComposer localComposer;
    fuse::hybrid::HybridComposer* composer = &localComposer;
#if defined(FUSE_HAS_VULKAN_RHI)
    fuse::hybrid::HybridRendererBootstrapDesc bootstrapDesc{};
    // Validation on (the Khronos layer is used when installed; the gate below asserts 0 messages).
    bootstrapDesc.renderer.rhi.bootstrap.instance.enableValidation = true;
    fuse::renderer::resetVulkanValidationCounters();
    bootstrapDesc.scene.width = 160;
    bootstrapDesc.scene.height = 120;
    std::unique_ptr<fuse::hybrid::HybridRendererBootstrap> gpuRuntime =
        fuse::hybrid::HybridRendererBootstrap::create(bootstrapDesc);
    if (gpuRuntime != nullptr && gpuRuntime->isReady()) {
        composer = &gpuRuntime->composer();
    }
#endif

    composer->setProjectFlags(fuse::project::toDimensionFlags(project.manifest.dimensions));
    composer->attachWorld3D(&world3D);
    world3D.setClearColor(0.12f, 0.18f, 0.28f);
    world3D.loadWorld(fuse::dimension::WorldHandle(1u, 1u));

    fuse::frame::FrameCtx ctx;
    for (int frame = 0; frame < 30; ++frame) {
        ctx.dt = 1.f / 60.f;
        ctx.frameIndex = static_cast<fuse::u32>(frame);
#if defined(FUSE_HAS_VULKAN_RHI)
        if (composer != &localComposer) {
            gpuRuntime->runFrame(ctx);
            continue;
        }
#endif
        composer->tick(ctx);
        composer->render(ctx);
    }

    fuse::demo::check(composer->frameCount() == 30u, "30 frames ticked");
    fuse::demo::check(world3D.readSnapshot().objects().size() >= level.objects, "3D snapshot includes the level objects");
    fuse::demo::check(world3D.physics().bodyCount() >= level.physicsLinked && level.physicsLinked >= 1u,
                      "level collider entities are physics bodies");
#if defined(FUSE_HAS_VULKAN_RHI)
    if (composer->gpuSceneActive()) {
        // GPU readback: the spawn-point camera looks down at the converted ground plane (frame centre).
        fuse::hybrid::HybridSceneRenderer& gpu = *composer->gpuScene();
        fuse::demo::check(gpu.waitIdle(), "GPU frames retired");
        fuse::demo::check(composer->gpuSceneFrames() == 30u && world3D.gpuFramesRendered() == 30u,
                          "30 frames rendered by the SceneRenderer from World3D");
        fuse::demo::check(gpu.lastFrame().instances == level.meshes, "every level mesh is a GPU instance");
        fuse::u8 rgb[3] = {0, 0, 0};
        fuse::demo::check(fuse::hybrid::readbackPixel(gpu, gpu.width() / 2u, gpu.height() / 2u, rgb) && rgb[0] > 40u &&
                              rgb[1] > 40u && rgb[2] > 40u,
                          "lit converted ground plane at the frame centre (GPU readback)");
        fuse::demo::check(fuse::hybrid::readbackPixel(gpu, 2u, 2u, rgb) && (rgb[0] | rgb[1] | rgb[2]) != 0u,
                          "sky / scene present at the frame corner (GPU readback)");
        const fuse::renderer::VulkanValidationCounters validation = fuse::renderer::vulkanValidationCounters();
        fuse::log::info("demo_3d_empty: validation errors=%u warnings=%u", validation.errors, validation.warnings);
        fuse::demo::check(validation.errors == 0u && validation.warnings == 0u,
                          "0 Vulkan validation messages over 30 frames");
        fuse::demo::check(composer->renderer().pixelCount() == 0u, "PlaceholderRenderer off on the GPU path");
        fuse::log::info("demo_3d_empty: GPU path (%ux%u)", gpu.width(), gpu.height());
    } else
#endif
    {
        fuse::demo::check(composer->renderer().sample(160, 120) > 0, "3D clear colour visible");
        fuse::log::info("demo_3d_empty: PlaceholderRenderer fallback (no Vulkan device)");
    }

#if defined(FUSE_HAS_VULKAN_RHI)
    if (gpuRuntime != nullptr) {
        gpuRuntime->shutdown();
    }
#endif
    fuse::core::shutdown();
    return fuse::demo::finish("demo_3d_empty");
}
