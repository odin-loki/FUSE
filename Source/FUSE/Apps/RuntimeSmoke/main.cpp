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
