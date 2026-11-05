#include "demo_check.hpp"
#include "demo_present.hpp"
#include "demo_project_wiring.hpp"

#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/world3d/world_3d.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#include <fuse/renderer/vk/instance.hpp>
#endif

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(FUSE_HAS_VULKAN_RHI)
namespace {

/// Every pixel of the last headless frame (r, g, b per pixel).
std::vector<fuse::u8> readbackImage(const fuse::hybrid::HybridSceneRenderer& gpu) {
    std::vector<fuse::u8> out(static_cast<std::size_t>(gpu.width()) * gpu.height() * 3u, 0u);
    for (fuse::u32 y = 0; y < gpu.height(); ++y) {
        for (fuse::u32 x = 0; x < gpu.width(); ++x) {
            (void)fuse::hybrid::readbackPixel(gpu, x, y, &out[(static_cast<std::size_t>(y) * gpu.width() + x) * 3u]);
        }
    }
    return out;
}

/// Asset plan 4a: TSStatic shapes draw. A mission with two TSStatics (unit_cube.dts through the in-house DTS reader,
/// carwheel.dae through assimp) converts, its shapes cook to game:/cooked/, World3D resolves the entities' mesh ids,
/// the hybrid renderer uploads the meshes and binds ecs::Mesh::vertex_buffer, and hiding the TSStatics changes the
/// rendered pixels (headless GPU readback, Lavapipe in CI).
void tsStaticStage(fuse::world3d::World3D& world, fuse::hybrid::HybridComposer& composer,
                   fuse::hybrid::HybridRendererBootstrap& runtime, fuse::frame::FrameCtx& ctx) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path stage = fs::temp_directory_path(ec) / "fuse_demo_3d_empty_tsstatic";
    fs::remove_all(stage, ec);
    fs::create_directories(stage / "worlds", ec);
    const fs::path mission = stage / "worlds" / "shapes.mis";
    {
        std::ofstream out(mission, std::ios::binary);
        out << "new Scene(ShapeLevel) {\n"
               "   new GroundPlane(Floor) { position = \"0 0 0\"; };\n"
               "   new TSStatic(Cube) { position = \"0 5 1\"; rotation = \"0 0 1 30\"; "
               "shapeAsset = \"Core_GameObjects:unit_cube\"; };\n"
               "   new TSStatic(Wheel) { position = \"2 7 0.6\"; shapeAsset = \"Prototyping:carwheel_shape\"; };\n"
               "};\n";
    }
    const std::string level = (stage / "worlds" / "shapes.fuselevel").generic_string();
    const fuse::project::ConvertResult converted = fuse::project::convertT3DMissionToFuselevel(mission.string(), level);
    fuse::demo::check(converted.status == fuse::project::ConvertStatus::Ok, "TSStatic mission converts");
    const fuse::project::T3DShapeCookResult shapes =
        fuse::project::cookT3DMissionShapes(mission.string(), (stage / "cooked").string());
    fuse::log::info("demo_3d_empty: %s", shapes.note.c_str());
#if defined(FUSE_HAS_ASSIMP)
    const fuse::u32 expected = 2u;
#else
    const fuse::u32 expected = 1u; // carwheel.dae needs assimp
#endif
    fuse::demo::check(shapes.cooked == expected, "TSStatic shapes cook (.dts in-house, .dae via assimp)");

    (void)world.exitPlayMode();
    const fuse::world3d::World3DLevelLoadResult loaded = world.loadWorldFromFuselevel(level); // root: <stage>/cooked
    fuse::demo::check(loaded.ok && loaded.cookedMeshes == expected, "TSStatic mesh ids resolve to the cooked meshes");
    const float toSun[3] = {0.45f, 0.8f, 0.4f};
    const float sunColor[3] = {1.f, 0.95f, 0.85f};
    (void)world.spawnDirectionalLight(toSun, sunColor, 2.5f);
    fuse::world3d::RenderCamera3D camera{};
    camera.eye[0] = 0.5f;
    camera.eye[1] = 2.5f;
    camera.eye[2] = 0.f;
    camera.target[0] = 1.f;
    camera.target[1] = 0.6f;
    camera.target[2] = -6.f;
    camera.valid = true;
    world.setCamera(camera);

