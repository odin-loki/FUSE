// FUSE Relight RL-5.4 Vulkan gates (Lavapipe; PE under Wine in the MinGW tree): the hash-grid radiance cache on the
// GPU (RadianceCacheGpu, radiance_cache_gpu.hpp: the "relight.radiance_cache.*" passes before PathTracerGpu's
// "relight.pt.trace") against the CPU reference (RadianceCacheCpu, the same single-source core
// kernels/radiance_cache_core.h / radiance_cache_path.h) on the RL-5.1 Cornell scene.
//
//   parity       per kernel language, kParityFrames frames (the first clears the table):
//                cells      the GPU's cells after update + resolve == the CPU replay of both stages on the GPU's own
//                           read-back inputs (the previous frame's table, this frame's records and params): the same key
//                           set (cell ids), integer words exact, resolved radiance / counts within 1e-5 relative;
//                query      the GPU's answer at every training record ("relight.radiance_cache.probe") == the CPU
//                           lookup in the GPU's table (hit flags equal, radiance within 1e-5 relative);
//                train      the GPU's training records == RadianceCacheCpu::train on the same frame (same training
//                           pixels; >= 90% of the records within 1e-3: the GPU traverses the TLAS in float, the CPU
//                           oracle in double);
//                trace      the GPU path tracer's cache-terminated radiance == renderReference with the hook on the GPU's
//                           table for >= 95% of the pixels (1e-3), and the header's query / hit counters within 5%;
//                converge   64 GPU frames with the cache: image mean within 3% of the CPU reference (1024 spp), finite.
//   determinism  two GPU runs give bit-identical radiance and identical cells per key.
//   zero_alloc   80 steady-state frames: no operator-new call in PathTracerGpu / RadianceCacheGpu beginFrame, the graph
//                build (importInto + addPasses + addTracePass + read-back) and the relight.* pass callbacks.
// Validation: every run is under the Khronos layer with sync validation (native) or the host-injected layer (Wine,
// --external-validation with a negative control); any message fails. Exit 77 = skip.
#include "pt_test_scenes.hpp"

#include <fuse/relight/render/pathtrace/pt_gpu.hpp>
#include <fuse/relight/render/pathtrace/pt_reference.hpp>
#include <fuse/relight/render/pathtrace/radiance_cache.hpp>
#include <fuse/relight/render/pathtrace/radiance_cache_gpu.hpp>

#include "pt_reference_kernels.hpp"

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace {
thread_local bool t_count = false;
thread_local unsigned long long t_allocations = 0;
} // namespace

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

[[maybe_unused]] void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}
[[maybe_unused]] void check(bool condition, const std::string& message) { check(condition, message.c_str()); }
} // namespace

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    std::printf("SKIP: stub build (no Vulkan backend)\n");
    return kSkip;
}

#else

namespace {

using namespace fuse;
using namespace fuse::renderer;
using namespace pt_test;

struct BorrowedStandIns {
    FUSE_RELIGHT_OPTION("rtx", float, sceneScale, 1.f, "Test stand-in for the scene package's option.");
};

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr usize kStagingBytes = 16u * 1024u * 1024u;
constexpr u32 kW = 24u, kH = 24u;
constexpr u32 kParityFrames = 5u;

u32 g_messages = 0;
bool g_countMessages = true;

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) ==
            0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    ++g_messages;
    if (g_countMessages && g_messages <= 20u) {
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

struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    std::unique_ptr<rg::Executor> executor;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    BindlessDescriptors bindless;
    Buffer staging{};
    UploadQueue upload;
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (allocator != nullptr && staging.handle != nullptr) {
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

/// Negative control for a layer the process cannot enumerate (Wine: the host loader injects it): an invalid sampler
/// (VUID-VkSamplerCreateInfo-mipLodBias-01069) must produce a message.
u32 validationControl(VkPhysicalDevice pd, VkDevice device) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    const u32 before = g_messages;
    g_countMessages = false;
    VkSamplerCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.mipLodBias = props.limits.maxSamplerLodBias + 64.f;
    ci.maxLod = 1.f;
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device, &ci, nullptr, &sampler) == VK_SUCCESS) {
        vkDestroySampler(device, sampler, nullptr);
    }
    const u32 produced = g_messages - before;
    g_messages = before;
    g_countMessages = true;
    return produced;
}

