// WP-0.3 CPU gate: render graph v2 compiler and barrier planner (no Vulkan device needed; runs in
// the stub build too).
//   * RAW / WAR / WAW / RAR on buffers and images produce exactly the expected sync2 barriers
//     (stage + access masks, layouts), image subresource ranges and buffer byte ranges.
//   * Culling, the removed 32-pass cap, per-queue batches with timeline waits, queue-family
//     ownership release/acquire pairs, prologue batches, final layouts + trackers.
//   * Steady-state frames (reset + declare + compile + plan) perform zero heap allocations (B2.11).
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rg/sync_model.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

thread_local bool t_count = false;
thread_local unsigned long t_allocations = 0;

} // namespace

void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

namespace {

using namespace fuse::renderer::rg;
using fuse::u32;
using fuse::u64;
using fuse::u8;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr u32 kR32Uint = 98; // VK_FORMAT_R32_UINT
constexpr u32 kD32 = 126;

void* fakeHandle(u64 v) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(v));
}

bool planAll(Graph& g, const CompileOptions& options = {}) {
    return g.compile(options) && g.plan();
}

/// The single barrier on buffer `id` in front of `pass` (null when none or several).
const BufferBarrier* bufferBarrierFor(const Graph& g, u32 pass, u32 id) {
    const BarrierRange& r = g.passBarriers(pass);
    const BufferBarrier* found = nullptr;
    for (u32 i = r.bufferBegin; i < r.bufferBegin + r.bufferCount; ++i) {
        if (g.bufferBarriers()[i].resource == id) {
            if (found != nullptr) {
                return nullptr;
            }
            found = &g.bufferBarriers()[i];
        }
    }
    return found;
}

void testBufferHazards() {
    Graph g;
    const BufferRef t = g.createBuffer({4096, 0, "t"});
    const BufferRef out = g.importBuffer({fakeHandle(0x10), 4096, kNoQueue, nullptr, "out"});
    // 0: W (transfer)   1: R (compute)  RAW   2: R (compute) RAR   3: W (transfer) WAR   4: W (transfer) WAW
    // 5: R transfer -> out
    g.addPass("fill", nullptr, nullptr).use(t, Access::TransferDst, {0, 1024});
    g.addPass("read_a", nullptr, nullptr).use(t, Access::StorageRead, {0, 1024}, kStageCompute).use(out, Access::StorageWrite, {0, 16}, kStageCompute);
    g.addPass("read_b", nullptr, nullptr).use(t, Access::StorageRead, {0, 1024}, kStageCompute).use(out, Access::StorageWrite, {16, 16}, kStageCompute);
    g.addPass("overwrite", nullptr, nullptr).use(t, Access::TransferDst, {1024, 1024});
    g.addPass("overwrite2", nullptr, nullptr).use(t, Access::TransferDst, {1024, 1024});
    g.addPass("copy_out", nullptr, nullptr).use(t, Access::TransferSrc).use(out, Access::TransferDst, {1024, 1024});
    expect(planAll(g), "buffer graph compiles");
    expect(g.stats().executedPasses == 6u, "buffer graph: nothing culled");

    const BufferBarrier* first = bufferBarrierFor(g, 0, t.id);
    expect(first != nullptr && first->srcStages == vkc::kStageAllCommands && first->srcAccess == vkc::kAccessMemoryWrite,
           "first write of a transient orders after all prior work (alias / previous frame)");
    const BufferBarrier* raw = bufferBarrierFor(g, 1, t.id);
    expect(raw != nullptr, "RAW: one barrier on t before read_a");
    if (raw != nullptr) {
        expect(raw->srcStages == vkc::kStageTransfer && raw->srcAccess == vkc::kAccessTransferWrite,
               "RAW src = transfer write");
        expect(raw->dstStages == vkc::kStageComputeShader && raw->dstAccess == vkc::kAccessShaderRead,
               "RAW dst = compute shader read");
        expect(raw->offset == 0u && raw->size == 1024u, "RAW range = tracked bytes [0,1024)");
    }
    const BarrierRange& rar = g.passBarriers(2);
    bool rarOnT = false;
    for (u32 i = rar.bufferBegin; i < rar.bufferBegin + rar.bufferCount; ++i) {
        rarOnT = rarOnT || g.bufferBarriers()[i].resource == t.id;
    }
    expect(!rarOnT, "RAR (same stage/access, already visible): no barrier on t");
    const BufferBarrier* war = bufferBarrierFor(g, 3, t.id);
    // The reads' stage is in the source scope (WAR); the planner also keeps the last write (WAW,
    // conservative even though the RAW barrier already chained it).
    expect(war != nullptr && (war->srcStages & vkc::kStageComputeShader) != 0u &&
               war->dstStages == vkc::kStageTransfer && war->dstAccess == vkc::kAccessTransferWrite,
           "WAR: compute readers in the source scope of the transfer write");
    expect(war != nullptr && war->offset == 0u && war->size == 2048u, "WAR range = union of tracked accesses");
    const BufferBarrier* waw = bufferBarrierFor(g, 4, t.id);
    expect(waw != nullptr && waw->srcStages == vkc::kStageTransfer && waw->srcAccess == vkc::kAccessTransferWrite &&
               waw->dstAccess == vkc::kAccessTransferWrite,
           "WAW: transfer write -> transfer write");
    expect(g.stats().queueFallbacks == 0u && g.stats().batchCount == 1u, "single graphics batch");
}

