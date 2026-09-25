// FUSE Relight RL-5.6 Vulkan gates (Lavapipe): volumetrics and the particle composite on the GPU (VolumetricsGpu,
// volumetrics_gpu.hpp: the "relight.vol.*" passes after the RL-5.1 path tracer's "relight.pt.trace" in the same render
// graph, reading its radiance, depth, RL-4.4 light ring and TLAS - the path-tracer hook) against the CPU reference
// (VolumetricsCpu, the same single-source core rl_vol_core.h) on the RL-5.1 Cornell scene in a medium, with shadows,
// reservoir reuse, reprojection and two particle systems.
//
//   parity       per kernel language, four frames (history and reuse from the second): every GPU pass replayed by the
//                CPU reference on the GPU's own read-back inputs -
//                  inject     current (sigma_s L, sigma_t) and the reservoirs: >= 97% of the froxels within 1e-3
//                             relative (RIS selection and shadow rays against the double-precision CPU BVH may flip);
//                  temporal / integrate   >= 99.9% within 1e-4 relative;
//                  apply      (fog + particles over the path tracer's radiance) >= 99.9% of the pixels within 1e-4;
//                the composite differs from the path tracer's radiance (the medium and the particles are visible) and
//                is finite.
//   determinism  two GPU runs of the same frames give bit-identical sections.
//   omm_caps     VK_EXT_opacity_micromap capability query (RL-5.6 OMM): absent (Lavapipe) -> any-hit fallback path.
//   zero_alloc   64 steady-state frames: no operator-new call in VolumetricsGpu::beginFrame, the graph build
//                (PathTracerGpu importInto + addTracePass, volExternalFromPathTracer, VolumetricsGpu::addPasses,
//                read-back) and the relight.* pass callbacks; no reallocation.
// Validation: every run is under the Khronos layer with sync validation (native) or the host-injected layer (Wine,
// --external-validation with a negative control); any message fails. Exit 77 = skip.
#include "pt_test_scenes.hpp"
#include "vol_test_common.hpp"

#include <fuse/relight/render/pathtrace/omm/omm.hpp>
#include <fuse/relight/render/pathtrace/pt_gpu.hpp>
#include <fuse/relight/render/volumetrics/volumetrics.hpp>
#include <fuse/relight/render/volumetrics/volumetrics_gpu.hpp>

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
constexpr u32 kW = 24u, kH = 20u;

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
    instanceDesc.appName = "rl_vol_vk";
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
    d.name = "rl_vol.staging";
    if (!ctx.allocator->createBuffer(d, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    return 0;
}

namespace vol = fuse::relight::render::volumetrics;
namespace pa = fuse::relight::particles;

struct Gpus {
    pt::PathTracerGpu pt;
    vol::VolumetricsGpu vol;
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
    vol::VolumetricsGpuDesc vd{};
    vd.device = ctx.device.get();
    vd.allocator = ctx.allocator.get();
    vd.bindless = &ctx.bindless;
    vd.language = language == pt::PtKernelLanguage::Glsl    ? vol::VolKernelLanguage::Glsl
                  : language == pt::PtKernelLanguage::Slang ? vol::VolKernelLanguage::Slang
                                                            : vol::VolKernelLanguage::Auto;
    vd.framesInFlight = 3;
    return g.vol.init(vd);
}

pt::PtSettings ptSettings() {
    pt::PtSettings st;
    st.maxBounces = 2;
    st.samplesPerPixel = 1;
    return st;
}

/// The volumetrics frame over the Cornell scene: medium, lights, shadows, reuse, reprojection, two particle systems.
struct VolScene {
    pt::PtScene scene = cornell(false);
    pt::PtCompiledScene compiled;
    vol::VolFrameDesc desc;
    std::vector<pa::GpuParticleVertex> verts;
};

bool makeScene(VolScene& s) {
    std::string error;
    if (!s.compiled.compile(s.scene, {}, &error)) {
        check(false, "compile: " + error);
        return false;
    }
    vol::VolFrameDesc& d = s.desc;
    d.camera = s.scene.camera;
    d.prevCamera = s.scene.camera;
    d.width = kW;
    d.height = kH;
    d.gridX = 8;
    d.gridY = 6;
    d.gridZ = 16;
    d.nearZ = 0.1f;
    d.farZ = 16.f;
    d.medium.density = 0.08f;
    d.medium.falloff = 0.1f;
    d.medium.anisotropy = 0.3f;
    d.medium.ambient[0] = d.medium.ambient[1] = d.medium.ambient[2] = 0.05f;
    d.candidates = 4;
    d.temporalAlpha = 0.2f;
    d.flags = vol::kVolLights | vol::kVolShadows | vol::kVolReuse | vol::kVolFog | vol::kVolReproject |
              vol::kVolParticles;
    s.verts.clear();
    vol_test::addQuad(s.verts, -0.6f, 0.2f, 3.f, 0.9f, vol_test::packColor(0.9f, 0.4f, 0.1f, 0.6f));
    vol_test::addQuad(s.verts, 0.2f, -0.1f, 2.f, 0.7f, vol_test::packColor(0.1f, 0.5f, 0.9f, 0.5f));
    vol_test::addQuad(s.verts, 0.5f, 0.5f, 1.f, 0.5f, vol_test::packColor(1.f, 1.f, 0.6f, 0.4f));
    d.systemCount = 2;
    d.systems[0] = vol::VolParticleSystem{0u, 2u, vol::kVolAlpha, 0u};
    d.systems[1] = vol::VolParticleSystem{2u, 1u, vol::kVolAdditive, vol::kVolSoftDisc};
    return true;
}

bool buildGraph(Gpus& g, rg::Graph& graph) {
    graph.reset();
    const pt::PtGraphRefs refs = g.pt.importInto(graph);
    if (!refs.valid || !g.pt.addTracePass(graph, refs)) {
        return false;
    }
    const vol::VolExternal ext = vol::volExternalFromPathTracer(g.pt, refs);
    if (!g.vol.addPasses(graph, ext)) {
        return false;
    }
    rg::PassBuilder rb = graph.addPass("readback.host", nullptr, nullptr);
    rb.use(refs.outputs, rg::Access::HostRead);
    for (u32 s = 0; s < vol::kVolSections; ++s) {
        rb.use(g.vol.graphRefs().sections[s], rg::Access::HostRead);
    }
    return true;
}

struct FrameAllocs {
    unsigned long long begin = 0;
    unsigned long long graph = 0;
};

bool runFrame(Context& ctx, Gpus& g, rg::Graph& graph, VolScene& s, u32 frame, bool measure = false,
              FrameAllocs* allocs = nullptr) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    pt::PtFrameDesc fd;
    fd.width = kW;
    fd.height = kH;
    fd.frameSeed = 5u;
    fd.sampleBase = frame;
    fd.settings = ptSettings();
    s.desc.frame = frame;
    bool ok = g.pt.setScene(s.compiled) && g.pt.beginFrame(ctx.serial, s.compiled, fd);
    t_allocations = 0;
    t_count = measure;
    ok = ok && g.vol.beginFrame(ctx.serial, s.desc, s.compiled.lightSet().lightCount(), s.verts);
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
        std::fprintf(stderr, "  frame setup failed: %s / %s\n", g.pt.reason(), g.vol.reason());
        return false;
    }
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    g.pt.collectRetired(ctx.serial);
    g.vol.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    return result.ok && waited;
}

