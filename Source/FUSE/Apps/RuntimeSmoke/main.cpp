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

#include <atomic>
#include <cstdio>
#include <cstdlib>

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

} // namespace

int main() {
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

    fuse::core::shutdown();

    if (g_failures == 0) {
        fuse::log::info("fuse_runtime_smoke: PASS — core jobs, vfs, and world facades in one process");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_runtime_smoke: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
