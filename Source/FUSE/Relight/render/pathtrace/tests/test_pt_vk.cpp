// FUSE Relight RL-5.1 Vulkan gates (Lavapipe): PathTracerGpu ("relight.pt.trace", pt_gpu.hpp) against the CPU
// reference path tracer (pt_reference.hpp) on the same PtCompiledScene.
//
// Every frame is one render graph (WP-0.3): the GPU scene / acceleration structures (gpu_scene.*, rt.*), the RL-4.4
// light ring ("relight.lights.convert"), "relight.pt.trace" and a host read-back declaration.
//
//   converge     GPU accumulation (frames of kSppFrame samples, sampleBase continuing, accumulate on) vs the CPU
//                reference over the same sample indices, per pixel and channel: |mean_gpu - mean_cpu| <= 5 sigma
//                (both estimators' standard errors) + 1e-3 (1 + |mean_cpu|), on the Cornell scene with and without
//                glass and the PSR mirror scene; the sample count of the accumulation equals frames x spp; and against
//                an independent CPU estimate (another seed): 8x8 block means within 4 sigma.
//   psr          mirror scene, jitter off: the GPU G-buffer carries the reflected wall (instance slot, PSR length 1,
//                normal +x within 1e-3) on every mirror pixel, and the instance section == the CPU G-buffer's.
//   determinism  two identical GPU frames give bit-identical output sections.
//   zero_alloc   64 steady-state frames with a moving instance: no operator-new call in PtCompiledScene::update,
//                PathTracerGpu::setScene / beginFrame, the graph build (importInto + addTracePass + read-back) and the
//                relight.* pass callbacks (counted without an in-process validation layer: it allocates).
//
// Options: --language slang | glsl | all (default all built), --no-validation (Wine: no layer can be enumerated),
// --external-validation (the host loader injects the layer: a negative control proves it is live, then every
// message fails the run). Exit 77 = skip (stub build, no ICD / layer / ray-query capability / kernel).
#include "pt_test_scenes.hpp"

#include <fuse/relight/render/pathtrace/pt_gpu.hpp>
#include <fuse/relight/render/pathtrace/pt_reference.hpp>

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
constexpr u32 kW = 32u, kH = 32u;
constexpr u32 kSppFrame = 16u;
constexpr u32 kFrames = 16u;

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
    instanceDesc.appName = "rl_pt_vk";
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
    d.name = "rl_pt.staging";
    if (!ctx.allocator->createBuffer(d, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    return 0;
}

bool initGpu(Context& ctx, pt::PathTracerGpu& gpu, pt::PtKernelLanguage language) {
    pt::PathTracerGpuDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.language = language;
    d.framesInFlight = 3;
    return gpu.init(d);
}

bool compile(const pt::PtScene& s, pt::PtCompiledScene& c) {
    std::string error;
    const bool ok = c.compile(s, {}, &error);
    check(ok, "compile: " + error);
    return ok;
}

/// The mirror / red wall scene of the CPU psr suite.
pt::PtScene psrScene() {
    pt::PtScene s;
    s.materials = {metal(0.9f, 0.8f, 0.7f, 0.0f), lambert(0.8f, 0.1f, 0.1f), lambert(0.5f, 0.5f, 0.5f)};
    const float mc[3] = {0.f, 0.f, 0.f}, mu[3] = {0.7071f, 0.f, 0.7071f}, mv[3] = {0.f, 1.f, 0.f};
    s.meshes.push_back(quad(mc, mu, mv, 0));
    const float wc[3] = {-3.f, 0.f, 0.f}, wu[3] = {0.f, 0.f, 3.f}, wv[3] = {0.f, 3.f, 0.f};
    s.meshes.push_back(quad(wc, wu, wv, 1));
    s.instances.push_back(pt::PtInstance{});
    pt::PtInstance wall;
    wall.mesh = 1;
    s.instances.push_back(wall);
    s.lights.push_back(rl::makeDistantLight(lk::float3(-1.f, -0.5f, -0.2f), 0.05f, lk::float3(2.f, 2.f, 2.f)));
    s.camera = camera(0.f, 0.f, 5.f, 0.f, 0.f, -1.f, 0.f, 1.f, 0.f, 20.f);
    return s;
}

/// Records one frame: scene / AS / lights / trace / host read-back (no allocation in steady state).
bool buildGraph(pt::PathTracerGpu& gpu, rg::Graph& graph) {
    graph.reset();
    const pt::PtGraphRefs refs = gpu.importInto(graph);
    if (!refs.valid || !gpu.addTracePass(graph, refs)) {
        return false;
    }
    graph.addPass("readback.host", nullptr, nullptr).use(refs.outputs, rg::Access::HostRead);
    return true;
}

/// setScene + beginFrame + graph + execute + wait. The pieces are timed for allocations by the caller's counters.
struct FrameAllocs {
    unsigned long long begin = 0;
    unsigned long long graph = 0;
};

bool runFrame(Context& ctx, pt::PathTracerGpu& gpu, rg::Graph& graph, pt::PtCompiledScene& c,
              const pt::PtFrameDesc& fd, bool measure = false, FrameAllocs* allocs = nullptr) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    t_allocations = 0;
    t_count = measure;
    bool ok = gpu.setScene(c) && gpu.beginFrame(ctx.serial, c, fd);
    t_count = false;
    const unsigned long long a0 = t_allocations;
    ctx.upload.flush();
    t_allocations = 0;
    t_count = measure;
    ok = ok && buildGraph(gpu, graph);
    t_count = false;
    const unsigned long long a1 = t_allocations;
    t_allocations = 0; // from here: the hooked pass callbacks only
    if (allocs != nullptr) {
        allocs->begin += a0;
        allocs->graph += a1;
    }
    if (!ok) {
        std::fprintf(stderr, "  frame setup failed: %s\n", gpu.reason());
        return false;
    }
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    return result.ok && waited;
}

