#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/composite_pass.hpp>
#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/rhi_context.hpp>

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

void testCompositePassScaffoldReady() {
    fuse::renderer::CompositePassDesc desc{};
    desc.defaultBlend = 0.75f;

    auto pass = fuse::renderer::CompositePass::create(desc);
    expectTrue(pass != nullptr, "composite pass allocated");
    expectTrue(pass->isReady(), "composite pass scaffold ready");
    expectTrue(pass->lastStats().ready, "stats report ready");
}

void testCompositePassRecordsFrame() {
    fuse::renderer::CompositePassDesc desc{};
    desc.defaultBlend = 0.25f;

    auto pass = fuse::renderer::CompositePass::create(desc);
    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.1f, 0.2f, 0.3f);
    commands.drawSprite2D(0.f, 0.f, 0.f, 255, 128, 64);

    expectTrue(pass->blendForFrame(commands) == 0.25f, "blend mirrors desc default");
    expectTrue(pass->recordFrame(commands), "composite frame recorded");
    expectTrue(pass->lastStats().framesRecorded == 1u, "frame counter advanced");
    expectTrue(pass->lastStats().lastBlend == 0.25f, "last blend stored");
}

void testGraphCompositeBeforePresent() {
    fuse::renderer::RenderGraph graph;
    graph.beginFrame(0u);

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.2f, 0.3f, 0.4f);
    commands.drawSprite2D(0.f, 0.f, 0.f, 255, 200, 64);

    fuse::renderer::populateRenderGraphFromCommandList(graph, commands, 0.5f);
    graph.compile();

    expectTrue(graph.compileInfo().passCount >= 4u,
               "clear + sprites + composite + present passes");
    expectTrue(graph.compileInfo().executablePassCount >= 4u, "composite kept before present");
    expectTrue(graph.compileInfo().compiled, "composite graph compiles");
}

void testCommandRecorderComposite() {
    fuse::renderer::CommandBufferRecorder recorder;
    recorder.beginRecording(nullptr);
    recorder.beginPass("composite");
    recorder.composite(0.5f);
    recorder.endPass();
    recorder.endRecording();

    bool foundComposite = false;
    for (const fuse::renderer::CommandRecord& record : recorder.records()) {
        if (record.kind == fuse::renderer::CommandRecordKind::Composite) {
            foundComposite = true;
            expectTrue(record.compositeBlend == 0.5f, "composite blend recorded");
        }
    }
    expectTrue(foundComposite, "composite command recorded");
}

void testRhiContextCompositeStats() {
    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;
    desc.composite.defaultBlend = 0.6f;

    auto context = fuse::renderer::RhiContext::create(desc);
    expectTrue(context != nullptr, "RHI context allocated");
    expectTrue(context->compositePass() == nullptr, "composite pass lazy until submit");

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.1f, 0.2f, 0.3f);

#if defined(FUSE_VULKAN_BACKEND)
    expectTrue(context->beginFrame(0u), "beginFrame accepted on render thread");
    expectTrue(context->submitFrame(commands, 0u), "submit accepted when Vulkan device ready");
    expectTrue(context->compositePass() != nullptr, "composite pass created on submit");
    expectTrue(context->lastCompositeStats().framesRecorded >= 1u,
               "composite stats updated after submit");
    expectTrue(context->lastGraphPassCount() >= 3u, "graph includes composite before present");
#else
    expectTrue(!context->submitFrame(commands, 0u), "stub mode rejects GPU submit");
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testCompositePassScaffoldReady();
    testCompositePassRecordsFrame();
    testGraphCompositeBeforePresent();
    testCommandRecorderComposite();
    testRhiContextCompositeStats();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_composite_pass: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_composite_pass: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