enum class Validation { Layer, External, Off };

int setup(Context& ctx, Validation validation) {
    if (validation == Validation::Layer) {
        if (!layerAvailable(kValidationLayer)) {
            std::printf("SKIP: %s not installed (--no-validation runs without)\n", kValidationLayer);
            return kSkip;
        }
        setEnv("VK_INSTANCE_LAYERS", kValidationLayer);
        setEnv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
        setEnv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true");
        setEnv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK");
    } else if (validation == Validation::Off) {
        setEnv("VK_INSTANCE_LAYERS", "");
    }
    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "rl_radiance_cache_vk";
    instanceDesc.enableValidation = validation == Validation::Layer;
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
    if (validation != Validation::Off) {
        if (createMessenger == nullptr) {
            std::printf("SKIP: VK_EXT_debug_utils unavailable\n");
            return kSkip;
        }
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = onMessage;
        createMessenger(vkInstance, &info, nullptr, &ctx.messenger);
    }
    ctx.device = VulkanDevice::create(*ctx.instance, VulkanDeviceDesc{});
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    if (validation == Validation::External) {
        const u32 control =
            validationControl(static_cast<VkPhysicalDevice>(ctx.device->nativePhysicalDevice()), ctx.vkDevice);
        if (control == 0u) {
            std::printf("SKIP: no validation layer reached this process (negative control silent)\n");
            return kSkip;
        }
        std::printf("external validation layer live (negative control: %u message(s), not counted)\n", control);
    }
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s (%s)\n", ctx.device->info().deviceName.c_str(),
                validation == Validation::Off ? "no validation layer" : "validation + sync validation");
    if (!ctx.bindless.init(*ctx.device, BindlessDesc{})) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    BufferDesc d{};
    d.size = kStagingBytes;
    d.usage = BufferUsage::TransferSrc;
    d.memoryUsage = MemoryUsage::CpuToGpu;
    d.name = "rl_radiance_cache.staging";
    if (!ctx.allocator->createBuffer(d, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    return 0;
}

struct Gpus {
    pt::PathTracerGpu pt;
    pt::RadianceCacheGpu rc;
};

bool initGpus(Context& ctx, Gpus& g, pt::PtKernelLanguage language) {
    pt::PathTracerGpuDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.language = language;
    d.framesInFlight = 3;
    if (!g.pt.init(d)) {
        return false;
    }
    pt::RadianceCacheGpuDesc rd{};
    rd.device = ctx.device.get();
    rd.allocator = ctx.allocator.get();
    rd.bindless = &ctx.bindless;
    rd.language = language;
    rd.framesInFlight = 3;
    return g.rc.init(rd);
}

bool compile(const pt::PtScene& s, pt::PtCompiledScene& c) {
    std::string error;
    const bool ok = c.compile(s, {}, &error);
    check(ok, "compile: " + error);
    return ok;
}

pt::RadianceCacheSettings rcSettings() {
    pt::RadianceCacheSettings s;
    s.enabled = true;
    s.capacity = 1u << 14;
    s.cellSize = 0.05f;
    s.cellPixels = 2.f;
    s.trainStride = 2;
    s.minSamples = 4.f;
    s.maxSamples = 512.f;
    s.maxAge = 16;
    return s;
}

pt::PtSettings ptSettings() {
    pt::PtSettings st;
    st.maxBounces = 4;
    st.rrStart = 3;
    st.samplesPerPixel = 1;
    return pt::withRadianceCache(st);
}

bool buildGraph(Gpus& g, rg::Graph& graph) {
    graph.reset();
    const pt::PtGraphRefs refs = g.pt.importInto(graph);
    if (!refs.valid || !g.rc.addPasses(graph, g.pt, refs) || !g.pt.addTracePass(graph, refs)) {
        return false;
    }
    rg::PassBuilder rb = graph.addPass("readback.host", nullptr, nullptr);
    rb.use(refs.outputs, rg::Access::HostRead)
        .use(g.rc.graphRefs().table, rg::Access::HostRead)
        .use(g.rc.graphRefs().records, rg::Access::HostRead);
    if (g.rc.graphRefs().probe.valid()) {
        rb.use(g.rc.graphRefs().probe, rg::Access::HostRead);
    }
    return true;
}

struct FrameAllocs {
    unsigned long long begin = 0;
    unsigned long long graph = 0;
};

bool runFrame(Context& ctx, Gpus& g, rg::Graph& graph, pt::PtCompiledScene& c, const pt::PtFrameDesc& fd,
              const pt::RadianceCacheSettings& rs, bool measure = false, FrameAllocs* allocs = nullptr) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    t_allocations = 0;
    t_count = measure;
    bool ok = g.pt.setScene(c) && g.pt.beginFrame(ctx.serial, c, fd) && g.rc.beginFrame(ctx.serial, c, fd, rs);
    t_count = false;
    const unsigned long long a0 = t_allocations;
    ctx.upload.flush();
    t_allocations = 0;
    t_count = measure;
    ok = ok && buildGraph(g, graph);
    t_count = false;
    const unsigned long long a1 = t_allocations;
    t_allocations = 0;
    if (allocs != nullptr) {
        allocs->begin += a0;
        allocs->graph += a1;
    }
    if (!ok) {
        std::fprintf(stderr, "  frame setup failed: %s / %s\n", g.pt.reason(), g.rc.reason());
        return false;
    }
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    g.pt.collectRetired(ctx.serial);
    g.rc.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    return result.ok && waited;
}

