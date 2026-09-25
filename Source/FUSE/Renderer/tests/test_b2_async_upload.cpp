// B2.11 gate 3.4: "Async upload completes and signals fence correctly — verified with fence wait
// timeout test". Uploads go through ResourceManager's asynchronous staging-ring queue: copies are
// batched into transfer command buffers and submitted with a fence, the caller never waits per
// copy, and ring space is reused only after the batch that read it has signalled. The test keeps
// many batches in flight while the ring wraps several times, then waits every ticket with a
// bounded timeout and verifies all data by readback.
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <algorithm>
#include <chrono>
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

using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::renderer::BufferHandle;
using fuse::renderer::ResourceManager;
using fuse::renderer::TextureHandle;
using fuse::renderer::UploadTicket;

constexpr u64 kFenceTimeoutNs = 1000000000ull; // 1 s bound per ticket

void fillPattern(std::vector<u32>& words, u32 seed) {
    for (usize i = 0; i < words.size(); ++i) {
        words[i] = (seed * 0x9E3779B1u) ^ static_cast<u32>(i * 2654435761u) ^ 0xA5A5A5A5u;
    }
}

fuse::renderer::BufferDesc deviceLocalBuffer(usize bytes) {
    fuse::renderer::BufferDesc desc{};
    desc.size = bytes;
    desc.usage = static_cast<fuse::renderer::BufferUsage>(
        static_cast<u32>(fuse::renderer::BufferUsage::Storage) |
        static_cast<u32>(fuse::renderer::BufferUsage::TransferSrc) |
        static_cast<u32>(fuse::renderer::BufferUsage::TransferDst));
    desc.memoryUsage = fuse::renderer::MemoryUsage::GpuOnly;
    return desc;
}

struct Context {
    std::unique_ptr<fuse::renderer::VulkanBootstrap> bootstrap;
    fuse::renderer::BindlessDescriptors bindless;
    ResourceManager resources;
    bool ready = false;

    bool init(usize ringBytes) {
        fuse::renderer::VulkanBootstrapDesc bootstrapDesc{};
        bootstrapDesc.instance.enableValidation = false;
        bootstrap = fuse::renderer::VulkanBootstrap::create(bootstrapDesc);
        if (bootstrap == nullptr || !bootstrap->status().deviceReady) {
            return false;
        }
        bindless.init(*bootstrap->device());
        ResourceManager::Desc desc{};
        desc.stagingRingBytes = ringBytes;
        ready = resources.init(*bootstrap->device(), bindless, desc);
        return ready;
    }

    ~Context() {
        if (bootstrap != nullptr && bootstrap->device() != nullptr) {
            resources.destroy();
            bindless.destroy(*bootstrap->device());
        }
    }
};

