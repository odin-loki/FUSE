#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/command_buffer.hpp>
#include <fuse/renderer/composite_pass.hpp>
#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/composite_gpu_path.hpp>
#include <fuse/renderer/vk/raster_path.hpp>

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
    expectTrue(recorder.vulkanViewportCount() == 0u, "logical-only viewport count stays 0");
    expectTrue(recorder.vulkanScissorCount() == 0u, "logical-only scissor count stays 0");
}

void testRhiContextCompositeStats() {
    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;
    desc.bootstrap.createSwapchain = false;
    desc.composite.defaultBlend = 0.6f;

    auto context = fuse::renderer::RhiContext::create(desc);
    expectTrue(context != nullptr, "RHI context allocated");
    expectTrue(context->compositePass() == nullptr, "composite pass lazy until submit");

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.1f, 0.2f, 0.3f);

#if defined(FUSE_VULKAN_BACKEND)
    if (!context->bootstrap().status().deviceReady) {
        std::printf("SKIP: Vulkan device not ready — headless ICD unavailable\n");
        return;
    }
    expectTrue(context->beginFrame(0u), "beginFrame accepted on render thread");
    expectTrue(context->submitFrame(commands, 0u), "submit accepted when Vulkan device ready");
    expectTrue(context->compositePass() != nullptr, "composite pass created on submit");
    expectTrue(context->lastCompositeStats().framesRecorded >= 1u,
               "composite stats updated after submit");
    expectTrue(context->lastGraphPassCount() >= 3u, "graph includes composite before present");
    if (context->commandRecorder().vulkanCompositeDrawCount() >= 1u) {
        expectTrue(context->commandRecorder().vulkanViewportCount() >= 1u,
                   "viewport set after composite GPU encode");
        expectTrue(context->commandRecorder().vulkanScissorCount() >= 1u,
                   "scissor set after composite GPU encode");
    }
    if (context->compositeGpuPath() != nullptr && context->compositeGpuPath()->isReady() &&
        context->rasterPath() != nullptr && context->rasterPath()->isReady()) {
        expectTrue(context->lastCompositeGpuStats().depthTextureBound,
                   "submitFrame registers raster depth into composite bindless");
        expectTrue(context->lastCompositeGpuStats().depthTextureIndex != UINT32_MAX,
                   "depthTextureIndex assigned after submitFrame depth registration");
    }
#else
    expectTrue(!context->submitFrame(commands, 0u), "stub mode rejects GPU submit");
#endif
}

void testCompositeGpuPathRegisterRasterDepth() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for composite depth bindless");
    fuse::renderer::VulkanDevice* device = bootstrap->device();
    if (device == nullptr) {
        expectTrue(!bootstrap->status().deviceReady, "device unavailable without Vulkan loader");
        return;
    }

    const std::string rasterVertPath = fixturePath("minimal.vert.spv");
    const std::string rasterFragPath = fixturePath("minimal.frag.spv");
    fuse::renderer::RasterPathDesc rasterDesc{};
    rasterDesc.vertexSpirvPath = rasterVertPath.c_str();
    rasterDesc.fragmentSpirvPath = rasterFragPath.c_str();
    auto rasterPath = fuse::renderer::RasterPath::create(*device, rasterDesc);
    expectTrue(rasterPath != nullptr, "raster path allocated for depth bindless");

    const std::string compositeVertPath = fixturePath("composite.vert.spv");
    const std::string compositeFragPath = fixturePath("composite.frag.spv");
    fuse::renderer::CompositeGpuPathDesc compositeDesc{};
    compositeDesc.vertexSpirvPath = compositeVertPath.c_str();
    compositeDesc.fragmentSpirvPath = compositeFragPath.c_str();
    auto compositePath = fuse::renderer::CompositeGpuPath::create(*device, compositeDesc);
    expectTrue(compositePath != nullptr, "composite gpu path allocated for depth bindless");

#if defined(FUSE_VULKAN_BACKEND)
    if (compositePath->isReady() && rasterPath->isReady()) {
        void* depthView = rasterPath->depthViewHandle();
        void* colorView = rasterPath->colorViewHandle();
        void* bindlessView = depthView != nullptr ? depthView : colorView;
        expectTrue(bindlessView != nullptr, "raster path has a view for bindless depth write");
        expectTrue(compositePath->registerRasterDepth(bindlessView),
                   "registerRasterDepth succeeds as a bindless write when ready");
        expectTrue(compositePath->lastStats().depthTextureBound,
                   "depthTextureBound true after successful registerRasterDepth");
        expectTrue(compositePath->lastStats().depthTextureIndex != UINT32_MAX,
                   "depthTextureIndex assigned on successful registerRasterDepth");
        if (colorView != nullptr) {
            expectTrue(compositePath->registerRasterDepth(colorView),
                       "registerRasterDepth(colorView) still succeeds as a bindless write");
            expectTrue(compositePath->lastStats().depthTextureBound,
                       "depthTextureBound remains true after colorView bindless write");
        }

        expectTrue(compositePath->registerRasterSource(colorView),
                   "registerRasterSource succeeds so fillEncodeContext can copy depth index");
        fuse::renderer::VkFrameEncodeContext encode = rasterPath->vulkanEncodeContext();
        compositePath->fillEncodeContext(encode, 0.5f, false);
        expectTrue(encode.depthTextureBindlessIndex == compositePath->lastStats().depthTextureIndex,
                   "fillEncodeContext copies depthTextureBindlessIndex");
    } else {
        expectTrue(!compositePath->registerRasterDepth(nullptr),
                   "unready registerRasterDepth returns false");
        expectTrue(!compositePath->lastStats().depthTextureBound,
                   "unready depthTextureBound stays false");
    }
#else
    expectTrue(!compositePath->registerRasterDepth(rasterPath->colorViewHandle()),
               "stub registerRasterDepth returns false");
    expectTrue(!compositePath->lastStats().depthTextureBound, "stub depthTextureBound stays false");
    expectTrue(compositePath->lastStats().depthTextureIndex == UINT32_MAX,
               "stub depthTextureIndex stays unbound");
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
    testCompositeGpuPathRegisterRasterDepth();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_composite_pass: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_composite_pass: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
