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

void testExplicitPassDependencyReordersCompileOrder() {
    fuse::renderer::RenderGraph graph;
    graph.beginFrame(0u);

    fuse::renderer::RGPassDesc passA{};
    passA.name = "pass_a";
    graph.addPass(passA);

    fuse::renderer::RGPassDesc passB{};
    passB.name = "pass_b";
    graph.addPass(passB);

    fuse::renderer::RGTextureAccess colorWrite{};
    colorWrite.texture = {fuse::renderer::RenderGraph::kBackbufferTextureId};
    colorWrite.access = fuse::renderer::RGResourceAccess::ColorAttachmentWrite;

    fuse::renderer::RGPassDesc passC{};
    passC.name = "pass_c";
    passC.textureAccesses = &colorWrite;
    passC.textureAccessCount = 1;
    graph.addPass(passC);

    graph.addPassDependency(2u, 0u);

    fuse::renderer::RGTextureAccess present{};
    present.texture = {fuse::renderer::RenderGraph::kBackbufferTextureId};
    present.access = fuse::renderer::RGResourceAccess::Present;

    fuse::renderer::RGPassDesc presentPass{};
    presentPass.name = "present";
    presentPass.textureAccesses = &present;
    presentPass.textureAccessCount = 1;
    graph.addPass(presentPass);

    graph.addPassDependency(0u, 3u);

    graph.compile();

    expectTrue(graph.compileInfo().compiled, "dependency graph compiled");
    expectTrue(graph.dependencyEdges().size() >= 1u, "explicit dependency edge recorded");
    expectTrue(graph.compileOrder().size() == 3u, "pass_a + pass_c + present kept");
    expectTrue(graph.compileOrder().front() == 2u, "pass_c runs before pass_a via explicit edge");
}

void testResourceAccessBuildsDependencyEdge() {
    fuse::renderer::RenderGraph graph;
    graph.beginFrame(0u);

    fuse::renderer::RGTextureAccess writeAccess{};
    writeAccess.texture = {fuse::renderer::RenderGraph::kBackbufferTextureId};
    writeAccess.access = fuse::renderer::RGResourceAccess::ColorAttachmentWrite;

    fuse::renderer::RGPassDesc writePass{};
    writePass.name = "write";
    writePass.textureAccesses = &writeAccess;
    writePass.textureAccessCount = 1;
    graph.addPass(writePass);

    fuse::renderer::RGTextureAccess readAccess{};
    readAccess.texture = {fuse::renderer::RenderGraph::kBackbufferTextureId};
    readAccess.access = fuse::renderer::RGResourceAccess::ShaderRead;

    fuse::renderer::RGPassDesc readPass{};
    readPass.name = "read";
    readPass.textureAccesses = &readAccess;
    readPass.textureAccessCount = 1;
    graph.addPass(readPass);

    fuse::renderer::RGTextureAccess present{};
    present.texture = {fuse::renderer::RenderGraph::kBackbufferTextureId};
    present.access = fuse::renderer::RGResourceAccess::Present;

    fuse::renderer::RGPassDesc presentPass{};
    presentPass.name = "present";
    presentPass.textureAccesses = &present;
    presentPass.textureAccessCount = 1;
    graph.addPass(presentPass);

    graph.compile();

    bool foundResourceEdge = false;
    for (const fuse::renderer::RGPassDependencyEdge& edge : graph.dependencyEdges()) {
        if (edge.kind == fuse::renderer::RGPassDependencyKind::ResourceAccess &&
            edge.fromPassIndex == 0u && edge.toPassIndex == 1u) {
            foundResourceEdge = true;
        }
    }

    expectTrue(foundResourceEdge, "write-then-read creates resource dependency edge");
    expectTrue(graph.compileOrder().size() == 3u, "write/read/present ordered");
    expectTrue(graph.compileOrder()[0] == 0u && graph.compileOrder()[1] == 1u,
               "write pass precedes read pass in compile order");
}

void testTransientResourceLifetimeTracked() {
    fuse::renderer::RenderGraph graph;
    graph.beginFrame(0u);

    fuse::renderer::RGTextureRef transient = graph.createTransient({});

    fuse::renderer::RGTextureAccess writeAccess{};
    writeAccess.texture = transient;
    writeAccess.access = fuse::renderer::RGResourceAccess::ColorAttachmentWrite;

    fuse::renderer::RGPassDesc writePass{};
    writePass.name = "write";
    writePass.textureAccesses = &writeAccess;
    writePass.textureAccessCount = 1;
    graph.addPass(writePass);

    fuse::renderer::RGTextureAccess readAccess{};
    readAccess.texture = transient;
    readAccess.access = fuse::renderer::RGResourceAccess::ShaderRead;

    fuse::renderer::RGPassDesc readPass{};
    readPass.name = "read";
    readPass.textureAccesses = &readAccess;
    readPass.textureAccessCount = 1;
    graph.addPass(readPass);

    fuse::renderer::RGTextureAccess compositeWrite{};
    compositeWrite.texture = {fuse::renderer::RenderGraph::kBackbufferTextureId};
    compositeWrite.access = fuse::renderer::RGResourceAccess::ColorAttachmentWrite;

    fuse::renderer::RGPassDesc compositePass{};
    compositePass.name = "composite";
    compositePass.textureAccesses = &compositeWrite;
    compositePass.textureAccessCount = 1;
    graph.addPass(compositePass);

    graph.addPassDependency(1u, 2u);

    fuse::renderer::RGTextureAccess present{};
    present.texture = {fuse::renderer::RenderGraph::kBackbufferTextureId};
    present.access = fuse::renderer::RGResourceAccess::Present;

    fuse::renderer::RGPassDesc presentPass{};
    presentPass.name = "present";
    presentPass.textureAccesses = &present;
    presentPass.textureAccessCount = 1;
    graph.addPass(presentPass);

    graph.compile();

    const fuse::renderer::RGResourceLifetime* lifetime = nullptr;
    for (const fuse::renderer::RGResourceLifetime& candidate : graph.resourceLifetimes()) {
        if (candidate.resourceId == transient.id && candidate.isTexture) {
            lifetime = &candidate;
            break;
        }
    }

    expectTrue(lifetime != nullptr, "transient texture lifetime recorded");
    expectTrue(lifetime->phase == fuse::renderer::RGResourceLifetimePhase::TransientCreated,
               "transient texture phase tagged");
    expectTrue(lifetime->firstPassIndex == 0u, "lifetime starts at first writer pass");
    expectTrue(lifetime->lastPassIndex == 1u, "lifetime ends at last transient reader pass");
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
    testExplicitPassDependencyReordersCompileOrder();
    testResourceAccessBuildsDependencyEdge();
    testTransientResourceLifetimeTracked();
    testRhiContextUsesRenderGraph();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_render_graph: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_render_graph: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