/// Many uploads in flight, ring wraps under them, every ticket signals within the bound.
void testManyUploadsInFlightAcrossWraps() {
    Context ctx;
    constexpr usize kRingBytes = 4u * 1024u * 1024u;
    if (!ctx.init(kRingBytes)) {
        std::printf("SKIP: no Vulkan device — async upload needs an ICD (Lavapipe in CI)\n");
        return;
    }
    ResourceManager& resources = ctx.resources;

    // 96 x 192 KB = 18 MB through a 4 MB ring (> 4 wraps); flush every 3 uploads so each batch
    // carries several copies and many batches are outstanding at once.
    constexpr u32 kBuffers = 96;
    constexpr usize kBytes = 192u * 1024u;
    constexpr u32 kUploadsPerBatch = 3;

    std::vector<BufferHandle> buffers;
    for (u32 i = 0; i < kBuffers; ++i) {
        buffers.push_back(resources.createBuffer(deviceLocalBuffer(kBytes)));
        expectTrue(buffers.back().isValid(), "device-local destination created");
    }

    std::vector<u32> words(kBytes / sizeof(u32));
    std::vector<UploadTicket> tickets;
    u32 accepted = 0;
    u32 maxOutstandingTickets = 0;
    const auto uploadStart = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kBuffers; ++i) {
        fillPattern(words, i);
        const UploadTicket ticket = resources.uploadBuffer(buffers[i], words.data(), kBytes);
        accepted += ticket.isValid() ? 1u : 0u;
        // The ring holds its own copy: scribbling the caller's memory must not affect the upload.
        std::fill(words.begin(), words.end(), 0xDEADBEEFu);
        if ((i + 1u) % kUploadsPerBatch == 0u) {
            tickets.push_back(resources.flushUploads());
        }
        u32 outstanding = 0;
        for (const UploadTicket& t : tickets) {
            outstanding += t.serial > resources.completedUploadSerial() ? 1u : 0u;
        }
        maxOutstandingTickets = std::max(maxOutstandingTickets, outstanding);
    }
    tickets.push_back(resources.flushUploads());
    const double submitMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uploadStart).count();

    const fuse::renderer::UploadQueueStats statsAfterSubmit = resources.uploadStats();
    std::printf("async: %u uploads (%zu KB) in %zu batches, submit loop %.1f ms, wraps=%u, "
                "max batches in flight=%u, max bytes in flight=%zu KB, ring stalls=%u, dedicated transfer=%d\n",
                accepted, kBytes / 1024u, tickets.size(), submitMs, statsAfterSubmit.ringWraps,
                statsAfterSubmit.maxBatchesInFlight, statsAfterSubmit.maxBytesInFlight / 1024u,
                statsAfterSubmit.ringStalls, statsAfterSubmit.dedicatedTransferQueue ? 1 : 0);
    expectTrue(accepted == kBuffers, "every upload accepted");
    expectTrue(statsAfterSubmit.ringWraps >= 4u, "ring wrapped at least 4 times under in-flight uploads");
    expectTrue(statsAfterSubmit.maxBatchesInFlight >= 4u, "several batches were in flight at once");
    expectTrue(statsAfterSubmit.maxBytesInFlight <= kRingBytes, "bytes in flight never exceed the ring");
    expectTrue(maxOutstandingTickets >= 2u, "caller held several unretired tickets at once");
    expectTrue(statsAfterSubmit.fenceTimeouts == 0u, "no stall wait timed out");

    // Bounded wait on every ticket, newest last; each must signal well inside 1 s.
    u32 signalled = 0;
    u32 timedOut = 0;
    double worstWaitMs = 0.0;
    for (const UploadTicket& ticket : tickets) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = resources.waitUpload(ticket, kFenceTimeoutNs);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        worstWaitMs = std::max(worstWaitMs, ms);
        signalled += ok ? 1u : 0u;
        timedOut += ok ? 0u : 1u;
        expectTrue(resources.isUploadComplete(ticket), "waited ticket reports complete");
    }
    std::printf("async: %u / %zu tickets signalled, %u timed out, worst wait %.2f ms\n", signalled,
                tickets.size(), timedOut, worstWaitMs);
    expectTrue(timedOut == 0u, "every upload fence signalled within the 1 s bound");
    expectTrue(!resources.hasPendingUploads(), "queue drained");
    expectTrue(resources.stagingRingBytesInFlight() == 0u, "all ring space retired");

    std::vector<u32> readback(words.size());
    u32 corrupted = 0;
    for (u32 i = 0; i < kBuffers; ++i) {
        fillPattern(words, i);
        if (!resources.readBuffer(buffers[i], readback.data(), kBytes) || readback != words) {
            ++corrupted;
        }
    }
    std::printf("async: %u / %u buffers verified intact\n", kBuffers - corrupted, kBuffers);
    expectTrue(corrupted == 0u, "every buffer reads back its own pattern after the ring wrapped");

    for (const BufferHandle handle : buffers) {
        resources.destroyBuffer(handle);
    }
}

