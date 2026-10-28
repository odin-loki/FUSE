#include "hud_anchor.hpp"
#include "hybrid_module_gates.hpp"
#include "module_order.hpp"

#include <fuse/core/init.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testHudAnchorX() {
    expectTrue(fuse::hud_anchor_x(100, 10, 2) == 88, "right anchor leaves the box and margin on screen");
    expectTrue(fuse::hud_anchor_x(10, 20, 2) == 0, "anchor clamps to zero when the box overruns the screen");
    expectTrue(fuse::hud_anchor_x(100, 10, -4) == 90, "margin below zero counts as zero");
    expectTrue(fuse::hud_anchor_x(0, 10, 3) == 3, "non-positive screen returns margin");
    expectTrue(fuse::hud_anchor_x(100, -1, 3) == 3, "non-positive box returns margin");
    expectTrue(fuse::hud_anchor_x(-8, 4, -2) == 0, "non-positive screen with a negative margin returns zero");
}

void testHudModuleInitOrder() {
    using fuse::hud::HudModule;
    using fuse::hud::module_order_ok;
    expectTrue(module_order_ok(HudModule::Platform, HudModule::Hud), "Platform before Hud is ok");
    expectTrue(!module_order_ok(HudModule::Hud, HudModule::Renderer), "Hud before Renderer is not");
}

} // namespace

int main() {
    testHudAnchorX();
    testHudModuleInitOrder();

    fuse::core::initialize();

    fuse::hybrid::HybridComposer composer;
    fuse::hybrid::gates::State state;
    fuse::hybrid::gates::setup(state, composer);

    fuse::frame::FrameCtx ctx;
    for (int frame = 0; frame < fuse::hybrid::gates::kFrameCount; ++frame) {
        ctx.dt = fuse::hybrid::gates::kDt;
        ctx.time = static_cast<float>(frame) * fuse::hybrid::gates::kDt;
        ctx.frameIndex = static_cast<fuse::u32>(frame);
        fuse::hybrid::gates::tickFrame(state, composer, ctx);
        composer.render(ctx);
    }

    const fuse::hybrid::gates::VerifyResult result = fuse::hybrid::gates::verify(state, composer);
    expectTrue(result.ok, result.message != nullptr ? result.message : "hybrid outpost slice checks pass");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_hybrid_module_gates_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_hybrid_module_gates_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
