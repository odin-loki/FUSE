// WP-5.3 cluster streaming Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation;
// every validation message fails the run). CPU gates: test_rp_geometry_streaming_cpu.cpp.
//
// Asset: bumpy icosphere (6 subdivisions, 82k triangles) -> WP-5.2 DAG -> FCPG pages of 32 KiB. For each kernel
// language built (Slang, GLSL) one GeometryStreaming (budget: coarse + a third of the pages, 8 reserve slots,
// 6 loads / frame). Every frame is one render graph: stream.state, stream.cut, stream.feedback (+ read-back
// copies of the cut flags, the draw list and rig 0's page pool). Frame serial s applies the feedback of frame
// s - 1 (read back through the ring), completes the uploads of frame s - 1 and issues new ones.
//
//   --mode parity      a scripted 96-frame fly-through (approach, skim the surface, climb out, hold): per frame
//                      the GPU cut flags, the per-page feedback priorities and header counts equal the CPU
//                      reference kernel geometry_stream_cut run on the frame's resident bitmask and view (word
//                      for word), the request list and the draw list equal it as sets; Slang == GLSL (every
//                      cut word, every priority, both rigs' residency identical); every frame's cut is
//                      watertight and area-bounded when decoded from the READ-BACK POOL bytes of resident slots
//                      only (0 frames missing geometry), the coarse LOD is resident on every frame, the page
//                      budget is never exceeded, pages stream in and get evicted
//   --mode zero_alloc  64 steady-state frames: 0 operator-new calls in GeometryStreaming::beginFrame (feedback,
//                      residency update, page uploads), the stream.* pass callbacks and the whole graph build
//                      (validated run first; validation off for the count)
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_geometry_streaming_common.hpp"

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/geometry_streaming/geometry_streaming.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <set>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

// --- allocation counter (operator new) -----------------------------------------------------------
namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

// GCC may inline the replacement operators into callers and then report a false
// -Wmismatched-new-delete at -O2; replacement functions must not be inline anyway.
#if defined(__GNUC__)
#define FUSE_TEST_REPLACEMENT_NOINLINE __attribute__((noinline))
#else
#define FUSE_TEST_REPLACEMENT_NOINLINE
#endif

FUSE_TEST_REPLACEMENT_NOINLINE void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
FUSE_TEST_REPLACEMENT_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete(void* p, std::size_t) noexcept { std::free(p); }
FUSE_TEST_REPLACEMENT_NOINLINE void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
constexpr int kSkip = 77;
int g_failures = 0;

[[maybe_unused]] void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}
} // namespace

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return kSkip;
}

#else

namespace {

using namespace fuse::renderer;
using namespace stream_test;
namespace sk = gs::stream_kernel;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr usize kStagingBytes = 16u * 1024u * 1024u;
constexpr u32 kLanguages = 2; // Slang, GLSL
constexpr f32 kRadius = 10.f;

u32 g_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) ==
            0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    ++g_messages;
    if (g_messages <= 20u) {
        std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n",
                     data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)",
                     data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
    }
    return VK_FALSE;
}

bool layerAvailable(const char* name) {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

// --- Vulkan context -------------------------------------------------------------------------------
struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    std::unique_ptr<rg::Executor> executor;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    BindlessDescriptors bindless;
    UploadQueue upload;
    Buffer staging{};
    Buffer readback{};
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (allocator != nullptr) {
            allocator->destroyBuffer(readback);
            allocator->destroyBuffer(staging);
        }
        if (device != nullptr) {
            bindless.collectRetired(~0ull);
            bindless.destroy(*device);
        }
        allocator.reset();
        device.reset();
        if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr && instance != nullptr) {
            destroyMessenger(static_cast<VkInstance>(instance->nativeHandle()), messenger, nullptr);
        }
        instance.reset();
    }
};

