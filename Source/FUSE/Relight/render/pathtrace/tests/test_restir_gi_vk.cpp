// FUSE Relight RL-5.3 Vulkan gates (Lavapipe): ReSTIR GI on the GPU (RestirGiGpu, restir_gi_gpu.hpp: the
// "relight.restir_gi.*" passes before PathTracerGpu's "relight.pt.trace") against the CPU oracle (RestirGiCpu, the same
// single-source core restir_gi_core.h) on the RL-5.1 Cornell scene.
//
//   parity       per kernel language, three frames with deterministic seeds (history on from the second):
//                surface    GPU surface records == CPU records (same instance / primitive / vertex index,
//                           barycentrics and position within 1e-3) on >= 99% of the pixels (the GPU traverses the
//                           TLAS in float, the CPU oracle in double);
//                initial / temporal / spatial / shade   each GPU pass replayed by the CPU oracle on the GPU's own
//                           read-back inputs (surfaces, source and history reservoirs): the same sample (flags, x2 key,
//                           M) with W within 1e-3 relative (initial: the whole reservoir, tail included, within 1e-3)
//                           on >= 97% of the pixels; the shaded indirect light within 1e-3 on >= 97%;
//                the frame   GPU ReSTIR GI accumulation (64 frames) image mean within 3% of the CPU reference path
//                           tracer (1024 spp), finite, and every GI surface matched by the trace pass.
//   determinism  two GPU runs of the same frames give bit-identical ReSTIR GI output and radiance.
//   zero_alloc   64 steady-state frames: no operator-new call in PathTracerGpu / RestirGiGpu beginFrame, the graph
//                build (importInto + addPasses + addTracePass + read-back) and the relight.* pass callbacks.
// Validation: every run is under the Khronos layer with sync validation (native) or the host-injected layer (Wine,
// --external-validation with a negative control); any message fails. Exit 77 = skip.
#include "pt_test_scenes.hpp"

#include <fuse/relight/render/pathtrace/pt_gpu.hpp>
#include <fuse/relight/render/pathtrace/pt_reference.hpp>
#include <fuse/relight/render/pathtrace/restir_gi.hpp>
#include <fuse/relight/render/pathtrace/restir_gi_gpu.hpp>

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
    instanceDesc.appName = "rl_restir_gi_vk";
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
    d.name = "rl_restir_gi.staging";
    if (!ctx.allocator->createBuffer(d, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    return 0;
}

struct Gpus {
    pt::PathTracerGpu pt;
    pt::RestirGiGpu gi;
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
    pt::RestirGiGpuDesc rd{};
    rd.device = ctx.device.get();
    rd.allocator = ctx.allocator.get();
    rd.bindless = &ctx.bindless;
    rd.language = language;
    rd.framesInFlight = 3;
    return g.gi.init(rd);
}

bool compile(const pt::PtScene& s, pt::PtCompiledScene& c) {
    std::string error;
    const bool ok = c.compile(s, {}, &error);
    check(ok, "compile: " + error);
    return ok;
}

pt::RestirGiSettings giSettings() {
    pt::RestirGiSettings d;
    d.enabled = true;
    d.spatialSamples = 4;
    d.spatialIterations = 1;
    d.spatialRadius = 8.f;
    return d;
}

pt::PtSettings ptSettings() {
    pt::PtSettings st;
    st.maxBounces = 3;
    st.samplesPerPixel = 1;
    return pt::withRestirGi(st);
}

bool buildGraph(Gpus& g, rg::Graph& graph) {
    graph.reset();
    const pt::PtGraphRefs refs = g.pt.importInto(graph);
    if (!refs.valid || !g.gi.addPasses(graph, g.pt, refs) || !g.pt.addTracePass(graph, refs)) {
        return false;
    }
    rg::PassBuilder rb = graph.addPass("readback.host", nullptr, nullptr);
    rb.use(refs.outputs, rg::Access::HostRead);
    for (u32 s = 0; s < pt::kRgiSections; ++s) {
        rb.use(g.gi.graphRefs().sections[s], rg::Access::HostRead);
    }
    return true;
}

struct FrameAllocs {
    unsigned long long begin = 0;
    unsigned long long graph = 0;
};

bool runFrame(Context& ctx, Gpus& g, rg::Graph& graph, pt::PtCompiledScene& c, const pt::PtFrameDesc& fd,
              const pt::RestirGiSettings& gi, bool measure = false, FrameAllocs* allocs = nullptr) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    t_allocations = 0;
    t_count = measure;
    bool ok = g.pt.setScene(c) && g.pt.beginFrame(ctx.serial, c, fd) && g.gi.beginFrame(ctx.serial, c, fd, gi);
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
        std::fprintf(stderr, "  frame setup failed: %s / %s\n", g.pt.reason(), g.gi.reason());
        return false;
    }
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    g.pt.collectRetired(ctx.serial);
    g.gi.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    return result.ok && waited;
}