std::vector<vol::Word> section(const Gpus& g, u32 s, std::size_t words) {
    const auto* p = static_cast<const vol::Word*>(g.vol.mappedSection(s));
    return std::vector<vol::Word>(p, p + words);
}

const u8* ptSection(const pt::PathTracerGpu& gpu, u32 s) {
    return static_cast<const u8*>(gpu.mappedOutputs()) + u64(s) * gpu.outputStride();
}

bool close1(float a, float b, float tol) { return std::fabs(a - b) <= tol * std::max(1.f, std::fabs(b)) || (a == b); }
bool closeW(const vol::Word& a, const vol::Word& b, float tol) {
    return close1(a.x, b.x, tol) && close1(a.y, b.y, tol) && close1(a.z, b.z, tol) && close1(a.w, b.w, tol);
}
double agreement(const std::vector<vol::Word>& gpu, const std::vector<vol::Word>& cpu, std::size_t n, float tol,
                 u32 stride = 1u) {
    std::size_t same = 0;
    for (std::size_t i = 0; i < n; ++i) {
        bool ok = true;
        for (u32 k = 0; k < stride; ++k) {
            ok = ok && closeW(gpu[i * stride + k], cpu[i * stride + k], tol);
        }
        same += ok ? 1u : 0u;
    }
    return double(same) / double(std::max<std::size_t>(n, 1u));
}