void testImageLayoutsAndSubresources() {
    Graph g;
    ImageDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.mipLevels = 4;
    desc.arrayLayers = 2;
    desc.format = kR32Uint;
    const TextureRef img = g.createImage(desc);
    const BufferRef out = g.importBuffer({fakeHandle(0x20), 1 << 20, kNoQueue, nullptr, "out"});
    // 0: clear all (TransferDst), 1..3: mip i-1 -> mip i (TransferSrc / TransferDst), 4: sample all, 5: copy mip 3 layer 1
    g.addPass("clear", nullptr, nullptr).use(img, Access::TransferDst);
    for (u32 mip = 1; mip < 4; ++mip) {
        g.addPass("downsample", nullptr, nullptr)
            .use(img, Access::TransferSrc, {mip - 1u, 1, 0, 0})
            .use(img, Access::TransferDst, {mip, 1, 0, 0});
    }
    g.addPass("sample", nullptr, nullptr).use(img, Access::SampledRead, {}, kStageFragment).use(out, Access::StorageWrite, {0, 64}, kStageFragment);
    g.addPass("copy", nullptr, nullptr).use(img, Access::TransferSrc, {3, 1, 1, 1}).use(out, Access::TransferDst, {64, 256});
    expect(planAll(g), "image graph compiles");
    expect(g.stats().executedPasses == 6u, "image graph: nothing culled");

    const BarrierRange& clear = g.passBarriers(0);
    expect(clear.imageCount == 1u, "full-image transition coalesced into one barrier");
    if (clear.imageCount == 1u) {
        const ImageBarrier& b = g.imageBarriers()[clear.imageBegin];
        expect(b.oldLayout == vkc::kLayoutUndefined && b.newLayout == vkc::kLayoutTransferDst,
               "clear: UNDEFINED -> TRANSFER_DST");
        expect(b.baseMip == 0u && b.mipCount == 4u && b.baseLayer == 0u && b.layerCount == 2u,
               "clear: range covers 4 mips x 2 layers");
    }
    for (u32 pass = 1; pass <= 3; ++pass) {
        const BarrierRange& r = g.passBarriers(pass);
        expect(r.imageCount == 2u, "downsample: one barrier per touched mip");
        if (r.imageCount != 2u) {
            continue;
        }
        const ImageBarrier& src = g.imageBarriers()[r.imageBegin];
        const ImageBarrier& dst = g.imageBarriers()[r.imageBegin + 1u];
        expect(src.baseMip == pass - 1u && src.mipCount == 1u && src.newLayout == vkc::kLayoutTransferSrc &&
                   src.oldLayout == vkc::kLayoutTransferDst && src.srcAccess == vkc::kAccessTransferWrite &&
                   src.dstAccess == vkc::kAccessTransferRead,
               "downsample: RAW on mip i-1 (TRANSFER_DST -> TRANSFER_SRC)");
        expect(dst.baseMip == pass && dst.mipCount == 1u && dst.oldLayout == vkc::kLayoutTransferDst &&
                   dst.newLayout == vkc::kLayoutTransferDst && dst.srcAccess == vkc::kAccessTransferWrite,
               "downsample: WAW on mip i (same layout, memory barrier only)");
    }
    const BarrierRange& sample = g.passBarriers(4);
    expect(sample.imageCount == 2u, "sample: mips 0-2 (TRANSFER_SRC) and mip 3 (TRANSFER_DST) differ -> 2 barriers");
    if (sample.imageCount == 2u) {
        const ImageBarrier& a = g.imageBarriers()[sample.imageBegin];
        const ImageBarrier& b = g.imageBarriers()[sample.imageBegin + 1u];
        expect(a.baseMip == 0u && a.mipCount == 3u && a.layerCount == 2u && a.oldLayout == vkc::kLayoutTransferSrc &&
                   a.newLayout == vkc::kLayoutShaderReadOnly && a.dstStages == vkc::kStageFragmentShader,
               "sample: mips 0-2 coalesced, TRANSFER_SRC -> SHADER_READ_ONLY for the fragment stage");
        expect(b.baseMip == 3u && b.mipCount == 1u && b.oldLayout == vkc::kLayoutTransferDst &&
                   b.srcAccess == vkc::kAccessTransferWrite,
               "sample: mip 3 RAW from its transfer write");
    }
    const BarrierRange& copy = g.passBarriers(5);
    expect(copy.imageCount == 1u, "copy: one barrier for mip 3 layer 1 only");
    if (copy.imageCount == 1u) {
        const ImageBarrier& b = g.imageBarriers()[copy.imageBegin];
        expect(b.baseMip == 3u && b.mipCount == 1u && b.baseLayer == 1u && b.layerCount == 1u &&
                   b.oldLayout == vkc::kLayoutShaderReadOnly && b.newLayout == vkc::kLayoutTransferSrc,
               "copy: subresource-exact SHADER_READ_ONLY -> TRANSFER_SRC");
    }
}