std::vector<pt::Word> readWords(const Gpus& g, u32 section, std::size_t words) {
    const auto* p = static_cast<const pt::Word*>(g.gi.mappedSection(section));
    return std::vector<pt::Word>(p, p + words);
}

const float* ptSection(const pt::PathTracerGpu& gpu, u32 s) {
    return reinterpret_cast<const float*>(static_cast<const u8*>(gpu.mappedOutputs()) + u64(s) * gpu.outputStride());
}

bool closeTo(float a, float b, float tol) { return std::fabs(a - b) <= tol * (1.f + std::fabs(b)); }

/// Reservoir agreement: same sample flags and x2 key (barycentrics within 1e-3), M within 1e-4, W within 1e-3
/// relative; with `full` also the tail (directions, radiance terms, densities) and the residual within 1e-3 relative.
double reservoirAgreement(const std::vector<pt::Word>& gpu, const std::vector<pt::Word>& cpu, u32 pixels, bool full,
                          u32* live) {
    constexpr u32 R = pt::kRgiReservoirWords;
    u32 same = 0;
    u32 withSample = 0;
    auto near4 = [](const pt::Word& a, const pt::Word& b) {
        return closeTo(a.x, b.x, 1e-3f) && closeTo(a.y, b.y, 1e-3f) && closeTo(a.z, b.z, 1e-3f) &&
               closeTo(a.w, b.w, 1e-3f);
    };
    for (u32 i = 0; i < pixels; ++i) {
        const pt::Word* g = &gpu[std::size_t(i) * R];
        const pt::Word* c = &cpu[std::size_t(i) * R];
        const bool has = (u32(c[1].z) & 1u) != 0u;
        withSample += has ? 1u : 0u;
        bool ok = g[1].z == c[1].z && closeTo(g[1].y, c[1].y, 1e-4f);
        if (ok && has) {
            ok = g[0].x == c[0].x && g[0].y == c[0].y && std::fabs(g[0].z - c[0].z) < 1e-3f &&
                 std::fabs(g[0].w - c[0].w) < 1e-3f && closeTo(g[1].x, c[1].x, 1e-3f);
        }
        if (ok && full) {
            for (u32 k = 2; k < R && ok; ++k) {
                ok = near4(g[k], c[k]);
            }
        }
        same += ok ? 1u : 0u;
    }
    if (live != nullptr) {
        *live = withSample;
    }
    return double(same) / double(pixels);
}

