#include <fuse/core/init.hpp>
#include <fuse/math/mat.hpp>
#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/deferred/deferred_renderer.hpp>
#include <fuse/renderer/deferred/deferred_target.hpp>
#include <fuse/renderer/deferred/frame_pipeline.hpp>
#include <fuse/renderer/deferred/gbuffer_slot.hpp>
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

fuse::renderer::DeferredGBufferDraw makeGBufferDraw(fuse::u32 materialId,
                                                    fuse::u32 entityIndex,
                                                    fuse::u32 entityGeneration,
                                                    fuse::u32 indexCount,
                                                    float translationX) {
    fuse::renderer::DeferredGBufferDraw draw{};
    draw.materialId = materialId;
    draw.entity.index = entityIndex;
    draw.entity.generation = entityGeneration;
    draw.indexCount = indexCount;
    draw.transform = fuse::math::Mat4::identity();
    draw.transform.at(0, 3) = translationX;
    return draw;
}

bool drawMatches(const fuse::renderer::DeferredGBufferDraw& draw,
                 fuse::u32 materialId,
                 fuse::u32 entityIndex,
                 fuse::u32 entityGeneration,
                 fuse::u32 indexCount,
                 float translationX) {
    return draw.materialId == materialId && draw.entity.index == entityIndex &&
           draw.entity.generation == entityGeneration && draw.indexCount == indexCount &&
           draw.transform.at(0, 3) == translationX;
}

/// CPU gate. Must run before any GPU skip; a failure exits instead of continuing.
void testDeferredTargetOk() {
    expectTrue(fuse::renderer::deferred_target_ok(1), "deferred_target_ok(1) is true");
    expectTrue(fuse::renderer::deferred_target_ok(8), "deferred_target_ok(8) is true");
    expectTrue(!fuse::renderer::deferred_target_ok(0), "deferred_target_ok(0) is false");
    expectTrue(!fuse::renderer::deferred_target_ok(9), "deferred_target_ok(9) is false");
}

/// CPU gate. Must run before any GPU skip; a failure exits instead of continuing.
void testGbufferSlotOk() {
    expectTrue(fuse::renderer::gbuffer_slot_ok(0), "gbuffer_slot_ok(0) is true");
    expectTrue(fuse::renderer::gbuffer_slot_ok(1), "gbuffer_slot_ok(1) is true");
    expectTrue(fuse::renderer::gbuffer_slot_ok(2), "gbuffer_slot_ok(2) is true");
    expectTrue(fuse::renderer::gbuffer_slot_ok(3), "gbuffer_slot_ok(3) is true");
    expectTrue(fuse::renderer::gbuffer_slot_ok(4), "gbuffer_slot_ok(4) is true");
    expectTrue(!fuse::renderer::gbuffer_slot_ok(-1), "gbuffer_slot_ok(-1) is false");
    expectTrue(!fuse::renderer::gbuffer_slot_ok(5), "gbuffer_slot_ok(5) is false");
}