    auto frames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            ctx.dt = 1.f / 60.f;
            ++ctx.frameIndex;
            runtime.runFrame(ctx);
        }
    };
    fuse::hybrid::HybridSceneRenderer& gpu = *composer.gpuScene();
    frames(3);
    fuse::demo::check(gpu.waitIdle(), "TSStatic frames retired");
    fuse::u32 bound = 0;
    world.registry().each<fuse::ecs::MeshAssets, fuse::ecs::Mesh>(
        [&](fuse::ecs::EntityID, fuse::ecs::MeshAssets& assets, fuse::ecs::Mesh& mesh) {
            bound += assets.mesh.valid() && mesh.vertex_buffer.isValid() ? 1u : 0u;
        });
    fuse::log::info("demo_3d_empty: TSStatic: %u cooked mesh(es) uploaded, %u entity vertex buffer(s) bound, %u instances",
                    gpu.cookedMeshCount(), bound, gpu.lastFrame().instances);
    fuse::demo::check(gpu.cookedMeshCount() >= expected && bound == expected,
                      "TSStatic ecs::Mesh::vertex_buffer non-null (cooked mesh uploaded)");
    const std::vector<fuse::u8> shown = readbackImage(gpu);

    world.registry().each<fuse::ecs::MeshAssets, fuse::ecs::Mesh>(
        [&](fuse::ecs::EntityID, fuse::ecs::MeshAssets& assets, fuse::ecs::Mesh& mesh) {
            if (assets.mesh.valid()) {
                mesh.visible = false;
            }
        });
    frames(3);
    fuse::demo::check(gpu.waitIdle(), "hidden-TSStatic frames retired");
    const std::vector<fuse::u8> hidden = readbackImage(gpu);
    fuse::u32 changed = 0;
    for (std::size_t p = 0; p + 2u < shown.size() && p + 2u < hidden.size(); p += 3u) {
        const int d = std::abs(int(shown[p]) - int(hidden[p])) + std::abs(int(shown[p + 1u]) - int(hidden[p + 1u])) +
                      std::abs(int(shown[p + 2u]) - int(hidden[p + 2u]));
        changed += d > 24 ? 1u : 0u;
    }
    const fuse::u32 total = gpu.width() * gpu.height();
    fuse::log::info("demo_3d_empty: TSStatic pixels changed: %u of %u", changed, total);
    fuse::demo::check(changed * 100u >= total, "TSStatic shapes change >= 1% of the frame's pixels (GPU readback)");
    const fuse::renderer::VulkanValidationCounters validation = fuse::renderer::vulkanValidationCounters();
    fuse::demo::check(validation.errors == 0u && validation.warnings == 0u, "0 Vulkan validation messages with TSStatics");
    fs::remove_all(stage, ec);
}

} // namespace
#endif

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_3d_empty: 3D dimension path + U7 mission convert/load");

    const bool presentGameWindow = fuse::demo::present::requestedFromArgv(argc, argv);
    const std::string projectPath =
        fuse::demo::present::projectPathFromArgv(argc, argv, "Samples/unification/demo_3d_empty");
    const int frameTarget = presentGameWindow ? 3 : 30;

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
    fuse::demo::check(world3D.enterPlayMode(), "standalone play mode places camera at spawn and enables physics");

    // GPU path when the build has the Vulkan RHI and a device exists; the PlaceholderRenderer otherwise.
    fuse::hybrid::HybridComposer localComposer;
    fuse::hybrid::HybridComposer* composer = &localComposer;