void parityCase(Context& ctx, Gpus& g, rg::Graph& graph) {
    VolScene s;
    if (!makeScene(s)) {
        return;
    }
    g.vol.resetHistory();
    vol::VolumetricsCpu cpu;
    cpu.resize(s.desc.gridX, s.desc.gridY, s.desc.gridZ, kW, kH);
    const u32 froxels = s.desc.gridX * s.desc.gridY * s.desc.gridZ;
    const u32 pixels = kW * kH;
    std::vector<float> depth(pixels);
    for (u32 f = 0; f < 4u; ++f) {
        if (!runFrame(ctx, g, graph, s, f)) {
            check(false, "parity: GPU frame");
            return;
        }
        const bool history = (g.vol.frameFlags() & vol::kVolHistory) != 0u;
        const std::vector<vol::Word> gCur = section(g, vol::kVolSecCurrent, froxels);
        const std::vector<vol::Word> gResCur = section(g, g.vol.reservoirSection(true), froxels * 2u);
        const std::vector<vol::Word> gResPrev = section(g, g.vol.reservoirSection(false), froxels * 2u);
        const std::vector<vol::Word> gHistCur = section(g, g.vol.historySection(true), froxels);
        const std::vector<vol::Word> gHistPrev = section(g, g.vol.historySection(false), froxels);
        const std::vector<vol::Word> gInt = section(g, vol::kVolSecIntegrated, froxels);
        const std::vector<vol::Word> gOut = section(g, vol::kVolSecOutput, pixels);
        const auto* rad = reinterpret_cast<const vol::Word*>(ptSection(g.pt, pt::kPtOutRadiance));
        const std::vector<vol::Word> color(rad, rad + pixels);
        std::memcpy(depth.data(), ptSection(g.pt, pt::kPtOutDepth), pixels * sizeof(float));
        vol::VolCpuInputs in;
        in.lights = &s.compiled.lightSet();
        in.scene = &s.compiled.reference();
        in.color = color.data();
        in.depth = depth.data();
        in.vertices = s.verts;
        // inject
        cpu.buffer(vol::kVolResPrev) = gResPrev;
        check(cpu.runStage(vol::kVolInject, s.desc, in, history), "parity: CPU inject");
        const double aInject = agreement(gCur, cpu.buffer(vol::kVolCurrent), froxels, 1e-3f);
        const double aRes = agreement(gResCur, cpu.buffer(vol::kVolResCur), froxels, 1e-3f, 2u);
        // temporal
        cpu.buffer(vol::kVolCurrent) = gCur;
        cpu.buffer(vol::kVolHistPrev) = gHistPrev;
        check(cpu.runStage(vol::kVolTemporal, s.desc, in, history), "parity: CPU temporal");
        const double aTemporal = agreement(gHistCur, cpu.buffer(vol::kVolHistCur), froxels, 1e-4f);
        // integrate
        cpu.buffer(vol::kVolHistCur) = gHistCur;
        check(cpu.runStage(vol::kVolIntegrate, s.desc, in, history), "parity: CPU integrate");
        const double aIntegrate = agreement(gInt, cpu.buffer(vol::kVolIntegrated), froxels, 1e-4f);
        // apply
        cpu.buffer(vol::kVolIntegrated) = gInt;
        check(cpu.runStage(vol::kVolApply, s.desc, in, history), "parity: CPU apply");
        const double aApply = agreement(gOut, cpu.buffer(vol::kVolColorOut), pixels, 1e-4f);
        u32 changed = 0, finite = 0, lit = 0;
        for (u32 i = 0; i < pixels; ++i) {
            changed += std::fabs(gOut[i].x - color[i].x) + std::fabs(gOut[i].z - color[i].z) > 1e-3f ? 1u : 0u;
            finite += std::isfinite(gOut[i].x) && std::isfinite(gOut[i].y) && std::isfinite(gOut[i].z) ? 1u : 0u;
        }
        for (u32 i = 0; i < froxels; ++i) {
            lit += gResCur[i * 2u].w >= 0.f ? 1u : 0u;
        }
        std::printf("  frame %u (history %s): inject %.2f%% (reservoirs %.2f%%, %u/%u froxels with a light sample), "
                    "temporal %.2f%%, integrate %.2f%%, apply %.2f%% (%u/%u pixels changed by medium / particles)\n",
                    f, history ? "on" : "off", 100.0 * aInject, 100.0 * aRes, lit, froxels, 100.0 * aTemporal,
                    100.0 * aIntegrate, 100.0 * aApply, changed, pixels);
        check(aInject >= 0.97 && aRes >= 0.97, "parity: inject GPU == CPU replay (>= 97%)");
        check(lit * 2u > froxels, "parity: most froxels hold a light sample");
        check(aTemporal >= 0.999, "parity: temporal GPU == CPU replay (>= 99.9%)");
        check(aIntegrate >= 0.999, "parity: integrate GPU == CPU replay (>= 99.9%)");
        check(aApply >= 0.999, "parity: apply GPU == CPU replay (>= 99.9%)");
        check(finite == pixels && changed * 2u > pixels, "parity: composite finite and visibly different");
        if (f > 0u) {
            check(history, "parity: history on after the first frame");
        }
    }
}

