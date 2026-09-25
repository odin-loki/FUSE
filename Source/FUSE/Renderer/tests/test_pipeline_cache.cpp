#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/shader/shader_io.hpp>
#include <fuse/renderer/shader/shader_module.hpp>
#include <fuse/renderer/shader/shader_reflection.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/compute_pipeline.hpp>
#include <fuse/renderer/vk/graphics_pipeline.hpp>
#include <fuse/renderer/vk/pipeline_cache.hpp>
#include <fuse/renderer/vk/pipeline_layout.hpp>
#include <fuse/renderer/vk/render_pass.hpp>

#include "rp_wp05_vk.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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

// ---- pipeline cache v2 (WP-0.5) ------------------------------------------------------------------
//
// Default mode also runs the v2 unit gates below (round trip, corruption recovery, key mismatch,
// driver-rejected blob, atomic writes, background precompile). `--mode cold|warm --dir D` are the
// two halves of the warm-start exit test (ctest fixture: cold writes D, warm requires a 100% hit
// rate from it). Both run with validation + synchronization validation and require 0 messages.

#if defined(FUSE_VULKAN_BACKEND)

using fuse::u32;
using fuse::u64;
using fuse::u8;
using namespace fuse::renderer;

constexpr u64 kFnvOffset = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

u64 fnv(const u8* data, size_t size) {
    u64 hash = kFnvOffset;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= kFnvPrime;
    }
    return hash;
}

void putU64(u8* out, u64 value) {
    for (u32 i = 0; i < 8u; ++i) {
        out[i] = static_cast<u8>(value >> (8u * i));
    }
}

std::vector<u8> readBytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeBytes(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

/// Re-seals a v2 file after a test edited its payload (format: pipeline_cache.hpp / .cpp).
void resealV2(std::vector<u8>& bytes) {
    constexpr size_t kHeader = 128;
    putU64(bytes.data() + 104, fnv(bytes.data() + kHeader, bytes.size() - kHeader));
    putU64(bytes.data() + 120, fnv(bytes.data(), 120));
}

bool noTempFiles(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().filename().string().find(".tmp.") != std::string::npos) {
            return false;
        }
    }
    return true;
}

/// Everything a set of pipelines needs, created once and shared (read-only) by the recipes, which
/// may run concurrently on job workers during background precompile.
struct PipelineKit {
    std::unique_ptr<RenderPass> renderPass;
    std::unique_ptr<PipelineLayout> emptyLayout;
    std::unique_ptr<ShaderModule> vert;
    std::unique_ptr<ShaderModule> frag;
    std::unique_ptr<ShaderModule> comp;
    struct Reflected {
        std::unique_ptr<ShaderModule> module;
        std::unique_ptr<PipelineLayout> layout;
        u32 localSizeX = 1;
    };
    std::vector<Reflected> reflected;
    std::vector<std::vector<u32>> spirv; // for the content hash

    bool create(VulkanDevice& device) {
        renderPass = RenderPass::create(device);
        emptyLayout = PipelineLayout::create(device);
        const char* files[] = {"minimal.vert.spv", "minimal.frag.spv", "minimal.comp.spv"};
        for (const char* file : files) {
            spirv.push_back(loadSpirvFile(fixturePath(file).c_str()));
        }
        vert = ShaderModule::create(device, ShaderStage::Vertex, spirv[0].data(), static_cast<u32>(spirv[0].size()));
        frag = ShaderModule::create(device, ShaderStage::Fragment, spirv[1].data(), static_cast<u32>(spirv[1].size()));
        comp = ShaderModule::create(device, ShaderStage::Compute, spirv[2].data(), static_cast<u32>(spirv[2].size()));
#if defined(FUSE_RP_SLANG_TWIN_BUILT)
        // Slang kernels too: their pipelines warm-start like any other.
        for (const char* path : {FUSE_RP_SLANG_HISTOGRAM_SPV, FUSE_RP_SLANG_FLOAT_SPV}) {
            spirv.push_back(loadSpirvFile(path));
            const std::vector<u32>& words = spirv.back();
            const ShaderReflection reflection = reflectSpirv(words.data(), words.size());
            Reflected r;
            r.module = ShaderModule::create(device, ShaderStage::Compute, words.data(), static_cast<u32>(words.size()));
            r.layout = PipelineLayout::createFromReflection(device, reflection);
            r.localSizeX = reflection.localSize[0];
            if (!r.module->isValid() || !r.layout->isValid()) {
                return false;
            }
            reflected.push_back(std::move(r));
        }
#endif
        return renderPass->isValid() && emptyLayout->isValid() && vert->isValid() && frag->isValid() &&
               comp->isValid();
    }