#if defined(FUSE_HAS_VULKAN_RHI)
    fuse::demo::present::HostPresentUnlock presentUnlock;
    fuse::hybrid::HybridRendererBootstrapDesc bootstrapDesc{};
    // Validation on (the Khronos layer is used when installed; the gate below asserts 0 messages).
    bootstrapDesc.renderer.rhi.bootstrap.instance.enableValidation = !presentGameWindow;
    fuse::renderer::resetVulkanValidationCounters();
    bootstrapDesc.scene.width = 160;
    bootstrapDesc.scene.height = 120;
    if (presentGameWindow && fuse::demo::present::gameWindowPresentAvailable()) {
        fuse::demo::present::applyGameWindowToBootstrapDesc(bootstrapDesc, 256, 160, "demo_3d_empty");
    }
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
    for (int frame = 0; frame < frameTarget; ++frame) {
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

    fuse::demo::check(composer->frameCount() == static_cast<fuse::u32>(frameTarget),
                      presentGameWindow ? "present mode frames ticked" : "30 frames ticked");
    fuse::demo::check(world3D.readSnapshot().objects().size() >= level.objects, "3D snapshot includes the level objects");
    fuse::demo::check(world3D.physics().bodyCount() >= level.physicsLinked && level.physicsLinked >= 1u,
                      "level collider entities are physics bodies");
#if defined(FUSE_HAS_VULKAN_RHI)
    if (composer->gpuSceneActive()) {
        // GPU readback: the spawn-point camera looks down at the converted ground plane (frame centre).
        fuse::hybrid::HybridSceneRenderer& gpu = *composer->gpuScene();
        fuse::demo::check(gpu.waitIdle(), "GPU frames retired");
        fuse::demo::check(composer->gpuSceneFrames() == static_cast<fuse::u32>(frameTarget) &&
                              world3D.gpuFramesRendered() == static_cast<fuse::u32>(frameTarget),
                          "frames rendered by the SceneRenderer from World3D");
        fuse::demo::check(gpu.lastFrame().instances == level.meshes, "every level mesh is a GPU instance");
        // Headless frames keep a host-visible copy; presented (swapchain) frames do not, so pixel checks
        // only run without --present.
        fuse::u8 rgb[3] = {0, 0, 0};
        if (gpu.readbackFrames() != 0u) {
        fuse::demo::check(fuse::hybrid::readbackPixel(gpu, gpu.width() / 2u, gpu.height() / 2u, rgb) && rgb[0] > 40u &&
                              rgb[1] > 40u && rgb[2] > 40u,
                          "lit converted ground plane at the frame centre (GPU readback)");
        fuse::demo::check(fuse::hybrid::readbackPixel(gpu, 2u, 2u, rgb) && (rgb[0] | rgb[1] | rgb[2]) != 0u,
                          "sky / scene present at the frame corner (GPU readback)");
        } else {
            fuse::log::info("demo_3d_empty: presented frames (no headless readback)");
        }
        const fuse::renderer::VulkanValidationCounters validation = fuse::renderer::vulkanValidationCounters();
        fuse::log::info("demo_3d_empty: validation errors=%u warnings=%u", validation.errors, validation.warnings);
        fuse::demo::check(validation.errors == 0u && validation.warnings == 0u,
                          "0 Vulkan validation messages over 30 frames");
        fuse::demo::check(composer->renderer().pixelCount() == 0u, "PlaceholderRenderer off on the GPU path");
        if (gpu.readbackFrames() != 0u && gpuRuntime != nullptr) {
            tsStaticStage(world3D, *composer, *gpuRuntime, ctx);
        }
        fuse::log::info("demo_3d_empty: GPU path (%ux%u)", gpu.width(), gpu.height());
        if (presentGameWindow && gpuRuntime != nullptr) {
            fuse::demo::check(fuse::demo::present::realPresentCallCount(*gpuRuntime) >= 1u,
                              "vkQueuePresentKHR via GameWindow present path");
        }
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
