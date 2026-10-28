// B2 gate row: "Push constants correctly pass per-draw data to shaders — verified with RenderDoc
// capture".
//
// Headless proof: RhiContext renders a DrawList through the raster path with
// shaders/raster/push_constants.{vert,frag} (compiled by glslangValidator at build time). Both
// draws use the same vertex/index buffers and pipeline; the only per-draw difference is the
// push-constant block CommandBufferRecorder writes before each vkCmdDrawIndexed ({materialId,..}).
// The vertex stage uses it to place the triangle (1 -> left, 2 -> right) and the fragment stage to
// colour it (1 -> red, 2 -> green). Readback must show a red left and a green right triangle on
// black — any stale/shared push data would put both triangles in one place or one colour.
#include "b5_rhi_test_common.hpp"

#include <fuse/core/init.hpp>
#include <fuse/renderer/postprocess/vignette.hpp>
#include <fuse/renderer/rhi/push_bytes.hpp>
#include <fuse/renderer/rhi_context.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

using b5rhi::expectTrue;
using fuse::u32;
using fuse::u8;

struct Rgb {
    u8 r, g, b;
};

[[maybe_unused]] Rgb pixel(const std::vector<u8>& rgba, u32 w, u32 x, u32 y) {
    const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4u;
    return {rgba[i], rgba[i + 1], rgba[i + 2]};
}

[[maybe_unused]] bool is(Rgb p, u8 r, u8 g, u8 b) {
    return p.r == r && p.g == g && p.b == b;
}

void testPushBytesOk() {
    using fuse::renderer::push_bytes_ok;
    expectTrue(push_bytes_ok(0), "push_bytes_ok(0) is true");
    expectTrue(push_bytes_ok(128), "push_bytes_ok(128) is true");
    expectTrue(push_bytes_ok(4), "push_bytes_ok(4) is true");
    expectTrue(!push_bytes_ok(2), "push_bytes_ok(2) is false");
    expectTrue(!push_bytes_ok(132), "push_bytes_ok(132) is false");
    expectTrue(!push_bytes_ok(-4), "push_bytes_ok(-4) is false");
}

void testVignetteFactor() {
    using fuse::renderer::vignette_factor;
    expectTrue(vignette_factor(0.5f, 0.5f, 1.f, 1.f) == 1.f, "vignette center is 1");
    expectTrue(vignette_factor(0.f, 0.f, 0.f, 0.f) == 1.f, "intensity 0 leaves a corner at 1");
    expectTrue(vignette_factor(1.f, 1.f, 0.f, 1.f) == 1.f, "intensity 0 leaves the opposite corner at 1");

    const float squareCorner = vignette_factor(0.f, 0.f, 1.f, 0.f);
    const float roundCorner = vignette_factor(0.f, 1.f, 1.f, 1.f);
    expectTrue(squareCorner >= 0.f && squareCorner < 0.25f, "square corner at intensity 1 is below 0.25");
    expectTrue(roundCorner >= 0.f && roundCorner < 0.25f, "round corner at intensity 1 is below 0.25");

    // (0.75, 0.75): max(|0.25|, |0.25|) = 0.25, so roundness 0 is exactly 0.5.
    expectTrue(vignette_factor(0.75f, 0.75f, 1.f, 0.f) == 0.5f, "roundness 0 uses the max absolute offset");
    const float circle = vignette_factor(0.75f, 0.75f, 1.f, 1.f);
    expectTrue(circle < 0.5f && circle > 0.25f, "roundness 1 uses the euclidean radius");
    const float blended = vignette_factor(0.75f, 0.75f, 1.f, 0.5f);
    expectTrue(std::fabs(blended - (0.5f + circle) * 0.5f) < 1e-6f, "roundness blends square and circle");
    expectTrue(vignette_factor(0.75f, 0.75f, 1.f, 4.f) == circle, "roundness above 1 matches a circle");
    expectTrue(vignette_factor(0.75f, 0.75f, 1.f, -2.f) == 0.5f, "roundness below 0 matches a square");
    expectTrue(vignette_factor(0.75f, 0.75f, 3.f, 0.f) == 0.5f, "intensity above 1 clamps to 1");
    expectTrue(vignette_factor(0.f, 0.f, -1.f, 1.f) == 1.f, "negative intensity clamps to a clear frame");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    expectTrue(vignette_factor(nan, 0.5f, 1.f, 1.f) == 1.f, "non-finite uv returns 1");
    expectTrue(vignette_factor(0.5f, 0.5f, inf, 0.f) == 1.f, "non-finite intensity returns 1");
    expectTrue(vignette_factor(0.5f, 0.5f, 1.f, nan) == 1.f, "non-finite roundness returns 1");
}

} // namespace

