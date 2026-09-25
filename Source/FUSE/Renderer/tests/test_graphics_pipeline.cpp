#include <fuse/core/init.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/draw_list.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/shader/shader_io.hpp>
#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/shader/shader_watch.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/graphics_pipeline.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/renderer/vk/composite_gpu_path.hpp>
#include <fuse/renderer/vk/raster_path.hpp>
#include <fuse/renderer/vk/render_pass.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

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

std::filesystem::path uniqueTempSpvPath(const char* tag) {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(::getpid());
#endif
    static int seq = 0;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("fuse_shader_reload_" + std::string(tag) + "_" + std::to_string(pid) + "_" +
            std::to_string(stamp) + "_" + std::to_string(++seq) + ".spv");
}

bool copyBinaryFile(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::ifstream in(from, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << in.rdbuf();
    out.flush();
    return static_cast<bool>(out);
}

void touchExistingFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    std::error_code ec;
    std::filesystem::last_write_time(
        path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2), ec);
}

void testGraphicsPipelineFromFixtures() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for graphics pipeline tests");

    fuse::renderer::VulkanDevice* device = bootstrap->device();
    if (device == nullptr) {
        expectTrue(!bootstrap->status().deviceReady, "device unavailable without Vulkan loader");
        return;
    }

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
        expectTrue(!graphicsPipeline->info().blendEnabled,
                   "default pipeline blendEnabled is false");
        expectTrue(graphicsPipeline->info().vertexStrideBytes == 12u,
                   "default pipeline vertexStrideBytes is 12");
        expectTrue(graphicsPipeline->info().topology == 3u,
                   "default pipeline topology is TRIANGLE_LIST (3)");
        expectTrue(!graphicsPipeline->info().depthBiasEnabled,
                   "default pipeline depthBiasEnabled is false");
        expectTrue(!graphicsPipeline->info().stencilEnabled,
                   "default pipeline stencilEnabled is false");
        expectTrue(graphicsPipeline->rebuild(), "graphics pipeline rebuild succeeds");
        expectTrue(graphicsPipeline->isValid(), "graphics pipeline valid after rebuild");
        expectTrue(graphicsPipeline->nativeHandle() != nullptr,
                   "graphics pipeline has native handle after rebuild");
        expectTrue(!graphicsPipeline->info().blendEnabled,
                   "default pipeline blendEnabled stays false after rebuild");
        expectTrue(graphicsPipeline->info().vertexStrideBytes == 12u,
                   "default pipeline vertexStrideBytes stays 12 after rebuild");
        expectTrue(graphicsPipeline->info().topology == 3u,
                   "default pipeline topology stays TRIANGLE_LIST after rebuild");
    } else {
        expectTrue(!graphicsPipeline->isValid(), "graphics pipeline invalid without ICD");
        expectTrue(!graphicsPipeline->rebuild(), "rebuild returns false without valid device");
    }
#else
    expectTrue(graphicsPipeline->isValid(), "graphics pipeline valid in stub backend");
    expectTrue(graphicsPipeline->nativeHandle() == nullptr, "stub backend has no native handle");
    expectTrue(!graphicsPipeline->info().blendEnabled, "stub default pipeline blendEnabled is false");
    expectTrue(graphicsPipeline->info().vertexStrideBytes == 12u,
               "stub default pipeline vertexStrideBytes is 12");
    expectTrue(graphicsPipeline->info().topology == 3u,
               "stub default pipeline topology is TRIANGLE_LIST (3)");
    expectTrue(!graphicsPipeline->info().depthBiasEnabled,
               "stub default pipeline depthBiasEnabled is false");
    expectTrue(!graphicsPipeline->info().stencilEnabled,
               "stub default pipeline stencilEnabled is false");
    expectTrue(graphicsPipeline->rebuild(), "stub graphics pipeline rebuild succeeds");
    expectTrue(graphicsPipeline->isValid(), "stub graphics pipeline valid after rebuild");
