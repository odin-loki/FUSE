#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/graphics_pipeline.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/renderer/vk/raster_path.hpp>
#include <fuse/renderer/vk/render_pass.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef FUSE_SHADER_FIXTURE_DIR
#define FUSE_SHADER_FIXTURE_DIR "Source/FUSE/Renderer/shaders/fixtures"
#endif

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

std::string fixturePath(const char* name) {
    return std::string(FUSE_SHADER_FIXTURE_DIR) + "/" + name;
}

void testGraphicsPipelineFromFixtures() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for graphics pipeline tests");

    fuse::renderer::VulkanDevice* device = bootstrap->device();
    expectTrue(device != nullptr, "device pointer available");

    auto renderPass = fuse::renderer::RenderPass::create(*device);
    expectTrue(renderPass != nullptr && renderPass->isValid(), "render pass created");

    const std::string vertPath = fixturePath("minimal.vert.spv");
    const std::string fragPath = fixturePath("minimal.frag.spv");

    auto vertModule =
        fuse::renderer::ShaderModule::createFromFile(*device, fuse::renderer::ShaderStage::Vertex,
                                                     vertPath.c_str());
    auto fragModule =
        fuse::renderer::ShaderModule::createFromFile(*device, fuse::renderer::ShaderStage::Fragment,
                                                     fragPath.c_str());
    expectTrue(vertModule != nullptr && fragModule != nullptr, "fixture shader modules allocated");

    auto pipelineLayout = fuse::renderer::PipelineLayout::create(*device);
    expectTrue(pipelineLayout != nullptr && pipelineLayout->isValid(), "pipeline layout created");

    fuse::renderer::GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.layout = pipelineLayout.get();
    pipelineDesc.vertexShader = vertModule.get();
    pipelineDesc.fragmentShader = fragModule.get();
    pipelineDesc.renderPass = renderPass.get();
    pipelineDesc.debugName = "test_graphics_pipeline";

    auto graphicsPipeline = fuse::renderer::GraphicsPipeline::create(*device, pipelineDesc);
    expectTrue(graphicsPipeline != nullptr, "graphics pipeline allocated");

#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        expectTrue(graphicsPipeline->isValid(), "graphics pipeline valid with Vulkan device");
        expectTrue(graphicsPipeline->nativeHandle() != nullptr,
                   "graphics pipeline has native handle");
    } else {
        expectTrue(!graphicsPipeline->isValid(), "graphics pipeline invalid without ICD");
    }
#else
    expectTrue(graphicsPipeline->isValid(), "graphics pipeline valid in stub backend");
    expectTrue(graphicsPipeline->nativeHandle() == nullptr, "stub backend has no native handle");
#endif
}

void testRasterPathClearTriangle() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for raster path tests");

    const std::string rasterVertPath = fixturePath("minimal.vert.spv");
    const std::string rasterFragPath = fixturePath("minimal.frag.spv");
    fuse::renderer::RasterPathDesc rasterDesc{};
    rasterDesc.vertexSpirvPath = rasterVertPath.c_str();
    rasterDesc.fragmentSpirvPath = rasterFragPath.c_str();

    auto rasterPath = fuse::renderer::RasterPath::create(*bootstrap->device(), rasterDesc);
    expectTrue(rasterPath != nullptr, "raster path allocated");
#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        expectTrue(rasterPath->isReady(), "raster path ready with Vulkan device");

        fuse::renderer::RenderCommandList commands;
        commands.clear3D(0.1f, 0.2f, 0.3f);
        expectTrue(rasterPath->recordFrame(commands), "raster path records clear + triangle");

        const fuse::renderer::RasterPathStats& stats = rasterPath->lastStats();
        expectTrue(stats.clearCount == 1u, "one clear command mirrored");
        expectTrue(stats.triangleDrawCount == 1u, "triangle draw issued");
        expectTrue(stats.framesRecorded == 1u, "one frame recorded");
    } else {
        expectTrue(!rasterPath->isReady(), "raster path not ready without ICD");
    }
#else
    expectTrue(rasterPath->isReady(), "raster path ready in stub backend");

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.1f, 0.2f, 0.3f);
    expectTrue(rasterPath->recordFrame(commands), "raster path records clear + triangle");

    const fuse::renderer::RasterPathStats& stats = rasterPath->lastStats();
    expectTrue(stats.clearCount == 1u, "one clear command mirrored");
    expectTrue(stats.triangleDrawCount == 1u, "triangle draw issued");
    expectTrue(stats.framesRecorded == 1u, "one frame recorded");
#endif
}

void testRhiContextWiresRasterPath() {
    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;
    desc.bootstrap.createSwapchain = false;

    auto context = fuse::renderer::RhiContext::create(desc);
    expectTrue(context != nullptr, "RHI context allocated");

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.4f, 0.5f, 0.6f);

    expectTrue(fuse::platform::mayTouchGpuContext(), "render thread available");
#if defined(FUSE_VULKAN_BACKEND)
    expectTrue(context->beginFrame(0u), "beginFrame accepted on render thread");
#endif
    const bool submitted = context->submitFrame(commands, 0u);
#if defined(FUSE_VULKAN_BACKEND)
    expectTrue(submitted, "submit accepted when Vulkan device ready");
    if (context->bootstrap().status().deviceReady) {
        expectTrue(context->rasterPath() != nullptr, "raster path created lazily");
        expectTrue(context->lastRasterStats().triangleDrawCount == 1u,
                   "RHI context wired clear + triangle path");
    }
#else
    expectTrue(!submitted, "stub mode rejects GPU submit");
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testGraphicsPipelineFromFixtures();
    testRasterPathClearTriangle();
    testRhiContextWiresRasterPath();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_graphics_pipeline: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_graphics_pipeline: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
