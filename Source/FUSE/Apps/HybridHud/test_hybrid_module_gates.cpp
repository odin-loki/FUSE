#include "hybrid_module_gates.hpp"

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

} // namespace

int main() {
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
    expectTrue(result.ok, result.message != nullptr ? result.message : "prestarter §10 U5 gates pass");

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_hybrid_module_gates_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_hybrid_module_gates_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
