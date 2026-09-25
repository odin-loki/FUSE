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
#include <fuse/renderer/rhi_context.hpp>

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

} // namespace

int main() {
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
