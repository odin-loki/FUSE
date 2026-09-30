#include <fuse/core/b7_test_registry.hpp>
#include <fuse/core/init.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/renderer/renderer_bootstrap.hpp>
#endif
#if defined(FUSE_SMOKE_HAS_HYBRID)
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#include <fuse/world3d/world_3d.hpp>
#endif

#if defined(FUSE_SMOKE_HAS_SCRIPT)
#include <fuse/ecs/components/script.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/script/script_cook.hpp>
#include <fuse/script/script_host_service.hpp>
#include <fuse/script/script_runtime.hpp>
#include <fuse/script/script_system.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "SMOKE FAIL: %s\n", message);
        ++g_failures;
    }
}

void runJobSmoke() {
    std::atomic<fuse::u32> sum{0};
    fuse::jobs::parallel_for(0u, 100u, 10u, [&sum](fuse::u32 i) {
        sum.fetch_add(i, std::memory_order_relaxed);
    });
    check(sum.load(std::memory_order_acquire) == 4950u, "parallel_for sum matches serial expectation");
}

void runWorldSmoke() {
    fuse::SceneObject2D sprite("sprite");
    sprite.setPosition(4.f, 5.f);
    sprite.setLayer(2);

    fuse::SceneObject3D mesh("mesh");
    mesh.setPosition(1.f, 2.f);
    mesh.setZ(3.f);
    sprite.addChild(&mesh);

    check(sprite.children().size() == 1u, "2D root holds 3D child");
    check(mesh.parent() == &sprite, "3D child parent is 2D node");
    check(mesh.z() > 2.9f && mesh.z() < 3.1f, "3D depth stored on child");
}

#if defined(FUSE_SMOKE_HAS_SCRIPT)
/// UNI-U3-SCRIPT-1 / MP-B7.3-SCRIPT-COMPONENT: the runtime loads chunks through the single
/// ScriptHostService (FUSE Lua route + legacy `t3d:` route), runs cooked `.fusescript` files, and a
/// Script component drives a behaviour on the service's VM through ScriptSystem.
void runScriptSmoke() {
    namespace script = fuse::script;
    script::ScriptHostService& service = script::ScriptHostService::instance();
    check(service.ensure_initialized(), "ScriptHostService initializes");
    if (!service.host().vm().has_lua_backend()) {
        fuse::log::info("fuse_runtime_smoke: script phase skipped (no Lua backend in this build)");
        service.shutdown();
        return;
    }

    const script::ScriptLoadResult fuseChunk =
        service.load_chunk("function smoke_answer() return 40 + 2 end", "fuse:smoke");
    check(fuseChunk.ok(), "ScriptHostService runs a fuse: Lua chunk");
    fuse::script::bind::ScriptValue answer;
    check(service.host().vm().call_global("smoke_answer", nullptr, 0, &answer).ok() &&
              fuse::script::bind::to_number(answer) == 42.0,
          "fuse: chunk defined a callable global");

    const script::ScriptLoadResult t3dChunk = service.load_chunk("echo(\"smoke\");", "t3d:smoke");
    check(t3dChunk.ok() || t3dChunk.status == script::ScriptLoadStatus::BackendUnavailable,
          "t3d: chunk routes to the Compat VM (or reports it is not linked)");
    check(service.compat_route_count() == 1u && service.fuse_route_count() == 1u, "chunk routes counted");

    // Cook a behaviour and a legacy chunk the way AssetCooker does, then load them at runtime.
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "fuse_runtime_smoke_script";
    std::filesystem::create_directories(dir, ec);
    const std::string moverSource = (dir / "mover.lua").string();
    const std::string moverCooked = (dir / "mover.fusescript").string();
    const std::string legacySource = (dir / "legacy.cs").string();
    const std::string legacyCooked = (dir / "legacy.fusescript").string();
    {
        std::ofstream(moverSource) << "function on_start(self) self.ticks = 0 end\n"
                                      "function on_update(self, dt)\n"
                                      "  self.ticks = self.ticks + 1\n"
                                      "  local p = Entity.get_position(self)\n"
                                      "  Entity.set_position(self, {x = p.x + self.speed * dt, y = p.y, z = p.z})\n"
                                      "end\n";
        std::ofstream(legacySource) << "echo(\"legacy\");\n";
    }
    check(script::cook_script_file(moverSource, moverCooked).ok, "cook mover.lua -> Lua bytecode .fusescript");
    check(script::cook_script_file(legacySource, legacyCooked).ok, "cook legacy.cs -> t3d passthrough .fusescript");
    const script::ScriptLoadResult legacy = service.load_cooked(legacyCooked.c_str());
    check(legacy.ok() || legacy.status == script::ScriptLoadStatus::BackendUnavailable,
          "cooked legacy chunk routes through ScriptHostService");
    check(service.last_loaded_dialect() == script::LegacyScriptDialect::T3dTorqueScript,
          "cooked legacy chunk keeps its t3d: tag");

    {
        fuse::ecs::Registry registry;
        registry.init(64);
        const fuse::ecs::EntityID entity = registry.create();
        registry.add(entity, fuse::ecs::Transform{});
        fuse::ecs::Script component{};
        (void)component.set_path(moverCooked);
        (void)component.set_number("speed", 2.0);
        registry.add(entity, component);

        script::ScriptEngineBindings bindings;
        bindings.registry = &registry;
        script::ScriptRuntime runtime;
        script::ScriptSystem system;
        check(runtime.init(service.host().vm(), bindings), "ScriptRuntime on the host service VM");
        check(system.init(registry, runtime), "ScriptSystem init");
        system.enterPlay();
        for (int frame = 0; frame < 30; ++frame) {
            system.update(1.f / 30.f);
        }
        const fuse::ecs::Transform* transform = registry.get<fuse::ecs::Transform>(entity);
        check(system.is_attached(entity) && runtime.error_count() == 0u, "Script component behaviour attached");
        check(transform != nullptr && transform->position.x > 1.9f && transform->position.x < 2.1f,
              "Script component moved its entity (speed 2 for 1 s)");
        if (runtime.error_count() != 0u) {
            std::fprintf(stderr, "script error: %s\n", runtime.last_error().c_str());
        }
        system.exitPlay();
        check(registry.get<fuse::ecs::Script>(entity)->lua_ref == fuse::ecs::Script::kNoRef,
              "exitPlay clears the component's lua_ref");
        system.shutdown();
        runtime.shutdown();
    }
    std::filesystem::remove_all(dir, ec);
    service.shutdown();
    fuse::log::info("fuse_runtime_smoke: script host service + Script component OK");
}
#endif

