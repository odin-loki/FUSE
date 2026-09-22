// B2.11 gate: "Composite pass correctly blends CUDA and raster output at all GRIA α values".
// For each α the RhiContext frame renders the white fixture triangle on black, composite blends
// it with the CUDA source, and the offscreen composite target is read back. composite.frag computes
// mix(cuda, raster, α); without a CUDA toolkit the CUDA source is the shader's constant
// (0.05, 0.15, 0.35), so every pixel is checked against the exact expected value.
#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/composite_gpu_path.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr float kCudaStub[3] = {0.05f, 0.15f, 0.35f}; // composite.frag fallback when no CUDA source
constexpr int kTolerance = 2;                          // UNORM8 rounding + filtering at texel centres

struct Pixel {
    int r, g, b;
};

Pixel pixelAt(const std::vector<fuse::u8>& rgba, fuse::u32 width, fuse::u32 x, fuse::u32 y) {
    const fuse::usize i = (static_cast<fuse::usize>(y) * width + x) * 4u;
    return {rgba[i], rgba[i + 1], rgba[i + 2]};
}

bool near(Pixel p, const float expected[3]) {
    const int e[3] = {static_cast<int>(std::lround(expected[0] * 255.f)),
                      static_cast<int>(std::lround(expected[1] * 255.f)),
                      static_cast<int>(std::lround(expected[2] * 255.f))};
    return std::abs(p.r - e[0]) <= kTolerance && std::abs(p.g - e[1]) <= kTolerance &&
           std::abs(p.b - e[2]) <= kTolerance;
}

bool blendOnce(float alpha) {
    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;
    desc.bootstrap.createSwapchain = false;
    desc.raster.width = 128;
    desc.raster.height = 96;
    desc.composite.defaultBlend = alpha;

    auto context = fuse::renderer::RhiContext::create(desc);
    if (context == nullptr || !context->bootstrap().status().deviceReady) {
        return false;
    }

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.f, 0.f, 0.f);
    commands.drawSprite2D(0.f, 0.f, 0.f, 255, 255, 255);
    expectTrue(context->beginFrame(0u), "beginFrame");
    expectTrue(context->submitFrame(commands, 0u), "submitFrame");

    const fuse::renderer::CompositeGpuPath* composite = context->compositeGpuPath();
    expectTrue(composite != nullptr && composite->isReady(), "composite GPU path ready");
    if (composite == nullptr || !composite->isReady()) {
        return true;
    }
    expectTrue(composite->lastStats().framesEncoded >= 1u, "composite pass encoded");

    std::vector<fuse::u8> rgba;
    expectTrue(composite->readbackOutput(rgba), "composite output readback");
    const fuse::u32 w = desc.raster.width;
    const fuse::u32 h = desc.raster.height;
    if (rgba.size() != static_cast<fuse::usize>(w) * h * 4u) {
        expectTrue(false, "composite readback size");
        return true;
    }

    const Pixel background = pixelAt(rgba, w, 2, 2);        // raster black
    const Pixel triangle = pixelAt(rgba, w, w / 2, h / 2);  // raster white
    std::printf("alpha %.2f: background (%d,%d,%d) triangle (%d,%d,%d)\n", alpha, background.r,
                background.g, background.b, triangle.r, triangle.g, triangle.b);

    if (composite->lastStats().cudaTextureActive) {
        // A live CUDA source makes the second input frame-dependent; check the raster weight only.
        expectTrue(triangle.r >= background.r && triangle.g >= background.g && triangle.b >= background.b,
                   "raster contribution never darkens the composite");
        return true;
    }

    float expectedBackground[3];
    float expectedTriangle[3];
    for (int c = 0; c < 3; ++c) {
        expectedBackground[c] = kCudaStub[c] * (1.f - alpha);        // mix(cuda, 0, α)
        expectedTriangle[c] = kCudaStub[c] * (1.f - alpha) + alpha;  // mix(cuda, 1, α)
    }
    expectTrue(near(background, expectedBackground), "background = mix(cuda, black, alpha)");
    expectTrue(near(triangle, expectedTriangle), "triangle = mix(cuda, white, alpha)");
    return true;
}

void testCompositeBlendAtAllAlpha() {
    const float alphas[] = {0.f, 0.25f, 0.5f, 0.75f, 1.f};
    for (const float alpha : alphas) {
        if (!blendOnce(alpha)) {
            std::printf("SKIP: no Vulkan device — composite blend needs an ICD (Lavapipe in CI)\n");
            return;
        }
    }
}

} // namespace

int main() {
    fuse::core::initialize();
    testCompositeBlendAtAllAlpha();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b2_composite_blend: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b2_composite_blend: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