const float* ptSection(const pt::PathTracerGpu& gpu, u32 s) {
    return reinterpret_cast<const float*>(static_cast<const u8*>(gpu.mappedOutputs()) + u64(s) * gpu.outputStride());
}

bool closeTo(float a, float b, float tol) { return std::fabs(a - b) <= tol * (1.f + std::fabs(b)); }

pt::PtFrameDesc frameDesc(u32 frame, u32 seed) {
    pt::PtFrameDesc fd;
    fd.width = kW;
    fd.height = kH;
    fd.frameSeed = seed;
    fd.sampleBase = frame;
    fd.accumulate = frame != 0u;
    fd.settings = ptSettings();
    return fd;
}

/// Per-key comparison of two tables: same key set, integer words exact, float words within `tol` relative.
bool cellsAgree(const u32* a, const u32* b, u32 capacity, float tol, std::size_t* cells, std::size_t* mismatches) {
    std::vector<pt::RadianceCacheEntry> ea, eb;
    pt::radianceCacheEntries(a, capacity, ea);
    pt::radianceCacheEntries(b, capacity, eb);
    *cells = ea.size();
    *mismatches = 0;
    if (ea.size() != eb.size()) {
        *mismatches = ea.size() > eb.size() ? ea.size() - eb.size() : eb.size() - ea.size();
        return false;
    }
    for (std::size_t i = 0; i < ea.size(); ++i) {
        bool same = ea[i].check == eb[i].check;
        for (u32 w = 0; w < 7u && same; ++w) { // slot words 1..7: sums, count, age, tag, reserved
            same = ea[i].words[w] == eb[i].words[w];
        }
        for (u32 w = 7; w < 11u && same; ++w) { // 8..11: resolved radiance and count (f32)
            float fa, fb;
            std::memcpy(&fa, &ea[i].words[w], 4);
            std::memcpy(&fb, &eb[i].words[w], 4);
            same = closeTo(fa, fb, tol);
        }
        *mismatches += same ? 0u : 1u;
    }
    return *mismatches == 0u;
}

