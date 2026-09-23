// ResourceManager follow-ups (vk_followups): per-texture layout tracking and deferred destroy.
//
// 1. generateMips transitions mip 0 from its tracked layout (not UNDEFINED), so an uploaded mip 0
//    survives mip generation; mip 1 must equal a CPU 2x2 box filter of mip 0 (within rounding).
// 2. readTexture of any mip returns the level to its tracked layout, so repeated readbacks, uploads
//    and mip generation chain without layout mismatches.
// 3. destroyBuffer / destroyTexture never wait on the upload queue: a resource with an in-flight
//    upload is queued behind that upload's serial and released once it retires.
// Run under VK_LAYER_KHRONOS_validation (+ synchronization validation) for the zero-message gate.
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
using namespace fuse::renderer;

constexpr u32 kShaderReadOnlyLayout = 5u;  // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
constexpr u32 kTransferSrcLayout = 6u;     // VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL

struct Context {
    std::unique_ptr<VulkanBootstrap> bootstrap;
    BindlessDescriptors bindless;
    ResourceManager resources;

    bool init() {
        VulkanBootstrapDesc desc{};
        desc.instance.enableValidation = false;
        desc.createSwapchain = false;
        bootstrap = VulkanBootstrap::create(desc);
        if (bootstrap == nullptr || !bootstrap->status().deviceReady) {
            return false;
        }
        bindless.init(*bootstrap->device());
        return resources.init(*bootstrap->device(), bindless);
    }

    ~Context() {
        if (bootstrap != nullptr && bootstrap->device() != nullptr) {
            resources.destroy();
            bindless.destroy(*bootstrap->device());
        }
    }
};

ImageUsage mipUsage() {
    return static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferSrc) |
                                   static_cast<u32>(ImageUsage::TransferDst));
}

std::vector<u8> makePattern(u32 width, u32 height, u32 seed) {
    std::vector<u8> texels(static_cast<usize>(width) * height * 4u);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            u8* t = &texels[(static_cast<usize>(y) * width + x) * 4u];
            t[0] = static_cast<u8>((x * 37u + y * 11u + seed) & 0xFFu);
            t[1] = static_cast<u8>(((x ^ y) * 29u + seed * 3u) & 0xFFu);
            t[2] = static_cast<u8>((x * y + seed * 7u) & 0xFFu);
            t[3] = static_cast<u8>(255u - ((x + y) & 0x3Fu));
        }
    }
    return texels;
}

/// 2x2 box filter (what a linear blit of an exact 2:1 reduction samples), rounded to nearest.
std::vector<u8> boxDownsample(const std::vector<u8>& src, u32 width, u32 height) {
    const u32 w = std::max(width / 2u, 1u);
    const u32 h = std::max(height / 2u, 1u);
    std::vector<u8> dst(static_cast<usize>(w) * h * 4u);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            for (u32 c = 0; c < 4u; ++c) {
                u32 sum = 0;
                for (u32 dy = 0; dy < 2u; ++dy) {
                    for (u32 dx = 0; dx < 2u; ++dx) {
                        sum += src[((static_cast<usize>(y) * 2u + dy) * width + x * 2u + dx) * 4u + c];
                    }
                }
                dst[(static_cast<usize>(y) * w + x) * 4u + c] = static_cast<u8>((sum + 2u) / 4u);
            }
        }
    }
    return dst;
}

u32 maxAbsDiff(const std::vector<u8>& a, const std::vector<u8>& b) {
    u32 worst = a.size() == b.size() ? 0u : 255u;
    for (usize i = 0; i < std::min(a.size(), b.size()); ++i) {
        worst = std::max(worst, static_cast<u32>(std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]))));
    }
    return worst;
}