#endif

    pipelineDesc.cullMode = 2u; // VK_CULL_MODE_BACK_BIT
    auto culledPipeline = fuse::renderer::GraphicsPipeline::create(*device, pipelineDesc);
    expectTrue(culledPipeline != nullptr, "graphics pipeline allocated with back-face cull");
#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        expectTrue(culledPipeline->isValid(), "graphics pipeline valid with cullMode BACK_BIT");
        expectTrue(culledPipeline->nativeHandle() != nullptr,
                   "cullMode BACK_BIT pipeline has native handle");
    } else {
        expectTrue(!culledPipeline->isValid(), "cullMode BACK_BIT pipeline invalid without ICD");
    }
#else
    expectTrue(culledPipeline->isValid(), "cullMode BACK_BIT pipeline valid in stub backend");
#endif

    pipelineDesc.blendEnable = true;
    auto blendedPipeline = fuse::renderer::GraphicsPipeline::create(*device, pipelineDesc);
    expectTrue(blendedPipeline != nullptr, "graphics pipeline allocated with blendEnable");
#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        expectTrue(blendedPipeline->isValid(), "graphics pipeline valid with blendEnable");
        if (blendedPipeline->isValid()) {
            expectTrue(blendedPipeline->info().blendEnabled,
                       "info.blendEnabled is true when blend pipeline is valid");
            expectTrue(blendedPipeline->info().vertexStrideBytes == 12u,
                       "blend pipeline records vertexStrideBytes");
        }
    } else {
        expectTrue(!blendedPipeline->isValid(), "blendEnable pipeline invalid without ICD");
    }
#else
    expectTrue(blendedPipeline->isValid(), "blendEnable pipeline valid in stub backend");
    expectTrue(blendedPipeline->info().blendEnabled, "stub info.blendEnabled is true when valid");
    expectTrue(blendedPipeline->info().vertexStrideBytes == 12u,
               "stub blend pipeline records vertexStrideBytes");
#endif

    pipelineDesc.depthBiasEnable = true;
    auto biasedPipeline = fuse::renderer::GraphicsPipeline::create(*device, pipelineDesc);
    expectTrue(biasedPipeline != nullptr, "graphics pipeline allocated with depthBiasEnable");
#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        if (biasedPipeline->isValid()) {
            expectTrue(biasedPipeline->info().depthBiasEnabled,
                       "info.depthBiasEnabled is true when bias pipeline is valid");
            expectTrue(biasedPipeline->info().topology == 3u,
                       "bias pipeline records default TRIANGLE_LIST topology");
        }
    } else {
        expectTrue(!biasedPipeline->isValid(), "depthBiasEnable pipeline invalid without ICD");
    }
#else
    expectTrue(biasedPipeline->isValid(), "depthBiasEnable pipeline valid in stub backend");
    expectTrue(biasedPipeline->info().depthBiasEnabled,
               "stub info.depthBiasEnabled is true when valid");
    expectTrue(biasedPipeline->info().topology == 3u,
               "stub bias pipeline records default TRIANGLE_LIST topology");
#endif

    pipelineDesc.stencilTest = true;
    pipelineDesc.stencilFailOp = 2u;  // VK_STENCIL_OP_REPLACE
    pipelineDesc.stencilPassOp = 2u;
    pipelineDesc.stencilCompareOp = 1u; // VK_COMPARE_OP_LESS
    auto stencilPipeline = fuse::renderer::GraphicsPipeline::create(*device, pipelineDesc);
    expectTrue(stencilPipeline != nullptr, "graphics pipeline allocated with stencilTest");
#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        if (stencilPipeline->isValid()) {
            expectTrue(stencilPipeline->info().stencilEnabled,
                       "info.stencilEnabled is true when stencil pipeline is valid");
        }
    } else {
        expectTrue(!stencilPipeline->isValid(), "stencilTest pipeline invalid without ICD");
    }