void testAttachmentsAndSamePassMerge() {
    Graph g;
    ImageDesc color;
    color.format = 37;
    const TextureRef c = g.createImage(color);
    ImageDesc depthDesc;
    depthDesc.format = kD32;
    const TextureRef d = g.createImage(depthDesc);
    const TextureRef swap = g.importImage({fakeHandle(0x30), nullptr, 44, 1, 1, 1, 1, 1, vkc::kLayoutUndefined, kNoQueue,
                                           vkc::kLayoutPresentSrc, nullptr, nullptr, "swapchain"});
    const BufferRef rw = g.createBuffer({256, 0, "rw"});
    g.addPass("gbuffer", nullptr, nullptr).use(c, Access::ColorAttachmentWrite).use(d, Access::DepthAttachmentWrite);
    g.addPass("rmw", nullptr, nullptr)
        .use(rw, Access::StorageRead, {}, kStageCompute)
        .use(rw, Access::StorageWrite, {}, kStageCompute)
        .use(d, Access::DepthAttachmentRead);
    g.addPass("resolve", nullptr, nullptr)
        .use(c, Access::SampledRead, {}, kStageFragment)
        .use(rw, Access::StorageRead, {}, kStageFragment)
        .use(swap, Access::ColorAttachmentWrite);
    g.addPass("present", nullptr, nullptr).use(swap, Access::Present);
    expect(planAll(g), "attachment graph compiles");
    expect(g.stats().executedPasses == 4u, "attachment graph: nothing culled");
    const BarrierRange& gb = g.passBarriers(0);
    bool depthOk = false;
    for (u32 i = gb.imageBegin; i < gb.imageBegin + gb.imageCount; ++i) {
        const ImageBarrier& b = g.imageBarriers()[i];
        if (b.resource == d.id) {
            depthOk = b.newLayout == vkc::kLayoutDepthStencilAttachment &&
                      b.dstStages == (vkc::kStageEarlyFragmentTests | vkc::kStageLateFragmentTests);
        }
    }
    expect(depthOk, "depth attachment: DEPTH_STENCIL_ATTACHMENT at early|late fragment tests");
    const BarrierRange& rmw = g.passBarriers(1);
    u32 rwBarriers = 0;
    for (u32 i = rmw.bufferBegin; i < rmw.bufferBegin + rmw.bufferCount; ++i) {
        const BufferBarrier& b = g.bufferBarriers()[i];
        if (b.resource == rw.id) {
            ++rwBarriers;
            expect(b.dstAccess == (vkc::kAccessShaderRead | vkc::kAccessShaderWrite),
                   "same-pass read+write declarations merged into one barrier");
        }
    }
    expect(rwBarriers == 1u, "one barrier for the merged read+write");
    const BarrierRange& present = g.passBarriers(3);
    expect(present.imageCount == 1u && g.imageBarriers()[present.imageBegin].newLayout == vkc::kLayoutPresentSrc &&
               g.imageBarriers()[present.imageBegin].dstStages == vkc::kStageNone,
           "present: COLOR_ATTACHMENT -> PRESENT_SRC with dst stage NONE");
    expect(g.stats().layoutConflicts == 0u, "no layout conflicts");
}