void parityCase(Context& ctx, Gpus& g, rg::Graph& graph) {
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c) || !g.pt.setScene(c)) {
        check(false, "parity: scene");
        return;
    }
    const pt::RestirGiSettings gi = giSettings();
    const pt::PtSettings st = ptSettings();
    const u32 pixels = kW * kH;
    constexpr u32 R = pt::kRgiReservoirWords;
    constexpr u32 kSeed = 17u;
    pt::RestirGiCpu cpu;
    g.gi.resetHistory();
    std::vector<pt::Word> prevSurf;
    std::vector<pt::Word> prevHistory;
    for (u32 f = 0; f < 3u; ++f) {
        pt::PtFrameDesc fd;
        fd.width = kW;
        fd.height = kH;
        fd.frameSeed = kSeed;
        fd.sampleBase = f;
        fd.accumulate = f != 0u;
        fd.settings = st;
        if (!runFrame(ctx, g, graph, c, fd, gi)) {
            check(false, "parity: GPU frame");
            return;
        }
        check(cpu.frame(c, st, kW, kH, kSeed, f, gi, kernel::Backend::CpuParallel), "parity: CPU frame");
        const u32 flags = g.gi.frameFlags();
        const std::vector<pt::Word> surf = readWords(g, g.gi.currentSurfaceSection(), std::size_t(pixels) * 4u);
        const std::vector<pt::Word> init = readWords(g, pt::kRgiSecInitial, std::size_t(pixels) * R);
        const std::vector<pt::Word> resA = readWords(g, pt::kRgiSecA, std::size_t(pixels) * R);
        const std::vector<pt::Word> fin = readWords(g, g.gi.finalSection(), std::size_t(pixels) * R);
        const std::vector<pt::Word> out = readWords(g, pt::kRgiSecOutput, std::size_t(pixels) * 2u);
        // surface records vs the CPU oracle's.
        u32 surfSame = 0;
        for (u32 i = 0; i < pixels; ++i) {
            const pt::Word& a = surf[i * 4u];
            const pt::Word& b = cpu.surfaces()[i * 4u];
            const pt::Word& pa = surf[i * 4u + 2u];
            const pt::Word& pb = cpu.surfaces()[i * 4u + 2u];
            const bool validA = surf[i * 4u + 1u].w > 0.5f, validB = cpu.surfaces()[i * 4u + 1u].w > 0.5f;
            bool ok = validA == validB;
            if (ok && validA) {
                ok = a.x == b.x && a.y == b.y && std::fabs(a.z - b.z) < 1e-3f && std::fabs(a.w - b.w) < 1e-3f &&
                     closeTo(pa.x, pb.x, 1e-3f) && closeTo(pa.y, pb.y, 1e-3f) && closeTo(pa.z, pb.z, 1e-3f) &&
                     surf[i * 4u + 3u].w == cpu.surfaces()[i * 4u + 3u].w;
            }
            surfSame += ok ? 1u : 0u;
        }
        // Each pass replayed on the GPU's own inputs.
        std::vector<pt::Word> outInit, outTemporal, outSpatial, outShade;
        const bool r0 = cpu.replayStage(c, st, kW, kH, kSeed, f, gi, flags, pt::RestirGiCpu::kStageInitial, 0u, surf,
                                        prevSurf, {}, {}, outInit);
        const bool r1 = cpu.replayStage(c, st, kW, kH, kSeed, f, gi, flags, pt::RestirGiCpu::kStageTemporal, 0u, surf,
                                        prevSurf.empty() ? surf : prevSurf, init,
                                        prevHistory.empty() ? init : prevHistory, outTemporal);
        const bool r2 = cpu.replayStage(c, st, kW, kH, kSeed, f, gi, flags, pt::RestirGiCpu::kStageSpatial, 0u, surf,
                                        surf, resA, resA, outSpatial);
        const bool r3 = cpu.replayStage(c, st, kW, kH, kSeed, f, gi, flags, pt::RestirGiCpu::kStageShade, 0u, surf,
                                        surf, fin, fin, outShade);
        u32 liveI = 0, liveT = 0, liveS = 0;
        const double aI = r0 ? reservoirAgreement(init, outInit, pixels, true, &liveI) : 0.0;
        const double aT = r1 ? reservoirAgreement(resA, outTemporal, pixels, false, &liveT) : 0.0;
        const double aS = r2 ? reservoirAgreement(fin, outSpatial, pixels, false, &liveS) : 0.0;
        u32 shadeSame = 0;
        for (u32 i = 0; r3 && i < pixels; ++i) {
            const pt::Word& a = out[i * 2u + 1u];
            const pt::Word& b = outShade[i * 2u + 1u];
            shadeSame += a.w == b.w && closeTo(a.x, b.x, 1e-3f) && closeTo(a.y, b.y, 1e-3f) && closeTo(a.z, b.z, 1e-3f)
                             ? 1u
                             : 0u;
        }
        const double aSh = double(shadeSame) / double(pixels);
        std::printf("  frame %u (history %s): surfaces %u/%u, initial %.1f%% (%u live), temporal %.1f%% (%u), "
                    "spatial %.1f%% (%u), shade %.1f%%\n",
                    f, (flags & pt::kRgiFlagHistory) != 0u ? "on" : "off", surfSame, pixels, 100.0 * aI, liveI,
                    100.0 * aT, liveT, 100.0 * aS, liveS, 100.0 * aSh);
        check(surfSame * 100u >= pixels * 99u, "parity: surface records GPU == CPU on >= 99% of the pixels");
        check(r0 && aI >= 0.97 && liveI > pixels / 2u, "parity: initial pass GPU == CPU replay (>= 97%)");
        check(r1 && aT >= 0.97, "parity: temporal pass GPU == CPU replay (>= 97%)");
        check(r2 && aS >= 0.97, "parity: spatial pass GPU == CPU replay (>= 97%)");
        check(r3 && aSh >= 0.97, "parity: shade pass GPU == CPU replay (>= 97%)");
        if (f > 0u) {
            check((flags & pt::kRgiFlagHistory) != 0u, "parity: history on after the first frame");
        }
        prevSurf = surf;
        prevHistory = readWords(g, pt::kRgiSecHistory, std::size_t(pixels) * R);
    }
}

