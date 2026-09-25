// FUSE Relight RL-5.5 Vulkan gates (Lavapipe; PE under Wine in the MinGW tree): PtDenoiser - the A-SVGF gradient
// producer "relight.denoise.gradient" on the path tracer and the rdn.* passes on RL-5.1's output sections - against the
// CPU oracle (PtDenoiseCpu::runGradient, RdnReference) on the GPU's own inputs.
//
//   parity       6 frames of the sphere-lit Cornell box (1 spp, a fresh seed per frame): the producer's records and
//                samples == the CPU producer replayed on the GPU's previous records and the same parameter words (per
//                value |gpu - cpu| <= 1e-3 max(|cpu|, 1e-3) on >= 99% of the values: the GPU traverses a float TLAS, the
//                CPU a double BVH); the denoised output == RdnReference fed the GPU's sections and gradient samples
//                (relative RMS <= 1e-3); static scene: every valid GPU sample re-shades bit-exactly to its previous value.
//   light        the sphere light x 0.25: GPU samples dCur = 0.25 dPrev on >= 95% of the lit strata.
//   determinism  two fresh PtDenoiser runs give bit-identical denoised outputs and gradient buffers.
//   zero_alloc   64 steady-state frames: no operator-new call in PtDenoiser::beginFrame, the graph build (path tracer +
//                producer + denoiser passes) and the relight.* / rdn.* pass callbacks (counted without an in-process
//                validation layer).
//
// Options: --language slang | glsl | all, --no-validation (Wine), --external-validation (the host loader injects the
// layer; negative control). Exit 77 = skip (stub build, no ICD / layer / ray query / kernel).
#include "pt_test_scenes.hpp"

#include <fuse/relight/render/denoise/pt_denoise.hpp>
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
constexpr u32 kW = 48u, kH = 36u;

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
    if ((severity &
         (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) == 0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) ==
            0) {
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
    instanceDesc.appName = "rl_denoise_vk";
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
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
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
        std::printf("external validation layer live (negative control: %u message(s), not counted)\n",
                    control);
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
    d.name = "rl_denoise.staging";
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

namespace dn = fuse::relight::render::denoise;
namespace rdn = fuse::renderer::denoise;

pt::PtScene sphereLitCornell(float scale) {
    pt::PtScene s = cornell(true);
    s.materials[3].bsdf.emission = bk::float3(0.f, 0.f, 0.f);
    s.lights[0] =
        rl::makeSphereLight(lk::float3(0.9f, 1.4f, 0.6f), 0.25f, lk::float3(40.f * scale, 34.f * scale, 28.f * scale));
    return s;
}

pt::PtFrameDesc frameDesc(u32 seed) {
    pt::PtFrameDesc fd;
    fd.width = kW;
    fd.height = kH;
    fd.frameSeed = seed;
    fd.sampleBase = 0u;
    fd.accumulate = false;
    fd.settings.maxBounces = 4;
    fd.settings.samplesPerPixel = 1;
    return fd;
}

struct FrameAllocs {
    unsigned long long begin = 0;
    unsigned long long graph = 0;
};

/// One path-traced + denoised frame (the frame renderer's order: trace, producer, denoiser, read-backs).
bool runFrame(Context& ctx, pt::PathTracerGpu& gpu, dn::PtDenoiser& den, rg::Graph& graph, pt::PtCompiledScene& c,
              const pt::PtFrameDesc& fd, bool measure = false, FrameAllocs* allocs = nullptr) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    t_allocations = 0;
    t_count = measure;
    bool ok = gpu.setScene(c) && gpu.beginFrame(ctx.serial, c, fd) && den.beginFrame(ctx.serial, gpu, c, fd);
    t_count = false;
    const unsigned long long a0 = t_allocations;
    ctx.upload.flush();
    t_allocations = 0;
    t_count = measure;
    if (ok) {
        graph.reset();
        const pt::PtGraphRefs refs = gpu.importInto(graph);
        ok = refs.valid && gpu.addTracePass(graph, refs) && den.addPasses(graph, gpu, refs);
        graph.addPass("readback.host", nullptr, nullptr).use(refs.outputs, rg::Access::HostRead);
    }
    t_count = false;
    const unsigned long long a1 = t_allocations;
    t_allocations = 0;
    if (allocs != nullptr) {
        allocs->begin += a0;
        allocs->graph += a1;
    }
    if (!ok) {
        std::fprintf(stderr, "  frame setup failed: %s / %s\n", gpu.reason(), den.reason());
        return false;
    }
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    gpu.collectRetired(ctx.serial);
    den.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    return result.ok && waited;
}