const float* section(const pt::PathTracerGpu& gpu, u32 s) {
    return reinterpret_cast<const float*>(static_cast<const u8*>(gpu.mappedOutputs()) + u64(s) * gpu.outputStride());
}

pt::PtSettings baseSettings() {
    pt::PtSettings st;
    st.maxBounces = 4;
    st.samplesPerPixel = kSppFrame;
    return st;
}

/// GPU accumulation vs the CPU reference over the same sample indices.
void convergeCase(Context& ctx, pt::PathTracerGpu& gpu, rg::Graph& graph, const char* name, const pt::PtScene& s) {
    pt::PtCompiledScene c;
    if (!compile(s, c) || !gpu.setScene(c)) {
        check(false, std::string("converge ") + name + ": scene");
        return;
    }
    const pt::PtSettings st = baseSettings();
    constexpr u32 kSeed = 5u;
    bool ran = true;
    for (u32 f = 0; f < kFrames && ran; ++f) {
        pt::PtFrameDesc fd;
        fd.width = kW;
        fd.height = kH;
        fd.frameSeed = kSeed;
        fd.sampleBase = f * kSppFrame;
        fd.accumulate = f != 0u;
        fd.settings = st;
        ran = runFrame(ctx, gpu, graph, c, fd);
    }
    check(ran, std::string("converge ") + name + ": GPU frames");
    if (!ran) {
        return;
    }
    pt::PtReferenceImage img;
    img.resize(kW, kH);
    if (!pt::renderReference(c, st, kW, kH, kSeed, 0u, kFrames * kSppFrame, img)) {
        check(false, "renderReference");
        return;
    }
    const float* acc = section(gpu, pt::kPtOutAccum);
    const float* accSq = section(gpu, pt::kPtOutAccumSq);
    const double n = double(kFrames * kSppFrame);
    u32 failing = 0, countBad = 0, close = 0, nonFinite = 0;
    double worstZ = 0.0, sumG = 0.0, sumC = 0.0;
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const u32 i = y * kW + x;
            countBad += acc[i * 4u + 3u] == float(n) ? 0u : 1u;
            bool pixelClose = true;
            for (u32 ch = 0; ch < 3u; ++ch) {
                const double sg = acc[i * 4u + ch], sq = accSq[i * 4u + ch];
                if (!std::isfinite(sg) || !std::isfinite(sq)) {
                    ++nonFinite;
                    continue;
                }
                const double mg = sg / n;
                const double vg = std::max(0.0, (sq - sg * sg / n) / (n - 1.0));
                const double mc = img.mean(x, y, ch);
                const double vc = img.variance(x, y, ch);
                const double se = std::sqrt(vg / n + vc / n);
                const double d = std::fabs(mg - mc);
                worstZ = std::max(worstZ, d / std::max(se, 1e-12));
                if (d > 5.0 * se + 1e-3 * (1.0 + std::fabs(mc))) {
                    if (failing < 5u) {
                        std::printf("    pixel (%u,%u) ch %u: gpu %.5f cpu %.5f se %.5f\n", x, y, ch, mg, mc, se);
                    }
                    ++failing;
                }
                pixelClose = pixelClose && d <= 1e-3 * (1.0 + std::fabs(mc));
                sumG += mg;
                sumC += mc;
            }
            close += pixelClose ? 1u : 0u;
        }
    }
    std::printf("  converge %-14s %ux%u at %u spp (%u frames): mean gpu %.5f cpu %.5f, worst z %.2f, failing %u, "
                "pixels within 1e-3 %u/%u\n",
                name, kW, kH, u32(n), kFrames, sumG / (kW * kH * 3.0), sumC / (kW * kH * 3.0), worstZ, failing, close,
                kW * kH);
    // Independent estimate: the CPU reference with another seed, 8x8 block means within 4 sigma (as the CPU mis / rr
    // suites): convergence of the GPU image to the reference, not only the shared random numbers.
    pt::PtReferenceImage indep;
    indep.resize(kW, kH);
    u32 blockFailing = 1;
    if (pt::renderReference(c, st, kW, kH, kSeed + 101u, 0u, kFrames * kSppFrame, indep)) {
        std::vector<double> meanG(usize(kW) * kH * 3u), varG(meanG.size()), meanI, varI;
        for (u32 i = 0; i < kW * kH; ++i) {
            for (u32 ch = 0; ch < 3u; ++ch) {
                const double sg = acc[i * 4u + ch], sq = accSq[i * 4u + ch];
                meanG[i * 3u + ch] = sg / n;
                varG[i * 3u + ch] = std::max(0.0, (sq - sg * sg / n) / (n - 1.0));
            }
        }
        referenceStats(indep, meanI, varI);
        const BlockResult r = compareBlocks(kW, kH, 8, meanG, varG, n, meanI, varI, n, 4.0, 1e-4);
        blockFailing = r.failing;
        std::printf("    vs independent CPU seed: %u blocks, worst z %.2f, failing %u\n", r.blocks, r.worstZ, r.failing);
    }
    check(blockFailing == 0u, std::string("converge ") + name + ": GPU == independent CPU estimate (8x8 blocks, 4 sigma)");
    check(nonFinite == 0u, std::string("converge ") + name + ": finite accumulation");
    check(countBad == 0u, std::string("converge ") + name + ": accumulated sample count == frames x spp");
    check(failing == 0u, std::string("converge ") + name + ": GPU == CPU reference per pixel within 5 sigma");
    check(sumG > 0.0, std::string("converge ") + name + ": lit");
}

