// WP-8.2 Hillaire atmosphere Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation; every
// validation message fails the run). CPU gates: test_rp_atmosphere_gpu_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): AtmosphereGpu adds atmos.transmittance / atmos.multiscatter (when the
// medium changed) + atmos.skyview + atmos.aerial, then atmos.probe runs the includable sampling helpers
// (at_sample.*) for a probe list, then read-back copies of every LUT. Both kernel languages built (Slang, GLSL)
// run every check.
//
//   --mode luts        3 configurations (noon, sunrise with ozone + camera at 3 km + odd LUT sizes, low sun
//                      single scattering): each LUT against its CPU reference on the GPU's own read-back inputs
//                      (transmittance from nothing, multi-scattering from the GPU transmittance, sky view and
//                      froxels from both), the probes (sky radiance + sun disk, sun illuminance at world points,
//                      aerial perspective, Psi_ms) against the CPU helpers on the GPU LUTs, Slang == GLSL; a
//                      second frame with the same medium skips the static passes and leaves their LUTs unchanged
//   --mode reference   sky radiance from the GPU sky-view LUT (single scattering; probe on the device and the CPU
//                      bilinear helper) vs the brute-force double single-scatter reference: every channel +-6%
//   --mode energy      GPU multi-scattering LUT: 0 <= f_ms < 1; ground irradiance with MS <= TOA flux, > without
//   --mode sweep       time of day -10 .. 90 degrees in 0.25 degree frames (static LUTs built once): every LUT
//                      finite and >= 0 every frame; sky (6 directions, device helper) and aerial perspective stable:
//                      |d log L| <= 1 and |d2 log L| <= 0.3 per frame (log(L + 1e-3 peak); a pop is a curvature
//                      spike); zenith monotone; the sky view == CPU every 20th frame and every frame of sun -1 .. 1
//                      degree (the sun in the horizon penumbra)
//   --mode zero_alloc  64 steady-state frames (sun and camera moving): 0 operator-new calls in beginFrame, the
//                      atmos.* callbacks and the whole graph build (validated run first; validation off for the count)
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Tolerances: the kernels are the CPU reference's twins in f32, but the device's exp / acos / asin / atan / cos
// differ from libm by a few ulp (Vulkan allows more), and the compilers may contract multiply-adds. Transmittance
// exp(-tau) with tau up to ~40 near the horizon amplifies that, so the LUT gates are relative with an absolute
// floor tied to the LUT's peak.
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include <fuse/renderer/atmosphere/atmosphere_brute_force.hpp>
#include <fuse/renderer/atmosphere/atmosphere_gpu.hpp>
#include <fuse/renderer/atmosphere/atmosphere_luts.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

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

[[maybe_unused]] void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
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
using namespace fuse::renderer::atmosphere;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using fuse::math::Vec3;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr f64 kPiD = 3.14159265358979323846;
constexpr f64 kDeg = kPiD / 180.0;
constexpr u32 kMaxProbes = 256;
constexpr u32 kLutCount = 5;

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

