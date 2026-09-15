#include <fuse/core/init.hpp>
#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/deferred/deferred_renderer.hpp>
#include <fuse/renderer/deferred/frame_pipeline.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testPassSchedule() {
    expectTrue(fuse::renderer::DeferredFramePipeline::passCount() == 16u,
               "deferred pipeline exposes 16 passes");
    expectTrue(std::string(fuse::renderer::DeferredFramePipeline::passName(
                   fuse::renderer::DeferredPassId::GBuffer)) == "gbuffer",
               "gbuffer pass name");
    expectTrue(std::string(fuse::renderer::DeferredFramePipeline::passName(
                   fuse::renderer::DeferredPassId::DeferredShading)) == "deferred_shading",
               "deferred shading pass name");
}

void testDeferredGraphBuild() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for deferred graph test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    expectTrue(resources.init(*bootstrap->device(), bindless), "resource manager ready");

    fuse::renderer::DeferredRendererDesc desc{};
    desc.pipeline.width = 128;
    desc.pipeline.height = 128;

    auto renderer = fuse::renderer::DeferredRenderer::create(desc);
    expectTrue(renderer->init(resources), "deferred renderer initialized");

    fuse::renderer::RenderGraph graph;
    expectTrue(renderer->buildFrameGraph(graph, 0u), "deferred frame graph compiled");
    expectTrue(graph.compileInfo().passCount == fuse::renderer::DeferredFramePipeline::passCount(),
               "all deferred passes registered");
    expectTrue(graph.compileInfo().executablePassCount > 0u, "executable passes present");
    expectTrue(renderer->pipeline().lastStats().cudaPassCount == 6u, "six CUDA passes scheduled");
    expectTrue(renderer->pipeline().lastStats().vulkanPassCount == 10u, "ten Vulkan passes scheduled");

    renderer->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

void testDeferredRendererExecuteStub() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for deferred execute test");

    fuse::renderer::BindlessDescriptors bindless{};
    bindless.init(*bootstrap->device());

    fuse::renderer::ResourceManager resources;
    resources.init(*bootstrap->device(), bindless);

    auto frames = fuse::renderer::FrameManager::create(*bootstrap->device());
    fuse::renderer::CommandBufferRecorder recorder;

    auto renderer = fuse::renderer::DeferredRenderer::create({});
    renderer->init(resources);

    const fuse::renderer::RenderGraphExecuteInfo info =
        renderer->executeFrame(*bootstrap->device(), *frames, recorder, 0u);
    expectTrue(info.executedPassCount > 0u, "deferred renderer executed passes");
    expectTrue(recorder.recordCount() > 0u, "command recorder captured deferred work");

    renderer->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    fuse::core::initialize();

    testPassSchedule();
    testDeferredGraphBuild();
    testDeferredRendererExecuteStub();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_deferred_pipeline: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_deferred_pipeline: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