const float* section(const pt::PathTracerGpu& gpu, u32 s) {
    return reinterpret_cast<const float*>(static_cast<const u8*>(gpu.mappedOutputs()) + u64(s) * gpu.outputStride());
}

struct Lang {
    pt::PtKernelLanguage pt;
    dn::PtDenoiseLanguage dn;
    const char* name;
};
constexpr Lang kLangs[] = {{pt::PtKernelLanguage::Slang, dn::PtDenoiseLanguage::Slang, "slang"},
                           {pt::PtKernelLanguage::Glsl, dn::PtDenoiseLanguage::Glsl, "glsl"}};

bool initDenoiser(Context& ctx, dn::PtDenoiser& den, dn::PtDenoiseLanguage language) {
    dn::PtDenoiserDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.language = language;
    d.hostVisible = true;
    return den.init(d);
}

/// Copies of the host-visible gradient buffer (records, samples) after a frame.
void copyGradient(const dn::PtDenoiser& den, std::vector<pt::Word>& records, std::vector<pt::Word>& samples) {
    const usize ns = usize(den.strataW()) * den.strataH();
    records.resize(ns);
    samples.resize(ns);
    std::memcpy(static_cast<void*>(records.data()), den.gradientRecords(), ns * 16u);
    std::memcpy(static_cast<void*>(samples.data()), den.gradientSamples(), ns * 16u);
}

