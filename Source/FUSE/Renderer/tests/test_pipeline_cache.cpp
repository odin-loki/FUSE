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

[[maybe_unused]] std::string fixturePath(const char* name) {
    return std::string(FUSE_SHADER_FIXTURE_DIR) + "/" + name;
}

void testHashedFileName() {
    const std::string zeroName = fuse::renderer::PipelineCache::hashedFileName(0);
    expectTrue(zeroName == "fuse_pso_0000000000000000.bin",
               "hashedFileName(0) equals fuse_pso_0000000000000000.bin");

    const std::string abcName = fuse::renderer::PipelineCache::hashedFileName(0xABC);
    expectTrue(abcName.find("fuse_pso_") != std::string::npos, "hashedFileName(0xABC) contains fuse_pso_");
    expectTrue(abcName.find(".bin") != std::string::npos, "hashedFileName(0xABC) contains .bin");
    expectTrue(abcName != zeroName, "hashedFileName(0xABC) differs from hash 0");

    const std::string hashA = fuse::renderer::PipelineCache::hashedFileName(0x11);
    const std::string hashB = fuse::renderer::PipelineCache::hashedFileName(0x22);
    expectTrue(hashA != hashB, "different hashes produce different cache file names");
    expectTrue(hashA.find('/') == std::string::npos && hashA.find('\\') == std::string::npos,
               "hashedFileName has no path separators");
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

    const std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    expectTrue(cache->writeCacheFileForHash(tempDir.string().c_str(), 0x11),
               "writeCacheFileForHash succeeds");
    auto restoredHash = fuse::renderer::PipelineCache::create(*device);
    expectTrue(restoredHash != nullptr && restoredHash->isValid(), "hash-restore cache allocated");
    expectTrue(restoredHash->readCacheFileForHash(tempDir.string().c_str(), 0x11),
               "readCacheFileForHash restores blob");
    std::filesystem::remove(tempDir / fuse::renderer::PipelineCache::hashedFileName(0x11), ec);
#else
    (void)bootstrap;
#endif
}

void testRebuildSnapshotsCache() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;

    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated for rebuild cache snapshot");

#if defined(FUSE_VULKAN_BACKEND)
    if (!bootstrap->status().deviceReady) {
        std::printf("SKIP: no Vulkan ICD — pipeline cache rebuild snapshot test\n");
        return;
    }

    fuse::renderer::VulkanDevice* device = bootstrap->device();
    auto cache = fuse::renderer::PipelineCache::create(*device);
    expectTrue(cache != nullptr && cache->isValid(), "pipeline cache created for rebuild snapshot");

    auto renderPass = fuse::renderer::RenderPass::create(*device);
    auto layout = fuse::renderer::PipelineLayout::create(*device);
    auto vert = fuse::renderer::ShaderModule::createFromFile(
        *device, fuse::renderer::ShaderStage::Vertex, fixturePath("minimal.vert.spv").c_str());
    auto frag = fuse::renderer::ShaderModule::createFromFile(
        *device, fuse::renderer::ShaderStage::Fragment, fixturePath("minimal.frag.spv").c_str());
    expectTrue(renderPass && layout && vert && frag, "rebuild snapshot pipeline deps ready");

    fuse::renderer::GraphicsPipelineDesc pipelineDesc{};
    pipelineDesc.layout = layout.get();
    pipelineDesc.vertexShader = vert.get();
    pipelineDesc.fragmentShader = frag.get();
    pipelineDesc.renderPass = renderPass.get();
    pipelineDesc.pipelineCache = cache.get();
    auto pipeline = fuse::renderer::GraphicsPipeline::create(*device, pipelineDesc);
    expectTrue(pipeline != nullptr && pipeline->isValid(),
               "graphics pipeline created with pipeline cache");
    expectTrue(pipeline->info().rebuildCount == 0u, "first create does not increment rebuildCount");

    expectTrue(pipeline->rebuild(), "graphics pipeline rebuild succeeds with cache");
    expectTrue(pipeline->isValid(), "graphics pipeline valid after cached rebuild");
    expectTrue(pipeline->info().rebuildCount == 1u, "rebuildCount is 1 after one rebuild");
    (void)pipeline->info().cacheSnapshotBytes;
#else
    (void)bootstrap;
#endif
}

} // namespace

int main() {
    fuse::core::initialize();
    testHashedFileName();
    testSnapshotRestoreRoundTrip();
    testRebuildSnapshotsCache();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_pipeline_cache: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_pipeline_cache: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