void testGBufferCpuDrawList() {
    auto renderer = fuse::renderer::DeferredRenderer::create({});
    expectTrue(renderer->gbufferDrawCount() == 0u, "gbuffer draw list starts empty");
    expectTrue(renderer->gbufferDraws().empty(), "gbuffer draw span starts empty");

    renderer->appendGBufferDrawList({});
    expectTrue(renderer->gbufferDrawCount() == 0u, "empty span on an empty list stays empty");

    const fuse::renderer::DeferredGBufferDraw first[] = {
        makeGBufferDraw(2u, 10u, 1u, 36u, 3.f),
        makeGBufferDraw(1u, 4u, 2u, 12u, 1.f),
        makeGBufferDraw(2u, 10u, 1u, 99u, 8.f),
        makeGBufferDraw(2u, 10u, 0u, 3u, -1.f),
        makeGBufferDraw(2u, 3u, 9u, 6u, 0.f),
    };
    renderer->appendGBufferDrawList(first);
    expectTrue(renderer->gbufferDrawCount() == 5u, "gbuffer draw count matches the submitted span");

    const auto draws = renderer->gbufferDraws();
    expectTrue(draws.size() == 5u, "gbuffer draw span size matches the count");
    expectTrue(draws.size() == 5u && drawMatches(draws[0], 1u, 4u, 2u, 12u, 1.f),
               "lower materialId sorts first");
    expectTrue(draws.size() == 5u && drawMatches(draws[1], 2u, 3u, 9u, 6u, 0.f),
               "same material sorts by entity index");
    expectTrue(draws.size() == 5u && drawMatches(draws[2], 2u, 10u, 0u, 3u, -1.f),
               "same entity index sorts by generation");
    expectTrue(draws.size() == 5u && drawMatches(draws[3], 2u, 10u, 1u, 36u, 3.f),
               "stable tie keeps the earlier record and its transform");
    expectTrue(draws.size() == 5u && drawMatches(draws[4], 2u, 10u, 1u, 99u, 8.f),
               "stable tie keeps the later record after the earlier one");

    const fuse::renderer::DeferredGBufferDraw extra[] = {
        makeGBufferDraw(0u, 1u, 1u, 18u, 4.f),
        makeGBufferDraw(2u, 10u, 1u, 50u, 6.f),
    };
    renderer->appendGBufferDrawList(extra);
    expectTrue(renderer->gbufferDrawCount() == 7u, "append grows the gbuffer draw list");

    const auto grown = renderer->gbufferDraws();
    expectTrue(grown.size() == 7u && drawMatches(grown[0], 0u, 1u, 1u, 18u, 4.f),
               "appended draw is sorted into place");
    expectTrue(grown.size() == 7u && drawMatches(grown[4], 2u, 10u, 1u, 36u, 3.f),
               "earlier tie stays ahead of later ones");
    expectTrue(grown.size() == 7u && drawMatches(grown[5], 2u, 10u, 1u, 99u, 8.f),
               "original later tie stays in submission order");
    expectTrue(grown.size() == 7u && drawMatches(grown[6], 2u, 10u, 1u, 50u, 6.f),
               "appended tie sorts after records already stored");

    renderer->appendGBufferDrawList({});
    expectTrue(renderer->gbufferDrawCount() == 0u, "empty span clears the gbuffer draw list");
    expectTrue(renderer->gbufferDraws().empty(), "cleared gbuffer draw span is empty");

    const fuse::renderer::DeferredGBufferDraw again[] = {
        makeGBufferDraw(7u, 2u, 3u, 24u, 5.f),
    };
    renderer->appendGBufferDrawList(again);
    expectTrue(renderer->gbufferDrawCount() == 1u, "draw list accepts records after a clear");
    expectTrue(renderer->gbufferDrawCount() == 1u &&
                   drawMatches(renderer->gbufferDraws()[0], 7u, 2u, 3u, 24u, 5.f),
               "stored record keeps material, entity, index count, and transform");
}

void testPassSchedule() {
    expectTrue(fuse::renderer::DeferredFramePipeline::passCount() == 19u,
               "deferred pipeline exposes 19 passes");
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
    expectTrue(renderer->pipeline().lastStats().cudaPassCount == 7u, "seven CUDA passes scheduled");
    expectTrue(renderer->pipeline().lastStats().vulkanPassCount == 12u, "twelve Vulkan passes scheduled");

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

    const fuse::renderer::DeferredGBufferDraw submitted[] = {
        makeGBufferDraw(3u, 8u, 1u, 30u, 1.f),
        makeGBufferDraw(1u, 2u, 4u, 12u, 2.f),
    };
    renderer->appendGBufferDrawList(submitted);
    expectTrue(renderer->gbufferDrawCount() == 2u, "execute path stored both gbuffer draws");
    expectTrue(renderer->gbufferDrawCount() == 2u &&
                   drawMatches(renderer->gbufferDraws()[0], 1u, 2u, 4u, 12u, 2.f),
               "execute path sorts by material before the pass");
    expectTrue(renderer->gbufferDrawCount() == 2u &&
                   drawMatches(renderer->gbufferDraws()[1], 3u, 8u, 1u, 30u, 1.f),
               "execute path keeps the higher material after the lower one");

    const fuse::renderer::RenderGraphExecuteInfo info =
        renderer->executeFrame(*bootstrap->device(), *frames, recorder, 0u);
    expectTrue(info.executedPassCount > 0u, "deferred renderer executed passes");
    expectTrue(recorder.recordCount() > 0u, "command recorder captured deferred work");
    expectTrue(renderer->pipeline().lastStats().gbufferCpuDrawCount == 2u,
               "gbuffer pass read the cpu draw count");

    renderer->destroy();
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    testDeferredTargetOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: testDeferredTargetOk\n");
        return EXIT_FAILURE;
    }

    testGbufferSlotOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: testGbufferSlotOk\n");
        return EXIT_FAILURE;
    }

    fuse::core::initialize();

    testGBufferCpuDrawList();
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