    u64 contentHash() const {
        std::vector<const std::vector<u32>*> modules;
        for (const auto& words : spirv) {
            modules.push_back(&words);
        }
        return hashShaderContent(modules);
    }

    u32 recipeCount() const { return 3u + static_cast<u32>(reflected.size()); }

    /// Builds pipeline `index` through `cache`; returns its manifest key (0 on failure).
    u64 build(VulkanDevice& device, PipelineCache& cache, u32 index) const {
        if (index < 2u) {
            GraphicsPipelineDesc desc{};
            desc.layout = emptyLayout.get();
            desc.vertexShader = vert.get();
            desc.fragmentShader = frag.get();
            desc.renderPass = renderPass.get();
            desc.pipelineCache = &cache;
            desc.blendEnable = index == 1u;
            auto pipeline = GraphicsPipeline::create(device, desc);
            return pipeline->isValid() ? pipeline->info().cache.key : 0u;
        }
        ComputePipelineDesc desc{};
        desc.pipelineCache = &cache;
        if (index == 2u) {
            desc.layout = emptyLayout.get();
            desc.computeShader = comp.get();
        } else {
            const Reflected& r = reflected[index - 3u];
            desc.layout = r.layout.get();
            desc.computeShader = r.module.get();
            desc.localSizeX = r.localSizeX;
        }
        auto pipeline = ComputePipeline::create(device, desc);
        return pipeline->isValid() ? pipeline->info().cache.key : 0u;
    }
};

void printStats(const char* label, const PipelineCacheStats& st) {
    std::printf("%s: lookups %u, warm hits %u, misses %u (warm hit rate %.0f%%); driver feedback %u, driver "
                "hits %u; precompile %u scheduled / %u ok / %u failed\n",
                label, st.lookups, st.warmHits, st.misses, st.warmHitRate() * 100.0, st.feedbackReports,
                st.driverHits, st.precompileScheduled, st.precompileSucceeded, st.precompileFailed);
}