void parityCase(Context& ctx, pt::PathTracerGpu& gpu, dn::PtDenoiser& den, rg::Graph& graph) {
    pt::PtScene s = sphereLitCornell(1.f);
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return;
    }
    den.reset();
    std::vector<pt::Word> prevRecords, records, samples, cpuRecords, cpuSamples;
    pt::Word words[2u * pt::kPtParamWords];
    u64 values = 0, within = 0, staticValid = 0, staticExact = 0;
    f64 worst = 0.0, chainSe = 0.0, chainRef = 0.0;
    rdn::RdnReference ref;
    ref.init(kW, kH, den.settings());
    std::vector<rdn::rdnk::float4> dIn, sIn, nIn;
    std::vector<f32> depth;
    std::vector<rdn::rdnk::float2> motion;
    std::vector<u32> instance;
    for (u32 t = 0; t < 6u; ++t) {
        const pt::PtFrameDesc fd = frameDesc(500u + t);
        pt::Word cur[pt::kPtParamWords];
        c.packParams(fd.settings, kW, kH, fd.frameSeed, fd.sampleBase, cur);
        if (!runFrame(ctx, gpu, den, graph, c, fd)) {
            check(false, "parity: frame");
            return;
        }
        copyGradient(den, records, samples);
        const u32 sw = den.strataW(), sh = den.strataH();
        if (t > 0u) {
            // CPU producer on the GPU's previous records and the same words.
            std::memcpy(words, cur, sizeof(cur));
            cpuRecords = prevRecords;
            cpuSamples.assign(samples.size(), pt::Word(0.f, 0.f, 0.f, 0.f));
            check(dn::PtDenoiseCpu::runGradient(c, words, true, sw, sh, cpuRecords.data(), cpuSamples.data()),
                  "parity: CPU producer");
            for (usize i = 0; i < samples.size(); ++i) {
                const float* g[2] = {&samples[i].x, &records[i].x};
                const float* q[2] = {&cpuSamples[i].x, &cpuRecords[i].x};
                for (u32 k = 0; k < 2u; ++k) {
                    for (u32 j = 0; j < 4u; ++j) {
                        const f64 e = std::fabs(f64(g[k][j]) - f64(q[k][j])) / std::max(std::fabs(f64(q[k][j])), 1e-3);
                        worst = std::max(worst, e);
                        within += e <= 1e-3 ? 1u : 0u;
                        ++values;
                    }
                }
                if (samples[i].y >= 0.f) {
                    ++staticValid;
                    staticExact += samples[i].x == samples[i].y && samples[i].z == samples[i].w ? 1u : 0u;
                }
            }
        }
        prevRecords = records;
        std::memcpy(words + pt::kPtParamWords, cur, sizeof(cur));
        // Denoiser chain on the GPU's inputs.
        const usize n = usize(kW) * kH;
        dIn.resize(n), sIn.resize(n), nIn.resize(n), depth.resize(n), motion.resize(n), instance.resize(n);
        std::memcpy(static_cast<void*>(dIn.data()), section(gpu, pt::kPtOutDiffuse), n * 16u);
        std::memcpy(static_cast<void*>(sIn.data()), section(gpu, pt::kPtOutSpecular), n * 16u);
        std::memcpy(static_cast<void*>(nIn.data()), section(gpu, pt::kPtOutNormal), n * 16u);
        std::memcpy(depth.data(), section(gpu, pt::kPtOutDepth), n * 4u);
        std::memcpy(static_cast<void*>(motion.data()), section(gpu, pt::kPtOutMotion), n * 8u);
        std::memcpy(instance.data(), section(gpu, pt::kPtOutInstance), n * 4u);
        rdn::RdnReferenceInputs in{};
        in.diffuse = dIn.data();
        in.specular = sIn.data();
        in.normal = nIn.data();
        in.depth = depth.data();
        in.motion = motion.data();
        in.instance = instance.data();
        in.gradient = reinterpret_cast<const rdn::rdnk::float4*>(samples.data());
        ref.runFrame(in, dn::cameraFromParams(cur, false), dn::cameraFromParams(cur, true));
        const float* gd = den.denoisedDiffuse();
        const float* gs = den.denoisedSpecular();
        for (usize i = 0; i < n; ++i) {
            for (u32 k = 0; k < 3u; ++k) {
                const f64 a = (&ref.outputD()[i].x)[k];
                const f64 b = (&ref.outputS()[i].x)[k];
                chainSe += (gd[i * 4u + k] - a) * (gd[i * 4u + k] - a) + (gs[i * 4u + k] - b) * (gs[i * 4u + k] - b);
                chainRef += a * a + b * b;
            }
        }
    }
    const f64 chain = std::sqrt(chainSe / std::max(chainRef, 1e-30));
    std::printf("  parity: producer %llu / %llu values within 1e-3 (worst %.3g); static re-shade exact %llu / %llu; "
                "denoiser chain relative RMS %.3g\n",
                static_cast<unsigned long long>(within), static_cast<unsigned long long>(values), worst,
                static_cast<unsigned long long>(staticExact), static_cast<unsigned long long>(staticValid), chain);
    check(values > 0u && within * 100u >= values * 99u, "producer GPU == CPU on >= 99% of the values");
    check(staticValid > 0u && staticExact == staticValid, "static scene: GPU re-shades are bit-exact");
    check(chain <= 1e-3, "denoiser chain GPU == RdnReference on the GPU's inputs");

    // Light change.
    s = sphereLitCornell(0.25f);
    check(c.update(s), "light update");
    if (!runFrame(ctx, gpu, den, graph, c, frameDesc(600u))) {
        check(false, "light: frame");
        return;
    }
    copyGradient(den, records, samples);
    u32 lit = 0, scaled = 0;
    for (const pt::Word& g : samples) {
        if (g.y > 1e-3f) {
            ++lit;
            scaled += std::fabs(g.x - 0.25f * g.y) <= 1e-3f * g.y + 1e-6f ? 1u : 0u;
        }
    }
    std::printf("  light x 0.25: %u lit samples, %u at 0.25 x prev\n", lit, scaled);
    check(lit > 0u && scaled * 100u >= lit * 95u, "GPU gradient samples follow the light change");
}