void convergeCase(Context& ctx, Gpus& g, rg::Graph& graph) {
    pt::PtCompiledScene c;
    if (!compile(cornell(false), c) || !g.pt.setScene(c)) {
        check(false, "converge: scene");
        return;
    }
    const pt::RestirGiSettings gi = giSettings();
    const pt::PtSettings st = ptSettings();
    constexpr u32 kFrames = 64u, kRefSpp = 1024u;
    g.gi.resetHistory();
    bool ran = true;
    u32 matched = 0, surfaces = 0;
    for (u32 f = 0; f < kFrames && ran; ++f) {
        pt::PtFrameDesc fd;
        fd.width = kW;
        fd.height = kH;
        fd.frameSeed = 3u;
        fd.sampleBase = f;
        fd.accumulate = f != 0u;
        fd.settings = st;
        ran = runFrame(ctx, g, graph, c, fd, gi);
        if (ran && f + 1u == kFrames) {
            // Every ReSTIR surface is the trace pass's G-buffer vertex: its instance section equals the key's.
            const auto* out = static_cast<const pt::Word*>(g.gi.mappedSection(pt::kRgiSecOutput));
            const auto* inst = reinterpret_cast<const u32*>(ptSection(g.pt, pt::kPtOutInstance));
            for (u32 i = 0; i < kW * kH; ++i) {
                if (out[i * 2u + 1u].w > 0.5f) {
                    ++surfaces;
                    matched += inst[i] == u32(out[i * 2u].x) ? 1u : 0u;
                }
            }
        }
    }
    check(ran, "converge: GPU frames");
    if (!ran) {
        return;
    }
    pt::PtSettings plain = st;
    plain.flags &= ~pt::kPtFlagRestirGi;
    pt::PtReferenceImage ref;
    ref.resize(kW, kH);
    check(pt::renderReference(c, plain, kW, kH, 911u, 0u, kRefSpp, ref), "converge: reference");
    const float* acc = ptSection(g.pt, pt::kPtOutAccum);
    double sg[3] = {0, 0, 0}, sc[3] = {0, 0, 0};
    bool finite = true;
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const u32 i = y * kW + x;
            for (u32 ch = 0; ch < 3u; ++ch) {
                const double v = acc[i * 4u + ch] / double(kFrames);
                finite = finite && std::isfinite(v);
                sg[ch] += v;
                sc[ch] += ref.mean(x, y, ch);
            }
        }
    }
    double worst = 0.0;
    for (u32 ch = 0; ch < 3u; ++ch) {
        worst = std::max(worst, std::fabs(sg[ch] - sc[ch]) / std::max(sc[ch], 1e-9));
    }
    std::printf("  converge cornell %ux%u, %u ReSTIR frames: image mean gpu (%.5f %.5f %.5f) cpu reference (%.5f %.5f "
                "%.5f), worst relative %.2f%%; trace pass matched %u/%u ReSTIR surfaces\n",
                kW, kH, kFrames, sg[0] / (kW * kH), sg[1] / (kW * kH), sg[2] / (kW * kH), sc[0] / (kW * kH),
                sc[1] / (kW * kH), sc[2] / (kW * kH), 100.0 * worst, matched, surfaces);
    check(finite, "converge: finite");
    check(worst < 0.03, "converge: GPU ReSTIR image mean within 3% of the CPU reference path tracer");
    check(surfaces > kW * kH / 2u && matched == surfaces, "converge: every ReSTIR surface is the trace's G-buffer vertex");
}