void testCullingAndNoPassCap() {
    Graph g;
    const BufferRef out = g.importBuffer({fakeHandle(0x40), 64, kNoQueue, nullptr, "out"});
    const BufferRef dead = g.createBuffer({64, 0, "dead"});
    const BufferRef dead2 = g.createBuffer({64, 0, "dead2"});
    g.addPass("dead_write", nullptr, nullptr).use(dead, Access::TransferDst);
    g.addPass("kept", nullptr, nullptr).use(dead2, Access::TransferDst).neverCull();
    g.addPass("live", nullptr, nullptr).use(out, Access::TransferDst);
    expect(planAll(g), "culling graph compiles");
    expect(g.passCulled(0) && !g.passCulled(1) && !g.passCulled(2), "unobserved write culled; neverCull kept");

    // 300 chained passes: v1 capped at 32, v2 has no cap.
    Graph big;
    constexpr u32 kPasses = 300;
    BufferRef chain[2] = {big.createBuffer({1024, 0, "a"}), big.createBuffer({1024, 0, "b"})};
    const BufferRef sink = big.importBuffer({fakeHandle(0x50), 1024, kNoQueue, nullptr, "sink"});
    for (u32 p = 0; p + 1u < kPasses; ++p) {
        big.addPass("chain", nullptr, nullptr)
            .use(chain[p & 1u], Access::StorageRead, {}, kStageCompute)
            .use(chain[(p + 1u) & 1u], Access::StorageWrite, {}, kStageCompute);
    }
    big.addPass("sink", nullptr, nullptr).use(chain[(kPasses - 1u) & 1u], Access::TransferSrc).use(sink, Access::TransferDst);
    expect(planAll(big), "300-pass graph compiles");
    expect(big.passCount() == kPasses && big.stats().executedPasses == kPasses, "all 300 passes kept (no 32-pass cap)");
    expect(big.stats().passesWithBarriers >= kPasses - 2u, "every chained pass got its own barrier batch");
}