Vec3 dirElAz(f64 el, f64 az) {
    return Vec3{static_cast<f32>(std::cos(el) * std::sin(az)), static_cast<f32>(std::sin(el)),
                static_cast<f32>(std::cos(el) * std::cos(az))};
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
    Buffer readback{};
    Buffer probeIn{};
    Buffer probeOut{};
    u64 readbackBytes = 0;
    u8 readbackQueue = rg::kNoQueue;
    u8 probeInQueue = rg::kNoQueue;
    u8 probeOutQueue = rg::kNoQueue;
    u64 serial = 0;

    void destroyTargets() {
        if (allocator == nullptr) {
            return;
        }
        for (Buffer* b : {&readback, &probeIn, &probeOut}) {
            if (b->handle != nullptr) {
                allocator->destroyBuffer(*b);
            }
            *b = Buffer{};
        }
        readbackBytes = 0;
        readbackQueue = probeInQueue = probeOutQueue = rg::kNoQueue;
    }

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        destroyTargets();
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
    instanceDesc.appName = "fuse_rp_atmosphere_gpu";
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
    const AtmosCapabilities caps = queryAtmosphereCapabilities(ctx.device.get());
    if (!caps.atmosphere) {
        std::printf("SKIP: atmosphere passes unsupported: %s\n", caps.reason);
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
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    return 0;
}

u64 align256(u64 v) { return (v + 255u) & ~u64{255u}; }

/// Read-back offsets: the 5 LUT sections back to back (each 256-aligned).
struct ReadbackLayout {
    u64 at[kLutCount] = {};
    u64 bytes[kLutCount] = {};
    u64 total = 0;
};

ReadbackLayout readbackLayout(const AtmosphereGpu& atmos) {
    ReadbackLayout r{};
    u64 cursor = 0;
    for (u32 i = 0; i < kLutCount; ++i) {
        r.at[i] = cursor;
        r.bytes[i] = atmos.lutBytes(static_cast<AtmosLut>(i));
        cursor = align256(cursor + r.bytes[i]);
    }
    r.total = cursor;
    return r;
}

bool makeBuffer(Context& ctx, Buffer& b, u64 size, BufferUsage usage, MemoryUsage memory, const char* name) {
    BufferDesc d{};
    d.size = static_cast<usize>(size);
    d.usage = usage;
    d.memoryUsage = memory;
    d.name = name;
    return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr;
}

bool ensureTargets(Context& ctx, u64 readbackBytes) {
    if (ctx.probeIn.handle == nullptr) {
        const auto storage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                      static_cast<u32>(BufferUsage::ShaderDeviceAddress));
        if (!makeBuffer(ctx, ctx.probeIn, u64{kMaxProbes} * kAtProbeInputs * 16u, storage, MemoryUsage::CpuToGpu,
                        "rp_atmos.probe_in") ||
            !makeBuffer(ctx, ctx.probeOut, u64{kMaxProbes} * kAtProbeOutputs * 16u, storage, MemoryUsage::GpuToCpu,
                        "rp_atmos.probe_out") ||
            ctx.probeIn.deviceAddress == 0u || ctx.probeOut.deviceAddress == 0u) {
            return false;
        }
    }
    if (ctx.readbackBytes < readbackBytes) {
        if (ctx.readback.handle != nullptr) {
            vkDeviceWaitIdle(ctx.vkDevice);
            ctx.allocator->destroyBuffer(ctx.readback);
            ctx.readback = Buffer{};
        }
        if (!makeBuffer(ctx, ctx.readback, readbackBytes, BufferUsage::TransferDst, MemoryUsage::GpuToCpu,
                        "rp_atmos.readback")) {
            return false;
        }
        ctx.readbackBytes = readbackBytes;
        ctx.readbackQueue = rg::kNoQueue;
    }
    return true;
}

// --- probes ----------------------------------------------------------------------------------------------
struct Probe {
    Vec3 dir;
    f32 u = 0.f;
    f32 v = 0.f;
    f32 distance = 0.f;
    Vec3 pos;
};

std::vector<Probe> makeProbes(const AtParams& p) {
    std::vector<Probe> probes;
    const Vec3 sun{p.sunDir[0], p.sunDir[1], p.sunDir[2]};
    u32 k = 0;
    for (f64 el : {89.0, 60.0, 30.0, 12.0, 4.0, 1.0, -3.0, -30.0}) {
        for (f64 az : {0.0, 70.0, 160.0, 250.0}) {
            Probe q{};
            q.dir = dirElAz(el * kDeg, az * kDeg);
            q.u = std::fmod(0.07f + 0.137f * static_cast<f32>(k), 1.f);
            q.v = std::fmod(0.11f + 0.291f * static_cast<f32>(k), 1.f);
            q.distance = p.cameraPos[3] * std::fmod(0.013f * static_cast<f32>(k * k), 1.1f);
            q.pos = Vec3{137.f * static_cast<f32>(k), 25.f + 750.f * static_cast<f32>(k % 9u), -91.f * static_cast<f32>(k)};
            probes.push_back(q);
            ++k;
        }
    }
    // Sun-disk probes: the centre and inside the limb.
    for (f32 off : {0.f, 0.001f, 0.0035f}) {
        Probe q{};
        q.dir = Vec3{sun.x + off, sun.y, sun.z - off * 0.5f};
        q.pos = Vec3{0.f, 1.f, 0.f};
        probes.push_back(q);
    }
    return probes;
}

void writeProbes(Context& ctx, const std::vector<Probe>& probes) {
    f32* in = static_cast<f32*>(ctx.probeIn.mapped);
    for (usize i = 0; i < probes.size(); ++i) {
        const Probe& q = probes[i];
        const f32 v[12] = {q.dir.x, q.dir.y, q.dir.z, 0.f, q.u, q.v, q.distance, 0.f, q.pos.x, q.pos.y, q.pos.z, 0.f};
        std::memcpy(in + i * 12u, v, sizeof(v));
    }
}

// --- frame ----------------------------------------------------------------------------------------------
struct Frame {
    std::vector<AtTexel> lut[kLutCount];
    std::vector<AtTexel> probes;
};

bool initAtmos(Context& ctx, AtmosphereGpu& atmos, AtmosKernelLanguage language) {
    AtmosphereGpuDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.language = language;
    return atmos.init(d);
}