#else
    expectTrue(stencilPipeline->isValid(), "stencilTest pipeline valid in stub backend");
    expectTrue(stencilPipeline->info().stencilEnabled,
               "stub info.stencilEnabled is true when valid");
#endif
}

void testRasterPathClearTriangle() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for raster path tests");

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
    expectTrue(rasterPath != nullptr, "raster path allocated");
    if (rasterPath->isReady()) {
        expectTrue(rasterPath->lastStats().pipelineContentHash != 0,
                   "raster path pipelineContentHash set when ready");
#if defined(FUSE_VULKAN_BACKEND)
        if (bootstrap->status().deviceReady) {
            expectTrue(rasterPath->indexBufferHandle() != nullptr,
                       "raster path indexBufferHandle set when ready");
            expectTrue(rasterPath->lastStats().indexBufferReady,
                       "raster path indexBufferReady when ready");
            expectTrue(rasterPath->lastStats().depthAttachmentReady,
                       "raster path depthAttachmentReady when ready");
            expectTrue(rasterPath->depthImageHandle() != nullptr,
                       "raster path depthImageHandle set when ready");
            expectTrue(rasterPath->depthViewHandle() != nullptr,
                       "raster path depthViewHandle set when ready");
            const fuse::renderer::VkFrameEncodeContext encode = rasterPath->vulkanEncodeContext();
            expectTrue(encode.active, "raster path vulkanEncodeContext.active with depth");
            expectTrue(encode.depthImage != nullptr, "encode context depthImage set when ready");
            const fuse::renderer::RasterPathStats& bdaStats = rasterPath->lastStats();
            if (device->info().bufferDeviceAddress) {
                // NVIDIA/AMD typically return a non-zero address. Lavapipe may return 0 — skip, don't fail.
                if (bdaStats.vertexDeviceAddress != 0) {
                    expectTrue(bdaStats.bufferDeviceAddressReady,
                               "raster path bufferDeviceAddressReady when vertexDeviceAddress is set");
                }
            } else {
                expectTrue(bdaStats.vertexDeviceAddress == 0,
                           "raster path vertexDeviceAddress is 0 without bufferDeviceAddress");
                expectTrue(bdaStats.indexDeviceAddress == 0,
                           "raster path indexDeviceAddress is 0 without bufferDeviceAddress");
                expectTrue(!bdaStats.bufferDeviceAddressReady,
                           "raster path bufferDeviceAddressReady is false without bufferDeviceAddress");
            }
        }
#endif
    }
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
        expectTrue(stats.pipelineReloadCount == 0u, "no pipeline reload without shader change");
    } else {
        expectTrue(!rasterPath->isReady(), "raster path not ready without ICD");
    }
#else
    expectTrue(rasterPath->isReady(), "raster path ready in stub backend");
    (void)rasterPath->lastStats().depthAttachmentReady;
    (void)rasterPath->vulkanEncodeContext();
    (void)rasterPath->depthImageHandle();
    (void)rasterPath->depthViewHandle();

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.1f, 0.2f, 0.3f);
    expectTrue(rasterPath->recordFrame(commands), "raster path records clear + triangle");

    const fuse::renderer::RasterPathStats& stats = rasterPath->lastStats();
    expectTrue(stats.clearCount == 1u, "one clear command mirrored");
    expectTrue(stats.triangleDrawCount == 1u, "triangle draw issued");
    expectTrue(stats.framesRecorded == 1u, "one frame recorded");
    expectTrue(stats.pipelineReloadCount == 0u, "no pipeline reload without shader change");
    expectTrue(stats.vertexDeviceAddress == 0,
               "stub raster path vertexDeviceAddress is 0");
    expectTrue(stats.indexDeviceAddress == 0,
               "stub raster path indexDeviceAddress is 0");
    expectTrue(!stats.bufferDeviceAddressReady,
               "stub raster path bufferDeviceAddressReady is false");