/// Unaligned sizes in a small ring force wrap padding and stalls; later uploads to the same buffer
/// and partial (offset) uploads must land in submission order.
void testWrapPaddingOrderingAndPartialUploads() {
    Context ctx;
    constexpr usize kRingBytes = 1024u * 1024u;
    if (!ctx.init(kRingBytes)) {
        return;
    }
    ResourceManager& resources = ctx.resources;

    constexpr u32 kBuffers = 40;
    constexpr usize kBytes = 300u * 1024u + 12u; // not a multiple of the 256 B ring alignment
    std::vector<BufferHandle> buffers;
    std::vector<std::vector<u32>> expected(kBuffers, std::vector<u32>(kBytes / sizeof(u32)));
    for (u32 i = 0; i < kBuffers; ++i) {
        buffers.push_back(resources.createBuffer(deviceLocalBuffer(kBytes)));
        fillPattern(expected[i], 1000u + i);
        const UploadTicket t = resources.uploadBuffer(buffers[i], expected[i].data(), kBytes);
        expectTrue(t.isValid(), "unaligned upload accepted");
        resources.flushUploads();
    }
    // Overwrite every other buffer: first a full rewrite, then a partial patch at an offset, all
    // without waiting; the final contents must reflect submission order.
    std::vector<u32> patch(256);
    for (u32 i = 0; i < kBuffers; i += 2u) {
        fillPattern(expected[i], 5000u + i);
        resources.uploadBuffer(buffers[i], expected[i].data(), kBytes);
        fillPattern(patch, 9000u + i);
        const usize patchOffset = 4096u + static_cast<usize>(i) * 4u;
        resources.uploadBuffer(buffers[i], patch.data(), patch.size() * sizeof(u32), patchOffset);
        std::copy(patch.begin(), patch.end(), expected[i].begin() + static_cast<std::ptrdiff_t>(patchOffset / 4u));
        if (i % 6u == 0u) {
            resources.flushUploads();
        }
    }
    // Ticket 0-timeout poll must return promptly (never hang), and the bounded wait must succeed.
    const UploadTicket last = resources.flushUploads();
    const auto pollStart = std::chrono::steady_clock::now();
    (void)resources.waitUpload(last, 0u);
    const double pollMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pollStart).count();
    expectTrue(pollMs < 250.0, "zero-timeout wait returns without blocking");
    expectTrue(resources.waitUpload(last, kFenceTimeoutNs), "final ticket signals within 1 s");

    const fuse::renderer::UploadQueueStats stats = resources.uploadStats();
    std::printf("async padding: wraps=%u, stalls=%u, batches submitted=%llu retired=%llu, "
                "fence timeouts (0 ns poll only)=%u\n",
                stats.ringWraps, stats.ringStalls, static_cast<unsigned long long>(stats.submittedBatches),
                static_cast<unsigned long long>(stats.retiredBatches), stats.fenceTimeouts);
    expectTrue(stats.ringWraps >= 10u, "small ring wrapped repeatedly");
    expectTrue(stats.submittedBatches == stats.retiredBatches, "every submitted batch retired");
    expectTrue(stats.fenceTimeouts <= 1u, "only the deliberate 0 ns poll may report a timeout");

    std::vector<u32> readback(kBytes / sizeof(u32));
    u32 corrupted = 0;
    for (u32 i = 0; i < kBuffers; ++i) {
        if (!resources.readBuffer(buffers[i], readback.data(), kBytes) || readback != expected[i]) {
            ++corrupted;
        }
    }
    std::printf("async padding: %u / %u buffers verified (rewrites + partial patches in order)\n",
                kBuffers - corrupted, kBuffers);
    expectTrue(corrupted == 0u, "rewrites and partial uploads applied in submission order");

    // Oversized upload is rejected cleanly without touching the ring.
    const usize headBefore = resources.stagingRingOffset();
    std::vector<u8> tooLarge(kRingBytes + 16u, 0x5A);
    const BufferHandle big = resources.createBuffer(deviceLocalBuffer(tooLarge.size()));
    expectTrue(!resources.uploadBuffer(big, tooLarge.data(), tooLarge.size()).isValid(),
               "upload larger than the ring is rejected");
    expectTrue(resources.stagingRingOffset() == headBefore, "rejected upload leaves the ring untouched");
}

/// Texture uploads batched in one submit; poll until complete, then verify every texel.
void testAsyncTextureUploads() {
    Context ctx;
    if (!ctx.init(512u * 1024u)) {
        return;
    }
    ResourceManager& resources = ctx.resources;

    constexpr u32 kTextures = 48; // 48 x 16 KB = 768 KB through a 512 KB ring
    constexpr u32 kSize = 64;
    fuse::renderer::TextureDesc desc{};
    desc.width = kSize;
    desc.height = kSize;
    desc.format = fuse::renderer::GpuFormat::R8G8B8A8Unorm;
    desc.usage = static_cast<fuse::renderer::ImageUsage>(
        static_cast<u32>(fuse::renderer::ImageUsage::Sampled) |
        static_cast<u32>(fuse::renderer::ImageUsage::TransferSrc) |
        static_cast<u32>(fuse::renderer::ImageUsage::TransferDst));

    std::vector<TextureHandle> textures;
    std::vector<u32> texels(kSize * kSize);
    for (u32 i = 0; i < kTextures; ++i) {
        textures.push_back(resources.createTexture(desc));
        fillPattern(texels, 20000u + i);
        expectTrue(resources.uploadTexture(textures.back(), texels.data()).isValid(), "texture upload accepted");
    }
    const UploadTicket last = resources.flushUploads();

    const auto start = std::chrono::steady_clock::now();
    bool complete = false;
    while (!complete) {
        complete = resources.isUploadComplete(last);
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(1)) {
            break;
        }
    }
    const double pollMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("async textures: %u uploads, polled complete=%d after %.2f ms, wraps=%u\n", kTextures,
                complete ? 1 : 0, pollMs, resources.uploadStats().ringWraps);
    expectTrue(complete, "texture batch signalled within 1 s (polling)");

    std::vector<u32> readback(texels.size());
    u32 corrupted = 0;
    for (u32 i = 0; i < kTextures; ++i) {
        fillPattern(texels, 20000u + i);
        if (!resources.readTexture(textures[i], readback.data(), readback.size() * sizeof(u32)) ||
            readback != texels) {
            ++corrupted;
        }
    }
    std::printf("async textures: %u / %u verified\n", kTextures - corrupted, kTextures);
    expectTrue(corrupted == 0u, "every texture reads back its uploaded texels");
}

} // namespace

int main() {
    testManyUploadsInFlightAcrossWraps();
    testWrapPaddingOrderingAndPartialUploads();
    testAsyncTextureUploads();

    if (g_failures == 0) {
        std::printf("fuse_b2_async_upload: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_b2_async_upload: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
