#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

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

void testBarrierInsertedBetweenColorAndPresent() {
    fuse::renderer::RenderGraph graph;
    graph.beginFrame(0u);

    fuse::renderer::RGTextureAccess colorWrite{};
    colorWrite.texture = {fuse::renderer::RenderGraph::kBackbufferTextureId};
    colorWrite.access = fuse::renderer::RGResourceAccess::ColorAttachmentWrite;

    fuse::renderer::RGTextureAccess present{};
    present.texture = {fuse::renderer::RenderGraph::kBackbufferTextureId};
    present.access = fuse::renderer::RGResourceAccess::Present;

    fuse::renderer::RGPassDesc clearPass{};
    clearPass.name = "clear";
    clearPass.textureAccesses = &colorWrite;
    clearPass.textureAccessCount = 1;
    graph.addPass(clearPass);

    fuse::renderer::RGPassDesc presentPass{};
    presentPass.name = "present";
    presentPass.textureAccesses = &present;
    presentPass.textureAccessCount = 1;
    graph.addPass(presentPass);

    graph.compile();

    expectTrue(graph.compileInfo().compiled, "graph compiled");
    expectTrue(graph.compileInfo().executablePassCount == 2u, "clear + present kept");
    expectTrue(graph.plannedBarriers().size() >= 1u,
               "layout transition barrier planned between color write and present");
}

void testUnusedPassCulled() {
    fuse::renderer::RenderGraph graph;
    graph.beginFrame(0u);

    fuse::renderer::RGTextureRef transient = graph.createTransient({});
    fuse::renderer::RGTextureAccess transientWrite{};
    transientWrite.texture = transient;
    transientWrite.access = fuse::renderer::RGResourceAccess::ColorAttachmentWrite;

    fuse::renderer::RGPassDesc orphanPass{};
    orphanPass.name = "orphan";
    orphanPass.textureAccesses = &transientWrite;
    orphanPass.textureAccessCount = 1;
    graph.addPass(orphanPass);

    fuse::renderer::RGTextureAccess present{};
    present.texture = {fuse::renderer::RenderGraph::kBackbufferTextureId};
    present.access = fuse::renderer::RGResourceAccess::Present;

    fuse::renderer::RGPassDesc presentPass{};
    presentPass.name = "present";
    presentPass.textureAccesses = &present;
    presentPass.textureAccessCount = 1;
    graph.addPass(presentPass);

    graph.compile();

    expectTrue(graph.compileInfo().culledPassCount >= 1u, "orphan pass culled");
    expectTrue(graph.compileInfo().executablePassCount == 1u, "only present pass remains");
}

void testPopulateFromCommandList() {
    fuse::renderer::RenderGraph graph;
    graph.beginFrame(1u);

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.2f, 0.3f, 0.4f);
    commands.drawSprite2D(0.f, 0.f, 0.f, 255, 200, 64);
    commands.drawSprite2D(10.f, 5.f, 0.f, 255, 200, 64);

    fuse::renderer::populateRenderGraphFromCommandList(graph, commands);
    graph.compile();

    expectTrue(graph.compileInfo().passCount >= 4u, "clear + sprites + composite + present passes");
    expectTrue(graph.compileInfo().compiled, "hybrid command list graph compiles");
}

void testCommandBufferRecorderCapturesPasses() {
    fuse::renderer::CommandBufferRecorder recorder;
    recorder.beginRecording(nullptr);
    recorder.beginPass("test");
    recorder.clearColor(0.1f, 0.2f, 0.3f);
    recorder.draw(2u);
    recorder.endPass();
    recorder.present();
    recorder.endRecording();

    expectTrue(recorder.recordCount() >= 5u, "logical commands recorded");
    expectTrue(recorder.records().front().kind == fuse::renderer::CommandRecordKind::BeginPass,
               "first record is pass begin");
}

void testRhiContextUsesRenderGraph() {
    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;

    auto context = fuse::renderer::RhiContext::create(desc);
    expectTrue(context != nullptr, "RHI context allocated");

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.1f, 0.2f, 0.3f);
    commands.drawSprite2D(0.f, 0.f, 0.f, 255, 128, 64);

#if defined(FUSE_VULKAN_BACKEND)
    expectTrue(context->beginFrame(0u), "beginFrame accepted on render thread");
    const bool submitted = context->submitFrame(commands, 0u);
    expectTrue(submitted, "submit accepted when Vulkan device ready");
    expectTrue(context->lastGraphPassCount() >= 3u, "render graph executed passes");
    expectTrue(context->lastRecordedCommandCount() > 0u, "command recorder captured work");
#else
    expectTrue(!context->submitFrame(commands, 0u), "stub mode rejects GPU submit");
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testBarrierInsertedBetweenColorAndPresent();
    testUnusedPassCulled();
    testPopulateFromCommandList();
    testCommandBufferRecorderCapturesPasses();
    testRhiContextUsesRenderGraph();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_render_graph: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_render_graph: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
