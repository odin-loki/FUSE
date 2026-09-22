// B2.11 Week 1 gate: "Triangle on screen — white triangle, black background, correct winding,
// no validation errors". Headless: the RhiContext frame renders into the raster colour target,
// which is read back and checked pixel-wise (validation is covered by fuse_vulkan_validation_gate).
#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/raster_path.hpp>

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

struct Rgba {
    fuse::u8 r, g, b, a;
};

Rgba pixelAt(const std::vector<fuse::u8>& rgba, fuse::u32 width, fuse::u32 x, fuse::u32 y) {
    const fuse::usize i = (static_cast<fuse::usize>(y) * width + x) * 4u;
    return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
}

bool isWhite(Rgba p) {
    return p.r == 255 && p.g == 255 && p.b == 255;
}

bool isBlack(Rgba p) {
    return p.r == 0 && p.g == 0 && p.b == 0;
}

void testWhiteTriangleOnBlack() {
    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;
    desc.bootstrap.createSwapchain = false;
    desc.raster.width = 320;
    desc.raster.height = 240;

    auto context = fuse::renderer::RhiContext::create(desc);
    expectTrue(context != nullptr, "RHI context allocated");
    if (context == nullptr) {
        return;
    }
    if (!context->bootstrap().status().deviceReady) {
        std::printf("SKIP: no Vulkan device — triangle readback needs an ICD (Lavapipe in CI)\n");
        return;
    }

    expectTrue(fuse::platform::mayTouchGpuContext(), "render thread available");
    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.f, 0.f, 0.f);                  // clear3d pass: black clear
    commands.drawSprite2D(0.f, 0.f, 0.f, 255, 255, 255); // sprites2d pass: draws the fixture triangle

    expectTrue(context->beginFrame(0u), "beginFrame on render thread");
    expectTrue(context->submitFrame(commands, 0u), "submitFrame renders clear + triangle");

    const fuse::renderer::RasterPath* raster = context->rasterPath();
    expectTrue(raster != nullptr && raster->isReady(), "raster path ready");
    if (raster == nullptr || !raster->isReady()) {
        return;
    }

    std::vector<fuse::u8> rgba;
    expectTrue(raster->readbackColor(rgba), "raster colour readback succeeds");
    const fuse::u32 w = desc.raster.width;
    const fuse::u32 h = desc.raster.height;
    expectTrue(rgba.size() == static_cast<fuse::usize>(w) * h * 4u, "readback is width*height RGBA8");
    if (rgba.size() != static_cast<fuse::usize>(w) * h * 4u) {
        return;
    }

    // Fixture triangle (NDC, Vulkan y-down): apex (0,-0.5), base (+-0.5, 0.5).
    // In pixels: apex at (w/2, h/4), base spans x in [w/4, 3w/4] at y = 3h/4.
    expectTrue(isBlack(pixelAt(rgba, w, 1, 1)), "top-left corner is black background");
    expectTrue(isBlack(pixelAt(rgba, w, w - 2, 1)), "top-right corner is black background");
    expectTrue(isBlack(pixelAt(rgba, w, 1, h - 2)), "bottom-left corner is black background");
    expectTrue(isBlack(pixelAt(rgba, w, w - 2, h - 2)), "bottom-right corner is black background");

    expectTrue(isWhite(pixelAt(rgba, w, w / 2, h / 2)), "triangle centre is white");
    expectTrue(isWhite(pixelAt(rgba, w, w / 2, (h * 3) / 10)), "near apex (top) is white");

    // Orientation: narrow at the top, wide at the bottom. A vertically flipped image fails here.
    expectTrue(isBlack(pixelAt(rgba, w, (w * 3) / 10, (h * 3) / 10)), "left of apex row is outside the triangle");
    expectTrue(isWhite(pixelAt(rgba, w, (w * 3) / 10, (h * 7) / 10)), "left of base row is inside the triangle");

    fuse::usize white = 0;
    fuse::usize other = 0;
    for (fuse::u32 y = 0; y < h; ++y) {
        for (fuse::u32 x = 0; x < w; ++x) {
            const Rgba p = pixelAt(rgba, w, x, y);
            if (isWhite(p)) {
                ++white;
            } else if (!isBlack(p)) {
                ++other;
            }
        }
    }
    // Area = 0.5 * (w/2) * (h/2) = 12.5% of the frame; allow rasterisation edge slack.
    const double coverage = static_cast<double>(white) / (static_cast<double>(w) * h);
    std::printf("triangle coverage %.4f (expected 0.125), non-binary pixels %zu\n", coverage, other);
    expectTrue(coverage > 0.115 && coverage < 0.135, "white coverage matches triangle area");
    expectTrue(other == 0u, "only black and white pixels (no blending/sampling artefacts)");

    // Stable across frames: the tracked-layout readback must not disturb the next frame.
    commands.reset();
    commands.clear3D(0.f, 0.f, 0.f);
    commands.drawSprite2D(0.f, 0.f, 0.f, 255, 255, 255);
    expectTrue(context->beginFrame(1u), "second beginFrame");
    expectTrue(context->submitFrame(commands, 1u), "second submitFrame");
    std::vector<fuse::u8> second;
    expectTrue(raster->readbackColor(second), "second readback succeeds");
    expectTrue(second == rgba, "second frame is pixel-identical");
}

} // namespace

int main() {
    fuse::core::initialize();
    testWhiteTriangleOnBlack();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_b2_triangle_readback: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b2_triangle_readback: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