#endif
}

void testDynamicRenderingPipeline() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for dynamic rendering pipeline tests");

    fuse::renderer::VulkanDevice* device = bootstrap->device();
    if (device == nullptr) {
        expectTrue(!bootstrap->status().deviceReady, "device unavailable without Vulkan loader");
        return;
    }

#if defined(FUSE_VULKAN_BACKEND)
    if (bootstrap->status().deviceReady) {
        bool hasDyn = device->info().dynamicRendering;
        for (const char* e : device->info().enabledExtensions) {
            if (e && std::strcmp(e, "VK_KHR_dynamic_rendering") == 0) {
                hasDyn = true;
            }
        }
        if (!hasDyn) {
            std::printf("SKIP: dynamic rendering not advertised by device\n");
            return;
        }
    } else {
        return;
    }
#endif

    const std::string vertPath = fixturePath("minimal.vert.spv");
    const std::string fragPath = fixturePath("minimal.frag.spv");

    auto vertModule =
        fuse::renderer::ShaderModule::createFromFile(*device, fuse::renderer::ShaderStage::Vertex,
                                                     vertPath.c_str());
    auto fragModule =
        fuse::renderer::ShaderModule::createFromFile(*device, fuse::renderer::ShaderStage::Fragment,
                                                     fragPath.c_str());
    expectTrue(vertModule != nullptr && fragModule != nullptr,
               "fixture shader modules allocated for dynamic rendering");

    auto pipelineLayout = fuse::renderer::PipelineLayout::create(*device);
    expectTrue(pipelineLayout != nullptr && pipelineLayout->isValid(),
               "pipeline layout created for dynamic rendering");

    fuse::renderer::GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.layout = pipelineLayout.get();
    pipelineDesc.vertexShader = vertModule.get();
    pipelineDesc.fragmentShader = fragModule.get();
    pipelineDesc.useDynamicRendering = true;
    pipelineDesc.debugName = "test_dynamic_rendering_pipeline";

    auto graphicsPipeline = fuse::renderer::GraphicsPipeline::create(*device, pipelineDesc);
    expectTrue(graphicsPipeline != nullptr, "dynamic rendering pipeline allocated");

#if defined(FUSE_VULKAN_BACKEND)
    if (device->info().dynamicRendering) {
        expectTrue(graphicsPipeline->isValid(),
                   "dynamic rendering pipeline valid when device.info().dynamicRendering");
        expectTrue(graphicsPipeline->info().dynamicRendering,
                   "pipeline info.dynamicRendering is true");
        expectTrue(graphicsPipeline->nativeHandle() != nullptr,
                   "dynamic rendering pipeline has native handle");
    } else if (!graphicsPipeline->isValid()) {
        std::printf("SKIP: ICD may lack dynamic rendering feature\n");
        return;
    } else {
        expectTrue(graphicsPipeline->info().dynamicRendering,
                   "pipeline info.dynamicRendering is true");
    }
#else
    expectTrue(graphicsPipeline->isValid(), "dynamic rendering pipeline valid in stub backend");
    expectTrue(graphicsPipeline->info().dynamicRendering,
               "stub pipeline info.dynamicRendering is true");
    expectTrue(graphicsPipeline->nativeHandle() == nullptr, "stub backend has no native handle");
#endif
}

