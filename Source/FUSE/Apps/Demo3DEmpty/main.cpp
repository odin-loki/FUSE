#include "demo_check.hpp"
#include "demo_project_wiring.hpp"

#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/world_3d.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#endif

#include <cstdlib>
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

    fuse::world3d::World3D world3D;
    fuse::SceneObject3D floorProxy("Floor");
    floorProxy.setPosition(0.f, 0.f);
    floorProxy.setZ(0.f);
    world3D.addObject(&floorProxy);

    // E03: the renderable scene (World3D's ECS registry): floor slab + a lit brick-red cube, sun, camera on the cube.
    const float floorCenter[3] = {0.f, -0.1f, -2.f};
    const float floorHalf[3] = {4.f, 0.1f, 4.f};
    const float cubeCenter[3] = {0.f, 0.5f, -2.f};
    const float cubeHalf[3] = {0.5f, 0.5f, 0.5f};
    (void)world3D.spawnMesh(fuse::world3d::BuiltinMesh::Cube, 0u, floorCenter, floorHalf);
    (void)world3D.spawnMesh(fuse::world3d::BuiltinMesh::Cube, 1u, cubeCenter, cubeHalf);
    const float toSun[3] = {0.45f, 0.8f, 0.4f};
    const float sunColor[3] = {1.f, 0.95f, 0.85f};
    (void)world3D.spawnDirectionalLight(toSun, sunColor, 2.5f);
    fuse::world3d::RenderCamera3D camera{};
    camera.eye[0] = 0.f;
    camera.eye[1] = 1.4f;
    camera.eye[2] = 1.f;
    for (int a = 0; a < 3; ++a) {
        camera.target[a] = cubeCenter[a];
    }
    camera.valid = true;
    world3D.setCamera(camera);

    // GPU path when the build has the Vulkan RHI and a device exists; the PlaceholderRenderer otherwise.
    fuse::hybrid::HybridComposer localComposer;
    fuse::hybrid::HybridComposer* composer = &localComposer;
#if defined(FUSE_HAS_VULKAN_RHI)
    fuse::hybrid::HybridRendererBootstrapDesc bootstrapDesc{};
    bootstrapDesc.renderer.rhi.bootstrap.instance.enableValidation = false;
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
    fuse::demo::check(world3D.readSnapshot().objects().size() >= 1u, "3D snapshot includes floor proxy");
#if defined(FUSE_HAS_VULKAN_RHI)
    if (composer->gpuSceneActive()) {
        // GPU readback: the camera looks at the cube's centre, so the frame centre is the lit brick-red cube.
        fuse::hybrid::HybridSceneRenderer& gpu = *composer->gpuScene();
        fuse::demo::check(gpu.waitIdle(), "GPU frames retired");
        fuse::demo::check(composer->gpuSceneFrames() == 30u && world3D.gpuFramesRendered() == 30u,
                          "30 frames rendered by the SceneRenderer from World3D");
        fuse::demo::check(gpu.lastFrame().instances == 2u, "floor + cube instances on the GPU");
        fuse::u8 rgb[3] = {0, 0, 0};
        fuse::demo::check(fuse::hybrid::readbackPixel(gpu, gpu.width() / 2u, gpu.height() / 2u, rgb) && rgb[0] > rgb[1] &&
                              rgb[0] > rgb[2] && rgb[0] > 40u,
                          "lit cube visible at the frame centre (GPU readback)");
        fuse::demo::check(fuse::hybrid::readbackPixel(gpu, 2u, 2u, rgb) && (rgb[0] | rgb[1] | rgb[2]) != 0u,
                          "sky / scene present at the frame corner (GPU readback)");
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
