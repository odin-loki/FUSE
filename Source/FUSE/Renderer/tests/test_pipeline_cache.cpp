#include <fuse/core/init.hpp>
#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/graphics_pipeline.hpp>
#include <fuse/renderer/vk/pipeline_cache.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/renderer/vk/render_pass.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

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

void testSnapshotRestoreRoundTrip() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated");

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        std::printf("SKIP: no Vulkan ICD — pipeline cache restore test\n");
        return;
    }

    fuse::renderer::VulkanDevice* device = bootstrap->device();
    auto cache = fuse::renderer::PipelineCache::create(*device);
    expectTrue(cache != nullptr && cache->isValid(), "pipeline cache created");

    auto renderPass = fuse::renderer::RenderPass::create(*device);
    auto layout = fuse::renderer::PipelineLayout::create(*device);
    auto vert = fuse::renderer::ShaderModule::createFromFile(
        *device, fuse::renderer::ShaderStage::Vertex, fixturePath("minimal.vert.spv").c_str());
    auto frag = fuse::renderer::ShaderModule::createFromFile(
        *device, fuse::renderer::ShaderStage::Fragment, fixturePath("minimal.frag.spv").c_str());
    expectTrue(renderPass && layout && vert && frag, "pipeline deps ready");

    fuse::renderer::GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.layout = layout.get();
    pipelineDesc.vertexShader = vert.get();
    pipelineDesc.fragmentShader = frag.get();
    pipelineDesc.renderPass = renderPass.get();
    pipelineDesc.pipelineCache = cache.get();
    auto pipeline = fuse::renderer::GraphicsPipeline::create(*device, pipelineDesc);
    expectTrue(pipeline != nullptr && pipeline->isValid(), "graphics pipeline compiled into cache");

    std::vector<fuse::u8> blob;
    expectTrue(cache->snapshotData(blob), "pipeline cache snapshot succeeds");

    auto restored = fuse::renderer::PipelineCache::create(*device);
    expectTrue(restored != nullptr && restored->isValid(), "second cache allocated");
    expectTrue(restored->restoreFromData(blob), "restoreFromData accepts snapshot blob");

    const std::filesystem::path cachePath =
        std::filesystem::temp_directory_path() / "fuse_pipeline_cache_wp06e.bin";
    expectTrue(cache->writeCacheFile(cachePath.string().c_str()), "writeCacheFile succeeds");
    expectTrue(restored->readCacheFile(cachePath.string().c_str()), "readCacheFile restores blob");
    std::error_code ec;
    std::filesystem::remove(cachePath, ec);
#else
    (void)bootstrap;
#endif
}

} // namespace

int main() {
    fuse::core::initialize();
    testSnapshotRestoreRoundTrip();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_pipeline_cache: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_pipeline_cache: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