void testGenerateMipsPreservesUpload(Context& ctx) {
    ResourceManager& resources = ctx.resources;
    constexpr u32 kSize = 64;
    TextureDesc desc{};
    desc.width = kSize;
    desc.height = kSize;
    desc.mipLevels = 4;
    desc.usage = mipUsage();
    desc.name = "mip_chain";
    const TextureHandle texture = resources.createTexture(desc);
    expectTrue(texture.isValid(), "mip chain texture created");
    expectTrue(resources.textureLayout(texture) == 0u, "fresh texture tracked as UNDEFINED");

    const std::vector<u8> mip0 = makePattern(kSize, kSize, 3u);
    // Asynchronous upload, not waited on: generateMips must order itself after it.
    const UploadTicket ticket = resources.uploadTexture(texture, mip0.data());
    expectTrue(ticket.isValid(), "mip 0 upload accepted");
    expectTrue(resources.textureLayout(texture, 0) == kShaderReadOnlyLayout, "upload leaves mip 0 SHADER_READ_ONLY");
    expectTrue(resources.textureLayout(texture, 1) == 0u, "upload leaves mips 1.. untouched (UNDEFINED)");

    expectTrue(resources.generateMips(texture), "generateMips succeeds after upload");
    expectTrue(resources.lastMipGenerateCount() == 3u, "three blits for a 4-level chain");
    expectTrue(resources.textureLayout(texture, 0) == kShaderReadOnlyLayout &&
                   resources.textureLayout(texture, 2) == kShaderReadOnlyLayout,
               "generateMips leaves every level SHADER_READ_ONLY");

    std::vector<u8> readMip0(mip0.size());
    expectTrue(resources.readTexture(texture, readMip0.data(), readMip0.size(), 0), "read back mip 0");
    expectTrue(readMip0 == mip0, "mip 0 still holds the uploaded texels after generateMips");

    const std::vector<u8> expectedMip1 = boxDownsample(mip0, kSize, kSize);
    std::vector<u8> readMip1(expectedMip1.size());
    expectTrue(resources.readTexture(texture, readMip1.data(), readMip1.size(), 1), "read back mip 1");
    const u32 diff1 = maxAbsDiff(readMip1, expectedMip1);
    expectTrue(diff1 <= 1u, "mip 1 matches the CPU 2x2 box filter within 1 LSB");

    const std::vector<u8> expectedMip2 = boxDownsample(readMip1, kSize / 2u, kSize / 2u);
    std::vector<u8> readMip2(expectedMip2.size());
    expectTrue(resources.readTexture(texture, readMip2.data(), readMip2.size(), 2), "read back mip 2");
    const u32 diff2 = maxAbsDiff(readMip2, expectedMip2);
    expectTrue(diff2 <= 1u, "mip 2 matches the box filter of mip 1 within 1 LSB");
    expectTrue(resources.textureLayout(texture, 1) == kShaderReadOnlyLayout, "readback restores the level layout");
    std::printf("mips: mip0 exact=%d, mip1 max diff=%u, mip2 max diff=%u\n", readMip0 == mip0 ? 1 : 0, diff1, diff2);

    // Re-upload + regenerate on an already-sampled chain (defined old layouts on every level).
    const std::vector<u8> second = makePattern(kSize, kSize, 91u);
    expectTrue(resources.uploadTexture(texture, second.data()).isValid(), "second upload accepted");
    expectTrue(resources.generateMips(texture), "second generateMips succeeds");
    expectTrue(resources.readTexture(texture, readMip0.data(), readMip0.size(), 0) && readMip0 == second,
               "second upload survives regeneration");
    expectTrue(resources.readTexture(texture, readMip1.data(), readMip1.size(), 1) &&
                   maxAbsDiff(readMip1, boxDownsample(second, kSize, kSize)) <= 1u,
               "regenerated mip 1 matches the new box filter");
    resources.destroyTexture(texture);
}

void testReadbackLayoutRoundTrips(Context& ctx) {
    ResourceManager& resources = ctx.resources;
    TextureDesc desc{};
    desc.width = 16;
    desc.height = 8;
    desc.usage = mipUsage();
    const TextureHandle texture = resources.createTexture(desc);
    std::vector<u8> readback(static_cast<usize>(desc.width) * desc.height * 4u);

    // Never written: the readback transitions from UNDEFINED and parks the image in TRANSFER_SRC.
    expectTrue(resources.readTexture(texture, readback.data(), readback.size()), "read of an unwritten texture");
    expectTrue(resources.textureLayout(texture) == kTransferSrcLayout, "unwritten readback tracked as TRANSFER_SRC");

    const std::vector<u8> data = makePattern(desc.width, desc.height, 5u);
    expectTrue(resources.uploadTexture(texture, data.data()).isValid(), "upload after readback");
    for (u32 i = 0; i < 3u; ++i) {
        std::fill(readback.begin(), readback.end(), 0u);
        expectTrue(resources.readTexture(texture, readback.data(), readback.size()) && readback == data,
                   "repeated readbacks return the uploaded texels");
    }
    expectTrue(resources.textureLayout(texture) == kShaderReadOnlyLayout, "readbacks restore SHADER_READ_ONLY");
    expectTrue(!resources.readTexture(texture, readback.data(), readback.size(), 1), "out-of-range mip rejected");
    resources.destroyTexture(texture);
}

BufferDesc deviceLocalBuffer(usize bytes) {
    BufferDesc desc{};
    desc.size = bytes;
    desc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                          static_cast<u32>(BufferUsage::TransferSrc) |
                                          static_cast<u32>(BufferUsage::TransferDst));
    return desc;
}

