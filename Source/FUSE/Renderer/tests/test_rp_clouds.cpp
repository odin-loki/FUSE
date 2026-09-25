// WP-8.3 volumetric clouds Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation; every
// validation message fails the run). CPU gates: test_rp_clouds_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): the WP-8.2 atmosphere passes (when used), then one or more
// VolumetricClouds instances (clouds.noise.* when (re)baked, clouds.march, clouds.reconstruct, clouds.composite),
// optionally clouds.probe, then read-back copies. Both kernel languages built (Slang, GLSL) run the parity checks.
//
//   --mode noise       the GPU-baked shape / detail volumes and weather map vs the CPU reference (all texels);
//                      Slang == GLSL; a second frame does not re-bake and leaves them unchanged
//   --mode ray         lit by the atmosphere (sun illuminance, sky ambient): every pixel marched (block 1) vs the
//                      CPU single-ray integration on the GPU's own noise and LUTs; probes (single rays from
//                      below / inside / above the layer and the density at their origins) vs the CPU; the
//                      reconstruction without history == the fresh samples; the composite vs the CPU composite
//   --mode analytic    homogeneous shell, constant sun: vertical ray vs the closed forms (transmittance, single
//                      scattering), oblique rays vs a double-precision brute-force integral
//   --mode converge    static camera, 1/16 per frame with jitter, 160 frames: the amortised image vs a 128-step
//                      full-resolution reference beats a single jittered full-resolution frame; the period-to-
//                      period change decays; without jitter the amortised image == the full march after 16
//                      frames (bit-exact on the device)
//   --mode ghosting    camera turning and translating, wind: 1/16 amortised + reprojection vs the per-frame full
//                      march (relative L1, trail energy) against a same-pixel-history baseline; the device
//                      reconstruction == the CPU twin on the device's own inputs
//   --mode composite   caller background + scene depth and sky background with aerial perspective vs the CPU
//                      composite; no clouds -> the sky
//   --mode zero_alloc  64 steady-state frames (camera, sun and wind moving; noise re-baked every 16th frame):
//                      0 operator-new calls in beginFrame, the clouds.* callbacks and the graph build
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include <fuse/renderer/atmosphere/atmosphere_gpu.hpp>
#include <fuse/renderer/atmosphere/atmosphere_luts.hpp>
#include <fuse/renderer/clouds/cloud_reference.hpp>
#include <fuse/renderer/clouds/volumetric_clouds.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <algorithm>
#include <array>
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

