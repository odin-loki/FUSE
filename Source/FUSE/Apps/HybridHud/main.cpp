#include <fuse/ai/behavior_runtime.hpp>
#include <fuse/ai/behavior_tree.hpp>
#include <fuse/core/init.hpp>
#include <fuse/dimension/world_handle.hpp>
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

constexpr int kFrameCount = 60;
constexpr float kDt = 1.f / 60.f;

int g_failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "demo_hybrid_hud FAIL: %s\n", message);
        ++g_failures;
    }
}

} // namespace

int main() {
    fuse::log::info("demo_hybrid_hud: U4 hybrid frame (software placeholder renderer — no real GL yet)");

    check(fuse::core::initialize(), "fuse_core initialize");

    fuse::hybrid::HybridRendererBootstrapDesc bootstrapDesc{};
    bootstrapDesc.renderer.rhi.bootstrap.instance.enableValidation = false;
    auto runtime = fuse::hybrid::HybridRendererBootstrap::create(bootstrapDesc);
    check(runtime != nullptr, "HybridRendererBootstrap allocated");
    check(runtime->isReady(), "B2.10 renderer bootstrap initialized");

    fuse::hybrid::HybridComposer& composer = runtime->composer();
    fuse::world2d::World2D world2D;
    fuse::world3d::World3D world3D;
    fuse::SceneObject2D hudSprite("hud_sprite");

    hudSprite.setPosition(0.f, 0.f);
    hudSprite.setLayer(10);
    world2D.addSprite(&hudSprite);
    world3D.setClearColor(0.1f, 0.15f, 0.25f);

    const fuse::dimension::WorldHandle worldHandle(1u, 1u);
    world2D.loadWorld(worldHandle);
    world3D.loadWorld(worldHandle);

    composer.attachWorld2D(&world2D);
    composer.attachWorld3D(&world3D);

    fuse::ai::BehaviorRuntime aiRuntime;
    aiRuntime.setTree(fuse::ai::BehaviorTree::makePatrolWhenNearTarget());
    fuse::ai::AgentBinding hudAgent{};
    hudAgent.x = 0.f;
    hudAgent.y = 0.f;
    hudAgent.targetX = 2.f;
    hudAgent.targetY = 0.f;
    aiRuntime.addAgent(hudAgent);

    fuse::frame::FrameCtx ctx;
    for (int frame = 0; frame < kFrameCount; ++frame) {
        ctx.dt = kDt;
        ctx.time = static_cast<float>(frame) * kDt;
        ctx.frameIndex = static_cast<fuse::u32>(frame);

        runtime->tick(ctx);

        aiRuntime.buildSnapshots();
        aiRuntime.evaluate(ctx);
        aiRuntime.commit();

        runtime->render(ctx);
    }

    check(composer.frameCount() == static_cast<fuse::u32>(kFrameCount), "all frames ticked");
    check(aiRuntime.tickCount() == static_cast<fuse::u32>(kFrameCount), "fuse_ai module ticked each frame");
    check(aiRuntime.blackboard().flag(0, 0), "fuse_ai patrol flag set for near HUD agent");
    check(composer.renderer().sample(160, 120) > 0, "3D clear colour present");
    check(composer.renderer().sample(172, 132) > 0, "spinning 2D sprite visible");

    fuse::log::info("demo_hybrid_hud: rendered %d frames at %ux%u (RGBA software buffer)",
                    kFrameCount,
                    composer.renderer().width(),
                    composer.renderer().height());
    fuse::log::info("demo_hybrid_hud: real GL/Vulkan presentation deferred to Track B RHI");

    runtime->shutdown();
    fuse::core::shutdown();

    if (g_failures == 0) {
        fuse::log::info("demo_hybrid_hud: PASS");
        return EXIT_SUCCESS;
    }

    return EXIT_FAILURE;
}