void testDynamicRenderingDepthPipeline() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr,
               "bootstrap allocated for dynamic rendering depth pipeline tests");

    fuse::renderer::VulkanDevice* device = bootstrap->device();
    if (device == nullptr) {
        expectTrue(!bootstrap->status().deviceReady, "device unavailable without Vulkan loader");
        return;
    }

    const std::string vertPath = fixturePath("minimal.vert.spv");
    const std::string fragPath = fixturePath("minimal.frag.spv");

    auto vertModule =
        fuse::renderer::ShaderModule::createFromFile(*device, fuse::renderer::ShaderStage::Vertex,
                                                     vertPath.c_str());
    auto fragModule =
        fuse::renderer::ShaderModule::createFromFile(*device, fuse::renderer::ShaderStage::Fragment,
                                                     fragPath.c_str());
    expectTrue(vertModule != nullptr && fragModule != nullptr,
               "fixture shader modules allocated for dynamic rendering depth");

    auto pipelineLayout = fuse::renderer::PipelineLayout::create(*device);
    expectTrue(pipelineLayout != nullptr && pipelineLayout->isValid(),
               "pipeline layout created for dynamic rendering depth");

    fuse::renderer::GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.layout = pipelineLayout.get();
    pipelineDesc.vertexShader = vertModule.get();
    pipelineDesc.fragmentShader = fragModule.get();
    pipelineDesc.useDynamicRendering = true;
    pipelineDesc.depthFormat = 126u; // VK_FORMAT_D32_SFLOAT
    pipelineDesc.debugName = "test_dynamic_rendering_depth_pipeline";

    auto graphicsPipeline = fuse::renderer::GraphicsPipeline::create(*device, pipelineDesc);
    expectTrue(graphicsPipeline != nullptr, "dynamic rendering depth pipeline allocated");

#if defined(FUSE_VULKAN_BACKEND)
    if (device->info().dynamicRendering) {
        expectTrue(graphicsPipeline->isValid(),
                   "dynamic rendering depth pipeline valid when device.info().dynamicRendering");
        expectTrue(graphicsPipeline->info().dynamicRendering,
                   "depth pipeline info.dynamicRendering is true");
        expectTrue(graphicsPipeline->info().hasDynamicDepth,
                   "depth pipeline info.hasDynamicDepth is true");
        expectTrue(graphicsPipeline->info().depthFormat == 126u,
                   "depth pipeline info.depthFormat is D32_SFLOAT (126)");
        expectTrue(graphicsPipeline->nativeHandle() != nullptr,
                   "dynamic rendering depth pipeline has native handle");
    } else if (!graphicsPipeline->isValid()) {
        std::printf("SKIP: dynamic rendering depth create failed without device.dynamicRendering\n");
        return;
    } else {
        expectTrue(graphicsPipeline->isValid(),
                   "dynamic rendering depth pipeline isValid when create succeeded");
        expectTrue(graphicsPipeline->info().hasDynamicDepth,
                   "honest hasDynamicDepth when create succeeded with depthFormat=126");
        expectTrue(graphicsPipeline->info().depthFormat == 126u,
                   "honest depthFormat when create succeeded with depthFormat=126");
    }