void testDeferredDestroyDoesNotStall(Context& ctx) {
    ResourceManager& resources = ctx.resources;
    constexpr usize kBytes = 8u * 1024u * 1024u;
    constexpr u32 kRounds = 6;
    std::vector<u32> words(kBytes / sizeof(u32));
    for (usize i = 0; i < words.size(); ++i) {
        words[i] = static_cast<u32>(i * 2654435761u);
    }

    const ResourceManager::DestroyStats before = resources.destroyStats();
    const u32 liveBefore = resources.liveCounts().buffers;
    u32 deferredStillInFlight = 0;
    double worstDestroyMs = 0.0;
    for (u32 round = 0; round < kRounds; ++round) {
        const BufferHandle buffer = resources.createBuffer(deviceLocalBuffer(kBytes));
        const UploadTicket ticket = resources.uploadBuffer(buffer, words.data(), kBytes);
        expectTrue(ticket.isValid() && ticket.serial != 0u, "large upload staged");
        const u32 deferredBefore = resources.destroyStats().deferred;
        const auto start = std::chrono::steady_clock::now();
        resources.destroyBuffer(buffer); // batch still open: destroy must submit it, not wait on it
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        worstDestroyMs = std::max(worstDestroyMs, ms);
        const bool deferred = resources.destroyStats().deferred > deferredBefore;
        if (deferred && resources.completedUploadSerial() < ticket.serial) {
            ++deferredStillInFlight; // proof destroy returned before the upload finished
        }
        expectTrue(resources.getBuffer(buffer) == nullptr, "destroyed handle is invalid immediately");
        expectTrue(resources.liveCounts().buffers == liveBefore, "live count drops at destroy time");
    }

    // A texture destroyed with its upload still in the open batch.
    TextureDesc texDesc{};
    texDesc.width = 512;
    texDesc.height = 512;
    texDesc.usage = mipUsage();
    const TextureHandle texture = resources.createTexture(texDesc);
    const std::vector<u8> texels = makePattern(texDesc.width, texDesc.height, 17u);
    const UploadTicket texTicket = resources.uploadTexture(texture, texels.data());
    expectTrue(texTicket.isValid(), "texture upload staged");
    resources.destroyTexture(texture);
    expectTrue(resources.liveCounts().textures == 0u, "texture handle released immediately");

    const ResourceManager::DestroyStats mid = resources.destroyStats();
    const u32 deferred = mid.deferred - before.deferred;
    std::printf("deferred destroy: %u / %u deferred (%u proven in flight at return), worst destroy call %.3f ms, "
                "pending=%u\n",
                deferred, kRounds + 1u, deferredStillInFlight, worstDestroyMs, resources.pendingDestroyCount());
    expectTrue(deferred >= 1u, "at least one destroy was deferred behind an in-flight upload");
    expectTrue(deferredStillInFlight >= 1u, "destroy returned while the upload was still in flight (no stall)");

    expectTrue(resources.waitAllUploads(), "uploads drain");
    expectTrue(resources.pendingDestroyCount() == 0u, "deferred destroys released once uploads retired");
    const ResourceManager::DestroyStats after = resources.destroyStats();
    expectTrue(after.retired - before.retired == deferred, "every deferred destroy retired");

    // Resources created after deferred destroys (bindless slots / handles recycled) work normally.
    const BufferHandle fresh = resources.createBuffer(deviceLocalBuffer(4096));
    std::vector<u32> small(1024);
    for (u32 i = 0; i < small.size(); ++i) {
        small[i] = i ^ 0x5A5A5A5Au;
    }
    expectTrue(resources.uploadBuffer(fresh, small.data(), 4096).isValid(), "upload into a fresh buffer");
    std::vector<u32> back(1024);
    expectTrue(resources.readBuffer(fresh, back.data(), 4096) && back == small, "fresh buffer reads back");
    resources.destroyBuffer(fresh);
}

} // namespace

int main() {
    Context ctx;
    if (!ctx.init()) {
        std::printf("SKIP: no Vulkan device — layout tracking / deferred destroy need an ICD (Lavapipe in CI)\n");
        return 0;
    }
    testGenerateMipsPreservesUpload(ctx);
    testReadbackLayoutRoundTrips(ctx);
    testDeferredDestroyDoesNotStall(ctx);
    if (g_failures == 0) {
        std::printf("fuse_resource_layout_tracking: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "fuse_resource_layout_tracking: %d failure(s)\n", g_failures);
    return 1;
}