/// One frame: the atmosphere passes, the probes, and (readback) copies of every LUT.
bool buildGraph(Context& ctx, AtmosphereGpu& atmos, rg::Graph& graph, u32 probeCount, bool readback) {
    graph.reset();
    const AtmosphereGraphRefs refs = atmos.importInto(graph);
    if (!refs.luts.valid()) {
        return false;
    }
    atmos.addPasses(graph, refs);
    if (probeCount > 0u) {
        const rg::BufferRef in = graph.importBuffer(
            rg::ImportedBuffer{ctx.probeIn.handle, ctx.probeIn.desc.size, ctx.probeInQueue, &ctx.probeInQueue, "rp_atmos.in"});
        const rg::BufferRef out = graph.importBuffer(rg::ImportedBuffer{ctx.probeOut.handle, ctx.probeOut.desc.size,
                                                                        ctx.probeOutQueue, &ctx.probeOutQueue, "rp_atmos.out"});
        atmos.addProbe(graph, refs, in, ctx.probeIn.deviceAddress, out, ctx.probeOut.deviceAddress, probeCount);
    }
    if (readback) {
        const ReadbackLayout rl = readbackLayout(atmos);
        const rg::BufferRef rb = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size,
                                                                       ctx.readbackQueue, &ctx.readbackQueue, "rp_atmos.readback"});
        for (u32 i = 0; i < kLutCount; ++i) {
            atmos.addCopy(graph, refs, static_cast<AtmosLut>(i), rb, rl.at[i]);
        }
    }
    return true;
}

bool runFrame(Context& ctx, AtmosphereGpu& atmos, rg::Graph& graph, const AtmosphereLutSettings& settings,
              const AtmosphereLutView& view, const std::vector<Probe>& probes, Frame& out) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    if (!atmos.beginFrame(ctx.serial, settings, view)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    const ReadbackLayout rl = readbackLayout(atmos);
    if (!ensureTargets(ctx, rl.total)) {
        std::fprintf(stderr, "  targets failed\n");
        return false;
    }
    writeProbes(ctx, probes);
    if (!buildGraph(ctx, atmos, graph, static_cast<u32>(probes.size()), true)) {
        return false;
    }
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    atmos.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (!result.ok || !waited) {
        std::fprintf(stderr, "  execute failed\n");
        return false;
    }
    const u8* base = static_cast<const u8*>(ctx.readback.mapped);
    for (u32 i = 0; i < kLutCount; ++i) {
        out.lut[i].resize(static_cast<usize>(rl.bytes[i] / 16u));
        std::memcpy(out.lut[i].data(), base + rl.at[i], static_cast<usize>(rl.bytes[i]));
    }
    out.probes.resize(probes.size() * kAtProbeOutputs);
    std::memcpy(out.probes.data(), ctx.probeOut.mapped, out.probes.size() * sizeof(AtTexel));
    return true;
}

AtLutView viewOf(const Frame& f) {
    AtLutView v{};
    v.transmittance = f.lut[0].data();
    v.multiscatter = f.lut[1].data();
    v.skyView = f.lut[2].data();
    v.aerialScatter = f.lut[3].data();
    v.aerialTransmittance = f.lut[4].data();
    return v;
}

// --- comparisons ----------------------------------------------------------------------------------------------
struct Cmp {
    f64 worstRel = 0.0; ///< over values above the floor
    f64 worstAbs = 0.0; ///< over values below it (relative to the peak)
    f64 peak = 0.0;
    bool finite = true;
    usize values = 0;
    usize over = 0; ///< values above kLutTol
};

/// |gpu - cpu| / max(|cpu|, floor), floor = floorFrac x peak (rgb and, when withAlpha, a).
Cmp compare(const std::vector<AtTexel>& gpu, const std::vector<AtTexel>& cpu, f64 floorFrac, bool withAlpha) {
    Cmp c{};
    for (const AtTexel& t : cpu) {
        c.peak = std::max({c.peak, static_cast<f64>(t.r), static_cast<f64>(t.g), static_cast<f64>(t.b)});
    }
    const f64 floor = std::max(1e-30, c.peak * floorFrac);
    for (usize i = 0; i < std::min(gpu.size(), cpu.size()); ++i) {
        const f32 g[4] = {gpu[i].r, gpu[i].g, gpu[i].b, gpu[i].a};
        const f32 r[4] = {cpu[i].r, cpu[i].g, cpu[i].b, cpu[i].a};
        for (u32 k = 0; k < (withAlpha ? 4u : 3u); ++k) {
            c.finite = c.finite && std::isfinite(g[k]);
            const f64 d = std::fabs(static_cast<f64>(g[k]) - r[k]);
            const f64 e = d / std::max(static_cast<f64>(std::fabs(r[k])), floor);
            if (e > c.worstRel && std::getenv("FUSE_AT_DEBUG") != nullptr) {
                std::printf("    worst @%zu.%u gpu %.6e cpu %.6e (peak %.3e)\n", i, k, static_cast<f64>(g[k]),
                            static_cast<f64>(r[k]), c.peak);
            }
            c.worstRel = std::max(c.worstRel, e);
            ++c.values;
            if (e > 2e-3) {
                ++c.over;
            }
        }
    }
    if (gpu.size() != cpu.size()) {
        c.finite = false;
    }
    return c;
}