#else
    expectTrue(graphicsPipeline->isValid(),
               "dynamic rendering depth pipeline valid in stub backend");
    expectTrue(graphicsPipeline->info().hasDynamicDepth,
               "stub depth pipeline info.hasDynamicDepth is true");
    expectTrue(graphicsPipeline->info().depthFormat == 126u,
               "stub depth pipeline info.depthFormat is D32_SFLOAT (126)");
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
    if (submitted) {
        expectTrue(context->timestampWriteCount() >= 0u,
                   "timestampWriteCount is non-negative after submitFrame");
        if (context->timestampsReady()) {
            expectTrue(context->timestampWriteCount() > 0u,
                       "submitFrame writes GPU timestamps when timestampsReady");
        }
        (void)context->lastGpuTimeNs();
    }
    if (context->bootstrap().status().deviceReady) {
        expectTrue(context->rasterPath() != nullptr, "raster path created lazily");
        expectTrue(context->lastRasterStats().triangleDrawCount == 1u,
                   "RHI context wired clear + triangle path");
        if (context->commandRecorder().vulkanRenderPassBeginCount() >= 1u) {
            expectTrue(context->commandRecorder().vulkanViewportCount() >= 1u,
                       "viewport encoded when a real render pass began");
            expectTrue(context->commandRecorder().vulkanScissorCount() >= 1u,
                       "scissor encoded when a real render pass began");
        }
    }

    fuse::renderer::DrawList draws;
    fuse::renderer::DrawCall call{};
    call.indexCount = 3;
    expectTrue(draws.push(call), "DrawList accepts one indexed call");
    expectTrue(context->beginFrame(1u), "beginFrame accepted for DrawList submit");
    const bool drawSubmitted = context->submitDrawList(draws, 1u);
    if (drawSubmitted) {
        expectTrue(context->lastDrawListCount() == 1u, "lastDrawListCount records one draw");
        expectTrue(context->timestampWriteCount() >= 0u,
                   "timestampWriteCount is non-negative after submitDrawList");
        if (context->timestampsReady()) {
            expectTrue(context->timestampWriteCount() > 0u,
                       "submitDrawList writes GPU timestamps when timestampsReady");
        }
        (void)context->lastGpuTimeNs();
        if (context->compositeGpuPath() != nullptr && context->compositeGpuPath()->isReady() &&
            context->rasterPath() != nullptr && context->rasterPath()->isReady()) {
            expectTrue(context->lastCompositeGpuStats().depthTextureBound,
                       "submitDrawList registers raster depth into composite bindless");
        }
    }
#else
    expectTrue(!submitted, "stub mode rejects GPU submit");

    fuse::renderer::DrawList draws;
    fuse::renderer::DrawCall call{};
    call.indexCount = 3;
    expectTrue(draws.push(call), "DrawList accepts one indexed call");
    expectTrue(!context->submitDrawList(draws, 0u), "stub mode rejects GPU DrawList submit");

    fuse::renderer::VulkanDevice* device = context->bootstrap().device();
    if (device != nullptr) {
        const std::string compositeVertPath = fixturePath("composite.vert.spv");
        const std::string compositeFragPath = fixturePath("composite.frag.spv");
        fuse::renderer::CompositeGpuPathDesc compositeDesc{};
        compositeDesc.vertexSpirvPath = compositeVertPath.c_str();
        compositeDesc.fragmentSpirvPath = compositeFragPath.c_str();
        auto compositePath = fuse::renderer::CompositeGpuPath::create(*device, compositeDesc);
        expectTrue(compositePath != nullptr, "stub composite gpu path allocated");
        expectTrue(!compositePath->registerRasterDepth(nullptr),
                   "stub registerRasterDepth returns false");
        expectTrue(!compositePath->lastStats().depthTextureBound,
                   "stub depthTextureBound stays false");
    }
#endif
}

void testRhiContextComputePipeline() {
    fuse::renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;
    desc.bootstrap.createSwapchain = false;

    auto context = fuse::renderer::RhiContext::create(desc);
    expectTrue(context != nullptr, "RHI context allocated for compute pipeline");

    const fuse::renderer::ComputePipeline* pipeline = context->computePipeline();
    fuse::renderer::VulkanDevice* device = context->bootstrap().device();
    if (device != nullptr && device->isValid()) {
        if (pipeline != nullptr) {
            expectTrue(pipeline->isValid(), "compute pipeline isValid when device is valid");
        }
    } else {
        expectTrue(pipeline == nullptr || !pipeline->isValid(),
                   "compute pipeline is null or invalid without a valid device");
    }
}