void testQueuesAndOwnership() {
    auto build = [](Graph& g) {
        g.reset();
        const BufferRef x = g.importBuffer({fakeHandle(0x60), 4096, static_cast<u8>(QueueClass::Graphics), nullptr, "x"});
        const BufferRef z = g.importBuffer({fakeHandle(0x61), 4096, kNoQueue, nullptr, "z"});
        const BufferRef y = g.createBuffer({4096, 0, "y"});
        ImageDesc desc;
        desc.format = kR32Uint;
        const TextureRef img = g.createImage(desc);
        g.addPass("fill_x", nullptr, nullptr, QueueClass::Graphics).use(x, Access::TransferDst).use(img, Access::TransferDst);
        g.addPass("async", nullptr, nullptr, QueueClass::AsyncCompute)
            .use(x, Access::StorageRead)
            .use(img, Access::StorageRead)
            .use(y, Access::StorageWrite);
        g.addPass("upload_z", nullptr, nullptr, QueueClass::Transfer).use(y, Access::TransferSrc).use(z, Access::TransferDst);
        g.addPass("bad_async", nullptr, nullptr, QueueClass::AsyncCompute).use(z, Access::VertexRead).neverCull();
    };
    Graph g;
    build(g);
    CompileOptions options;
    options.asyncComputeAvailable = true;
    options.transferAvailable = true;
    expect(planAll(g, options), "multi-queue graph compiles");
    expect(g.passQueue(1) == QueueClass::AsyncCompute && g.passQueue(2) == QueueClass::Transfer,
           "async compute and transfer passes keep their queues");
    expect(g.passQueue(3) == QueueClass::Graphics && g.stats().queueFallbacks == 1u,
           "vertex-input access on async compute falls back to graphics");
    expect(g.stats().batchCount == 4u, "4 batches (graphics, compute, transfer, graphics)");
    const auto& batches = g.batches();
    if (batches.size() == 4u) {
        expect(batches[1].waitBatch[0] == 0, "compute batch waits for the graphics batch");
        expect(batches[2].waitBatch[1] == 1, "transfer batch waits for the compute batch");
        expect(batches[3].waitBatch[2] == 2, "last graphics batch waits for the transfer batch (z)");
        expect(batches[0].post.bufferCount == 1u && batches[0].post.imageCount == 1u,
               "graphics batch releases x and img");
        expect(batches[1].post.bufferCount == 1u, "compute batch releases y");
        if (batches[0].post.bufferCount == 1u) {
            const BufferBarrier& release = g.bufferBarriers()[batches[0].post.bufferBegin];
            expect(release.srcQueue == 0u && release.dstQueue == 1u && release.srcStages == vkc::kStageTransfer &&
                       release.dstStages == vkc::kStageNone,
                   "release x: graphics -> compute, src transfer write");
        }
    }
    const BarrierRange& acquire = g.passBarriers(1);
    bool acquireX = false;
    for (u32 i = acquire.bufferBegin; i < acquire.bufferBegin + acquire.bufferCount; ++i) {
        const BufferBarrier& b = g.bufferBarriers()[i];
        acquireX = acquireX || (b.srcQueue == 0u && b.dstQueue == 1u && b.srcStages == vkc::kStageAllCommands &&
                                b.srcAccess == 0u &&
                                b.dstStages == vkc::kStageComputeShader);
    }
    expect(acquireX, "acquire x on compute with the consumer's stage");
    const BarrierRange& late = g.passLateBarriers(1);
    expect(late.imageCount == 1u && g.lateImageBarriers()[late.imageBegin].oldLayout == vkc::kLayoutTransferDst &&
               g.lateImageBarriers()[late.imageBegin].newLayout == vkc::kLayoutGeneral &&
               g.lateImageBarriers()[late.imageBegin].srcQueue == kNoQueue,
           "img: ownership transfer keeps TRANSFER_DST, GENERAL applied after the acquire");
    expect(g.stats().ownershipTransfers >= 4u, "x, img (g->c), y (c->t), z (t->g) ownership transfers");

    // Same graph without async / transfer queues: one batch, no ownership transfers.
    build(g);
    expect(planAll(g), "single-queue graph compiles");
    expect(g.stats().batchCount == 1u && g.stats().ownershipTransfers == 0u && g.stats().crossQueueWaits == 0u,
           "single queue: 1 batch, no QFOT, no waits");
}