struct Config {
    const char* name;
    AtmosphereLutSettings settings;
    AtmosphereLutView view;
};

std::vector<Config> configs() {
    std::vector<Config> out;
    {
        Config c{"noon, MS, defaults", {}, {}};
        c.view.sunDirection = dirElAz(65.0 * kDeg, 30.0 * kDeg);
        c.view.sunIlluminance = Vec3{1.2f, 1.1f, 1.0f};
        out.push_back(c);
    }
    {
        Config c{"sunrise, ozone, camera 3 km, odd sizes", {}, {}};
        c.settings.ozoneAbsorption = Vec3{0.650e-6f, 1.881e-6f, 0.085e-6f};
        c.settings.groundAlbedo = Vec3{0.4f, 0.35f, 0.3f};
        c.settings.sizes = AtmosphereLutSizes{123, 37, 19, 21, 97, 61, 23, 17, 13};
        c.settings.sampling = AtmosphereLutSampling{32, 16, 6, 24, 3};
        c.view.cameraPosition = Vec3{500.f, 3000.f, -200.f};
        c.view.sunDirection = dirElAz(1.5 * kDeg, -100.0 * kDeg);
        c.view.forward = Vec3{0.3f, -0.1f, 1.f};
        c.view.right = Vec3{1.f, 0.f, -0.3f};
        c.view.up = Vec3{0.f, 1.f, 0.1f};
        c.view.aerialMaxDistance = 60000.f;
        out.push_back(c);
    }
    {
        Config c{"low sun, single scattering, looking down", {}, {}};
        c.settings.multiScattering = false;
        c.view.cameraPosition = Vec3{0.f, 800.f, 0.f};
        c.view.sunDirection = dirElAz(8.0 * kDeg, 170.0 * kDeg);
        c.view.forward = Vec3{0.f, -0.5f, 1.f};
        c.view.up = Vec3{0.f, 1.f, 0.5f};
        out.push_back(c);
    }
    return out;
}