void testShaderModuleReloadFromDisk() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for shader reload tests");

    fuse::renderer::VulkanDevice* device = bootstrap->device();
    if (device == nullptr) {
        expectTrue(!bootstrap->status().deviceReady, "device unavailable without Vulkan loader");
        return;
    }

    const std::string vertPath = fixturePath("minimal.vert.spv");
    std::string loadError;
    const std::vector<fuse::u32> words =
        fuse::renderer::loadSpirvFile(vertPath.c_str(), &loadError);
    expectTrue(!words.empty(), "fixture SPIR-V loads for reloadFromDisk test");
    if (words.empty()) {
        return;
    }

    auto memoryModule = fuse::renderer::ShaderModule::create(
        *device, fuse::renderer::ShaderStage::Vertex, words.data(),
        static_cast<fuse::u32>(words.size()));
    expectTrue(memoryModule != nullptr, "in-memory shader module allocated");
    expectTrue(!memoryModule->reloadFromDisk(), "create() without a file path cannot reloadFromDisk");

    auto fileModule = fuse::renderer::ShaderModule::createFromFile(
        *device, fuse::renderer::ShaderStage::Vertex, vertPath.c_str());
    expectTrue(fileModule != nullptr, "createFromFile shader module allocated");
    if (fileModule->isValid()) {
        const fuse::u64 hash = fileModule->info().spirvHash;
        expectTrue(fileModule->reloadFromDisk(), "reloadFromDisk succeeds on fixture SPIR-V");
        expectTrue(fileModule->isValid(), "shader module remains valid after reloadFromDisk");
        expectTrue(fileModule->info().spirvHash == hash, "reloadFromDisk preserves fixture spirvHash");
    } else {
        expectTrue(!fileModule->reloadFromDisk(),
                   "reloadFromDisk fails without a valid device/module");
    }
}

void testRasterPathHotReload() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for raster hot-reload tests");

    fuse::renderer::VulkanDevice* device = bootstrap->device();
    if (device == nullptr) {
        expectTrue(!bootstrap->status().deviceReady, "device unavailable without Vulkan loader");
        return;
    }

    const std::filesystem::path tempVert = uniqueTempSpvPath("vert");
    const std::filesystem::path tempFrag = uniqueTempSpvPath("frag");
    expectTrue(copyBinaryFile(fixturePath("minimal.vert.spv"), tempVert),
               "temp vertex SPIR-V copied");
    expectTrue(copyBinaryFile(fixturePath("minimal.frag.spv"), tempFrag),
               "temp fragment SPIR-V copied");

    const std::string vertUtf8 = tempVert.string();
    const std::string fragUtf8 = tempFrag.string();
    fuse::renderer::RasterPathDesc rasterDesc{};
    rasterDesc.vertexSpirvPath = vertUtf8.c_str();
    rasterDesc.fragmentSpirvPath = fragUtf8.c_str();

    auto rasterPath = fuse::renderer::RasterPath::create(*device, rasterDesc);
    expectTrue(rasterPath != nullptr, "raster path allocated for hot-reload");

    fuse::renderer::ShaderFileWatch probe;
    expectTrue(probe.watch(vertUtf8.c_str()), "probe watch records temp vertex SPIR-V");

    fuse::renderer::RenderCommandList commands;
    commands.clear3D(0.1f, 0.2f, 0.3f);
    rasterPath->recordFrame(commands);
    rasterPath->recordFrame(commands);
    expectTrue(rasterPath->lastStats().pipelineReloadCount == 0u,
               "recordFrame twice without file change keeps pipelineReloadCount at 0");

    if (rasterPath->isReady()) {
        touchExistingFile(tempVert);
        const fuse::u32 probeChanged = probe.pollChanged();
        rasterPath->recordFrame(commands);
        if (probeChanged == 0u) {
            std::printf("SKIP: shader watch did not observe mtime change\n");
        } else {
            expectTrue(rasterPath->lastStats().pipelineReloadCount >= 1u,
                       "watched SPIR-V mtime change rebuilds the graphics pipeline");
        }
    }

    std::error_code ec;
    std::filesystem::remove(tempVert, ec);
    std::filesystem::remove(tempFrag, ec);
}