void testPrologueAndFinalLayout() {
    Graph g;
    u32 layout = 0;
    u8 queue = kNoQueue;
    const BufferRef owned = g.importBuffer({fakeHandle(0x70), 64, static_cast<u8>(QueueClass::AsyncCompute), &queue, "owned"});
    const TextureRef target = g.importImage({fakeHandle(0x71), nullptr, 37, 8, 8, 1, 1, 1, vkc::kLayoutShaderReadOnly,
                                             static_cast<u8>(QueueClass::Graphics), vkc::kLayoutShaderReadOnly, &layout,
                                             nullptr, "target"});
    g.addPass("draw", nullptr, nullptr).use(owned, Access::UniformRead, {}, kStageFragment).use(target, Access::ColorAttachmentWrite);
    CompileOptions options;
    options.asyncComputeAvailable = true;
    expect(planAll(g, options), "prologue graph compiles");
    expect(g.batches().size() == 2u && g.batches()[0].prologue && g.batches()[0].queue == QueueClass::AsyncCompute,
           "compute-owned import used on graphics -> prologue batch on compute");
    if (g.batches().size() == 2u) {
        expect(g.batches()[0].post.bufferCount == 1u && g.batches()[1].waitBatch[1] == 0,
               "prologue releases the buffer; graphics waits for it");
        expect(g.batches()[1].post.imageCount == 1u, "final layout transition recorded after the last pass");
    }
    expect(layout == vkc::kLayoutShaderReadOnly, "layout tracker reports the final layout");
    expect(queue == static_cast<u8>(QueueClass::Graphics), "queue tracker reports the new owner");
}

void buildSteadyGraph(Graph& g) {
    g.reset();
    const BufferRef out = g.importBuffer({fakeHandle(0x80), 1 << 16, kNoQueue, nullptr, "out"});
    ImageDesc desc;
    desc.width = 256;
    desc.height = 256;
    desc.mipLevels = 3;
    desc.format = kR32Uint;
    TextureRef images[4];
    for (TextureRef& image : images) {
        image = g.createImage(desc);
    }
    for (u32 p = 0; p < 64; ++p) {
        g.addPass("steady", nullptr, nullptr)
            .use(images[p % 4u], Access::StorageRead, {p % 3u, 1, 0, 0}, kStageCompute)
            .use(images[(p + 1u) % 4u], Access::StorageWrite, {}, kStageCompute)
            .use(out, Access::StorageReadWrite, {(p % 8u) * 256u, 256}, kStageCompute);
    }
}

void testZeroSteadyStateAllocations() {
    Graph g;
    for (int i = 0; i < 4; ++i) { // warm-up grows the pools once
        buildSteadyGraph(g);
        planAll(g);
    }
    t_allocations = 0;
    t_count = true;
    for (int i = 0; i < 100; ++i) {
        buildSteadyGraph(g);
        planAll(g);
    }
    t_count = false;
    expect(g.stats().planned && g.stats().executedPasses == 64u, "steady graph planned");
    std::printf("steady state: 100 frames x 64 passes, %lu heap allocations, %u barriers/frame, compile+plan %uus\n",
                t_allocations, g.stats().imageBarriers + g.stats().bufferBarriers, g.stats().compileUs);
    expect(t_allocations == 0u, "zero heap allocations in steady-state reset/declare/compile/plan (B2.11)");
}

} // namespace

int main() {
    testBufferHazards();
    testImageLayoutsAndSubresources();
    testAttachmentsAndSamePassMerge();
    testCullingAndNoPassCap();
    testQueuesAndOwnership();
    testPrologueAndFinalLayout();
    testZeroSteadyStateAllocations();
    if (g_failures == 0) {
        std::printf("fuse_rp_rg_compile: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_rp_rg_compile: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