[[maybe_unused]] void expectLe(double value, double bound, const char* message) {
    if (!(value <= bound)) {
        std::fprintf(stderr, "FAIL: %s (got %.6g, bound %.6g)\n", message, value, bound);
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
using namespace fuse::renderer::clouds;
namespace at = fuse::renderer::atmosphere;
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
constexpr u32 kMaxProbes = 64;
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
    Buffer background{};
    Buffer depth{};
    u64 readbackBytes = 0;
    u8 readbackQueue = rg::kNoQueue;
    u8 probeInQueue = rg::kNoQueue;
    u8 probeOutQueue = rg::kNoQueue;
    u8 backgroundQueue = rg::kNoQueue;
    u8 depthQueue = rg::kNoQueue;
    u64 serial = 0;

    void destroyTargets() {
        if (allocator == nullptr) {
            return;
        }
        for (Buffer* b : {&readback, &probeIn, &probeOut, &background, &depth}) {
            if (b->handle != nullptr) {
                allocator->destroyBuffer(*b);
            }
            *b = Buffer{};
        }
        readbackBytes = 0;
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
    instanceDesc.appName = "fuse_rp_clouds";
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
    const CloudCapabilities caps = queryCloudCapabilities(ctx.device.get());
    if (!caps.clouds) {
        std::printf("SKIP: cloud passes unsupported: %s\n", caps.reason);
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

bool makeBuffer(Context& ctx, Buffer& b, u64 size, BufferUsage usage, MemoryUsage memory, const char* name) {
    BufferDesc d{};
    d.size = static_cast<usize>(size);
    d.usage = usage;
    d.memoryUsage = memory;
    d.name = name;
    return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr;
}

constexpr u32 kMaxOutPixels = 64u * 32u;

bool ensureTargets(Context& ctx, u64 readbackBytes) {
    if (ctx.probeIn.handle == nullptr) {
        const auto storage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                      static_cast<u32>(BufferUsage::ShaderDeviceAddress));
        if (!makeBuffer(ctx, ctx.probeIn, u64{kMaxProbes} * kClProbeInputs * 16u, storage, MemoryUsage::CpuToGpu,
                        "rp_clouds.probe_in") ||
            !makeBuffer(ctx, ctx.probeOut, u64{kMaxProbes} * kClProbeOutputs * 16u, storage, MemoryUsage::GpuToCpu,
                        "rp_clouds.probe_out") ||
            !makeBuffer(ctx, ctx.background, u64{kMaxOutPixels} * 16u, storage, MemoryUsage::CpuToGpu,
                        "rp_clouds.background") ||
            !makeBuffer(ctx, ctx.depth, u64{kMaxOutPixels} * 4u, storage, MemoryUsage::CpuToGpu, "rp_clouds.depth") ||
            ctx.probeIn.deviceAddress == 0u || ctx.probeOut.deviceAddress == 0u ||
            ctx.background.deviceAddress == 0u || ctx.depth.deviceAddress == 0u) {
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
                        "rp_clouds.readback")) {
            return false;
        }
        ctx.readbackBytes = readbackBytes;
        ctx.readbackQueue = rg::kNoQueue;
    }
    return true;
}

struct Lang {
    CloudKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[2] = {{CloudKernelLanguage::Slang, "slang"}, {CloudKernelLanguage::Glsl, "glsl"}};

// --- harness -------------------------------------------------------------------------------------------------
enum ReadBit : u32 {
    kReadShape = 1u << 0,
    kReadDetail = 1u << 1,
    kReadWeather = 1u << 2,
    kReadFresh = 1u << 3,
    kReadHistory = 1u << 4, ///< this frame's reconstruction (History0 or History1)
    kReadResult = 1u << 5,
};

struct Instance {
    VolumetricClouds clouds;
    CloudSettings settings{};
    u32 read = 0;
    bool background = false; ///< frame.backgroundAddress / depthAddress set to the context's buffers
};

struct Readback {
    std::vector<at::AtTexel> lut[kLutCount];
    std::vector<std::array<std::vector<ClTexel>, 6>> sections; ///< per instance, per ReadBit index
    std::vector<ClTexel> probes;
    at::AtLutView luts() const {
        at::AtLutView v{};
        v.transmittance = lut[0].data();
        v.multiscatter = lut[1].data();
        v.skyView = lut[2].data();
        v.aerialScatter = lut[3].data();
        v.aerialTransmittance = lut[4].data();
        return v;
    }
    ClNoiseView noise(usize i) const {
        return ClNoiseView{sections[i][0].data(), sections[i][1].data(), sections[i][2].data()};
    }
};

struct Probe {
    Vec3 origin;
    f32 jitter = 0.5f;
    Vec3 dir;
};

struct Harness {
    Context* ctx = nullptr;
    at::AtmosphereGpu atmos;
    bool useAtmos = false;
    at::AtmosphereLutSettings atmosSettings{};
    std::vector<std::unique_ptr<Instance>> instances;
    rg::Graph graph;
    // Per-frame layout of the read-back buffer.
    u64 lutAt[kLutCount] = {};
    u64 sectionAt[8][6] = {};
    u64 readbackTotal = 0;
    bool readAtmos = false;

    bool initAtmos() {
        at::AtmosphereGpuDesc d{};
        d.device = ctx->device.get();
        d.allocator = ctx->allocator.get();
        d.bindless = &ctx->bindless;
        return atmos.init(d);
    }
    Instance* add(CloudKernelLanguage language, const CloudSettings& settings, u32 read) {
        auto inst = std::make_unique<Instance>();
        CloudGpuDesc d{};
        d.device = ctx->device.get();
        d.allocator = ctx->allocator.get();
        d.bindless = &ctx->bindless;
        d.language = language;
        if (!inst->clouds.init(d)) {
            return nullptr;
        }
        inst->settings = settings;
        inst->read = read;
        instances.push_back(std::move(inst));
        return instances.back().get();
    }
    ~Harness() {
        if (ctx != nullptr && ctx->vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(ctx->vkDevice);
        }
        for (auto& i : instances) {
            i->clouds.destroy();
        }
        atmos.destroy();
    }
};

CloudSection sectionOf(const Instance& inst, u32 bit) {
    switch (bit) {
    case 0: return CloudSection::Shape;
    case 1: return CloudSection::Detail;
    case 2: return CloudSection::Weather;
    case 3: return CloudSection::Fresh;
    case 4: return inst.clouds.historySection();
    default: return CloudSection::Result;
    }
}

/// beginFrame of the atmosphere and of every instance (the same camera / sun / wind).
bool beginAll(Harness& h, const CloudFrame& frameIn, const at::AtmosphereLutView& av) {
    Context& ctx = *h.ctx;
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    CloudFrame frame = frameIn;
    if (h.useAtmos) {
        if (!h.atmos.beginFrame(ctx.serial, h.atmosSettings, av)) {
            return false;
        }
        frame.atmosphere = &h.atmos.params();
        frame.atmosphereAddress = h.atmos.frameAddress();
    }
    bool ok = true;
    for (auto& inst : h.instances) {
        CloudFrame f = frame;
        if (inst->background) {
            f.backgroundAddress = ctx.background.deviceAddress;
            f.depthAddress = ctx.depth.deviceAddress;
        }
        ok = inst->clouds.beginFrame(ctx.serial, inst->settings, f) && ok;
    }
    return ok;
}

/// The graph: atmosphere, clouds, probe (instance 0), read-back copies.
void buildGraph(Harness& h, u32 probeCount) {
    Context& ctx = *h.ctx;
    rg::Graph& graph = h.graph;
    graph.reset();
    at::AtmosphereGraphRefs aref{};
    if (h.useAtmos) {
        aref = h.atmos.importInto(graph);
        h.atmos.addPasses(graph, aref);
    }
    const u64 anyRead = h.readAtmos ? 1u : 0u;
    u32 mask = 0;
    for (auto& inst : h.instances) {
        mask |= inst->read;
    }
    rg::BufferRef rb{};
    if (anyRead != 0u || mask != 0u) {
        rb = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size, ctx.readbackQueue,
                                                   &ctx.readbackQueue, "rp_clouds.readback"});
    }
    rg::BufferRef bg{};
    rg::BufferRef dp{};
    for (auto& inst : h.instances) {
        if (inst->background && !bg.valid()) {
            bg = graph.importBuffer(rg::ImportedBuffer{ctx.background.handle, ctx.background.desc.size,
                                                       ctx.backgroundQueue, &ctx.backgroundQueue, "rp_clouds.bg"});
            dp = graph.importBuffer(rg::ImportedBuffer{ctx.depth.handle, ctx.depth.desc.size, ctx.depthQueue,
                                                       &ctx.depthQueue, "rp_clouds.depth"});
        }
    }
    for (usize i = 0; i < h.instances.size(); ++i) {
        Instance& inst = *h.instances[i];
        const CloudGraphRefs refs = inst.clouds.importInto(graph);
        CloudInputs in{};
        in.atmosphereLuts = aref.luts;
        if (inst.background) {
            in.background = bg;
            in.depth = dp;
        }
        inst.clouds.addPasses(graph, refs, in);
        if (i == 0u && probeCount > 0u) {
            const rg::BufferRef pin = graph.importBuffer(rg::ImportedBuffer{ctx.probeIn.handle, ctx.probeIn.desc.size,
                                                                            ctx.probeInQueue, &ctx.probeInQueue, "rp_clouds.in"});
            const rg::BufferRef pout = graph.importBuffer(rg::ImportedBuffer{
                ctx.probeOut.handle, ctx.probeOut.desc.size, ctx.probeOutQueue, &ctx.probeOutQueue, "rp_clouds.out"});
            inst.clouds.addProbe(graph, refs, in, pin, ctx.probeIn.deviceAddress, pout, ctx.probeOut.deviceAddress,
                                 probeCount);
        }
        for (u32 b = 0; b < 6u; ++b) {
            if ((inst.read & (1u << b)) != 0u) {
                inst.clouds.addCopy(graph, refs, sectionOf(inst, b), rb, h.sectionAt[i][b]);
            }
        }
    }
    if (h.readAtmos && h.useAtmos) {
        for (u32 l = 0; l < kLutCount; ++l) {
            h.atmos.addCopy(graph, aref, static_cast<at::AtmosLut>(l), rb, h.lutAt[l]);
        }
    }
}

void layoutReadback(Harness& h) {
    u64 cursor = 0;
    if (h.readAtmos && h.useAtmos) {
        for (u32 l = 0; l < kLutCount; ++l) {
            h.lutAt[l] = cursor;
            cursor = align256(cursor + std::max<u64>(h.atmos.lutBytes(static_cast<at::AtmosLut>(l)), 16u));
        }
    }
    for (usize i = 0; i < h.instances.size(); ++i) {
        const Instance& inst = *h.instances[i];
        for (u32 b = 0; b < 6u; ++b) {
            h.sectionAt[i][b] = cursor;
            if ((inst.read & (1u << b)) != 0u) {
                cursor = align256(cursor + std::max<u64>(inst.clouds.sectionBytes(sectionOf(inst, b)), 16u));
            }
        }
    }
    h.readbackTotal = std::max<u64>(cursor, 256u);
}

void writeProbes(Context& ctx, const std::vector<Probe>& probes) {
    f32* in = static_cast<f32*>(ctx.probeIn.mapped);
    for (usize i = 0; i < probes.size(); ++i) {
        const Probe& q = probes[i];
        const f32 v[8] = {q.origin.x, q.origin.y, q.origin.z, q.jitter, q.dir.x, q.dir.y, q.dir.z, 0.f};
        std::memcpy(in + i * 8u, v, sizeof(v));
    }
}

bool runFrame(Harness& h, const CloudFrame& frame, const at::AtmosphereLutView& av, Readback* out,
              const std::vector<Probe>* probes = nullptr) {
    Context& ctx = *h.ctx;
    if (!beginAll(h, frame, av)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    layoutReadback(h);
    if (!ensureTargets(ctx, h.readbackTotal)) {
        std::fprintf(stderr, "  targets failed\n");
        return false;
    }
    const u32 probeCount = probes != nullptr ? static_cast<u32>(probes->size()) : 0u;
    if (probeCount > 0u) {
        writeProbes(ctx, *probes);
    }
    buildGraph(h, probeCount);
    const rg::ExecuteResult result = ctx.executor->execute(h.graph);
    const bool waited = ctx.executor->waitIdle();
    if (h.useAtmos) {
        h.atmos.collectRetired(ctx.serial);
    }
    for (auto& inst : h.instances) {
        inst->clouds.collectRetired(ctx.serial);
    }
    ctx.bindless.collectRetired(ctx.serial);
    if (!result.ok || !waited) {
        std::fprintf(stderr, "  execute failed\n");
        return false;
    }
    if (out == nullptr) {
        return true;
    }
    const u8* base = static_cast<const u8*>(ctx.readback.mapped);
    if (h.readAtmos && h.useAtmos) {
        for (u32 l = 0; l < kLutCount; ++l) {
            const u64 bytes = h.atmos.lutBytes(static_cast<at::AtmosLut>(l));
            out->lut[l].resize(static_cast<usize>(bytes / 16u));
            std::memcpy(out->lut[l].data(), base + h.lutAt[l], static_cast<usize>(bytes));
        }
    }
    out->sections.resize(h.instances.size());
    for (usize i = 0; i < h.instances.size(); ++i) {
        const Instance& inst = *h.instances[i];
        for (u32 b = 0; b < 6u; ++b) {
            if ((inst.read & (1u << b)) == 0u) {
                continue;
            }
            const u64 bytes = inst.clouds.sectionBytes(sectionOf(inst, b));
            out->sections[i][b].resize(static_cast<usize>(bytes / 16u));
            std::memcpy(out->sections[i][b].data(), base + h.sectionAt[i][b], static_cast<usize>(bytes));
        }
    }
    if (probeCount > 0u) {
        out->probes.resize(static_cast<usize>(probeCount) * kClProbeOutputs);
        std::memcpy(out->probes.data(), ctx.probeOut.mapped, out->probes.size() * sizeof(ClTexel));
    }
    return true;
}

// --- scene ---------------------------------------------------------------------------------------------------
CloudSettings baseSettings() {
    CloudSettings s{};
    s.noise.shapeSize = 32;
    s.noise.detailSize = 16;
    s.noise.weatherSize = 32;
    s.noise.shapeFrequency = 4;
    s.noise.detailFrequency = 4;
    s.noise.weatherFrequency = 4;
    s.noise.seed = 3;
    s.resolution = CloudResolution{32, 16, 1, 32, 16};
    s.sampling.primarySteps = 32;
    s.sampling.lightSteps = 4;
    s.sampling.transmittanceCutoff = 1e-4f;
    s.medium.coverageBias = 0.1f;
    s.temporal.enabled = false;
    return s;
}

CloudCamera camera(f64 yawDeg, f64 pitchDeg, const Vec3& position) {
    CloudCamera c{};
    const f64 yaw = yawDeg * kDeg;
    const f64 pitch = pitchDeg * kDeg;
    c.position = position;
    c.forward = dirElAz(pitch, yaw);
    c.right = Vec3{static_cast<f32>(std::cos(yaw)), 0.f, static_cast<f32>(-std::sin(yaw))};
    c.up = dirElAz(pitch + kPiD / 2.0, yaw);
    c.tanHalfFovX = 1.f;
    c.tanHalfFovY = 0.5f;
    return c;
}

CloudFrame sceneFrame(const CloudCamera& cam) {
    CloudFrame f{};
    f.camera = cam;
    f.sunDirection = dirElAz(35.0 * kDeg, 40.0 * kDeg);
    f.sunIlluminance = Vec3{1.f, 0.95f, 0.9f};
    f.ambient = Vec3{0.05f, 0.06f, 0.08f};
    return f;
}

at::AtmosphereLutView atmosView(const CloudCamera& cam) {
    at::AtmosphereLutView v{};
    v.cameraPosition = cam.position;
    v.sunDirection = dirElAz(35.0 * kDeg, 40.0 * kDeg);
    v.sunIlluminance = Vec3{1.f, 0.95f, 0.9f};
    v.forward = cam.forward;
    v.right = cam.right;
    v.up = cam.up;
    v.tanHalfFovX = cam.tanHalfFovX;
    v.tanHalfFovY = cam.tanHalfFovY;
    v.aerialMaxDistance = 40000.f;
    return v;
}

at::AtmosphereLutSettings smallAtmosphere() {
    at::AtmosphereLutSettings s{};
    s.sizes = at::AtmosphereLutSizes{64, 16, 16, 16, 48, 27, 16, 16, 16};
    return s;
}

// --- comparisons ----------------------------------------------------------------------------------------------
struct Cmp {
    f64 worstRgb = 0.0; ///< |d| / peak radiance
    f64 worstA = 0.0;   ///< |d| of alpha (transmittance)
    f64 peak = 0.0;
    bool finite = true;
};

Cmp compare(const ClTexel* gpu, const ClTexel* cpu, usize count, usize stride = 1, bool alpha = true) {
    Cmp c{};
    for (usize i = 0; i < count; ++i) {
        const ClTexel& t = cpu[i * stride];
        c.peak = std::max({c.peak, static_cast<f64>(std::fabs(t.r)), static_cast<f64>(std::fabs(t.g)),
                           static_cast<f64>(std::fabs(t.b))});
    }
    const f64 peak = std::max(c.peak, 1e-12);
    for (usize i = 0; i < count; ++i) {
        const ClTexel& g = gpu[i * stride];
        const ClTexel& r = cpu[i * stride];
        c.finite = c.finite && std::isfinite(g.r) && std::isfinite(g.g) && std::isfinite(g.b) && std::isfinite(g.a);
        c.worstRgb = std::max({c.worstRgb, std::fabs(static_cast<f64>(g.r) - r.r) / peak,
                               std::fabs(static_cast<f64>(g.g) - r.g) / peak, std::fabs(static_cast<f64>(g.b) - r.b) / peak});
        if (alpha) {
            c.worstA = std::max(c.worstA, std::fabs(static_cast<f64>(g.a) - r.a));
        }
    }
    return c;
}

f64 maxAbsDiff(const std::vector<ClTexel>& a, const std::vector<ClTexel>& b) {
    if (a.size() != b.size()) {
        return 1e30;
    }
    f64 d = 0.0;
    for (usize i = 0; i < a.size(); ++i) {
        d = std::max({d, std::fabs(static_cast<f64>(a[i].r) - b[i].r), std::fabs(static_cast<f64>(a[i].g) - b[i].g),
                      std::fabs(static_cast<f64>(a[i].b) - b[i].b), std::fabs(static_cast<f64>(a[i].a) - b[i].a)});
        if (!std::isfinite(a[i].r) || !std::isfinite(a[i].g) || !std::isfinite(a[i].b) || !std::isfinite(a[i].a)) {
            return 1e30;
        }
    }
    return d;
}

/// Relative L1 of the radiance (rgb) and of the opacity (1 - T) of image `a` against `ref` (texel pairs).
struct ImageErr {
    f64 radiance = 0.0;
    f64 opacity = 0.0;
    f64 trail = 0.0; ///< excess opacity (a more opaque than ref) / ref opacity
};

ImageErr imageErr(const std::vector<ClTexel>& a, const std::vector<ClTexel>& ref, usize stride) {
    f64 dl = 0.0, nl = 0.0, dop = 0.0, nop = 0.0, excess = 0.0;
    const usize n = ref.size() / stride;
    for (usize i = 0; i < n; ++i) {
        const ClTexel& x = a[i * stride];
        const ClTexel& r = ref[i * stride];
        dl += std::fabs(static_cast<f64>(x.r) - r.r) + std::fabs(static_cast<f64>(x.g) - r.g) +
              std::fabs(static_cast<f64>(x.b) - r.b);
        nl += std::fabs(static_cast<f64>(r.r)) + std::fabs(static_cast<f64>(r.g)) + std::fabs(static_cast<f64>(r.b));
        dop += std::fabs(static_cast<f64>(x.a) - r.a);
        nop += 1.0 - static_cast<f64>(r.a);
        excess += std::max(0.0, static_cast<f64>(r.a) - x.a);
    }
    ImageErr e{};
    e.radiance = dl / std::max(nl, 1e-12);
    e.opacity = dop / std::max(nop, 1e-12);
    e.trail = excess / std::max(nop, 1e-12);
    return e;
}

// --- noise -------------------------------------------------------------------------------------------------------
int runNoise(Context& ctx) {
    std::vector<ClTexel> byLang[2][3];
    u32 ran = 0;
    for (u32 li = 0; li < 2u; ++li) {
        Harness h;
        h.ctx = &ctx;
        CloudSettings s = baseSettings();
        if (h.add(kLangs[li].language, s, kReadShape | kReadDetail | kReadWeather) == nullptr) {
            std::printf("  %s kernels not built: skipped\n", kLangs[li].name);
            continue;
        }
        ++ran;
        const CloudCamera cam = camera(0.0, 20.0, Vec3{0.f, 50.f, 0.f});
        Readback r1, r2;
        expect(runFrame(h, sceneFrame(cam), atmosView(cam), &r1), "noise frame 1");
        expect(runFrame(h, sceneFrame(cam), atmosView(cam), &r2), "noise frame 2");
        const CloudParams& p = h.instances[0]->clouds.params();
        std::vector<ClTexel> cpu[3];
        build_shape_noise(p, cpu[0]);
        build_detail_noise(p, cpu[1]);
        build_weather(p, cpu[2]);
        const char* names[3] = {"shape", "detail", "weather"};
        for (u32 k = 0; k < 3u; ++k) {
            const f64 d = maxAbsDiff(r1.sections[0][k], cpu[k]);
            std::printf("noise (%s): %-7s %zu texels, max |gpu - cpu| %.3g\n", kLangs[li].name, names[k], cpu[k].size(), d);
            expectLe(d, 2e-5, "GPU noise == CPU reference");
            expect(r2.sections[0][k].size() == r1.sections[0][k].size() &&
                       std::memcmp(r2.sections[0][k].data(), r1.sections[0][k].data(),
                                   r1.sections[0][k].size() * sizeof(ClTexel)) == 0,
                   "second frame leaves the noise unchanged");
            byLang[li][k] = r1.sections[0][k];
        }
        expect(h.instances[0]->clouds.stats().noiseBakes == 1u, "noise baked once");
    }
    if (ran == 0u) {
        std::printf("SKIP: no cloud kernel built\n");
        return kSkip;
    }
    if (ran == 2u) {
        f64 d = 0.0;
        for (u32 k = 0; k < 3u; ++k) {
            d = std::max(d, maxAbsDiff(byLang[0][k], byLang[1][k]));
        }
        std::printf("noise: Slang vs GLSL max |diff| %.3g\n", d);
        expectLe(d, 2e-5, "Slang == GLSL noise");
    }
    return 0;
}

// --- ray ---------------------------------------------------------------------------------------------------------
std::vector<Probe> rayProbes() {
    std::vector<Probe> p;
    u32 k = 0;
    for (f64 el : {80.0, 40.0, 20.0, 8.0}) {
        for (f64 az : {10.0, 130.0, 250.0}) {
            p.push_back(Probe{Vec3{50.f * k, 30.f, -70.f * k}, 0.25f + 0.05f * static_cast<f32>(k % 7u), dirElAz(el * kDeg, az * kDeg)});
            ++k;
        }
    }
    // Inside the layer (every direction) and above it (looking down).
    for (f64 el : {60.0, 0.0, -40.0}) {
        p.push_back(Probe{Vec3{1200.f, 2600.f, 900.f}, 0.5f, dirElAz(el * kDeg, 70.0 * kDeg)});
    }
    for (f64 el : {-80.0, -30.0}) {
        p.push_back(Probe{Vec3{-800.f, 5500.f, 300.f}, 0.5f, dirElAz(el * kDeg, 200.0 * kDeg)});
    }
    // Origins spread through the layer (the density field at the origin; rays straight up).
    for (u32 i = 0; i < 24u; ++i) {
        const f32 fi = static_cast<f32>(i);
        p.push_back(Probe{Vec3{-9000.f + 780.f * fi, 1650.f + 95.f * fi, 4000.f - 410.f * fi}, 0.5f, Vec3{0.f, 1.f, 0.f}});
    }
    return p;
}

int runRay(Context& ctx) {
    std::vector<ClTexel> freshByLang[2];
    u32 ran = 0;
    for (u32 li = 0; li < 2u; ++li) {
        Harness h;
        h.ctx = &ctx;
        h.useAtmos = true;
        h.readAtmos = true;
        h.atmosSettings = smallAtmosphere();
        if (!h.initAtmos()) {
            std::printf("SKIP: atmosphere unavailable\n");
            return kSkip;
        }
        CloudSettings s = baseSettings();
        if (h.add(kLangs[li].language, s, kReadShape | kReadDetail | kReadWeather | kReadFresh | kReadHistory | kReadResult) ==
            nullptr) {
            std::printf("  %s kernels not built: skipped\n", kLangs[li].name);
            continue;
        }
        ++ran;
        const CloudCamera cam = camera(30.0, 18.0, Vec3{0.f, 60.f, 0.f});
        const std::vector<Probe> probes = rayProbes();
        Readback r;
        expect(runFrame(h, sceneFrame(cam), atmosView(cam), &r, &probes), "ray frame");
        const CloudParams& p = h.instances[0]->clouds.params();
        const at::AtLutView lv = r.luts();
        const ClAtmosphereView atm{&h.atmos.params(), &lv};
        const ClNoiseView nv = r.noise(0);
        // Every pixel.
        std::vector<ClTexel> cpu(static_cast<usize>(p.freshWidth) * p.freshHeight * 2u);
        for (u32 fy = 0; fy < p.freshHeight; ++fy) {
            for (u32 fx = 0; fx < p.freshWidth; ++fx) {
                march_texel(p, nv, atm, fx, fy, &cpu[(fy * p.freshWidth + fx) * 2u]);
            }
        }
        const std::vector<ClTexel>& fresh = r.sections[0][3];
        const Cmp c = compare(fresh.data(), cpu.data(), cpu.size() / 2u, 2u);
        f64 depthErr = 0.0;
        u32 cloudy = 0;
        f64 meanT = 0.0;
        for (usize i = 0; i < cpu.size() / 2u; ++i) {
            meanT += cpu[i * 2u].a;
            if (cpu[i * 2u + 1u].g > 0.5f && fresh[i * 2u + 1u].g > 0.5f && cpu[i * 2u].a < 0.99f) {
                ++cloudy;
                depthErr = std::max(depthErr, std::fabs(static_cast<f64>(fresh[i * 2u + 1u].r) - cpu[i * 2u + 1u].r) /
                                                  cpu[i * 2u + 1u].r);
            }
        }
        meanT /= static_cast<f64>(cpu.size() / 2u);
        std::printf("ray (%s): %u x %u pixels, %u cloudy, mean T %.3f; |gpu - cpu| radiance %.3g x peak (%.3g), T %.3g, "
                    "depth %.3g rel\n",
                    kLangs[li].name, p.width, p.height, cloudy, meanT, c.worstRgb, c.peak, c.worstA, depthErr);
        expect(c.finite, "march output finite");
        expect(cloudy > p.width * p.height / 10u && meanT < 0.95, "the scene has clouds");
        expectLe(c.worstRgb, 5e-3, "GPU march == CPU single-ray integration (radiance)");
        expectLe(c.worstA, 5e-3, "GPU march == CPU single-ray integration (transmittance)");
        expectLe(depthErr, 1e-2, "GPU cloud depth == CPU");
        // Reconstruction without history (block 1): the fresh samples, exactly.
        const std::vector<ClTexel>& hist = r.sections[0][4];
        bool same = hist.size() == fresh.size();
        for (usize i = 0; same && i < fresh.size() / 2u; ++i) {
            same = std::memcmp(&hist[i * 2u], &fresh[i * 2u], sizeof(ClTexel)) == 0;
        }
        expect(same, "reconstruction without history == fresh samples");
        // Composite (sky background, aerial perspective) vs the CPU on the device's history and LUTs.
        std::vector<ClTexel> comp(static_cast<usize>(p.outWidth) * p.outHeight);
        for (u32 y = 0; y < p.outHeight; ++y) {
            for (u32 x = 0; x < p.outWidth; ++x) {
                comp[y * p.outWidth + x] = composite_texel(p, hist.data(), atm, nullptr, nullptr, x, y);
            }
        }
        const Cmp cc = compare(r.sections[0][5].data(), comp.data(), comp.size());
        std::printf("ray (%s): composite |gpu - cpu| %.3g x peak (%.3g)\n", kLangs[li].name, cc.worstRgb, cc.peak);
        expectLe(cc.worstRgb, 2e-3, "GPU composite == CPU composite");
        // Probes.
        f64 probeErr = 0.0, densErr = 0.0, probePeak = 0.0;
        for (usize i = 0; i < probes.size(); ++i) {
            const ClRay ray = cl_integrate(p, nv, atm, probes[i].origin, probes[i].dir, probes[i].jitter);
            probePeak = std::max({probePeak, static_cast<f64>(ray.scatter.x), static_cast<f64>(ray.scatter.y),
                                  static_cast<f64>(ray.scatter.z)});
        }
        u32 probeCloudy = 0;
        u32 dense = 0;
        for (usize i = 0; i < probes.size(); ++i) {
            const ClRay ray = cl_integrate(p, nv, atm, probes[i].origin, probes[i].dir, probes[i].jitter);
            const ClTexel& g0 = r.probes[i * 2u];
            const ClTexel& g1 = r.probes[i * 2u + 1u];
            probeErr = std::max({probeErr, std::fabs(g0.r - ray.scatter.x) / probePeak,
                                 std::fabs(g0.b - ray.scatter.z) / probePeak,
                                 static_cast<f64>(std::fabs(g0.a - ray.transmittance))});
            const f32 dens = cl_density_at(p, nv, probes[i].origin);
            densErr = std::max(densErr, std::fabs(static_cast<f64>(g1.b) - dens) / p.densityScale);
            dense += dens > 0.f ? 1u : 0u;
            probeCloudy += ray.hasCloud > 0.5f ? 1u : 0u;
        }
        std::printf("ray (%s): %zu probes (%u through cloud, %u origins in cloud): |gpu - cpu| %.3g; density %.3g x "
                    "densityScale\n",
                    kLangs[li].name, probes.size(), probeCloudy, dense, probeErr, densErr);
        expect(dense >= 4u, "density probes meet cloud");
        expectLe(probeErr, 5e-3, "probe rays == CPU single-ray integration");
        expectLe(densErr, 1e-4, "probe density == CPU density field");
        expect(probeCloudy >= probes.size() / 3u, "probes cross clouds");
        freshByLang[li] = fresh;
    }
    if (ran == 0u) {
        std::printf("SKIP: no cloud kernel built\n");
        return kSkip;
    }
    if (ran == 2u) {
        const Cmp c = compare(freshByLang[0].data(), freshByLang[1].data(), freshByLang[0].size() / 2u, 2u);
        std::printf("ray: Slang vs GLSL radiance %.3g x peak, T %.3g\n", c.worstRgb, c.worstA);
        expectLe(c.worstRgb, 5e-3, "Slang == GLSL march (radiance)");
        expectLe(c.worstA, 5e-3, "Slang == GLSL march (transmittance)");
    }
    return 0;
}

// --- analytic ------------------------------------------------------------------------------------------------------
f64 sphereFar(f64 ox, f64 oy, f64 oz, f64 dx, f64 dy, f64 dz, f64 R) {
    const f64 b = ox * dx + oy * dy + oz * dz;
    const f64 c = ox * ox + oy * oy + oz * oz - R * R;
    const f64 disc = b * b - c;
    return disc < 0.0 ? -1.0 : -b + std::sqrt(disc);
}

/// Double-precision single scattering through the homogeneous shell (camera below it), constant sun E = 1.
void bruteHomogeneous(const CloudParams& p, const Vec3& origin, const Vec3& dir, f64& L, f64& T) {
    const f64 R = p.planetRadius;
    const f64 ox = origin.x, oy = static_cast<f64>(origin.y) + R, oz = origin.z;
    const f64 dl = std::sqrt(static_cast<f64>(dir.x) * dir.x + static_cast<f64>(dir.y) * dir.y + static_cast<f64>(dir.z) * dir.z);
    const f64 dx = dir.x / dl, dy = dir.y / dl, dz = dir.z / dl;
    const f64 t0 = sphereFar(ox, oy, oz, dx, dy, dz, R + p.cloudBottom);
    const f64 t1 = std::min(sphereFar(ox, oy, oz, dx, dy, dz, R + p.cloudTop), static_cast<f64>(p.maxDistance));
    const f64 sigma = p.densityScale;
    const f64 lx = p.sunDir[0], ly = p.sunDir[1], lz = p.sunDir[2];
    const f64 cosT = dx * lx + dy * ly + dz * lz;
    const f64 g = p.phaseForward;
    const f64 phase = (1.0 - g * g) / (4.0 * kPiD * std::pow(1.0 + g * g - 2.0 * g * cosT, 1.5));
    const u32 n = 100000;
    const f64 dt = (t1 - t0) / n;
    L = 0.0;
    for (u32 i = 0; i < n; ++i) {
        const f64 t = t0 + (i + 0.5) * dt;
        const f64 light = std::min(sphereFar(ox + dx * t, oy + dy * t, oz + dz * t, lx, ly, lz, R + p.cloudTop),
                                   static_cast<f64>(p.lightDistance));
        L += sigma * p.albedo * phase * std::exp(-sigma * (t - t0)) * std::exp(-sigma * light) * dt;
    }
    T = std::exp(-sigma * (t1 - t0));
}

int runAnalytic(Context& ctx) {
    u32 ran = 0;
    for (u32 li = 0; li < 2u; ++li) {
        Harness h;
        h.ctx = &ctx;
        CloudSettings s = baseSettings();
        s.medium.homogeneous = true;
        s.medium.densityScale = 0.0008f;
        s.medium.albedo = 1.f;
        s.lighting.octaves = 1;
        s.lighting.powderStrength = 0.f;
        s.lighting.phaseBlend = 0.f;
        s.sampling.primarySteps = 64;
        s.sampling.lightSteps = 8;
        s.sampling.lightDistance = 1e6f;
        s.sampling.transmittanceCutoff = 0.f;
        s.temporal.jitter = false;
        if (h.add(kLangs[li].language, s, 0u) == nullptr) {
            std::printf("  %s kernels not built: skipped\n", kLangs[li].name);
            continue;
        }
        ++ran;
        for (f64 sunEl : {90.0, 60.0, 25.0}) {
            std::vector<Probe> probes;
            probes.push_back(Probe{Vec3{0.f, 0.f, 0.f}, 0.5f, Vec3{0.f, 1.f, 0.f}});
            for (f64 el : {75.0, 45.0, 25.0, 12.0}) {
                probes.push_back(Probe{Vec3{30.f, 10.f, -20.f}, 0.5f, dirElAz(el * kDeg, 10.0 * kDeg)});
            }
            const CloudCamera cam = camera(0.0, 20.0, Vec3{0.f, 50.f, 0.f});
            CloudFrame f = sceneFrame(cam);
            f.sunDirection = sunEl == 90.0 ? Vec3{0.f, 1.f, 0.f} : dirElAz(sunEl * kDeg, 120.0 * kDeg);
            f.sunIlluminance = Vec3{1.f, 1.f, 1.f};
            f.ambient = Vec3{0.f, 0.f, 0.f};
            Readback r;
            expect(runFrame(h, f, atmosView(cam), &r, &probes), "analytic frame");
            const CloudParams& p = h.instances[0]->clouds.params();
            f64 worstL = 0.0, worstT = 0.0;
            for (usize i = 0; i < probes.size(); ++i) {
                f64 L = 0.0, T = 1.0;
                if (i == 0u && sunEl == 90.0) {
                    const f64 D = p.cloudTop - p.cloudBottom;
                    const f64 g = p.phaseForward;
                    const f64 phase = (1.0 - g * g) / (4.0 * kPiD * std::pow(1.0 - g, 3.0));
                    T = std::exp(-p.densityScale * D);
                    L = phase * p.densityScale * D * T; // the closed form
                } else {
                    bruteHomogeneous(p, probes[i].origin, probes[i].dir, L, T);
                }
                const ClTexel& g0 = r.probes[i * 2u];
                worstL = std::max(worstL, std::fabs(g0.r - L) / L);
                worstT = std::max(worstT, std::fabs(g0.a - T) / T);
            }
            std::printf("analytic (%s): sun %.0f deg: single scattering rel err %.3g, transmittance rel err %.3g%s\n",
                        kLangs[li].name, sunEl, worstL, worstT, sunEl == 90.0 ? " (vertical ray: closed form)" : "");
            expectLe(worstL, 0.01, "GPU single scattering == closed form / brute force");
            expectLe(worstT, 2e-4, "GPU slab transmittance == exp(-sigma d)");
        }
    }
    if (ran == 0u) {
        std::printf("SKIP: no cloud kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- converge ------------------------------------------------------------------------------------------------------
int runConverge(Context& ctx) {
    Harness h;
    h.ctx = &ctx;
    h.useAtmos = true;
    h.atmosSettings = smallAtmosphere();
    if (!h.initAtmos()) {
        std::printf("SKIP: atmosphere unavailable\n");
        return kSkip;
    }
    CloudSettings amortised = baseSettings();
    amortised.resolution.block = 4;
    amortised.temporal.enabled = true;
    amortised.temporal.jitter = true;
    amortised.sampling.primarySteps = 12;
    CloudSettings reference = baseSettings();
    reference.sampling.primarySteps = 128;
    reference.temporal.jitter = false;
    CloudSettings single = baseSettings();
    single.sampling.primarySteps = 12;
    single.temporal.jitter = true;
    CloudSettings exact = amortised;
    exact.temporal.jitter = false;
    CloudSettings exactRef = baseSettings();
    exactRef.sampling.primarySteps = 12;
    exactRef.temporal.jitter = false;
    if (h.add(CloudKernelLanguage::Auto, amortised, kReadHistory) == nullptr ||
        h.add(CloudKernelLanguage::Auto, reference, kReadHistory) == nullptr ||
        h.add(CloudKernelLanguage::Auto, single, kReadHistory) == nullptr ||
        h.add(CloudKernelLanguage::Auto, exact, kReadHistory) == nullptr ||
        h.add(CloudKernelLanguage::Auto, exactRef, kReadHistory) == nullptr) {
        std::printf("SKIP: no cloud kernel built\n");
        return kSkip;
    }
    const CloudCamera cam = camera(30.0, 18.0, Vec3{0.f, 60.f, 0.f});
    const CloudFrame f = sceneFrame(cam);
    constexpr u32 kFrames = 160;
    std::vector<ClTexel> ref;
    std::vector<ClTexel> prevPeriod;
    std::vector<f64> periodDistance;
    f64 singleErr = 0.0;
    u32 singleCount = 0;
    ImageErr finalErr{};
    f32 minCount = 1e9f;
    bool exactOk = false;
    Readback r;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        if (!runFrame(h, f, atmosView(cam), &r)) {
            expect(false, "converge frame");
            break;
        }
        if (frame == 0u) {
            ref = r.sections[1][4];
        }
        if (frame < 8u) {
            singleErr += imageErr(r.sections[2][4], ref, 2u).radiance;
            ++singleCount;
        }
        if (frame == 15u) {
            // No jitter, static: after 16 frames of 1/16 every pixel holds its full-resolution march.
            const std::vector<ClTexel>& a = r.sections[3][4];
            const std::vector<ClTexel>& b = r.sections[4][4];
            exactOk = a.size() == b.size();
            for (usize i = 0; exactOk && i < a.size() / 2u; ++i) {
                exactOk = std::memcmp(&a[i * 2u], &b[i * 2u], sizeof(ClTexel)) == 0;
            }
        }
        if ((frame + 1u) % 16u == 0u) {
            const std::vector<ClTexel>& hist = r.sections[0][4];
            if (!prevPeriod.empty()) {
                periodDistance.push_back(imageErr(hist, prevPeriod, 2u).radiance);
            }
            prevPeriod = hist;
        }
    }
    const std::vector<ClTexel>& final = r.sections[0][4];
    finalErr = imageErr(final, ref, 2u);
    for (usize i = 0; i < final.size() / 2u; ++i) {
        minCount = std::min(minCount, final[i * 2u + 1u].g);
    }
    singleErr /= std::max(1u, singleCount);
    std::printf("converge: static camera, 1/16 per frame, jitter, %u primary steps, %u frames (%s kernels)\n",
                amortised.sampling.primarySteps, kFrames, h.instances[0]->clouds.kernelLanguage());
    std::printf("  vs 128-step full march: amortised rel L1 %.4f (opacity %.4f), single jittered frame %.4f\n",
                finalErr.radiance, finalErr.opacity, singleErr);
    std::printf("  period-to-period rel L1:");
    for (f64 d : periodDistance) {
        std::printf(" %.4f", d);
    }
    std::printf("\n  min history count %.0f; no jitter: amortised == full march after 16 frames: %s\n", minCount,
                exactOk ? "yes (bit-exact)" : "NO");
    expect(exactOk, "static, no jitter: amortised image == full-resolution march after 16 frames (bit-exact)");
    expect(finalErr.radiance < 0.6 * singleErr, "converged amortised image beats a single jittered frame");
    expect(!periodDistance.empty() && periodDistance.back() <= 0.5 * periodDistance.front(),
           "period-to-period change decays");
    // Steady state: a capped running mean (maxHistoryCount) of jittered marches keeps a floor of about the
    // per-sample jitter noise / maxHistoryCount per 16-frame period.
    expectLe(periodDistance.empty() ? 1.0 : periodDistance.back(), 0.03, "static image stable (period-to-period)");
    expect(minCount >= amortised.temporal.maxHistoryCount, "every pixel reached the full history length");
    return 0;
}

// --- ghosting ------------------------------------------------------------------------------------------------------
struct GhostScenario {
    const char* name;
    f64 pitch;      ///< degrees
    f64 yawRate;    ///< degrees per frame
    f32 move;       ///< x (25, 0, 10) m per frame
    f32 wind;       ///< x (20, 0, 8) m per frame
    f64 maxRadiance; ///< gate: reprojected relative L1
    f64 maxTrail;    ///< gate: reprojected trail energy
    f64 vsBaseline;  ///< gate: reprojected radiance <= this x the baseline's
};

int runGhosting(Context& ctx) {
    // The noise is scaled 3x larger than the defaults so that the 64 x 32 cloud image resolves it: at the default
    // scale the detail octaves alias per pixel, and no reprojection can predict an aliased image.
    const GhostScenario scenarios[] = {
        {"turn 0.5 deg + move 27 m + wind 21.5 m / frame", 30.0, 0.5, 1.f, 1.f, 0.06, 0.004, 0.5},
        {"turn 2 deg + move 81 m + wind 43 m / frame", 30.0, 2.0, 3.f, 2.f, 0.08, 0.005, 0.6},
        {"wind only, 21.5 m / frame", 30.0, 0.0, 0.f, 1.f, 0.03, 0.002, 0.7},
        {"horizon in view (repeated resampling of the horizon band)", 18.0, 0.5, 1.f, 1.f, 0.15, 0.02, 0.75},
    };
    const char* language = "none";
    for (const GhostScenario& sc : scenarios) {
        Harness h;
        h.ctx = &ctx;
        h.useAtmos = true;
        h.atmosSettings = smallAtmosphere();
        if (!h.initAtmos()) {
            std::printf("SKIP: atmosphere unavailable\n");
            return kSkip;
        }
        CloudSettings amortised = baseSettings();
        amortised.resolution = CloudResolution{64, 32, 4, 64, 32};
        amortised.medium.shapeScale /= 3.f;
        amortised.medium.detailScale /= 3.f;
        amortised.medium.weatherScale /= 3.f;
        amortised.temporal.enabled = true;
        amortised.temporal.jitter = false;
        CloudSettings baseline = amortised;
        baseline.temporal.reproject = false;
        CloudSettings truth = amortised;
        truth.temporal.enabled = false;
        truth.resolution.block = 1;
        if (h.add(CloudKernelLanguage::Auto, amortised, kReadHistory | kReadFresh) == nullptr ||
            h.add(CloudKernelLanguage::Auto, baseline, kReadHistory) == nullptr ||
            h.add(CloudKernelLanguage::Auto, truth, kReadHistory) == nullptr) {
            std::printf("SKIP: no cloud kernel built\n");
            return kSkip;
        }
        language = h.instances[0]->clouds.kernelLanguage();
        constexpr u32 kFrames = 64;
        constexpr u32 kWarmup = 16;
        ImageErr sumA{}, sumB{};
        u32 measured = 0;
        std::vector<ClTexel> prevHistory;
        f64 reconErr = 0.0;
        u32 reconFrames = 0;
        Readback r;
        for (u32 frame = 0; frame < kFrames; ++frame) {
            const f32 k = static_cast<f32>(frame);
            const CloudCamera cam = camera(20.0 + sc.yawRate * frame, sc.pitch, Vec3{25.f * sc.move * k, 60.f, 10.f * sc.move * k});
            CloudFrame f = sceneFrame(cam);
            f.windOffset = Vec3{20.f * sc.wind * k, 0.f, 8.f * sc.wind * k};
            if (!runFrame(h, f, atmosView(cam), &r)) {
                expect(false, "ghosting frame");
                break;
            }
            const std::vector<ClTexel>& a = r.sections[0][4];
            if (frame >= kWarmup) {
                const ImageErr ea = imageErr(a, r.sections[2][4], 2u);
                const ImageErr eb = imageErr(r.sections[1][4], r.sections[2][4], 2u);
                sumA.radiance += ea.radiance;
                sumA.opacity += ea.opacity;
                sumA.trail += ea.trail;
                sumB.radiance += eb.radiance;
                sumB.opacity += eb.opacity;
                sumB.trail += eb.trail;
                ++measured;
            }
            if (frame >= 1u && frame % 7u == 3u) {
                // The device reconstruction vs the CPU twin on the device's own inputs.
                const CloudParams& p = h.instances[0]->clouds.params();
                std::vector<ClTexel> cpu(a.size());
                for (u32 y = 0; y < p.height; ++y) {
                    for (u32 x = 0; x < p.width; ++x) {
                        reconstruct_texel(p, r.sections[0][3].data(), prevHistory.data(), x, y,
                                          &cpu[(y * p.width + x) * 2u]);
                    }
                }
                const Cmp c = compare(a.data(), cpu.data(), a.size() / 2u, 2u);
                reconErr = std::max({reconErr, c.worstRgb, c.worstA});
                ++reconFrames;
            }
            prevHistory = a;
        }
        const f64 n = std::max(1u, measured);
        const ImageErr A{sumA.radiance / n, sumA.opacity / n, sumA.trail / n};
        const ImageErr B{sumB.radiance / n, sumB.opacity / n, sumB.trail / n};
        std::printf("ghosting: %s (pitch %.0f deg), 1/16 per frame, 64 x 32, %u frames measured\n", sc.name, sc.pitch,
                    measured);
        std::printf("  reprojected history:           rel L1 radiance %.4f, opacity %.4f, trail energy %.4f\n", A.radiance,
                    A.opacity, A.trail);
        std::printf("  same-pixel history (baseline): rel L1 radiance %.4f, opacity %.4f, trail energy %.4f\n", B.radiance,
                    B.opacity, B.trail);
        std::printf("  device reconstruction vs CPU twin (%u frames): %.3g\n", reconFrames, reconErr);
        expectLe(A.radiance, sc.maxRadiance, "moving camera: amortised radiance close to the per-frame full march");
        expectLe(A.trail, sc.maxTrail, "moving camera: trail energy below threshold");
        expectLe(A.radiance, sc.vsBaseline * B.radiance, "reprojection beats the same-pixel baseline");
        expectLe(reconErr, 1e-4, "device reconstruction == CPU twin");
    }
    std::printf("ghosting: %s kernels\n", language);
    return 0;
}

// --- composite -----------------------------------------------------------------------------------------------------
int runComposite(Context& ctx) {
    Harness h;
    h.ctx = &ctx;
    h.useAtmos = true;
    h.readAtmos = true;
    h.atmosSettings = smallAtmosphere();
    if (!h.initAtmos()) {
        std::printf("SKIP: atmosphere unavailable\n");
        return kSkip;
    }
    CloudSettings s = baseSettings();
    s.resolution.outWidth = 64;
    s.resolution.outHeight = 32;
    CloudSettings clear = s;
    clear.medium.coverageBias = -1.f;
    Instance* withBg = h.add(CloudKernelLanguage::Auto, s, kReadHistory | kReadResult);
    Instance* sky = h.add(CloudKernelLanguage::Auto, s, kReadHistory | kReadResult);
    Instance* none = h.add(CloudKernelLanguage::Auto, clear, kReadResult);
    if (withBg == nullptr || sky == nullptr || none == nullptr) {
        std::printf("SKIP: no cloud kernel built\n");
        return kSkip;
    }
    withBg->background = true;
    if (!ensureTargets(ctx, 256u)) {
        expect(false, "targets");
        return 1;
    }
    const u32 W = s.resolution.outWidth, H = s.resolution.outHeight;
    std::vector<ClTexel> bg(static_cast<usize>(W) * H);
    std::vector<f32> depth(bg.size());
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            bg[y * W + x] = ClTexel{0.1f + 0.01f * x, 0.2f + 0.02f * y, 0.3f, 1.f};
            depth[y * W + x] = (x / 8u + y / 8u) % 2u == 0u ? 500.f : 1e9f; // a checkerboard of near geometry
        }
    }
    std::memcpy(ctx.background.mapped, bg.data(), bg.size() * sizeof(ClTexel));
    std::memcpy(ctx.depth.mapped, depth.data(), depth.size() * sizeof(f32));
    const CloudCamera cam = camera(30.0, 18.0, Vec3{0.f, 60.f, 0.f});
    Readback r;
    expect(runFrame(h, sceneFrame(cam), atmosView(cam), &r), "composite frame");
    const at::AtLutView lv = r.luts();
    const ClAtmosphereView atm{&h.atmos.params(), &lv};
    const CloudParams& pb = withBg->clouds.params();
    const CloudParams& ps = sky->clouds.params();
    const CloudParams& pn = none->clouds.params();
    std::vector<ClTexel> cb(bg.size()), cs(bg.size()), sk(bg.size());
    u32 kept = 0;
    for (u32 y = 0; y < H; ++y) {
        for (u32 x = 0; x < W; ++x) {
            cb[y * W + x] = composite_texel(pb, r.sections[0][4].data(), atm, bg.data(), depth.data(), x, y);
            cs[y * W + x] = composite_texel(ps, r.sections[1][4].data(), atm, nullptr, nullptr, x, y);
            const Vec3 L = at::at_sky_radiance(h.atmos.params(), lv, cl_view_dir(pn, (x + 0.5f) / W, (y + 0.5f) / H), true);
            sk[y * W + x] = ClTexel{L.x, L.y, L.z, 1.f};
            const ClTexel& g = r.sections[0][5][y * W + x];
            kept += (depth[y * W + x] < 1e8f && g.r == bg[y * W + x].r && g.a == 1.f) ? 1u : 0u;
        }
    }
    const Cmp e1 = compare(r.sections[0][5].data(), cb.data(), cb.size());
    const Cmp e2 = compare(r.sections[1][5].data(), cs.data(), cs.size());
    const Cmp e3 = compare(r.sections[2][5].data(), sk.data(), sk.size());
    std::printf("composite: caller background + scene depth |gpu - cpu| %.3g x peak (%u near-geometry pixels keep the "
                "background)\n",
                e1.worstRgb, kept);
    std::printf("composite: sky + aerial perspective |gpu - cpu| %.3g x peak; no clouds vs sky %.3g x peak, T %.3g\n",
                e2.worstRgb, e3.worstRgb, e3.worstA);
    expectLe(e1.worstRgb, 2e-3, "composite with caller background and depth == CPU");
    expectLe(e2.worstRgb, 2e-3, "composite with sky and aerial perspective == CPU");
    expectLe(e3.worstRgb, 2e-3, "no clouds: composite == sky");
    expectLe(e3.worstA, 0.0, "no clouds: transmittance 1");
    expect(kept == W * H / 2u, "scene depth in front of the clouds keeps the background");
    return 0;
}

// --- zero_alloc ----------------------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && (std::strncmp(name, "clouds.", 7) == 0 || std::strncmp(name, "atmos.", 6) == 0)) {
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
    Harness h;
    h.ctx = &ctx;
    h.useAtmos = true;
    h.atmosSettings = smallAtmosphere();
    if (!h.initAtmos()) {
        std::printf("SKIP: atmosphere unavailable\n");
        return kSkip;
    }
    CloudSettings s = baseSettings();
    s.resolution.block = 4;
    s.temporal.enabled = true;
    Instance* inst = h.add(CloudKernelLanguage::Auto, s, 0u);
    if (inst == nullptr) {
        std::printf("SKIP: no cloud kernel built\n");
        return kSkip;
    }
    const std::vector<Probe> probes = rayProbes();
    if (!ensureTargets(ctx, 256u)) {
        expect(false, "targets");
        return 1;
    }
    writeProbes(ctx, probes);
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long side = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    u32 rebuilds = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        const CloudCamera cam = camera(10.0 + 0.3 * frame, 18.0, Vec3{5.f * static_cast<f32>(frame), 60.f, 0.f});
        CloudFrame f = sceneFrame(cam);
        f.windOffset = Vec3{10.f * static_cast<f32>(frame), 0.f, 0.f};
        at::AtmosphereLutView av = atmosView(cam);
        av.sunDirection = dirElAz((30.0 + 0.2 * frame) * kDeg, 40.0 * kDeg);
        if (frame % 16u == 8u) {
            inst->clouds.invalidateNoise(); // the noise passes too, now and then
        }
        t_allocations = 0;
        t_count = measure;
        const bool begun = beginAll(h, f, av);
        t_count = false;
        const unsigned long long begin = t_allocations;
        layoutReadback(h);
        t_allocations = 0;
        t_count = measure;
        buildGraph(h, static_cast<u32>(probes.size()));
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(h.graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle();
        h.atmos.collectRetired(ctx.serial);
        inst->clouds.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(begun && result.ok && waited, "frame ok");
        if (measure) {
            side += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
        if (frame == kWarmup) {
            rebuilds = inst->clouds.stats().frameRebuilds + inst->clouds.stats().noiseRebuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(inst->clouds.stats().frameRebuilds + inst->clouds.stats().noiseRebuilds == rebuilds && rebuilds == 2u,
           "no buffer rebuild in steady state");
    expect(inst->clouds.stats().noiseBakes == 1u + (kTotal - 8u + 15u) / 16u, "noise re-baked only when invalidated");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s kernels; atmosphere + clouds 1/16 + probe every frame, noise every\n"
                    "  16th; camera, sun and wind moving)\n"
                    "  beginFrame + clouds.* / atmos.* pass callbacks: %llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (imports and passes): %llu\n",
                    kTotal - kWarmup, inst->clouds.kernelLanguage(), side, callbacks, build);
        expect(side == 0u, "the cloud passes make no steady-state heap allocations");
        expect(build == 0u, "graph build with the cloud passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok (%s)\n", kTotal,
                    inst->clouds.kernelLanguage());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "ray";
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
        if (mode == "noise") {
            rc = runNoise(ctx);
        } else if (mode == "ray") {
            rc = runRay(ctx);
        } else if (mode == "analytic") {
            rc = runAnalytic(ctx);
        } else if (mode == "converge") {
            rc = runConverge(ctx);
        } else if (mode == "ghosting") {
            rc = runGhosting(ctx);
        } else if (mode == "composite") {
            rc = runComposite(ctx);
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