void testRasterPathResize() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for raster resize tests");

    fuse::renderer::VulkanDevice* device = bootstrap->device();
    if (device == nullptr) {
        expectTrue(!bootstrap->status().deviceReady, "device unavailable without Vulkan loader");
        return;
    }

    fuse::renderer::RasterPathDesc unreadyDesc{};
    auto unreadyPath = fuse::renderer::RasterPath::create(*device, unreadyDesc);
    expectTrue(unreadyPath != nullptr, "unready raster path allocated");
    expectTrue(!unreadyPath->isReady(), "missing SPIR-V leaves raster path unready");
    expectTrue(!unreadyPath->resize(64, 48), "unready raster path resize returns false");
    expectTrue(unreadyPath->lastStats().resizeCount == 0u,
               "unready resize does not increment resizeCount");
    (void)unreadyPath->vulkanEncodeContext();

    const std::string rasterVertPath = fixturePath("minimal.vert.spv");
    const std::string rasterFragPath = fixturePath("minimal.frag.spv");
    fuse::renderer::RasterPathDesc rasterDesc{};
    rasterDesc.vertexSpirvPath = rasterVertPath.c_str();
    rasterDesc.fragmentSpirvPath = rasterFragPath.c_str();

    auto rasterPath = fuse::renderer::RasterPath::create(*device, rasterDesc);
    expectTrue(rasterPath != nullptr, "raster path allocated for resize");

    if (rasterPath->isReady()) {
        expectTrue(!rasterPath->resize(0, 48), "zero width resize rejected");
        expectTrue(!rasterPath->resize(64, 0), "zero height resize rejected");
        expectTrue(rasterPath->lastStats().resizeCount == 0u,
                   "rejected resize does not increment resizeCount");

        expectTrue(rasterPath->resize(64, 48), "resize succeeds when ready");
        expectTrue(rasterPath->lastStats().resizeCount == 1u,
                   "resizeCount increments on actual recreate");
        expectTrue(rasterPath->lastStats().pipelineReady, "pipeline stays ready after resize");

        const fuse::renderer::VkFrameEncodeContext encode = rasterPath->vulkanEncodeContext();
#if defined(FUSE_VULKAN_BACKEND)
        if (bootstrap->status().deviceReady) {
            expectTrue(encode.width == 64u, "encode context width matches resize");
            expectTrue(encode.height == 48u, "encode context height matches resize");
            expectTrue(encode.active, "encode context stays active after resize");
            expectTrue(rasterPath->lastStats().depthAttachmentReady,
                       "depth attachment ready after resize");
            expectTrue(rasterPath->depthImageHandle() != nullptr,
                       "depth image handle set after resize");
            expectTrue(rasterPath->indexBufferHandle() != nullptr,
                       "index buffer survives resize");
            expectTrue(rasterPath->lastStats().indexBufferReady,
                       "indexBufferReady survives resize");
        }
#else
        (void)encode;
#endif

        expectTrue(rasterPath->resize(64, 48), "same-size resize is success no-op");
        expectTrue(rasterPath->lastStats().resizeCount == 1u,
                   "same-size resize does not increment resizeCount");
        expectTrue(rasterPath->lastStats().resizeNoOpCount == 1u,
                   "same-size resize increments resizeNoOpCount");
    } else {
        expectTrue(!rasterPath->resize(64, 48), "stub/unready raster path resize returns false");
        expectTrue(rasterPath->lastStats().resizeCount == 0u,
                   "stub/unready resize does not increment resizeCount");
        (void)rasterPath->vulkanEncodeContext();
    }
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
#endif
}

} // namespace

int main() {
    fuse::core::initialize();

    testGraphicsPipelineFromFixtures();
    testDynamicRenderingPipeline();
    testDynamicRenderingDepthPipeline();
    testRasterPathClearTriangle();
    testRasterPathResize();
    testShaderModuleReloadFromDisk();
    testRasterPathHotReload();
    testRhiContextWiresRasterPath();
    testRhiContextComputePipeline();
    testCompositeGpuPathRegisterRasterDepth();

    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_graphics_pipeline: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_graphics_pipeline: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