void parityCase(Context& ctx, Gpus& g, rg::Graph& graph) {
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c)) {
        return;
    }
    const pt::RadianceCacheSettings rs = rcSettings();
    g.rc.reset();
    g.rc.enableProbe(true);
    std::vector<u32> prev;
    pt::RadianceCacheCpu cpu;
    cpu.configure(rs);
    const u64 words = pt::radianceCacheTableWords(rs);
    for (u32 f = 0; f < kParityFrames; ++f) {
        const pt::PtFrameDesc fd = frameDesc(f, 21u);
        if (!runFrame(ctx, g, graph, c, fd, rs)) {
            check(false, "parity: frame");
            return;
        }
        const u32* table = g.rc.mappedTable();
        const pt::Word* records = g.rc.mappedRecords();
        const pt::Word* probe = g.rc.mappedProbe();
        const u32 nrec = g.rc.recordCount();
        const pt::Word* params = g.rc.params();
        // cells: replay update + resolve on the GPU's inputs.
        std::vector<u32> replay = f == 0u ? std::vector<u32>(words, 0u) : prev;
        for (u32 w = pt::kRcCounterQueries; w < pt::kRcHeaderWords; ++w) {
            replay[w] = 0u; // the train stage's header write
        }
        const bool replayed = cpu.replayUpdate(params, records, nrec, replay) && cpu.replayResolve(params, replay);
        std::size_t cells = 0, bad = 0;
        const bool agree = replayed && cellsAgree(table, replay.data(), g.rc.capacity(), 1e-5f, &cells, &bad);
        const pt::RadianceCacheStats gs = pt::radianceCacheCounters(table);
        const pt::RadianceCacheStats cs = pt::radianceCacheCounters(replay.data());
        // query: the probe vs the CPU lookup in the GPU table.
        u32 valid = 0, qSame = 0, hits = 0;
        for (u32 i = 0; i < nrec; ++i) {
            const pt::Word& a = records[i * 3u];
            if (!(a.w > 0.5f)) {
                continue;
            }
            ++valid;
            const pt::Word& nw = records[i * 3u + 1u];
            const float p[3] = {a.x, a.y, a.z};
            const float n[3] = {nw.x, nw.y, nw.z};
            float L[3];
            float samples = 0.f;
            const bool hit = pt::radianceCacheLookup(params, table, p, n, L, &samples);
            const pt::Word& q = probe[i];
            const bool gpuHit = q.w >= 0.f;
            hits += hit ? 1u : 0u;
            qSame += (hit == gpuHit && (!hit || (closeTo(q.x, L[0], 1e-5f) && closeTo(q.y, L[1], 1e-5f) &&
                                                 closeTo(q.z, L[2], 1e-5f) && q.w == samples)))
                         ? 1u
                         : 0u;
        }
        // train: the CPU's records of the same frame (same frame index, same params).
        cpu.adopt(params, prev.empty() ? replay.data() : prev.data(), words);
        (void)cpu.train(c, fd.settings, kW, kH, fd.frameSeed, fd.sampleBase, kernel::Backend::CpuParallel);
        u32 tSame = 0;
        const std::vector<pt::Word>& cr = cpu.records();
        for (u32 i = 0; i < nrec && i * 3u + 2u < cr.size(); ++i) {
            const pt::Word& ga = records[i * 3u];
            const pt::Word& ca = cr[i * 3u];
            const pt::Word& gl = records[i * 3u + 2u];
            const pt::Word& cl = cr[i * 3u + 2u];
            const bool same = ga.w == ca.w && (ga.w < 0.5f || (closeTo(ga.x, ca.x, 1e-3f) && closeTo(ga.y, ca.y, 1e-3f) &&
                                                               closeTo(ga.z, ca.z, 1e-3f) && closeTo(gl.x, cl.x, 1e-3f) &&
                                                               closeTo(gl.y, cl.y, 1e-3f) && closeTo(gl.z, cl.z, 1e-3f)));
            tSame += same ? 1u : 0u;
        }
        // trace: the GPU's cache-terminated frame vs renderReference with the hook on the GPU's table.
        pt::RadianceCacheCpu adopted;
        adopted.adopt(params, table, words);
        adopted.table()[pt::kRcCounterQueries] = 0u;
        adopted.table()[pt::kRcCounterHits] = 0u;
        pt::PtReferenceImage img;
        img.resize(kW, kH);
        pt::renderReference(c, fd.settings, kW, kH, fd.frameSeed, fd.sampleBase, 1u, img, kernel::Backend::CpuParallel,
                            nullptr, nullptr, nullptr, adopted.hook(kW, kH));
        const float* rad = ptSection(g.pt, pt::kPtOutRadiance);
        u32 pSame = 0;
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                const std::size_t i = std::size_t(y) * kW + x;
                bool same = true;
                for (u32 k = 0; k < 3u; ++k) {
                    same = same && closeTo(rad[i * 4u + k], float(img.mean(x, y, k)), 1e-3f);
                }
                pSame += same ? 1u : 0u;
            }
        }
        const pt::RadianceCacheStats as = adopted.stats();
        const double qRel = std::fabs(double(gs.queries) - double(as.queries)) / std::max(1.0, double(as.queries));
        const double hRel = std::fabs(double(gs.hits) - double(as.hits)) / std::max(1.0, double(as.hits));
        std::printf("  frame %u: cells %zu (mismatch %zu) inserts gpu %u / cpu %u, live %u / %u; probe %u/%u equal "
                    "(%u hits); train %u/%u records equal; trace %u/%u pixels equal, queries %u / %u, hits %u / %u\n",
                    f, cells, bad, gs.inserts, cs.inserts, gs.live, cs.live, qSame, valid, hits, tSame, nrec, pSame,
                    kW * kH, gs.queries, as.queries, gs.hits, as.hits);
        check(agree, "parity: cells after update + resolve == CPU replay on the GPU's inputs (per key)");
        check(gs.inserts == cs.inserts && gs.dropped == cs.dropped && gs.live == cs.live && gs.evicted == cs.evicted,
              "parity: update / resolve counters");
        check(qSame == valid, "parity: query (probe) == CPU lookup in the GPU table");
        check(f == 0u || hits > 0u, "parity: the probe finds cells");
        check(tSame >= (nrec * 9u) / 10u, "parity: training records == CPU training (>= 90%)");
        check(pSame >= (kW * kH * 95u) / 100u, "parity: cache-terminated trace == CPU hook on the GPU table (>= 95%)");
        check(qRel <= 0.05 && hRel <= 0.05, "parity: query / hit counters within 5%");
        prev.assign(table, table + words);
    }
    g.rc.enableProbe(false);
}