void psrCase(Context& ctx, pt::PathTracerGpu& gpu, rg::Graph& graph) {
    pt::PtCompiledScene c;
    if (!compile(psrScene(), c) || !gpu.setScene(c)) {
        check(false, "psr: scene");
        return;
    }
    const u32 W = 16, H = 16;
    pt::PtSettings on;
    on.flags &= ~pt::kPtFlagJitter;
    on.maxBounces = 3;
    on.samplesPerPixel = 4;
    pt::PtFrameDesc fd;
    fd.width = W;
    fd.height = H;
    fd.frameSeed = 3u;
    fd.settings = on;
    if (!runFrame(ctx, gpu, graph, c, fd)) {
        check(false, "psr: GPU frame");
        return;
    }
    pt::PtReferenceImage img;
    img.resize(W, H);
    if (!pt::renderReference(c, on, W, H, 3u, 0u, 4u, img)) {
        check(false, "psr: renderReference");
        return;
    }
    pt::PtSettings off = on;
    off.flags &= ~pt::kPtFlagPsr;
    pt::PtReferenceImage noPsr;
    noPsr.resize(W, H);
    pt::renderReference(c, off, W, H, 3u, 0u, 1u, noPsr);
    const u32* inst = reinterpret_cast<const u32*>(section(gpu, pt::kPtOutInstance));
    const float* albedoD = section(gpu, pt::kPtOutAlbedoD);
    const float* normal = section(gpu, pt::kPtOutNormal);
    u32 mirror = 0, replaced = 0, instEqual = 0;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            const u32 i = y * W + x;
            instEqual += inst[i] == img.pixel(x, y).instance ? 1u : 0u;
            if (noPsr.pixel(x, y).instance == c.slotOf(0)) {
                ++mirror;
                const bool wallNormal = std::fabs(normal[i * 4u] - 1.f) < 1e-3f && std::fabs(normal[i * 4u + 1u]) < 1e-3f;
                replaced += inst[i] == c.slotOf(1) && albedoD[i * 4u + 3u] == 1.f && wallNormal ? 1u : 0u;
            }
        }
    }
    std::printf("  psr: mirror pixels %u, GPU G-buffer replaced %u, instance section == CPU %u/%u\n", mirror, replaced,
                instEqual, W * H);
    check(mirror > 40u, "psr: the mirror covers the view");
    check(replaced == mirror, "psr: GPU mirror pixels carry the reflected wall (instance, PSR 1, normal)");
    check(instEqual == W * H, "psr: GPU instance section == CPU reference G-buffer");
}