void determinismCase(Context& ctx, Gpus& g, rg::Graph& graph) {
    pt::PtCompiledScene c;
    if (!compile(cornell(true), c)) {
        return;
    }
    const pt::RestirGiSettings gi = giSettings();
    std::vector<u8> first;
    bool identical = true;
    for (int k = 0; k < 2; ++k) {
        g.gi.resetHistory();
        std::vector<u8> bytes;
        for (u32 f = 0; f < 2u; ++f) {
            pt::PtFrameDesc fd;
            fd.width = kW;
            fd.height = kH;
            fd.frameSeed = 9u;
            fd.sampleBase = f;
            fd.settings = ptSettings();
            if (!runFrame(ctx, g, graph, c, fd, gi)) {
                check(false, "determinism: GPU frame");
                return;
            }
        }
        const auto* out = static_cast<const u8*>(g.gi.mappedSection(pt::kRgiSecOutput));
        bytes.assign(out, out + std::size_t(kW) * kH * 32u);
        const auto* rad = reinterpret_cast<const u8*>(ptSection(g.pt, pt::kPtOutRadiance));
        bytes.insert(bytes.end(), rad, rad + std::size_t(kW) * kH * 16u);
        if (k == 0) {
            first = bytes;
        } else {
            identical = first == bytes;
        }
    }
    std::printf("  determinism: two identical runs %s\n", identical ? "bit-identical" : "DIFFER");
    check(identical, "determinism: identical GPU runs are bit-identical");
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
                std::strstr(g.gi.reason(), "no ReSTIR GI kernel") != nullptr) {
                std::printf("%s: kernel not built, skipped\n", lang.name);
                continue;
            }
            std::printf("SKIP: GPU path tracer unavailable: %s / %s\n", g.pt.reason(), g.gi.reason());
            return kSkip;
        }
        ++languages;
        std::printf("%s kernels:\n", g.gi.kernelLanguage());
        rg::Graph graph;
        parityCase(ctx, g, graph);
        convergeCase(ctx, g, graph);
        determinismCase(ctx, g, graph);
        ctx.executor->waitIdle();
        g.gi.destroy();
        g.pt.destroy();
    }
    if (languages == 0u) {
        std::printf("SKIP: no ReSTIR GI kernel built\n");
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
        g.gi.destroy();
        g.pt.destroy();
    }
    if (!ok) {
        std::printf("SKIP: GPU path tracer / ReSTIR GI unavailable: %s / %s\n", g.pt.reason(), g.gi.reason());
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
    const pt::RestirGiSettings gi = giSettings();
    const u32 moving = static_cast<u32>(s.instances.size()) - 1u;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        s.instances[moving].objectToWorld = translate(0.01f * float(frame % 7u), 0.f, 0.f);
        t_allocations = 0;
        t_count = measure;
        const bool updated = c.update(s);
        t_count = false;
        const unsigned long long a0 = t_allocations;
        pt::PtFrameDesc fd;
        fd.width = kW;
        fd.height = kH;
        fd.frameSeed = 1u;
        fd.sampleBase = frame;
        fd.accumulate = frame != 0u;
        fd.settings = ptSettings();
        FrameAllocs fa;
        t_allocations = 0;
        const bool ran = runFrame(ctx, g, graph, c, fd, gi, measure, &fa);
        const unsigned long long a3 = t_allocations;
        check(updated && ran, "zero_alloc: frame ok");
        if (measure) {
            updates += a0;
            allocs.begin += fa.begin;
            allocs.graph += fa.graph;
            callbacks += a3;
        }
        if (frame == kWarmup) {
            reallocations = g.pt.stats().reallocations + g.gi.stats().reallocations;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    check(g.pt.stats().reallocations + g.gi.stats().reallocations == reallocations,
          "zero_alloc: no reallocation in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (%s kernels, %u steady-state frames, %u ReSTIR passes / frame):\n"
                    "  PtCompiledScene::update: %llu operator-new calls\n"
                    "  PathTracerGpu + RestirGiGpu setScene / beginFrame: %llu\n"
                    "  graph build (importInto + addPasses + addTracePass + read-back): %llu\n"
                    "  relight.* pass callbacks: %llu\n",
                    g.gi.kernelLanguage(), kTotal - kWarmup, g.gi.stats().passes, updates, allocs.begin, allocs.graph,
                    callbacks);
        check(updates == 0u && allocs.begin == 0u && allocs.graph == 0u && callbacks == 0u,
              "steady-state ReSTIR GI frames make no heap allocation");
    } else {
        std::printf("zero_alloc frames under validation: %u frames ok (%s)\n", kTotal, g.gi.kernelLanguage());
    }
    ctx.executor->waitIdle();
    g.gi.destroy();
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