void convergeCase(Context& ctx, Gpus& g, rg::Graph& graph) {
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c)) {
        return;
    }
    const pt::RadianceCacheSettings rs = rcSettings();
    g.rc.reset();
    constexpr u32 kFrames = 64u, kWarm = 8u, kRefSpp = 1024u;
    std::vector<double> sum(std::size_t(kW) * kH * 3u, 0.0);
    bool ok = true;
    for (u32 f = 0; f < kWarm + kFrames && ok; ++f) {
        pt::PtFrameDesc fd = frameDesc(f, 33u);
        fd.accumulate = false;
        ok = runFrame(ctx, g, graph, c, fd, rs);
        if (ok && f >= kWarm) {
            const float* rad = ptSection(g.pt, pt::kPtOutRadiance);
            for (std::size_t i = 0; i < std::size_t(kW) * kH; ++i) {
                for (u32 k = 0; k < 3u; ++k) {
                    sum[i * 3u + k] += double(rad[i * 4u + k]) / kFrames;
                }
            }
        }
    }
    check(ok, "converge: frames");
    pt::PtSettings plain = ptSettings();
    plain.flags &= ~pt::kPtFlagRadianceCache;
    pt::PtReferenceImage ref;
    ref.resize(kW, kH);
    pt::renderReference(c, plain, kW, kH, 777u, 0u, kRefSpp, ref);
    double sg[3] = {0, 0, 0}, sr[3] = {0, 0, 0};
    bool finite = true;
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            for (u32 k = 0; k < 3u; ++k) {
                const double v = sum[(std::size_t(y) * kW + x) * 3u + k];
                finite = finite && std::isfinite(v);
                sg[k] += v;
                sr[k] += ref.mean(x, y, k);
            }
        }
    }
    double worst = 0.0;
    for (u32 k = 0; k < 3u; ++k) {
        worst = std::max(worst, std::fabs(sg[k] - sr[k]) / std::max(sr[k], 1e-9));
    }
    const pt::RadianceCacheStats s = pt::radianceCacheCounters(g.rc.mappedTable());
    std::printf("  converge: %u GPU frames with the cache vs CPU reference (%u spp): worst channel image bias %.4f; "
                "last frame hit rate %.3f (%u / %u), %u live cells\n",
                kFrames, kRefSpp, worst, s.hitRate(), s.hits, s.queries, s.live);
    check(finite && worst <= 0.03, "converge: GPU cache-terminated image mean within 3% of the CPU reference");
    check(s.hitRate() > 0.8, "converge: hit rate after warm-up");
}