void determinismCase(Context& ctx, Gpus& g, rg::Graph& graph) {
    std::vector<u8> first, bytes;
    bool identical = false;
    for (int k = 0; k < 2; ++k) {
        VolScene s;
        if (!makeScene(s)) {
            return;
        }
        g.vol.resetHistory();
        for (u32 f = 0; f < 3u; ++f) {
            if (!runFrame(ctx, g, graph, s, f)) {
                check(false, "determinism: GPU frame");
                return;
            }
        }
        const u32 froxels = s.desc.gridX * s.desc.gridY * s.desc.gridZ;
        bytes.clear();
        const u32 secs[4] = {vol::kVolSecCurrent, vol::kVolSecIntegrated, g.vol.historySection(true),
                             g.vol.reservoirSection(true)};
        for (u32 sct : secs) {
            const auto* p = static_cast<const u8*>(g.vol.mappedSection(sct));
            bytes.insert(bytes.end(), p, p + std::size_t(froxels) * 16u);
        }
        const auto* o = static_cast<const u8*>(g.vol.mappedSection(vol::kVolSecOutput));
        bytes.insert(bytes.end(), o, o + std::size_t(kW) * kH * 16u);
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
                std::strstr(g.vol.reason(), "no volumetrics kernel") != nullptr) {
                std::printf("%s: kernel not built, skipped\n", lang.name);
                continue;
            }
            std::printf("SKIP: GPU path tracer / volumetrics unavailable: %s / %s\n", g.pt.reason(), g.vol.reason());
            return kSkip;
        }
        ++languages;
        std::printf("%s kernels:\n", g.vol.kernelLanguage());
        rg::Graph graph;
        parityCase(ctx, g, graph);
        determinismCase(ctx, g, graph);
        ctx.executor->waitIdle();
        g.vol.destroy();
        g.pt.destroy();
    }
    if (languages == 0u) {
        std::printf("SKIP: no volumetrics kernel built\n");
        return kSkip;
    }
    return 0;
}

thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "relight.vol.", 12) == 0) {
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
        g.vol.destroy();
        g.pt.destroy();
    }
    if (!ok) {
        std::printf("SKIP: GPU path tracer / volumetrics unavailable: %s / %s\n", g.pt.reason(), g.vol.reason());
        return kSkip;
    }
    VolScene s;
    if (!makeScene(s)) {
        return 1;
    }
    rg::Graph graph;
    constexpr u32 kWarmup = 16, kTotal = 80;
    FrameAllocs allocs;
    unsigned long long callbacks = 0;
    u32 reallocations = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        s.desc.prevCamera = s.desc.camera;
        s.desc.camera.origin[0] = 0.02f * float(frame % 5u);
        FrameAllocs fa;
        t_allocations = 0;
        const bool ran = runFrame(ctx, g, graph, s, frame, measure, &fa);
        const unsigned long long a3 = t_allocations;
        check(ran, "zero_alloc: frame ok");
        if (measure) {
            allocs.begin += fa.begin;
            allocs.graph += fa.graph;
            callbacks += a3;
        }
        if (frame == kWarmup) {
            reallocations = g.vol.stats().reallocations;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    check(g.vol.stats().reallocations == reallocations, "zero_alloc: no reallocation in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (%s kernels, %u steady-state frames, %u volumetrics passes / frame):\n"
                    "  VolumetricsGpu::beginFrame: %llu operator-new calls\n"
                    "  graph build (PT importInto + addTracePass + VolumetricsGpu::addPasses + read-back): %llu\n"
                    "  relight.vol.* pass callbacks: %llu\n",
                    g.vol.kernelLanguage(), kTotal - kWarmup, g.vol.stats().passes, allocs.begin, allocs.graph,
                    callbacks);
        check(allocs.begin == 0u && allocs.graph == 0u && callbacks == 0u,
              "steady-state volumetrics frames make no heap allocation");
    } else {
        std::printf("zero_alloc frames under validation: %u frames ok (%s)\n", kTotal, g.vol.kernelLanguage());
    }
    ctx.executor->waitIdle();
    g.vol.destroy();
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
        // RL-5.6 opacity micromaps: the device capability gate (VK_EXT_opacity_micromap; Lavapipe: absent -> the
        // any-hit fallback, whose equivalence with the micromap path is gated on the CPU by rl_omm_*).
        {
            namespace om = fuse::relight::render::pathtrace::omm;
            om::OmmDeviceCaps caps;
            const bool queried = om::ommQueryDevice(*ctx.device, caps);
            om::OmmBuildDesc od;
            const om::OmmPath path = om::ommSelectPath(caps, od);
            std::printf("omm: VK_EXT_opacity_micromap %s (micromap feature %d, max levels 2-state %u / 4-state %u) -> "
                        "%s path\n",
                        caps.extension ? "present" : "absent", caps.micromap ? 1 : 0, caps.max2StateLevel,
                        caps.max4StateLevel, path == om::OmmPath::Hardware ? "hardware" : "any-hit fallback");
            check(queried, "omm: capability query");
            check(caps.extension || path == om::OmmPath::AnyHitFallback, "omm: fallback selected without the extension");
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