void testV2Gates() {
    rp_wp05::Context ctx;
    const int setup = rp_wp05::setupContext(ctx, "fuse_pipeline_cache_v2");
    if (setup != 0) {
        expectTrue(setup == rp_wp05::kSkip, "v2 context setup");
        std::printf("SKIP: pipeline cache v2 gates (no validation layer / device)\n");
        return;
    }
    VulkanDevice& device = *ctx.device;
    PipelineKit kit;
    expectTrue(kit.create(device), "v2: pipeline kit");

    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fuse_pipeline_cache_v2_unit";
    std::filesystem::remove_all(dir, ec);
    const u64 content = kit.contentHash();

    // Key: device identity + content.
    auto cache = PipelineCache::create(device);
    const PipelineCacheKey key = cache->makeKey(content);
    expectTrue(key.contentHash == content && key.vendorID == cache->makeKey(0).vendorID, "v2: key carries content");
    expectTrue(PipelineCache::fileNameV2(key) != PipelineCache::fileNameV2(cache->makeKey(content + 1)),
               "v2: content hash selects the file");
    bool uuidSet = false;
    for (u8 b : key.pipelineCacheUUID) {
        uuidSet = uuidSet || b != 0u;
    }
    expectTrue(uuidSet, "v2: key holds the pipelineCacheUUID");
    expectTrue(cache->creationFeedback(), "v2: creation feedback available on a 1.3 device");

    // Cold: missing, build, save.
    PipelineCacheLoadResult load = cache->loadFromDirectory(dir.string().c_str(), content);
    expectTrue(load.status == PipelineCacheLoadStatus::Missing, "v2: cold start reports Missing");
    for (u32 i = 0; i < kit.recipeCount(); ++i) {
        expectTrue(kit.build(device, *cache, i) != 0u, "v2: pipeline built through the cache");
    }
    expectTrue(cache->stats().warmHits == 0u && cache->stats().lookups == kit.recipeCount(), "v2: cold run all misses");
    std::string error;
    expectTrue(cache->saveToDirectory(dir.string().c_str(), content, &error), "v2: save");
    expectTrue(noTempFiles(dir), "v2: atomic save leaves no temp files");
    const std::filesystem::path file = dir / PipelineCache::fileNameV2(key);
    expectTrue(std::filesystem::exists(file), "v2: file written under the keyed name");

    // Round trip.
    auto warm = PipelineCache::create(device);
    load = warm->loadFromDirectory(dir.string().c_str(), content);
    expectTrue(load.status == PipelineCacheLoadStatus::Loaded, "v2: warm load");
    expectTrue(load.manifestCount == kit.recipeCount(), "v2: manifest restored");
    for (u32 i = 0; i < kit.recipeCount(); ++i) {
        kit.build(device, *warm, i);
    }
    expectTrue(warm->stats().warmHits == kit.recipeCount() && warm->stats().misses == 0u, "v2: all warm hits");

    // Key mismatch: another content hash reading this file is refused and the file is untouched.
    auto other = PipelineCache::create(device);
    load = other->loadFileV2(file.string().c_str(), other->makeKey(content ^ 0x5555u));
    expectTrue(load.status == PipelineCacheLoadStatus::KeyMismatch, "v2: key mismatch detected");
    expectTrue(std::filesystem::exists(file), "v2: key mismatch leaves the file alone");

    const std::vector<u8> good = readBytes(file);
    auto expectCorrupt = [&](std::vector<u8> bytes, const char* what) {
        writeBytes(file, bytes);
        auto c = PipelineCache::create(device);
        const PipelineCacheLoadResult r = c->loadFileV2(file.string().c_str(), key);
        const bool movedAside = !std::filesystem::exists(file) && std::filesystem::exists(file.string() + ".corrupt");
        if (r.status != PipelineCacheLoadStatus::Corrupt || !movedAside || !c->isValid() || c->manifestSize() != 0u) {
            std::fprintf(stderr, "FAIL: corruption case '%s' -> %s (%s)\n", what, pipelineCacheLoadStatusName(r.status),
                         r.message.c_str());
            ++g_failures;
        }
        // The cache is still usable after recovery.
        if (kit.build(device, *c, 2u) == 0u) {
            std::fprintf(stderr, "FAIL: cache unusable after '%s'\n", what);
            ++g_failures;
        }
    };
    {
        std::vector<u8> flipped = good;
        flipped[flipped.size() - 5] ^= 0x40u; // inside the driver blob
        expectCorrupt(flipped, "payload bit flip");
        std::vector<u8> header = good;
        header[20] ^= 0x01u; // deviceID byte
        expectCorrupt(header, "header bit flip");
        expectCorrupt(std::vector<u8>(good.begin(), good.begin() + 100), "truncated header");
        expectCorrupt(std::vector<u8>(good.begin(), good.end() - 7), "truncated payload");
        std::vector<u8> magic = good;
        magic[0] = 'X';
        expectCorrupt(magic, "bad magic");
        expectCorrupt(std::vector<u8>(64, 0xEEu), "garbage");
        expectCorrupt({}, "empty file");
    }

    // Driver-level rejection: valid FUSE header, Vulkan blob from "another vendor".
    {
        std::vector<u8> foreign = good;
        const u32 manifestCount = static_cast<u32>(foreign[88]) | static_cast<u32>(foreign[89]) << 8u;
        const size_t blobOffset = 128u + manifestCount * 8u;
        foreign[blobOffset + 8] ^= 0xFFu; // VkPipelineCacheHeaderVersionOne::vendorID
        resealV2(foreign);
        writeBytes(file, foreign);
        auto c = PipelineCache::create(device);
        const PipelineCacheLoadResult r = c->loadFileV2(file.string().c_str(), key);
        expectTrue(r.status == PipelineCacheLoadStatus::DriverRejected, "v2: foreign Vulkan blob rejected before the driver");
        expectTrue(c->isValid() && c->manifestSize() == manifestCount, "v2: manifest kept when the blob is dropped");
    }

    // Recovery: a fresh save over the damaged state loads again.
    writeBytes(file, good);
    expectTrue(warm->saveToDirectory(dir.string().c_str(), content, &error), "v2: re-save");
    auto again = PipelineCache::create(device);
    expectTrue(again->loadFromDirectory(dir.string().c_str(), content).status == PipelineCacheLoadStatus::Loaded,
               "v2: reload after recovery");

    // A failed save (directory is a file) reports failure and leaves nothing behind.
    const std::filesystem::path blocker = dir / "not_a_dir";
    writeBytes(blocker, {1, 2, 3});
    expectTrue(!warm->saveToDirectory(blocker.string().c_str(), content, &error), "v2: save into a file path fails");
    expectTrue(noTempFiles(dir), "v2: failed save leaves no temp files");

    // Background precompile of the warm set on the JobScheduler.
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    if (scheduler.isInitialized() && scheduler.workerCount() < 2u) {
        scheduler.setWorkerCount(2u);
    }
    std::vector<u64> recipeKeys(kit.recipeCount(), 0u);
    {
        auto probe = PipelineCache::create(device);
        for (u32 i = 0; i < kit.recipeCount(); ++i) {
            recipeKeys[i] = kit.build(device, *probe, i);
        }
    }
    auto pre = PipelineCache::create(device);
    expectTrue(pre->loadFromDirectory(dir.string().c_str(), content).status == PipelineCacheLoadStatus::Loaded,
               "v2: precompile cache loaded");
    pre->setPrecompileHook([&](PipelineCache& target, u64 pipelineKey) {
        for (u32 i = 0; i < recipeKeys.size(); ++i) {
            if (recipeKeys[i] == pipelineKey) {
                return kit.build(device, target, i) == pipelineKey;
            }
        }
        return false;
    });
    const u32 scheduled = pre->precompileWarmSet(&scheduler);
    pre->waitForPrecompile();
    const PipelineCacheStats st = pre->stats();
    expectTrue(scheduled == kit.recipeCount(), "v2: every warm key scheduled for precompile");
    expectTrue(st.precompileSucceeded == scheduled && st.precompileFailed == 0u, "v2: background precompile succeeded");
    expectTrue(st.warmHits == scheduled && st.warmHitRate() == 1.0, "v2: precompiled pipelines are warm hits");
    expectTrue(pre->precompileWarmSet(&scheduler) == 0u, "v2: nothing left to precompile");
    printStats("v2 precompile", st);

    std::filesystem::remove_all(dir, ec);
    ctx.device->waitIdle();
    expectTrue(rp_wp05::validationMessageCount() == 0u, "v2: zero validation messages");
}