void determinismCase(Context& ctx, pt::PathTracerGpu& gpu, dn::PtDenoiseLanguage language, rg::Graph& graph) {
    std::vector<f32> out[2];
    std::vector<pt::Word> grad[2], rec[2];
    for (u32 run = 0; run < 2u; ++run) {
        dn::PtDenoiser den;
        if (!initDenoiser(ctx, den, language)) {
            check(false, "determinism: init");
            return;
        }
        pt::PtScene s = sphereLitCornell(1.f);
        pt::PtCompiledScene c;
        compile(s, c);
        for (u32 t = 0; t < 4u; ++t) {
            runFrame(ctx, gpu, den, graph, c, frameDesc(40u + t));
        }
        const usize n = usize(kW) * kH * 4u;
        out[run].assign(den.denoisedDiffuse(), den.denoisedDiffuse() + n);
        out[run].insert(out[run].end(), den.denoisedSpecular(), den.denoisedSpecular() + n);
        copyGradient(den, rec[run], grad[run]);
        ctx.executor->waitIdle();
        den.destroy();
    }
    const bool same = out[0] == out[1] && std::memcmp(grad[0].data(), grad[1].data(), grad[0].size() * 16u) == 0 &&
                      std::memcmp(rec[0].data(), rec[1].data(), rec[0].size() * 16u) == 0;
    std::printf("  determinism: two runs bit-identical: %s\n", same ? "yes" : "NO");
    check(same, "determinism");
}

int runParity(Context& ctx, const std::string& language) {
    u32 languages = 0;
    for (const Lang& lang : kLangs) {
        if (language != "all" && language != lang.name) {
            continue;
        }
        pt::PathTracerGpu gpu;
        if (!initGpu(ctx, gpu, lang.pt)) {
            std::printf("%s: path tracer unavailable (%s), skipped\n", lang.name, gpu.reason());
            continue;
        }
        dn::PtDenoiser den;
        if (!initDenoiser(ctx, den, lang.dn)) {
            std::printf("%s: denoiser unavailable (%s), skipped\n", lang.name, den.reason());
            gpu.destroy();
            continue;
        }
        ++languages;
        std::printf("%s kernels (producer %s, denoiser %s):\n", lang.name, den.kernelLanguage(),
                    den.denoiser().kernelLanguage());
        rg::Graph graph;
        parityCase(ctx, gpu, den, graph);
        ctx.executor->waitIdle();
        den.destroy();
        determinismCase(ctx, gpu, lang.dn, graph);
        ctx.executor->waitIdle();
        gpu.destroy();
    }
    if (languages == 0u) {
        std::printf("SKIP: no path-tracing / denoiser kernel built or ray query unavailable\n");
        return kSkip;
    }
    return 0;
}

thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && (std::strncmp(name, "relight.", 8) == 0 || std::strncmp(name, "rdn.", 4) == 0)) {
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
    dn::PtDenoiser den;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initGpu(ctx, gpu, lang.pt) && initDenoiser(ctx, den, lang.dn)) {
            ok = true;
            break;
        }
        gpu.destroy();
    }
    if (!ok) {
        std::printf("SKIP: path tracer / denoiser unavailable: %s / %s\n", gpu.reason(), den.reason());
        return kSkip;
    }
    pt::PtScene s = sphereLitCornell(1.f);
    pt::PtCompiledScene c;
    if (!compile(s, c)) {
        return 1;
    }
    rg::Graph graph;
    constexpr u32 kWarmup = 16, kTotal = 80;
    FrameAllocs allocs;
    unsigned long long callbacks = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    u32 rebuilds = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        FrameAllocs fa;
        t_allocations = 0;
        const bool ran = runFrame(ctx, gpu, den, graph, c, frameDesc(frame + 1u), measure, &fa);
        const unsigned long long a3 = t_allocations;
        check(ran, "zero_alloc: frame ok");
        if (measure) {
            allocs.begin += fa.begin;
            allocs.graph += fa.graph;
            callbacks += a3;
        }
        if (frame == kWarmup) {
            rebuilds = den.denoiser().stats().rebuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    check(den.denoiser().stats().rebuilds == rebuilds, "zero_alloc: no denoiser rebuild in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (%s / %s kernels, %u steady-state frames):\n  PathTracerGpu::setScene + beginFrame + "
                    "PtDenoiser::beginFrame: %llu operator-new calls\n"
                    "  graph build (path tracer + producer + denoiser passes + read-backs): %llu\n"
                    "  relight.* / rdn.* pass callbacks: %llu\n",
                    gpu.kernelLanguage(), den.kernelLanguage(), kTotal - kWarmup, allocs.begin, allocs.graph,
                    callbacks);
        check(allocs.begin == 0u && allocs.graph == 0u && callbacks == 0u,
              "steady-state denoised path-tracing frames make no heap allocation");
    } else {
        std::printf("zero_alloc frames under validation: %u frames ok\n", kTotal);
    }
    ctx.executor->waitIdle();
    den.destroy();
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