void determinismCase(Context& ctx, Gpus& g, rg::Graph& graph) {
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c)) {
        return;
    }
    const pt::RadianceCacheSettings rs = rcSettings();
    std::vector<float> radiance[2];
    std::vector<u32> tables[2];
    for (u32 run = 0; run < 2; ++run) {
        g.rc.reset();
        for (u32 f = 0; f < 3u; ++f) {
            if (!runFrame(ctx, g, graph, c, frameDesc(f, 5u), rs)) {
                check(false, "determinism: frame");
                return;
            }
        }
        const float* rad = ptSection(g.pt, pt::kPtOutRadiance);
        radiance[run].assign(rad, rad + std::size_t(kW) * kH * 4u);
        const u32* t = g.rc.mappedTable();
        tables[run].assign(t, t + pt::radianceCacheTableWords(rs));
    }
    std::size_t cells = 0, bad = 0;
    const bool same = cellsAgree(tables[0].data(), tables[1].data(), rs.capacity, 0.f, &cells, &bad);
    const bool radSame = std::memcmp(radiance[0].data(), radiance[1].data(), radiance[0].size() * sizeof(float)) == 0;
    std::printf("  determinism: %zu cells identical per key: %s; radiance bit-identical: %s\n", cells,
                same ? "yes" : "no", radSame ? "yes" : "no");
    check(same && radSame, "determinism: two GPU runs identical");
}

struct Lang {
    pt::PtKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[] = {{pt::PtKernelLanguage::Slang, "slang"}, {pt::PtKernelLanguage::Glsl, "glsl"}};

int runParity(Context& ctx, const std::string& language) {
    u32 languages = 0;
    for (const Lang& lang : kLangs) {
        if (language != "all" && language != lang.name) {
            continue;
        }
        Gpus g;
        if (!initGpus(ctx, g, lang.language)) {
            if (std::strstr(g.pt.reason(), "no path-tracing kernel") != nullptr ||
                std::strstr(g.rc.reason(), "no radiance cache kernel") != nullptr) {
                std::printf("%s: kernel not built, skipped\n", lang.name);
                continue;
            }
            std::printf("SKIP: GPU path tracer unavailable: %s / %s\n", g.pt.reason(), g.rc.reason());
            return kSkip;
        }
        ++languages;
        std::printf("%s kernels:\n", g.rc.kernelLanguage());
        rg::Graph graph;
        parityCase(ctx, g, graph);
        convergeCase(ctx, g, graph);
        determinismCase(ctx, g, graph);
        ctx.executor->waitIdle();
        g.rc.destroy();
        g.pt.destroy();
    }
    if (languages == 0u) {
        std::printf("SKIP: no radiance cache kernel built\n");
        return kSkip;
    }
    return 0;
}

thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "relight.", 8) == 0) {
        t_inPass = true;
        t_count = true;
    }
}

void hookEnd(const rg::PassContext&, const char*, void*) {
    if (t_inPass) {
        t_inPass = false;
        t_count = false;
    }
}