int runWarmStartMode(bool cold, const std::string& dirText) {
    rp_wp05::Context ctx;
    const int setup = rp_wp05::setupContext(ctx, cold ? "fuse_pipeline_cache_cold" : "fuse_pipeline_cache_warm");
    if (setup != 0) {
        return setup;
    }
    VulkanDevice& device = *ctx.device;
    PipelineKit kit;
    expectTrue(kit.create(device), "warm-start: pipeline kit");
    const u64 content = kit.contentHash();
    std::error_code ec;
    if (cold) {
        std::filesystem::remove_all(dirText, ec);
    }
    auto cache = PipelineCache::create(device);
    const PipelineCacheLoadResult load = cache->loadFromDirectory(dirText.c_str(), content);
    std::printf("%s run: load %s (%s), manifest %u, blob %llu bytes\n", cold ? "cold" : "warm",
                pipelineCacheLoadStatusName(load.status), load.message.c_str(), load.manifestCount,
                static_cast<unsigned long long>(load.blobBytes));
    if (cold) {
        expectTrue(load.status == PipelineCacheLoadStatus::Missing, "cold run starts without a cache file");
    } else {
        expectTrue(load.status == PipelineCacheLoadStatus::Loaded, "warm run restores the cold run's cache");
        expectTrue(load.manifestCount == kit.recipeCount(), "warm run restores every pipeline key");
    }
    for (u32 i = 0; i < kit.recipeCount(); ++i) {
        expectTrue(kit.build(device, *cache, i) != 0u, "pipeline created");
    }
    const PipelineCacheStats st = cache->stats();
    printStats(cold ? "cold" : "warm", st);
    if (cold) {
        expectTrue(st.lookups == kit.recipeCount() && st.warmHits == 0u, "cold run: every lookup misses");
        std::string error;
        expectTrue(cache->saveToDirectory(dirText.c_str(), content, &error), "cold run saves the cache");
    } else {
        expectTrue(st.lookups == kit.recipeCount() && st.warmHits == st.lookups && st.warmHitRate() == 1.0,
                   "warm run: 100% cache hit rate");
    }
    ctx.device->waitIdle();
    expectTrue(rp_wp05::validationMessageCount() == 0u, "zero validation messages");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif // FUSE_VULKAN_BACKEND

} // namespace

int main(int argc, char** argv) {
    std::string mode = "unit";
    std::string dir;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        } else if (std::strcmp(argv[i], "--dir") == 0) {
            dir = argv[i + 1];
        }
    }
    fuse::core::initialize();
    if (mode == "cold" || mode == "warm") {
#if defined(FUSE_VULKAN_BACKEND)
        if (dir.empty()) {
            std::fprintf(stderr, "--mode %s needs --dir\n", mode.c_str());
            fuse::core::shutdown();
            return EXIT_FAILURE;
        }
        const int rc = runWarmStartMode(mode == "cold", dir);
        fuse::core::shutdown();
        if (rc == EXIT_SUCCESS) {
            std::printf("fuse_pipeline_cache --mode %s: all checks passed\n", mode.c_str());
        }
        return rc;
#else
        (void)dir;
        fuse::core::shutdown();
        std::printf("SKIP: stub build (no Vulkan backend)\n");
        return 77;
#endif
    }

    testHashedFileName();
    testSnapshotRestoreRoundTrip();
    testRebuildSnapshotsCache();
#if defined(FUSE_VULKAN_BACKEND)
    testV2Gates();
#endif
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_pipeline_cache: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_pipeline_cache: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