int main() {
    testPushBytesOk();
    testVignetteFactor();
    if (b5rhi::failures() != 0) {
        return b5rhi::finish("fuse_b5_rhi_push_constants");
    }
#if !defined(FUSE_VULKAN_BACKEND)
    return b5rhi::skip("fuse_b5_rhi_push_constants", "Vulkan backend disabled");
#elif !defined(FUSE_B5_RHI_SHADERS_BUILT)
    return b5rhi::skip("fuse_b5_rhi_push_constants", "raster shaders not built (glslangValidator missing)");
#else
    fuse::core::initialize();
    const std::string vert = std::string(FUSE_B5_RHI_SHADER_DIR) + "/push_constants.vert.spv";
    const std::string frag = std::string(FUSE_B5_RHI_SHADER_DIR) + "/push_constants.frag.spv";

    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;
    desc.bootstrap.createSwapchain = false;
    desc.raster.width = 160;
    desc.raster.height = 120;
    desc.raster.vertexSpirvPath = vert.c_str();
    desc.raster.fragmentSpirvPath = frag.c_str();
    auto context = fuse::renderer::RhiContext::create(desc);
    if (context == nullptr || !context->bootstrap().status().deviceReady) {
        context.reset();
        fuse::core::shutdown();
        return b5rhi::skip("fuse_b5_rhi_push_constants", "no Vulkan device (needs an ICD, Lavapipe in CI)");
    }

    fuse::renderer::DrawList draws;
    fuse::renderer::DrawCall call{};
    call.indexCount = 3;
    call.materialId = 1;
    expectTrue(draws.push(call), "draw 1 (material 1)");
    call.materialId = 2;
    expectTrue(draws.push(call), "draw 2 (material 2)");

    expectTrue(context->beginFrame(0u), "beginFrame");
    expectTrue(context->submitDrawList(draws, 0u), "submitDrawList");
    const fuse::renderer::CommandBufferRecorder& recorder = context->commandRecorder();
    std::printf("encoded: %u indexed draws, %u push-constant updates, %u pipeline binds\n",
                recorder.vulkanDrawIndexedCount(), recorder.vulkanPushConstantCount(),
                recorder.vulkanPipelineBindCount());
    expectTrue(recorder.vulkanDrawIndexedCount() == 2u, "both draws encoded as vkCmdDrawIndexed");
    expectTrue(recorder.vulkanPushConstantCount() == 2u, "one push-constant update per draw");
    expectTrue(recorder.vulkanPipelineBindCount() == 1u, "one pipeline for both draws");

    const fuse::renderer::RasterPath* raster = context->rasterPath();
    std::vector<u8> rgba;
    expectTrue(raster != nullptr && raster->readbackColor(rgba), "colour readback");
    const u32 w = desc.raster.width;
    const u32 h = desc.raster.height;
    if (rgba.size() == static_cast<std::size_t>(w) * h * 4u) {
        const u32 y = h / 2u + h / 10u;
        const Rgb left = pixel(rgba, w, w / 4u, y);
        const Rgb right = pixel(rgba, w, (3u * w) / 4u, y);
        const Rgb middle = pixel(rgba, w, w / 2u, y);
        std::printf("left (%u,%u,%u) right (%u,%u,%u) middle (%u,%u,%u)\n", left.r, left.g, left.b, right.r,
                    right.g, right.b, middle.r, middle.g, middle.b);
        expectTrue(is(left, 255, 0, 0), "material 1 draw: red triangle on the left");
        expectTrue(is(right, 0, 255, 0), "material 2 draw: green triangle on the right");
        expectTrue(is(middle, 0, 0, 0), "gap between the triangles stays black");
        expectTrue(is(pixel(rgba, w, 1, 1), 0, 0, 0) && is(pixel(rgba, w, w - 2, h - 2), 0, 0, 0),
                   "corners stay black");

        u32 red = 0, green = 0, other = 0;
        for (u32 py = 0; py < h; ++py) {
            for (u32 px = 0; px < w; ++px) {
                const Rgb p = pixel(rgba, w, px, py);
                if (is(p, 255, 0, 0)) {
                    red += px < w / 2u ? 1u : 1000000u; // red must be entirely in the left half
                } else if (is(p, 0, 255, 0)) {
                    green += px >= w / 2u ? 1u : 1000000u;
                } else if (!is(p, 0, 0, 0)) {
                    ++other;
                }
            }
        }
        std::printf("red %u, green %u, other %u pixels\n", red, green, other);
        expectTrue(red > 0u && red < 1000000u, "red pixels only in the left half");
        expectTrue(green > 0u && green < 1000000u, "green pixels only in the right half");
        expectTrue(red == green, "both draws cover the same area (same mesh, mirrored placement)");
        expectTrue(other == 0u, "no pixels of any third colour");
    }

    context.reset();
    fuse::core::shutdown();
    return b5rhi::finish("fuse_b5_rhi_push_constants");
#endif
}
