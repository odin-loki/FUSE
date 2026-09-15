#include <fuse/core/init.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/jobs/parallel_for.hpp>
#include <fuse/legacy/parallel_for.hpp>
#include <fuse/legacy/t2d/api.hpp>
#include <fuse/legacy/t3d/api.hpp>
#include <fuse/log/logger.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/renderer/renderer_bootstrap.hpp>
#endif

#include <cstdio>
#include <cstdlib>
#include <atomic>

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

} // namespace

int main() {
    fuse::log::info("fuse_runtime_smoke: starting one-process quarantine test");

    check(fuse::core::initialize(), "fuse_core initialize");
    check(fuse::jobs::JobScheduler::instance().isInitialized(), "job scheduler initialized");

    fuse::io::VirtualFileSystem::instance().mount(fuse::io::MountKind::Game, ".", "/game");
    fuse::log::info("vfs mounts: %u", fuse::io::VirtualFileSystem::instance().mountCount());

    runJobSmoke();

    check(fuse::legacy::parallel_for_smoke_sum(0u, 100u, 10u) == 4950u,
          "legacy parallel_for adapter sum matches serial expectation");

    check(fuse::legacy::t3d::initialize(), "fuse_t3d_legacy initialize");
    check(fuse::legacy::t2d::initialize(), "fuse_t2d_legacy initialize");

    check(fuse::legacy::t3d::isInitialized(), "t3d legacy running");
    check(fuse::legacy::t2d::isInitialized(), "t2d legacy running");

    fuse::legacy::t3d::Con::execute("echo T3D dimension alive");
    fuse::legacy::t2d::Con::execute("echo T2D dimension alive");

    check(fuse::legacy::t3d::stringTableEntryCount() >= 1u, "t3d string table populated");
    check(fuse::legacy::t2d::stringTableEntryCount() >= 1u, "t2d string table populated");

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

    fuse::legacy::t2d::shutdown();
    fuse::legacy::t3d::shutdown();
    fuse::core::shutdown();

    if (g_failures == 0) {
        fuse::log::info("fuse_runtime_smoke: PASS — core + both prefixed legacy libs in one process");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_runtime_smoke: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