int runZeroAlloc(Context& ctx, bool countAllocations) {
    Gpus g;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initGpus(ctx, g, lang.language)) {
            ok = true;
            break;
        }
        g.rc.destroy();
        g.pt.destroy();
    }
    if (!ok) {
        std::printf("SKIP: GPU path tracer / radiance cache unavailable: %s / %s\n", g.pt.reason(), g.rc.reason());
        return kSkip;
    }
    pt::PtScene s = cornell(true);
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return 1;
    }
    rg::Graph graph;
    constexpr u32 kWarmup = 16, kTotal = 80;
    FrameAllocs allocs;
    unsigned long long updates = 0, callbacks = 0;
    u32 reallocations = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    const pt::RadianceCacheSettings rs = rcSettings();
    const u32 moving = static_cast<u32>(s.instances.size()) - 1u;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        s.instances[moving].objectToWorld = translate(0.01f * float(frame % 7u), 0.f, 0.f);
        t_allocations = 0;
        t_count = measure;
        const bool updated = c.update(s);
        t_count = false;
        const unsigned long long a0 = t_allocations;
        const pt::PtFrameDesc fd = frameDesc(frame, 1u);
        FrameAllocs fa;
        t_allocations = 0;
        const bool ran = runFrame(ctx, g, graph, c, fd, rs, measure, &fa);
        const unsigned long long a3 = t_allocations;
        check(updated && ran, "zero_alloc: frame ok");
        if (measure) {
            updates += a0;
            allocs.begin += fa.begin;
            allocs.graph += fa.graph;
            callbacks += a3;
        }
        if (frame == kWarmup) {
            reallocations = g.pt.stats().reallocations + g.rc.stats().reallocations;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    check(g.pt.stats().reallocations + g.rc.stats().reallocations == reallocations,
          "zero_alloc: no reallocation in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (%s kernels, %u steady-state frames, %u radiance cache passes / frame):\n"
                    "  PtCompiledScene::update: %llu operator-new calls\n"
                    "  PathTracerGpu + RadianceCacheGpu setScene / beginFrame: %llu\n"
                    "  graph build (importInto + addPasses + addTracePass + read-back): %llu\n"
                    "  relight.* pass callbacks: %llu\n",
                    g.rc.kernelLanguage(), kTotal - kWarmup, g.rc.stats().passes, updates, allocs.begin, allocs.graph,
                    callbacks);
        check(updates == 0u && allocs.begin == 0u && allocs.graph == 0u && callbacks == 0u,
              "steady-state radiance cache frames make no heap allocation");
    } else {
        std::printf("zero_alloc frames under validation: %u frames ok (%s)\n", kTotal, g.rc.kernelLanguage());
    }
    ctx.executor->waitIdle();
    g.rc.destroy();
    g.pt.destroy();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string language = "all";
    Validation validation = Validation::Layer;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--language") == 0 && i + 1 < argc) {
            language = argv[++i];
        } else if (std::strcmp(argv[i], "--no-validation") == 0) {
            validation = Validation::Off;
        } else if (std::strcmp(argv[i], "--external-validation") == 0) {
            validation = Validation::External;
        }
    }
    fuse::relight::options::setEnvironmentVariable(fuse::relight::options::kDxvkConfEnvVar, "");
    fuse::relight::options::setEnvironmentVariable(fuse::relight::options::kRtxConfEnvVar, "");
    (void)BorrowedStandIns::sceneScaleObject();
    fuse::relight::options::OptionManager::applyPendingValues(nullptr, false);
    int rc = 0;
    {
        Context ctx;
        const int setupRc = setup(ctx, validation);
        if (setupRc != 0) {
            return setupRc;
        }
        rc = runParity(ctx, language);
        if (rc == 0) {
            rc = runZeroAlloc(ctx, validation != Validation::Layer);
        }
    }
    if (rc == kSkip) {
        return kSkip;
    }
    if (rc == 0 && validation == Validation::Layer) {
        Context counted;
        const int setupRc = setup(counted, Validation::Off);
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
    std::printf("PASS (%s)\n", language.c_str());
    return 0;
}

#endif