struct Lang {
    AtmosKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[2] = {{AtmosKernelLanguage::Slang, "slang"}, {AtmosKernelLanguage::Glsl, "glsl"}};

// Gates (see the header comment): relative with an absolute floor of 1e-6 x the LUT peak.
constexpr f64 kFloor = 1e-6;
constexpr f64 kLutTol = 2e-3;

// --- luts -----------------------------------------------------------------------------------------------------
int runLuts(Context& ctx) {
    u32 ran = 0;
    std::vector<Frame> byLang[2];
    for (u32 li = 0; li < 2u; ++li) {
        AtmosphereGpu atmos;
        if (!initAtmos(ctx, atmos, kLangs[li].language)) {
            std::printf("  %s kernels not built: skipped\n", kLangs[li].name);
            continue;
        }
        ++ran;
        rg::Graph graph;
        for (const Config& cfg : configs()) {
            Frame f{};
            AtParams p{};
            resolve_at_params(cfg.settings, cfg.view, p);
            const std::vector<Probe> probes = makeProbes(p);
            if (!runFrame(ctx, atmos, graph, cfg.settings, cfg.view, probes, f)) {
                expect(false, "frame ran");
                continue;
            }
            expect(atmos.stats().staticRan, "a new medium runs the static passes");
            const AtParams& gp = atmos.params();
            const AtLutView gv = viewOf(f);
            // Each LUT against its CPU reference, on the GPU's own inputs.
            std::vector<AtTexel> cpuT, cpuM, cpuS, cpuAs, cpuAt;
            build_transmittance_lut(gp, cpuT);
            AtLutView onlyT{};
            onlyT.transmittance = gv.transmittance;
            build_multiscatter_lut(gp, onlyT, cpuM);
            build_sky_view_lut(gp, gv, cpuS);
            build_aerial_perspective(gp, gv, cpuAs, cpuAt);
            const Cmp ct = compare(f.lut[0], cpuT, kFloor, false);
            const Cmp cm = compare(f.lut[1], cpuM, kFloor, true);
            const Cmp cs = compare(f.lut[2], cpuS, kFloor, false);
            const Cmp ca = compare(f.lut[3], cpuAs, kFloor, false);
            const Cmp cat = compare(f.lut[4], cpuAt, kFloor, false);
            std::printf("  %s | %s: rel err vs CPU: transmittance %.2e, multiscatter %.2e, sky view %.2e, "
                        "aerial scatter %.2e, aerial transmittance %.2e\n",
                        kLangs[li].name, cfg.name, ct.worstRel, cm.worstRel, cs.worstRel, ca.worstRel, cat.worstRel);
            expect(ct.finite && cm.finite && cs.finite && ca.finite && cat.finite, "LUTs finite, full size");
            expect(ct.worstRel <= kLutTol, "transmittance LUT == CPU reference");
            expect(cm.worstRel <= kLutTol, "multi-scattering LUT == CPU reference (on the GPU transmittance)");
            expect(cs.worstRel <= kLutTol, "sky-view LUT == CPU reference (on the GPU LUTs)");
            expect(ca.worstRel <= kLutTol && cat.worstRel <= kLutTol, "aerial perspective == CPU reference");
            // Probes: the includable helpers on the device vs their CPU twins on the GPU LUTs.
            std::vector<Vec3> expected(probes.size() * kAtProbeOutputs);
            f64 peak[kAtProbeOutputs] = {};
            for (usize i = 0; i < probes.size(); ++i) {
                const Probe& q = probes[i];
                Vec3 s{}, t{};
                at_aerial(gp, gv, q.u, q.v, q.distance, s, t);
                const Vec3 e[kAtProbeOutputs] = {at_sky_radiance(gp, gv, q.dir, true), at_sky_radiance(gp, gv, q.dir, false),
                                                 at_sun_illuminance_at(gp, gv, q.pos), s, t,
                                                 at_multiscatter(gp, gv, gp.up[3], gp.sunDir[3])};
                for (u32 k = 0; k < kAtProbeOutputs; ++k) {
                    expected[i * kAtProbeOutputs + k] = e[k];
                    peak[k] = std::max({peak[k], static_cast<f64>(e[k].x), static_cast<f64>(e[k].y), static_cast<f64>(e[k].z)});
                }
            }
            f64 worstProbe = 0.0;
            for (usize i = 0; i < expected.size(); ++i) {
                const AtTexel& g = f.probes[i];
                const Vec3& e = expected[i];
                const f64 floorK = std::max(1e-30, peak[i % kAtProbeOutputs] * kFloor);
                const f64 gg[3] = {g.r, g.g, g.b};
                const f64 ee[3] = {e.x, e.y, e.z};
                for (u32 c = 0; c < 3u; ++c) {
                    worstProbe = std::max(worstProbe, std::fabs(gg[c] - ee[c]) / std::max(std::fabs(ee[c]), floorK));
                }
            }
            const f64 diskCentre = f.probes[(probes.size() - 3u) * kAtProbeOutputs].b;
            std::printf("  %s | %s: %zu probes (sky + disk, sky, sun illuminance, aerial, Psi_ms): max rel err %.2e;"
                        " sun-disk centre radiance %.3e\n",
                        kLangs[li].name, cfg.name, probes.size(), worstProbe, diskCentre);
            expect(worstProbe <= kLutTol, "device sampling helpers == CPU helpers");
            byLang[li].push_back(f);

            // Same medium, sun moved: the static passes are skipped and their LUTs stay put.
            AtmosphereLutView moved = cfg.view;
            moved.sunDirection = dirElAz(40.0 * kDeg, 10.0 * kDeg);
            Frame g{};
            if (runFrame(ctx, atmos, graph, cfg.settings, moved, probes, g)) {
                const bool same = std::memcmp(g.lut[0].data(), f.lut[0].data(), f.lut[0].size() * 16u) == 0 &&
                                  std::memcmp(g.lut[1].data(), f.lut[1].data(), f.lut[1].size() * 16u) == 0;
                expect(!atmos.stats().staticRan && same, "same medium: static LUTs reused unchanged");
                expect(atmos.stats().passes == 2u + 1u + kLutCount, "sky view + aerial + probe + copies only");
            } else {
                expect(false, "second frame ran");
            }
        }
        std::printf("  %s: LUT buffer rebuilds %u, static builds %u\n", kLangs[li].name, atmos.stats().lutRebuilds,
                    atmos.stats().staticBuilds);
        atmos.destroy();
    }
    if (ran == 0u) {
        std::printf("SKIP: no atmosphere kernel built\n");
        return kSkip;
    }
    if (!byLang[0].empty() && byLang[0].size() == byLang[1].size()) {
        f64 worst = 0.0;
        for (usize c = 0; c < byLang[0].size(); ++c) {
            for (u32 i = 0; i < kLutCount; ++i) {
                worst = std::max(worst, compare(byLang[0][c].lut[i], byLang[1][c].lut[i], kFloor, i == 1u).worstRel);
            }
        }
        std::printf("  Slang vs GLSL: max rel LUT difference %.2e\n", worst);
        expect(worst <= kLutTol, "Slang == GLSL");
    }
    return 0;
}

// --- reference ------------------------------------------------------------------------------------------------
int runReference(Context& ctx) {
    AtmosphereGpu atmos;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initAtmos(ctx, atmos, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: no atmosphere kernel built\n");
        return kSkip;
    }
    AtmosphereLutSettings s{};
    s.multiScattering = false;
    const BruteForceSingleScatter bf(s.atmosphere);
    rg::Graph graph;
    f64 worstLut = 0.0;
    f64 worstProbe = 0.0;
    u32 cases = 0;
    for (const f64 sunEl : {90.0, 65.0, 30.0, 10.0, 5.0, 2.0}) {
        AtmosphereLutView v{};
        v.sunDirection = dirElAz(sunEl * kDeg, 0.0);
        std::vector<Probe> probes;
        for (const f64 el : {90.0, 60.0, 30.0, 15.0, 10.0, 5.0, 3.0}) {
            for (const f64 az : {0.0, 45.0, 90.0, 135.0, 180.0}) {
                Probe q{};
                q.dir = dirElAz(el * kDeg, az * kDeg);
                q.pos = Vec3{0.f, 1.f, 0.f};
                probes.push_back(q);
            }
        }
        Frame f{};
        if (!runFrame(ctx, atmos, graph, s, v, probes, f)) {
            expect(false, "frame ran");
            continue;
        }
        const AtLutView gv = viewOf(f);
        f64 worstHere = 0.0;
        for (usize i = 0; i < probes.size(); ++i) {
            const Vec3 d = probes[i].dir;
            f64 ref[3];
            bf.inscatter(1.0, d.x, d.y, d.z, v.sunDirection.x, v.sunDirection.y, v.sunDirection.z, 1024, 400, ref);
            const Vec3 cpuSide = at_sky_radiance(atmos.params(), gv, d, false);
            const AtTexel& dev = f.probes[i * kAtProbeOutputs + 1u];
            const f64 lut[3] = {cpuSide.x, cpuSide.y, cpuSide.z};
            const f64 probe[3] = {dev.r, dev.g, dev.b};
            for (int c = 0; c < 3; ++c) {
                worstHere = std::max(worstHere, std::fabs(lut[c] / ref[c] - 1.0));
                worstProbe = std::max(worstProbe, std::fabs(probe[c] / ref[c] - 1.0));
            }
            ++cases;
        }
        std::printf("  sun %4.1f deg: GPU sky-view LUT vs brute force: max per-channel rel err %.4f\n", sunEl, worstHere);
        worstLut = std::max(worstLut, worstHere);
    }
    std::printf("reference (%s): %u directions x rgb: GPU sky-view LUT %.4f, device sampling helper %.4f "
                "(gate 0.06)\n",
                atmos.kernelLanguage(), cases, worstLut, worstProbe);
    expect(worstLut <= 0.06 && worstProbe <= 0.06, "GPU sky radiance within +-6% of the brute-force reference");
    atmos.destroy();
    return 0;
}

// --- energy ---------------------------------------------------------------------------------------------------
f64 skyIrradiance(const AtParams& p, const AtLutView& lv) {
    constexpr u32 kTheta = 32;
    constexpr u32 kPhi = 64;
    f64 e = 0.0;
    for (u32 i = 0; i < kTheta; ++i) {
        const f64 theta = (static_cast<f64>(i) + 0.5) / kTheta * (kPiD * 0.5);
        for (u32 j = 0; j < kPhi; ++j) {
            const f64 phi = (static_cast<f64>(j) + 0.5) / kPhi * 2.0 * kPiD;
            const Vec3 L = at_sky_view(p, lv, dirElAz(kPiD * 0.5 - theta, phi));
            e += (static_cast<f64>(L.x) + L.y + L.z) / 3.0 * std::cos(theta) * std::sin(theta) *
                 (kPiD * 0.5 / kTheta) * (2.0 * kPiD / kPhi);
        }
    }
    return e;
}

int runEnergy(Context& ctx) {
    AtmosphereGpu atmos;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initAtmos(ctx, atmos, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: no atmosphere kernel built\n");
        return kSkip;
    }
    rg::Graph graph;
    for (const f64 sunEl : {90.0, 30.0, 5.0}) {
        f64 e[2] = {0.0, 0.0};
        f64 direct = 0.0;
        f32 fMin = 1.f, fMax = 0.f;
        for (u32 ms = 0; ms < 2u; ++ms) {
            AtmosphereLutSettings s{};
            s.multiScattering = ms == 1u;
            AtmosphereLutView v{};
            v.sunDirection = dirElAz(sunEl * kDeg, 0.0);
            Frame f{};
            if (!runFrame(ctx, atmos, graph, s, v, {}, f)) {
                expect(false, "frame ran");
                continue;
            }
            const AtLutView gv = viewOf(f);
            e[ms] = skyIrradiance(atmos.params(), gv);
            const Vec3 t = at_sun_transmittance(atmos.params(), gv, atmos.params().up[3], atmos.params().sunDir[3]);
            direct = (static_cast<f64>(t.x) + t.y + t.z) / 3.0 * atmos.params().sunDir[3];
            for (const AtTexel& m : f.lut[1]) {
                fMin = std::min(fMin, m.a);
                fMax = std::max(fMax, m.a);
            }
        }
        const f64 toa = std::sin(sunEl * kDeg);
        std::printf("  sun %4.1f deg: GPU f_ms in [%.4f, %.4f]; sky irradiance single %.4f, with MS %.4f (+%.1f%%);"
                    " (sky + direct) / TOA %.3f\n",
                    sunEl, fMin, fMax, e[0], e[1], 100.0 * (e[1] / e[0] - 1.0), (e[1] + direct) / toa);
        expect(fMin >= 0.f && fMax < 1.f, "GPU multi-scattering LUT: 0 <= f_ms < 1");
        expect(e[1] > e[0], "multi-scattering adds sky irradiance");
        expect((e[1] + direct) / toa <= 1.0 && (e[1] + direct) / toa > 0.5, "ground irradiance within the TOA budget");
    }
    std::printf("energy (%s): ok\n", atmos.kernelLanguage());
    atmos.destroy();
    return 0;
}

// --- sweep ----------------------------------------------------------------------------------------------------
/// As the CPU sweep gate: the largest step and second difference of log(L + 1e-3 x peak) along the sweep.
void stability(const std::vector<f64>& series, f64 peak, f64& maxStep, f64& maxCurvature) {
    const f64 eps = 1e-3 * peak;
    for (usize i = 1; i < series.size(); ++i) {
        maxStep = std::max(maxStep, std::fabs(std::log(series[i] + eps) - std::log(series[i - 1] + eps)));
        if (i + 1u < series.size()) {
            maxCurvature = std::max(maxCurvature, std::fabs(std::log(series[i + 1] + eps) - 2.0 * std::log(series[i] + eps) +
                                                            std::log(series[i - 1] + eps)));
        }
    }
}

int runSweep(Context& ctx) {
    AtmosphereGpu atmos;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initAtmos(ctx, atmos, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: no atmosphere kernel built\n");
        return kSkip;
    }
    AtmosphereLutSettings s{};
    rg::Graph graph;
    const Vec3 dirs[] = {dirElAz(90.0 * kDeg, 0.0), dirElAz(30.0 * kDeg, 0.3), dirElAz(30.0 * kDeg, kPiD),
                         dirElAz(5.0 * kDeg, 0.5 * kPiD), dirElAz(2.0 * kDeg, 0.3), dirElAz(-5.0 * kDeg, 0.3)};
    constexpr u32 kDirs = sizeof(dirs) / sizeof(dirs[0]);
    std::vector<f64> sky[kDirs];
    std::vector<f64> aerial;
    f64 worstCpu = 0.0;
    usize cpuValues = 0;
    usize cpuOver = 0;
    bool finite = true;
    bool monotone = true;
    u32 frames = 0;
    std::vector<Probe> probes;
    for (const Vec3& d : dirs) {
        Probe q{};
        q.dir = d;
        q.u = 0.5f;
        q.v = 0.4f;
        q.distance = 20000.f;
        q.pos = Vec3{0.f, 1.f, 0.f};
        probes.push_back(q);
    }
    for (u32 step = 0; step <= 400u; ++step) {
        const f64 el = -10.0 + 0.25 * step;
        AtmosphereLutView v{};
        v.sunDirection = dirElAz(el * kDeg, 20.0 * kDeg);
        Frame f{};
        if (!runFrame(ctx, atmos, graph, s, v, probes, f)) {
            expect(false, "frame ran");
            break;
        }
        for (u32 i = 2; i < kLutCount; ++i) {
            for (const AtTexel& t : f.lut[i]) {
                finite = finite && std::isfinite(t.r) && std::isfinite(t.g) && std::isfinite(t.b) && t.r >= 0.f &&
                         t.g >= 0.f && t.b >= 0.f;
            }
        }
        for (u32 i = 0; i < kDirs; ++i) {
            const AtTexel& L = f.probes[i * kAtProbeOutputs + 1u];
            const f64 lum = (static_cast<f64>(L.r) + L.g + L.b) / 3.0;
            if (i == 0u && !sky[0].empty() && lum + 1e-12 < sky[0].back()) {
                monotone = false;
            }
            sky[i].push_back(lum);
        }
        const AtTexel& ap = f.probes[3u];
        aerial.push_back((static_cast<f64>(ap.r) + ap.g + ap.b) / 3.0);
        if (step % 20u == 0u || (el > -1.1 && el < 1.1)) { // every 20th frame + every frame around sunrise
            std::vector<AtTexel> cpuS;
            build_sky_view_lut(atmos.params(), viewOf(f), cpuS);
            const Cmp c = compare(f.lut[2], cpuS, kFloor, false);
            if (std::getenv("FUSE_AT_DEBUG") != nullptr) {
                std::printf("    sun %.2f deg: sky view vs CPU %.2e (%zu over)\n", el, c.worstRel, c.over);
            }
            worstCpu = std::max(worstCpu, c.worstRel);
            cpuValues += c.values;
            cpuOver += c.over;
        }
        ++frames;
    }
    f64 peak = 0.0;
    for (const std::vector<f64>& series : sky) {
        for (const f64 x : series) {
            peak = std::max(peak, x);
        }
    }
    f64 apPeak = 0.0;
    for (const f64 x : aerial) {
        apPeak = std::max(apPeak, x);
    }
    f64 maxStep = 0.0, maxCurvature = 0.0, apStep = 0.0, apCurvature = 0.0;
    for (const std::vector<f64>& series : sky) {
        stability(series, peak, maxStep, maxCurvature);
    }
    stability(aerial, apPeak, apStep, apCurvature);
    std::printf("sweep (%s): %u frames (sun -10 .. 90 deg, 0.25 deg / frame): finite %s, zenith monotone %s;\n"
                "  sky: max |d log L| %.3f, |d2 log L| %.3f per frame; aerial perspective: %.3f, %.3f;\n"
                "  sky view vs CPU (every 20th frame, every frame at sun -1 .. 1 deg): max rel err %.2e, %zu of %zu values above 2e-3; static builds %u\n",
                atmos.kernelLanguage(), frames, finite ? "yes" : "no", monotone ? "yes" : "no", maxStep, maxCurvature,
                apStep, apCurvature, worstCpu, cpuOver, cpuValues, atmos.stats().staticBuilds);
    expect(frames == 401u && finite, "sweep: every LUT finite and >= 0");
    expect(monotone, "sweep: zenith radiance non-decreasing with sun elevation");
    expect(maxStep <= 1.0 && apStep <= 1.0, "sweep: sky / aerial change by less than a factor e per frame");
    expect(maxCurvature <= 0.3 && apCurvature <= 0.3, "sweep: no pops (second difference of log radiance)");
    expect(worstCpu <= kLutTol && cpuOver == 0u, "sweep: sky view == CPU reference");
    expect(atmos.stats().staticBuilds == 1u, "sweep: transmittance / multi-scattering built once");
    atmos.destroy();
    return 0;
}

// --- zero_alloc -----------------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "atmos.", 6) == 0) {
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
    AtmosphereGpu atmos;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initAtmos(ctx, atmos, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: no atmosphere kernel built\n");
        return kSkip;
    }
    AtmosphereLutSettings s{};
    s.sizes = AtmosphereLutSizes{64, 16, 16, 16, 48, 27, 16, 16, 16};
    AtmosphereLutView v{};
    if (!atmos.beginFrame(1, s, v) || !ensureTargets(ctx, 16u)) {
        std::fprintf(stderr, "FAIL: setup\n");
        return 1;
    }
    std::vector<Probe> probes = makeProbes(atmos.params());
    writeProbes(ctx, probes);
    rg::Graph graph;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long side = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    u32 rebuilds = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        v.sunDirection = dirElAz((5.0 + 0.7 * frame) * kDeg, 0.1 * frame);
        v.cameraPosition = Vec3{10.f * static_cast<f32>(frame), 2.f + 5.f * static_cast<f32>(frame), 0.f};
        if (frame % 16u == 8u) {
            atmos.invalidate(); // the static passes too, now and then
        }
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        t_allocations = 0;
        t_count = measure;
        const bool begun = atmos.beginFrame(ctx.serial, s, v);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, atmos, graph, static_cast<u32>(probes.size()), false);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle();
        atmos.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(begun && result.ok && waited, "frame ok");
        if (measure) {
            side += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
        if (frame == kWarmup) {
            rebuilds = atmos.stats().lutRebuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(atmos.stats().lutRebuilds == rebuilds && rebuilds == 1u, "no LUT buffer rebuild in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s kernels; skyview + aerial + probe every frame, static passes every\n"
                    "  16th; sun and camera moving)\n"
                    "  AtmosphereGpu::beginFrame + atmos.* pass callbacks: %llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (imports and passes): %llu\n",
                    kTotal - kWarmup, atmos.kernelLanguage(), side, callbacks, build);
        expect(side == 0u, "the atmosphere passes make no steady-state heap allocations");
        expect(build == 0u, "graph build with the atmosphere passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok (%s)\n", kTotal,
                    atmos.kernelLanguage());
    }
    atmos.destroy();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "luts";
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
        if (mode == "luts") {
            rc = runLuts(ctx);
        } else if (mode == "reference") {
            rc = runReference(ctx);
        } else if (mode == "energy") {
            rc = runEnergy(ctx);
        } else if (mode == "sweep") {
            rc = runSweep(ctx);
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