int setup(Context& ctx, bool descriptorBuffer, bool validation) {
    if (validation) {
        if (!layerAvailable(kValidationLayer)) {
            std::printf("SKIP: %s not installed (the gate needs synchronization validation)\n", kValidationLayer);
            return kSkip;
        }
        setenv("VK_INSTANCE_LAYERS", kValidationLayer, 1);
        setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
        setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
        setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);
    } else {
        setenv("VK_INSTANCE_LAYERS", "", 1);
    }
    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_rp_geometry_streaming";
    instanceDesc.enableValidation = validation;
    ctx.instance = VulkanInstance::create(instanceDesc);
    if (ctx.instance == nullptr || !ctx.instance->isValid()) {
        std::printf("SKIP: no Vulkan instance\n");
        return kSkip;
    }
    const VkInstance vkInstance = static_cast<VkInstance>(ctx.instance->nativeHandle());
    auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    ctx.destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
    if (createMessenger == nullptr && validation) {
        std::printf("SKIP: VK_EXT_debug_utils unavailable\n");
        return kSkip;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = onMessage;
    if (createMessenger != nullptr) {
        createMessenger(vkInstance, &info, nullptr, &ctx.messenger);
    }
    ctx.device = VulkanDevice::create(*ctx.instance, VulkanDeviceDesc{});
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    if (descriptorBuffer && !ctx.device->info().caps.descriptorBuffer) {
        std::printf("SKIP: device has no VK_EXT_descriptor_buffer\n");
        return kSkip;
    }
    const gs::StreamingCapabilities caps = gs::queryStreamingCapabilities(ctx.device.get());
    if (!caps.ok) {
        std::printf("SKIP: streaming unsupported: %s\n", caps.reason);
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s\n", ctx.device->info().deviceName.c_str());
    BindlessDesc bdesc{};
    bdesc.backend = descriptorBuffer ? BindlessBackendPreference::DescriptorBuffer : BindlessBackendPreference::DescriptorSet;
    if (!ctx.bindless.init(*ctx.device, bdesc)) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    std::printf("bindless backend: %s\n", bindlessBackendName(ctx.bindless.backend()));
    BufferDesc stagingDesc{};
    stagingDesc.size = kStagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "rp_stream.staging";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    return 0;
}

// --- rigs ---------------------------------------------------------------------------------------------
struct Rig {
    gs::GeometryStreaming stream[kLanguages];
    bool built[kLanguages] = {false, false};
};

StreamAsset& asset() {
    static StreamAsset a;
    static bool done = false;
    if (!done) {
        done = true;
        std::string err;
        if (!build_asset(make_bumpy_sphere(6, kRadius, 0.08f), 32u * 1024u, a, &err)) {
            std::fprintf(stderr, "FAIL: asset: %s\n", err.c_str());
        }
    }
    return a;
}

gs::StreamerDesc streamerDesc(const StreamAsset& a) {
    gs::StreamerDesc d{};
    d.budget_pages = a.pages.coarse_page_count + a.pages.page_count() / 3u;
    d.reserve_slots = 8u;
    d.max_loads_per_frame = 6u;
    return d;
}

int makeRig(Context& ctx, const StreamAsset& a, Rig& rig) {
    const gs::StreamKernelLanguage langs[kLanguages] = {gs::StreamKernelLanguage::Slang, gs::StreamKernelLanguage::Glsl};
    u32 built = 0;
    for (u32 k = 0; k < kLanguages; ++k) {
        gs::GeometryStreamingDesc d{};
        d.device = ctx.device.get();
        d.allocator = ctx.allocator.get();
        d.bindless = &ctx.bindless;
        d.upload = &ctx.upload;
        d.dag = &a.mesh.dag;
        d.file = &a.pages;
        d.streamer = streamerDesc(a);
        d.framesInFlight = 3u;
        d.language = langs[k];
        d.name = k == 0u ? "stream.slang" : "stream.glsl";
        rig.built[k] = rig.stream[k].init(d);
        built += rig.built[k] ? 1u : 0u;
        std::printf("kernel %s: %s\n", k == 0u ? "slang" : "glsl", rig.built[k] ? "built, coarse LOD resident" : "not available");
    }
    if (built == 0u) {
        std::printf("SKIP: no geometry streaming kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- read-back ------------------------------------------------------------------------------------
struct CopyRecord {
    rg::BufferRef src, dst;
    u64 dstOffset = 0, bytes = 0;
};
void recordCopy(const rg::PassContext& pc, void* user) {
    const CopyRecord& c = *static_cast<const CopyRecord*>(user);
    const VkBufferCopy region{0, c.dstOffset, c.bytes};
    vkCmdCopyBuffer(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkBuffer>(pc.buffer(c.src)),
                    static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
}

struct Layout {
    u64 cut[kLanguages] = {};
    u64 draw[kLanguages] = {};
    u64 pool = 0;
    u64 end = 0;
};

Layout makeLayout(const Rig& rig) {
    Layout l{};
    u64 cursor = 0;
    for (u32 k = 0; k < kLanguages; ++k) {
        if (!rig.built[k]) {
            continue;
        }
        l.cut[k] = cursor;
        cursor += (rig.stream[k].cutBuffer().desc.size + 255u) & ~u64{255u};
        l.draw[k] = cursor;
        cursor += (rig.stream[k].drawListBuffer().desc.size + 255u) & ~u64{255u};
    }
    l.pool = cursor;
    const u32 k0 = rig.built[0] ? 0u : 1u;
    cursor += rig.stream[k0].poolBuffer().desc.size;
    l.end = cursor;
    return l;
}

struct FrameState {
    CopyRecord copies[8];
    u32 copyCount = 0;
    Layout layout{};
};

void buildGraph(Context& ctx, Rig& rig, rg::Graph& graph, FrameState& fs, bool readback) {
    graph.reset();
    fs.copyCount = 0;
    gs::StreamGraphRefs refs[kLanguages]{};
    for (u32 k = 0; k < kLanguages; ++k) {
        if (rig.built[k]) {
            refs[k] = rig.stream[k].importInto(graph);
            rig.stream[k].addFrame(graph, refs[k]);
        }
    }
    if (!readback) {
        return;
    }
    const rg::BufferRef rb =
        graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, fs.layout.end, rg::kNoQueue, nullptr, "rp_stream.readback"});
    auto addCopy = [&](rg::BufferRef src, u64 dstOffset, u64 bytes) {
        CopyRecord& c = fs.copies[fs.copyCount++];
        c = CopyRecord{src, rb, dstOffset, bytes};
        graph.addPass("readback.copy", &recordCopy, &c)
            .use(src, rg::Access::TransferSrc, rg::BufferRange{0, bytes})
            .use(rb, rg::Access::TransferDst, rg::BufferRange{dstOffset, bytes});
    };
    for (u32 k = 0; k < kLanguages; ++k) {
        if (rig.built[k]) {
            addCopy(refs[k].cut, fs.layout.cut[k], rig.stream[k].cutBuffer().desc.size);
            addCopy(refs[k].drawList, fs.layout.draw[k], rig.stream[k].drawListBuffer().desc.size);
        }
    }
    const u32 k0 = rig.built[0] ? 0u : 1u;
    addCopy(refs[k0].pool, fs.layout.pool, rig.stream[k0].poolBuffer().desc.size);
    graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
}

bool ensureReadback(Context& ctx, u64 bytes) {
    if (ctx.readback.handle != nullptr && ctx.readback.desc.size >= bytes) {
        return true;
    }
    if (ctx.readback.handle != nullptr) {
        ctx.allocator->destroyBuffer(ctx.readback);
    }
    BufferDesc d{};
    d.size = static_cast<usize>(bytes);
    d.usage = BufferUsage::TransferDst;
    d.memoryUsage = MemoryUsage::GpuToCpu;
    d.name = "rp_stream.readback";
    return ctx.allocator->createBuffer(d, ctx.readback) && ctx.readback.mapped != nullptr;
}

constexpr u32 kFlyFrames = 96u;
constexpr u32 kHold = 16u;

dag::cut_kernel::DagView frameView(u32 f) {
    f32 cam[3];
    flythrough_camera(f, kFlyFrames, kHold, kRadius, cam);
    return make_view(cam, 1.f);
}

bool beginFrames(Context& ctx, Rig& rig, u32 f) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    const dag::cut_kernel::DagView v = frameView(f);
    bool ok = true;
    for (u32 k = 0; k < kLanguages; ++k) {
        if (rig.built[k]) {
            ok = rig.stream[k].beginFrame(ctx.serial, v, ctx.serial - 1u) && ok;
        }
    }
    return ok;
}

bool finishFrame(Context& ctx, rg::Graph& graph) {
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    ctx.bindless.collectRetired(ctx.serial);
    return result.ok && waited;
}

void destroyRig(Rig& rig) {
    for (gs::GeometryStreaming& s : rig.stream) {
        s.destroy();
    }
}

// --- parity -------------------------------------------------------------------------------------------
int runParity(Context& ctx) {
    StreamAsset& a = asset();
    if (a.pages.page_count() == 0u) {
        return 1;
    }
    Rig rig;
    const int rc = makeRig(ctx, a, rig);
    if (rc != 0) {
        return rc;
    }
    const u32 n = a.mesh.dag.cluster_count();
    const u32 pages = a.pages.page_count();
    std::printf("asset: %u clusters, %zu groups, %u pages (%u coarse), budget %u pages + %u reserve slots, 32 KiB pages\n", n,
                a.mesh.dag.groups.size(), pages, a.pages.coarse_page_count, streamerDesc(a).budget_pages, 8u);
    // Source area (leaf cut from the file's payloads).
    std::vector<u32> leaves(n, 0u);
    for (u32 c = 0; c < a.mesh.dag.leaf_cluster_count; ++c) {
        leaves[c] = 1u;
    }
    const f64 area = check_cut(a, leaves, [&](u32 p) { return a.pages.page_payload(p); }).area;
    rg::Graph graph;
    FrameState fs;
    fs.layout = makeLayout(rig);
    if (!ensureReadback(ctx, fs.layout.end)) {
        std::fprintf(stderr, "FAIL: readback buffer\n");
        return 1;
    }
    const u32 k0 = rig.built[0] ? 0u : 1u;
    u32 cutMismatch = 0, fbMismatch = 0, listMismatch = 0, langMismatch = 0, missing = 0, coarseMissing = 0, overBudget = 0;
    u32 loads = 0, evictions = 0, feedbackFrames = 0, drawnMax = 0;
    f64 minArea = 10.0, maxArea = 0.0;
    std::vector<u32> bits;
    for (u32 f = 0; f < kFlyFrames; ++f) {
        if (!beginFrames(ctx, rig, f)) {
            expect(false, "beginFrame " + std::to_string(f));
            break;
        }
        ctx.upload.flush();
        buildGraph(ctx, rig, graph, fs, true);
        if (!finishFrame(ctx, graph)) {
            expect(false, "frame " + std::to_string(f) + " executed");
            break;
        }
        const u8* rb = static_cast<const u8*>(ctx.readback.mapped);
        const std::vector<u32>* ref0 = nullptr;
        std::vector<u32> refCut[kLanguages];
        std::vector<u32> gpuFeedback[kLanguages];
        for (u32 k = 0; k < kLanguages; ++k) {
            if (!rig.built[k]) {
                continue;
            }
            gs::GeometryStreaming& s = rig.stream[k];
            bits.assign(s.frameResidentBits(), s.frameResidentBits() + s.frameResidentWords());
            // CPU reference on this frame's inputs.
            std::vector<u32> cut(n, 0u), feedback(sk::feedback_layout(pages).words, 0u), draw(n, 0u);
            sk::Params p{};
            p.links = fuse::kernel::make_span(static_cast<const dag::DagClusterLink*>(a.mesh.dag.links.data()), n);
            p.info = fuse::kernel::make_span(static_cast<const gs::StreamClusterInfo*>(a.info.data()), n);
            p.resident = fuse::kernel::make_span(static_cast<const u32*>(bits.data()), static_cast<u32>(bits.size()));
            p.view = s.frameView();
            p.page_count = pages;
            p.cut = fuse::kernel::make_span(cut.data(), n);
            p.feedback = fuse::kernel::make_span(feedback.data(), static_cast<u32>(feedback.size()));
            p.draw_list = fuse::kernel::make_span(draw.data(), n);
            (void)fuse::kernel::launch(fuse::kernel::Backend::CpuReference, sk::make_launch(n), sk::Kernel{}, p);
            const u32* gpuCut = reinterpret_cast<const u32*>(rb + fs.layout.cut[k]);
            const u32* gpuDraw = reinterpret_cast<const u32*>(rb + fs.layout.draw[k]);
            const u32* fb = s.feedbackHost(ctx.serial);
            if (fb == nullptr) {
                expect(false, "feedback ring slot of this frame");
                continue;
            }
            const sk::FeedbackLayout l = sk::feedback_layout(pages);
            cutMismatch += std::memcmp(gpuCut, cut.data(), n * 4u) != 0 ? 1u : 0u;
            fbMismatch += std::memcmp(fb, feedback.data(), l.list * 4u) != 0 ? 1u : 0u;
            const bool lists = std::set<u32>(fb + l.list, fb + l.list + std::min(fb[0], pages)) ==
                                   std::set<u32>(feedback.begin() + l.list, feedback.begin() + l.list + feedback[0]) &&
                               std::set<u32>(gpuDraw, gpuDraw + std::min(fb[1], n)) ==
                                   std::set<u32>(draw.begin(), draw.begin() + feedback[1]);
            listMismatch += lists ? 0u : 1u;
            refCut[k] = cut;
            gpuFeedback[k].assign(fb, fb + l.list);
            if (ref0 == nullptr) {
                ref0 = &refCut[k];
            }
            drawnMax = std::max(drawnMax, fb[1]);
            if (f < 3u && k == k0) {
                std::printf("  frame %u: drawn %u clusters, %u pages requested\n", f, fb[1], fb[0]);
            }
        }
        if (rig.built[0] && rig.built[1]) {
            const bool same = refCut[0] == refCut[1] && gpuFeedback[0] == gpuFeedback[1] &&
                              std::memcmp(rb + fs.layout.cut[0], rb + fs.layout.cut[1], n * 4u) == 0 &&
                              std::equal(rig.stream[0].frameResidentBits(),
                                         rig.stream[0].frameResidentBits() + rig.stream[0].frameResidentWords(),
                                         rig.stream[1].frameResidentBits());
            langMismatch += same ? 0u : 1u;
            if (!same && langMismatch <= 3u) {
                std::fprintf(stderr, "  frame %u slang != glsl: ref cut %d, feedback %d, gpu cut %d, bits %d\n", f,
                             refCut[0] == refCut[1] ? 1 : 0, gpuFeedback[0] == gpuFeedback[1] ? 1 : 0,
                             std::memcmp(rb + fs.layout.cut[0], rb + fs.layout.cut[1], n * 4u) == 0 ? 1 : 0,
                             std::equal(rig.stream[0].frameResidentBits(),
                                        rig.stream[0].frameResidentBits() + rig.stream[0].frameResidentWords(),
                                        rig.stream[1].frameResidentBits())
                                 ? 1
                                 : 0);
            }
        }
        // Only resident pages, decoded from the read-back pool of rig 0.
        const gs::GeometryStreaming& s0 = rig.stream[k0];
        const gs::ClusterStreamer& st = s0.streamer();
        std::vector<u32> gpuCut(reinterpret_cast<const u32*>(rb + fs.layout.cut[k0]),
                                reinterpret_cast<const u32*>(rb + fs.layout.cut[k0]) + n);
        const CutCheck chk = check_cut(a, gpuCut, [&](u32 page) -> const u8* {
            const u32 slot = st.resident_slot(page);
            return slot == gs::kPageNone ? nullptr : rb + fs.layout.pool + static_cast<u64>(slot) * a.pages.page_bytes;
        });
        const f64 ratio = chk.area / area;
        minArea = std::min(minArea, ratio);
        maxArea = std::max(maxArea, ratio);
        const bool miss = !chk.pages_ok || chk.open_edges != 0u || ratio < 0.7 || ratio > 1.2 || chk.clusters == 0u;
        missing += miss ? 1u : 0u;
        if (miss && missing <= 3u) {
            std::fprintf(stderr, "  frame %u: pages_ok %d, open edges %llu, area %.3f, clusters %u\n", f, chk.pages_ok ? 1 : 0,
                         static_cast<unsigned long long>(chk.open_edges), ratio, chk.clusters);
        }
        coarseMissing += st.coarse_resident() ? 0u : 1u;
        overBudget += st.residency().resident_bytes() + st.residency().loading_bytes() >
                              static_cast<u64>(st.budget_pages()) * a.pages.page_bytes
                          ? 1u
                          : 0u;
        loads += st.stats().loads;
        evictions += st.stats().evictions;
        feedbackFrames += s0.stats().feedbackSerial != 0u ? 1u : 0u;
    }
    std::printf("parity: %u frames, languages %s%s: cut mismatches %u, feedback mismatches %u, list mismatches %u, "
                "slang != glsl %u\n",
                kFlyFrames, rig.built[0] ? "slang" : "", rig.built[1] ? " glsl" : "", cutMismatch, fbMismatch, listMismatch,
                langMismatch);
    std::printf("flythrough (GPU): %u frames missing geometry, %u missing coarse, %u over budget, area %.3f..%.3f, %u loads, "
                "%u evictions, %u frames driven by read-back feedback, up to %u clusters drawn\n",
                missing, coarseMissing, overBudget, minArea, maxArea, loads, evictions, feedbackFrames, drawnMax);
    expect(cutMismatch == 0u, "GPU cut flags == CPU reference");
    expect(fbMismatch == 0u, "GPU feedback (counts + per-page priorities) == CPU reference");
    expect(listMismatch == 0u, "GPU request / draw lists == CPU reference (sets)");
    expect(langMismatch == 0u, "Slang == GLSL");
    expect(missing == 0u, "0 frames missing geometry (watertight from resident pool bytes)");
    expect(coarseMissing == 0u, "coarse LOD resident on every frame");
    expect(overBudget == 0u, "page budget never exceeded");
    expect(loads > 20u && evictions > 0u, "pages stream in and are evicted");
    expect(feedbackFrames + 1u >= kFlyFrames, "every frame after the first applies read-back feedback");
    destroyRig(rig);
    return 0;
}

// --- zero_alloc ---------------------------------------------------------------------------------------
void hookBegin(const rg::PassContext&, const char* name, void*) { t_count = std::strncmp(name, "stream.", 7) == 0; }
void hookEnd(const rg::PassContext&, const char*, void*) { t_count = false; }

int runZeroAlloc(Context& ctx, bool countAllocations) {
    StreamAsset& a = asset();
    Rig rig;
    const int rc = makeRig(ctx, a, rig);
    if (rc != 0) {
        return rc;
    }
    rg::Graph graph;
    FrameState fs;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long begin = 0, callbacks = 0, build = 0;
    u32 loads = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        t_allocations = 0;
        t_count = measure;
        const bool ok = beginFrames(ctx, rig, frame % kFlyFrames);
        t_count = false;
        const unsigned long long b = t_allocations;
        ctx.upload.flush();
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, rig, graph, fs, false);
        t_count = false;
        const unsigned long long g = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long cb = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        ctx.bindless.collectRetired(ctx.serial);
        expect(ok && result.ok && waited, "frame ok");
        const u32 k0 = rig.built[0] ? 0u : 1u;
        if (measure) {
            begin += b;
            callbacks += cb;
            build += g;
            loads += rig.stream[k0].streamer().stats().loads;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames of the fly-through (%u page loads while measuring)\n"
                    "  GeometryStreaming::beginFrame (feedback, residency, uploads): %llu operator-new calls\n"
                    "  stream.* pass callbacks: %llu, whole graph build: %llu\n",
                    kTotal - kWarmup, loads, begin, callbacks, build);
        expect(begin == 0u, "beginFrame makes no steady-state heap allocations");
        expect(callbacks == 0u, "stream.* pass callbacks make no heap allocations");
        expect(build == 0u, "graph build makes no steady-state heap allocations");
        expect(loads > 0u, "pages were uploaded while measuring");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok\n", kTotal);
    }
    destroyRig(rig);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "parity";
    std::string backend = "set";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--mode") == 0) {
            mode = argv[i + 1];
        } else if (std::strcmp(argv[i], "--backend") == 0) {
            backend = argv[i + 1];
        }
    }
    int rc = 0;
    {
        Context ctx;
        const int setupRc = setup(ctx, backend == "buffer", true);
        if (setupRc != 0) {
            return setupRc;
        }
        if (mode == "parity") {
            rc = runParity(ctx);
        } else if (mode == "zero_alloc") {
            rc = runZeroAlloc(ctx, false);
        } else {
            std::fprintf(stderr, "unknown --mode %s\n", mode.c_str());
            return 2;
        }
    }
    if (rc == kSkip) {
        return kSkip;
    }
    if (mode == "zero_alloc" && rc == 0) {
        Context counted;
        const int setupRc = setup(counted, backend == "buffer", false);
        if (setupRc != 0) {
            return setupRc;
        }
        rc = runZeroAlloc(counted, true);
    }
    std::printf("validation messages: %u\n", g_messages);
    if (rc != 0 || g_failures != 0 || g_messages != 0u) {
        std::fprintf(stderr, "FAIL: %d failure(s), %u validation message(s)\n", g_failures + rc, g_messages);
        return 1;
    }
    std::printf("PASS %s (%s)\n", mode.c_str(), backend.c_str());
    return 0;
}

#endif