void determinismCase(Context& ctx, pt::PathTracerGpu& gpu, rg::Graph& graph) {
    pt::PtCompiledScene c;
    if (!compile(cornell(true), c)) {
        return;
    }
    pt::PtFrameDesc fd;
    fd.width = kW;
    fd.height = kH;
    fd.frameSeed = 9u;
    fd.settings = baseSettings();
    std::vector<u8> first;
    bool identical = true;
    for (int k = 0; k < 2; ++k) {
        if (!runFrame(ctx, gpu, graph, c, fd)) {
            check(false, "determinism: GPU frame");
            return;
        }
        const usize total = usize(gpu.outputStride() * pt::kPtOutSections);
        const u8* p = static_cast<const u8*>(gpu.mappedOutputs());
        if (k == 0) {
            first.assign(p, p + total);
        } else {
            identical = first.size() == total && std::memcmp(first.data(), p, total) == 0;
        }
    }
    std::printf("  determinism: two identical frames %s\n", identical ? "bit-identical" : "DIFFER");
    check(identical, "determinism: identical GPU frames are bit-identical");
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
        pt::PathTracerGpu gpu;
        if (!initGpu(ctx, gpu, lang.language)) {
            if (std::strcmp(gpu.reason(), "no path-tracing kernel of the requested language / pipeline creation "
                                          "failed") == 0) {
                std::printf("%s: kernel not built, skipped\n", lang.name);
                continue;
            }
            std::printf("SKIP: PathTracerGpu unavailable: %s\n", gpu.reason());
            return kSkip;
        }
        ++languages;
        std::printf("%s kernels:\n", gpu.kernelLanguage());
        rg::Graph graph;
        convergeCase(ctx, gpu, graph, "cornell", cornell(false));
        convergeCase(ctx, gpu, graph, "cornell+glass", cornell(true));
        convergeCase(ctx, gpu, graph, "psr-mirror", psrScene());
        psrCase(ctx, gpu, graph);
        determinismCase(ctx, gpu, graph);
        std::printf("  %s: %u scene builds, %u reallocations\n", gpu.kernelLanguage(), gpu.stats().sceneBuilds,
                    gpu.stats().reallocations);
        ctx.executor->waitIdle();
        gpu.destroy();
    }
    if (languages == 0u) {
        std::printf("SKIP: no path-tracing kernel built\n");
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
    pt::PathTracerGpu gpu;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initGpu(ctx, gpu, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: PathTracerGpu unavailable: %s\n", gpu.reason());
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
    u32 reallocations = 0, builds = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
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
        fd.settings = baseSettings();
        fd.settings.samplesPerPixel = 1;
        FrameAllocs fa;
        t_allocations = 0;
        const bool ran = runFrame(ctx, gpu, graph, c, fd, measure, &fa);
        const unsigned long long a3 = t_allocations; // the relight.* pass callbacks (hooked) of this frame
        check(updated && ran, "zero_alloc: frame ok");
        if (measure) {
            updates += a0;
            allocs.begin += fa.begin;
            allocs.graph += fa.graph;
            callbacks += a3;
        }
        if (frame == kWarmup) {
            reallocations = gpu.stats().reallocations;
            builds = gpu.stats().sceneBuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    check(gpu.stats().reallocations == reallocations, "zero_alloc: no reallocation in steady state");
    check(gpu.stats().sceneBuilds == builds, "zero_alloc: no structural rebuild for a moving instance");
    if (countAllocations) {
        std::printf("zero_alloc (%s kernels, %u steady-state frames):\n"
                    "  PtCompiledScene::update: %llu operator-new calls\n"
                    "  PathTracerGpu::setScene + beginFrame: %llu\n"
                    "  graph build (importInto + addTracePass + read-back): %llu\n"
                    "  relight.* pass callbacks: %llu\n",
                    gpu.kernelLanguage(), kTotal - kWarmup, updates, allocs.begin, allocs.graph, callbacks);
        check(updates == 0u && allocs.begin == 0u && allocs.graph == 0u && callbacks == 0u,
              "steady-state path-tracing frames make no heap allocation");
    } else {
        std::printf("zero_alloc frames under validation: %u frames ok (%s)\n", kTotal, gpu.kernelLanguage());
    }
    ctx.executor->waitIdle();
    gpu.destroy();
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