#if defined(FUSE_SMOKE_HAS_HYBRID)
/// E03 (--gpu-frame): the runtime renders a World3D scene through the hybrid SceneRenderer path and the frame is
/// checked by GPU readback. Without a Vulkan device the PlaceholderRenderer fallback is checked instead.
void runGpuFrameSmoke() {
    fuse::world3d::World3D world3D;
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
    camera.eye[1] = 1.4f;
    camera.eye[2] = 1.f;
    for (int a = 0; a < 3; ++a) {
        camera.target[a] = cubeCenter[a];
    }
    camera.valid = true;
    world3D.setCamera(camera);

    fuse::hybrid::HybridRendererBootstrapDesc desc{};
    desc.renderer.rhi.bootstrap.instance.enableValidation = false;
    desc.scene.width = 128;
    desc.scene.height = 96;
    desc.projectFlags.enable2D = false;
    desc.projectFlags.enableUI = false;
    auto runtime = fuse::hybrid::HybridRendererBootstrap::create(desc);
    check(runtime != nullptr && runtime->isReady(), "HybridRendererBootstrap ready");
    if (runtime == nullptr || !runtime->isReady()) {
        return;
    }
    runtime->composer().attachWorld3D(&world3D);
    fuse::frame::FrameCtx ctx{};
    ctx.dt = 1.f / 60.f;
    for (fuse::u32 f = 0; f < 4u; ++f) {
        ctx.frameIndex = f;
        runtime->runFrame(ctx);
    }
    fuse::hybrid::HybridComposer& composer = runtime->composer();
    if (composer.gpuSceneActive()) {
        fuse::hybrid::HybridSceneRenderer& gpu = *composer.gpuScene();
        check(gpu.waitIdle() && composer.gpuSceneFrames() == 4u, "4 frames on the GPU scene path");
        fuse::u8 rgb[3] = {0, 0, 0};
        check(fuse::hybrid::readbackPixel(gpu, gpu.width() / 2u, gpu.height() / 2u, rgb) && rgb[0] > rgb[1] &&
                  rgb[0] > rgb[2] && rgb[0] > 40u,
              "lit cube at the frame centre (GPU readback)");
        fuse::log::info("fuse_runtime_smoke: GPU frame %ux%u, centre %u %u %u", gpu.width(), gpu.height(), rgb[0], rgb[1],
                        rgb[2]);
    } else {
        check(composer.renderer().sample(160, 120) > 0, "PlaceholderRenderer fallback draws the frame");
        fuse::log::info("fuse_runtime_smoke: no GPU scene (%s) — placeholder fallback", composer.gpuSceneStatus());
    }
    runtime->shutdown();
}
#endif

} // namespace

int main(int argc, char** argv) {
    bool gpuFrame = false;
    for (int i = 1; i < argc; ++i) {
        gpuFrame = gpuFrame || std::strcmp(argv[i], "--gpu-frame") == 0;
    }
    fuse::log::info("fuse_runtime_smoke: starting FUSE product smoke");

    check(fuse::core::initialize(), "fuse_core initialize");
    check(fuse::jobs::JobScheduler::instance().isInitialized(), "job scheduler initialized");

    fuse::io::VirtualFileSystem::instance().mount(fuse::io::MountKind::Game, ".", "/game");
    fuse::log::info("vfs mounts: %u", fuse::io::VirtualFileSystem::instance().mountCount());

    runJobSmoke();
    runWorldSmoke();

#if defined(FUSE_HAS_B7_INTEGRATION)
    check(fuse::core::B7TestRegistry::runIntegrationSmoke(), "B7 cross-module integration smoke");
#endif

#if defined(FUSE_SMOKE_HAS_SCRIPT)
    runScriptSmoke();
#endif

#if defined(FUSE_HAS_VULKAN_RHI)
    {
        fuse::renderer::RendererBootstrapDesc rendererDesc{};
        rendererDesc.rhi.bootstrap.instance.enableValidation = false;
        auto rendererBootstrap = fuse::renderer::RendererBootstrap::create(rendererDesc);
        check(rendererBootstrap != nullptr, "RendererBootstrap allocated in smoke process");
        check(rendererBootstrap->isReady(), "RendererBootstrap init/shutdown path OK");
        rendererBootstrap->shutdown();
    }
#endif

#if defined(FUSE_SMOKE_HAS_HYBRID)
    if (gpuFrame) {
        runGpuFrameSmoke();
    }
#else
    (void)gpuFrame;
#endif

    fuse::core::shutdown();

    if (g_failures == 0) {
        fuse::log::info("fuse_runtime_smoke: PASS — core jobs, vfs, and world facades in one process");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_runtime_smoke: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
