// B2.11 gates:
//   "Staging ring buffer correctly wraps — upload of 256MB in 1MB chunks with no corruption"
//   "Async upload completes and signals fence correctly — verified with fence wait timeout test"
// A 16 MB ring uploads 256 x 1 MB device-local buffers (16 wraps). createBuffer(initialData) now
// submits each copy asynchronously (no CPU wait per copy); ring space is reused only after the
// batch's fence signals. Every buffer stays alive and is verified only after all uploads, so a ring
// slot overwritten before its copy retired would show. See test_b2_async_upload for the ticket API.
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr fuse::usize kChunkBytes = 1024u * 1024u;
constexpr fuse::u32 kChunkCount = 256u; // 256 MB total
constexpr fuse::usize kRingBytes = 16u * 1024u * 1024u;

/// Deterministic per-chunk pattern: every 32-bit word encodes (chunk, word index).
void fillPattern(std::vector<fuse::u32>& words, fuse::u32 chunk) {
    for (fuse::usize i = 0; i < words.size(); ++i) {
        words[i] = (chunk * 0x9E3779B1u) ^ static_cast<fuse::u32>(i * 2654435761u);
    }
}

void testStagingRingWrap256MB() {
    fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    auto bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
    expectTrue(bootstrap != nullptr, "bootstrap allocated");
    if (bootstrap == nullptr || !bootstrap->status().deviceReady) {
        std::printf("SKIP: no Vulkan device — staging wrap needs an ICD (Lavapipe in CI)\n");
        return;
    }

    fuse::renderer::BindlessDescriptors bindless;
    bindless.init(*bootstrap->device());
    fuse::renderer::ResourceManager resources;
    fuse::renderer::ResourceManager::Desc desc{};
    desc.stagingRingBytes = kRingBytes;
    expectTrue(resources.init(*bootstrap->device(), bindless, desc), "resource manager ready");
    expectTrue(resources.stagingRingCapacity() == kRingBytes, "16 MB staging ring");

    std::vector<fuse::u32> words(kChunkBytes / sizeof(fuse::u32));
    std::vector<fuse::renderer::BufferHandle> chunks;
    chunks.reserve(kChunkCount);

    fuse::u32 fencedCopies = 0;
    fuse::u32 timedOut = 0;
    for (fuse::u32 chunk = 0; chunk < kChunkCount; ++chunk) {
        fillPattern(words, chunk);
        fuse::renderer::BufferDesc bufferDesc{};
        bufferDesc.size = kChunkBytes;
        bufferDesc.usage = fuse::renderer::BufferUsage::TransferSrc; // readBuffer copies out of it
        bufferDesc.memoryUsage = fuse::renderer::MemoryUsage::GpuOnly;
        const fuse::renderer::BufferHandle handle = resources.createBuffer(bufferDesc, words.data());
        expectTrue(handle.isValid(), "1 MB device-local chunk created");
        chunks.push_back(handle);
        fencedCopies += resources.lastGpuCopyUsedFence() ? 1u : 0u;
        timedOut += resources.lastGpuCopyWaitTimedOut() ? 1u : 0u;
    }

    const fuse::u32 expectedWraps = static_cast<fuse::u32>((kChunkCount * kChunkBytes) / kRingBytes) - 1u;
    std::printf("staging: %u chunks, wraps=%u (expected >= %u), fenced=%u, timed out=%u\n", kChunkCount,
                resources.stagingRingWrapCount(), expectedWraps, fencedCopies, timedOut);
    expectTrue(resources.stagingRingWrapCount() >= expectedWraps, "ring wrapped for 256 MB through 16 MB");
    expectTrue(fencedCopies == kChunkCount, "every upload signalled its fence");
    expectTrue(timedOut == 0u, "no upload hit the fence wait timeout");

    const fuse::renderer::UploadQueueStats& uploads = resources.uploadStats();
    std::printf("staging: batches in flight max=%u, ring stalls=%u, fence timeouts=%u\n",
                uploads.maxBatchesInFlight, uploads.ringStalls, uploads.fenceTimeouts);
    expectTrue(uploads.maxBatchesInFlight > 1u, "uploads overlap: more than one batch in flight at once");
    expectTrue(uploads.fenceTimeouts == 0u, "no ring stall hit the bounded fence wait");
    expectTrue(resources.waitAllUploads(), "all uploads signal within the bounded wait");
    expectTrue(resources.stagingRingBytesInFlight() == 0u, "ring fully retired after the final wait");

    std::vector<fuse::u32> readback(words.size());
    fuse::u32 corrupted = 0;
    for (fuse::u32 chunk = 0; chunk < kChunkCount; ++chunk) {
        fillPattern(words, chunk);
        const bool read = resources.readBuffer(chunks[chunk], readback.data(), kChunkBytes);
        if (!read || readback != words) {
            ++corrupted;
        }
    }
    std::printf("staging: %u / %u chunks verified intact\n", kChunkCount - corrupted, kChunkCount);
    expectTrue(corrupted == 0u, "all 256 chunks read back identical after every wrap");

    for (const fuse::renderer::BufferHandle handle : chunks) {
        resources.destroyBuffer(handle);
    }
    resources.destroy();
    bindless.destroy(*bootstrap->device());
}

} // namespace

int main() {
    testStagingRingWrap256MB();

    if (g_failures == 0) {
        std::printf("fuse_b2_staging_wrap: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b2_staging_wrap: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
