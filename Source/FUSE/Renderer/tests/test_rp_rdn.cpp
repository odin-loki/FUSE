// RL-5.5 radiance denoiser: Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation; every
// validation message fails the run). CPU gates: test_rp_rdn_cpu.cpp.
//
// Every frame is one render graph: the analytic scene (rdn_synthetic.hpp) is written to host-visible input buffers
// (diffuse, specular, normal, depth, motion, instance, A-SVGF gradient samples), RadianceDenoiser adds its passes, the
// host reads the (host-visible, keepIntermediates) arenas after the fence.
//
//   --mode passes      each GPU pass against its CPU reference (rdn_kernel.hpp) on the GPU's own inputs of that pass
//                      (the arenas as the GPU left them, one pass replayed at a time), 8 frames of the moving-camera
//                      mirror scene with every feature on (A-SVGF, virtual motion, history fix, pre-blur, firefly
//                      clamps, history clipping, instance test), 61 x 37 (odd extent): per value
//                      |gpu - cpu| <= 1/1024 max(|cpu|, 1e-3 max|region|); the whole chain against RdnReference fed the
//                      same frames is reported (and must stay within 1e-2 relative RMS).
//   --mode zero_alloc  64 steady-state frames: 0 operator-new calls in beginFrame, the rdn.* pass callbacks and the
//                      graph build (validated run first; validation off for the count).
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//   --language slang | glsl  the kernel (skip if not built)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, kernel not built).
#include <fuse/renderer/denoise/radiance_denoiser.hpp>
#include <fuse/renderer/denoise/rdn_synthetic.hpp>
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
using namespace fuse::renderer::denoise;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using rdnk::float4;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr f64 kParityTol = 1.0 / 1024.0;

u32 g_messages = 0;

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

enum Input : u32 { kInDiffuse = 0, kInSpecular, kInNormal, kInDepth, kInMotion, kInInstance, kInGradient, kInCount };

struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    std::unique_ptr<rg::Executor> executor;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    BindlessDescriptors bindless;
    Buffer inputs[kInCount]{};
    u8 queues[kInCount] = {rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue,
                           rg::kNoQueue, rg::kNoQueue, rg::kNoQueue};
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        for (Buffer& b : inputs) {
            if (b.handle != nullptr && allocator != nullptr) {
                allocator->destroyBuffer(b);
            }
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
            std::printf("SKIP: %s not installed (the gate needs synchronization validation)\n",
                        kValidationLayer);
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
    instanceDesc.appName = "fuse_rp_rdn";
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
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
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
    const char* why = "";
    if (!rdn_supported(ctx.device.get(), &why)) {
        std::printf("SKIP: radiance denoiser unsupported: %s\n", why);
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
    bdesc.backend =
        descriptorBuffer ? BindlessBackendPreference::DescriptorBuffer : BindlessBackendPreference::DescriptorSet;
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

bool ensureInputs(Context& ctx, u32 w, u32 h) {
    const u64 n = static_cast<u64>(w) * h;
    const u64 ns = static_cast<u64>((w + 2u) / 3u) * ((h + 2u) / 3u);
    const u64 sizes[kInCount] = {n * 16u, n * 16u, n * 16u, n * 4u, n * 8u, n * 4u, ns * 16u};
    const char* names[kInCount] = {"rdn.diffuse", "rdn.specular", "rdn.normal",  "rdn.depth",
                                   "rdn.motion",  "rdn.instance", "rdn.gradient"};
    for (u32 i = 0; i < kInCount; ++i) {
        BufferDesc d{};
        d.size = static_cast<usize>(sizes[i]);
        d.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                           static_cast<u32>(BufferUsage::ShaderDeviceAddress));
        d.memoryUsage = MemoryUsage::CpuToGpu;
        d.name = names[i];
        if (!ctx.allocator->createBuffer(d, ctx.inputs[i]) || ctx.inputs[i].mapped == nullptr) {
            return false;
        }
    }
    return true;
}

void writeInputs(Context& ctx, const RdnSyntheticFrame& f) {
    const usize n = static_cast<usize>(f.width) * f.height;
    std::memcpy(ctx.inputs[kInDiffuse].mapped, f.diffuse.data(), n * 16u);
    std::memcpy(ctx.inputs[kInSpecular].mapped, f.specular.data(), n * 16u);
    std::memcpy(ctx.inputs[kInNormal].mapped, f.normal.data(), n * 16u);
    std::memcpy(ctx.inputs[kInDepth].mapped, f.depth.data(), n * 4u);
    std::memcpy(ctx.inputs[kInMotion].mapped, f.motion.data(), n * 8u);
    std::memcpy(ctx.inputs[kInInstance].mapped, f.instance.data(), n * 4u);
    std::memcpy(ctx.inputs[kInGradient].mapped, f.gradient.data(), f.gradient.size() * 16u);
}

RdnFrameDesc frameDesc(const Context& ctx, const RdnSyntheticFrame& f) {
    RdnFrameDesc d{};
    d.width = f.width;
    d.height = f.height;
    d.diffuse = ctx.inputs[kInDiffuse].deviceAddress;
    d.specular = ctx.inputs[kInSpecular].deviceAddress;
    d.normal = ctx.inputs[kInNormal].deviceAddress;
    d.depth = ctx.inputs[kInDepth].deviceAddress;
    d.motion = ctx.inputs[kInMotion].deviceAddress;
    d.instance = ctx.inputs[kInInstance].deviceAddress;
    d.gradient = ctx.inputs[kInGradient].deviceAddress;
    d.camera = f.camera;
    d.prevCamera = f.prevCamera;
    return d;
}

RdnGraphInputs importInputs(Context& ctx, rg::Graph& graph) {
    RdnGraphInputs in{};
    for (u32 i = 0; i < kInCount; ++i) {
        const Buffer& b = ctx.inputs[i];
        in.add(
            graph.importBuffer(rg::ImportedBuffer{b.handle, b.desc.size, ctx.queues[i], &ctx.queues[i], "rdn.input"}));
    }
    return in;
}

RdnSettings testSettings() {
    RdnSettings s = rdn_preset();
    s.motionScale = 1.f;
    s.gradients = true;
    s.instanceTest = true;
    s.fireflyRatio = 8.f;
    s.fireflySigma = 6.f;
    s.clampSigmaD = 3.f;
    return s;
}

bool runFrame(Context& ctx, RadianceDenoiser& dn, rg::Graph& graph, const RdnSyntheticFrame& f) {
    writeInputs(ctx, f);
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    if (!dn.beginFrame(ctx.serial, frameDesc(ctx, f))) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    graph.reset();
    const RdnGraphInputs in = importInputs(ctx, graph);
    const RdnGraphRefs refs = dn.importInto(graph);
    dn.addPasses(graph, refs, in);
    graph.addPass("test.readback", nullptr, nullptr)
        .use(refs.state, rg::Access::HostRead)
        .use(refs.work, rg::Access::HostRead)
        .use(refs.output, rg::Access::HostRead);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    dn.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (!result.ok || !waited) {
        std::fprintf(stderr, "  execute failed\n");
        return false;
    }
    return true;
}

// --- passes ----------------------------------------------------------------------------------------------------------
/// Device address -> host pointer of the same byte, over the arenas / inputs (0 stays 0).
struct AddressMap {
    struct Range {
        u64 device = 0;
        u64 size = 0;
        u8* host = nullptr;
    };
    Range ranges[16]{};
    u32 count = 0;
    void add(u64 device, u64 size, void* host) { ranges[count++] = Range{device, size, static_cast<u8*>(host)}; }
    u64 operator()(u64 a) const {
        if (a == 0u) {
            return 0u;
        }
        for (u32 i = 0; i < count; ++i) {
            if (a >= ranges[i].device && a < ranges[i].device + ranges[i].size) {
                return rdnk::rdnAddr(ranges[i].host + (a - ranges[i].device));
            }
        }
        std::fprintf(stderr, "FAIL: unmapped address 0x%llx\n", static_cast<unsigned long long>(a));
        ++g_failures;
        return 0u;
    }
};

RdnFrame translate(const RdnFrame& c, const AddressMap& m) {
    RdnFrame h = c;
    u64* fields[] = {&h.inDiffuse, &h.inSpecular, &h.inNormal, &h.inDepth, &h.inMotion, &h.inInstance, &h.inGradient,
                     &h.guideCur,  &h.guidePrev,  &h.auxCur,   &h.auxPrev, &h.histDCur, &h.histDPrev,  &h.histSCur,
                     &h.histSPrev, &h.momCur,     &h.momPrev,  &h.preD,    &h.preS,     &h.blurD,      &h.blurS,
                     &h.accD,      &h.accS,       &h.mip1,     &h.mip2,    &h.mip3,     &h.fixD,       &h.fixS,
                     &h.varD,      &h.varS,       &h.lambda};
    for (u64* f : fields) {
        *f = m(*f);
    }
    return h;
}

/// Byte ranges (device addresses) a pass writes.
u32 passOutputs(const RdnFrame& c, const RdnBufferLayout& l, const RdnPassDesc& p, u64 out[][2]) {
    const u64 n = static_cast<u64>(l.width) * l.height * 16u;
    const u64 ns = static_cast<u64>(l.strataW) * l.strataH * 16u;
    u32 k = 0;
    auto add = [&](u64 a, u64 bytes) {
        out[k][0] = a;
        out[k][1] = bytes;
        ++k;
    };
    switch (p.args.pass) {
    case FUSE_RDN_PASS_PREPARE:
        add(c.guideCur, n), add(c.auxCur, n), add(c.preD, n), add(c.preS, n);
        break;
    case FUSE_RDN_PASS_PREBLUR:
        add(c.blurD, n), add(c.blurS, n);
        break;
    case FUSE_RDN_PASS_GRAD_PREPARE:
    case FUSE_RDN_PASS_GRAD_ATROUS:
        add(p.args.a[2], ns);
        break;
    case FUSE_RDN_PASS_TEMPORAL:
        add(c.accD, n), add(c.accS, n), add(c.momCur, n);
        break;
    case FUSE_RDN_PASS_MIP:
        add(p.args.a[2], static_cast<u64>(l.mipW[p.args.step - 1u]) * l.mipH[p.args.step - 1u] * 64u);
        break;
    case FUSE_RDN_PASS_HISTFIX:
        add(c.fixD, n), add(c.fixS, n);
        break;
    case FUSE_RDN_PASS_VARIANCE:
        add(c.varD, n), add(c.varS, n);
        break;
    case FUSE_RDN_PASS_ATROUS:
        add(p.args.a[2], n), add(p.args.a[3], n);
        if (p.args.a[4] != 0u) {
            add(p.args.a[4], n), add(p.args.a[5], n);
        }
        break;
    default:
        break;
    }
    return k;
}

struct PassStat {
    f64 maxErr = 0.0;
    u64 values = 0;
    u64 over = 0;
};

int runPasses(Context& ctx, RdnKernelLanguage language) {
    constexpr u32 kW = 61;
    constexpr u32 kH = 37;
    if (!ensureInputs(ctx, kW, kH)) {
        std::fprintf(stderr, "FAIL: inputs\n");
        return 1;
    }
    RadianceDenoiser dn;
    RadianceDenoiserDesc desc{};
    desc.device = ctx.device.get();
    desc.allocator = ctx.allocator.get();
    desc.bindless = &ctx.bindless;
    desc.language = language;
    desc.keepIntermediates = true;
    desc.hostVisible = true;
    if (!dn.init(desc)) {
        std::printf("SKIP: the %s kernel is not built\n", language == RdnKernelLanguage::Slang ? "Slang" : "GLSL");
        return kSkip;
    }
    std::printf("kernel: %s\n", dn.kernelLanguage());
    const RdnSettings s = testSettings();
    dn.setSettings(s);
    RdnReference ref;
    ref.init(kW, kH, s, true);
    RdnSyntheticDesc sd{};
    sd.width = kW;
    sd.height = kH;
    sd.camSpeed = 0.08f;
    sd.lightStepFrame = 5u;
    sd.firefliesPerMille = 3u;
    RdnSyntheticFrame f;
    rg::Graph graph;
    PassStat stats[FUSE_RDN_PASS_COUNT]{};
    f64 chainSe = 0.0, chainRef = 0.0;
    std::vector<u8> cState, cWork, cOut;
    for (u32 t = 0; t < 8u; ++t) {
        rdn_synthetic_frame(sd, t, 29u, f);
        if (!runFrame(ctx, dn, graph, f)) {
            ++g_failures;
            break;
        }
        ref.runFrame(f.inputs(true, true), f.camera, f.prevCamera);
        const RdnBufferLayout& l = dn.layout();
        const u8* gState = static_cast<const u8*>(dn.stateBuffer().mapped);
        const u8* gWork = static_cast<const u8*>(dn.workBuffer().mapped);
        const u8* gOut = static_cast<const u8*>(dn.outputBuffer().mapped);
        AddressMap m{};
        for (u32 i = 0; i < kInCount; ++i) {
            m.add(ctx.inputs[i].deviceAddress, ctx.inputs[i].desc.size, ctx.inputs[i].mapped);
        }
        m.add(dn.stateBuffer().deviceAddress, l.stateBytes, nullptr);
        m.add(dn.workBuffer().deviceAddress, l.workBytes, nullptr);
        m.add(dn.outputBuffer().deviceAddress, l.outputBytes, nullptr);
        for (u32 k = 0; k < dn.passCount(); ++k) {
            const RdnPassDesc& p = dn.pass(k);
            // The GPU's arenas as the frame left them: every pass's inputs are intact
            // (keepIntermediates: no section is written twice in a frame, and the previous state is read-only).
            cState.assign(gState, gState + l.stateBytes);
            cWork.assign(gWork, gWork + l.workBytes);
            cOut.assign(gOut, gOut + l.outputBytes);
            m.ranges[kInCount + 0u].host = cState.data();
            m.ranges[kInCount + 1u].host = cWork.data();
            m.ranges[kInCount + 2u].host = cOut.data();
            // Poison what the pass writes so an unwritten value cannot pass.
            u64 outs[6][2];
            const u32 no = passOutputs(dn.constants(), l, p, outs);
            for (u32 o = 0; o < no; ++o) {
                std::memset(reinterpret_cast<u8*>(static_cast<std::uintptr_t>(m(outs[o][0]))), 0xFF,
                            static_cast<usize>(outs[o][1]));
            }
            const RdnFrame hc = translate(dn.constants(), m);
            RdnPassDesc hp = p;
            for (u64& a : hp.args.a) {
                a = m(a);
            }
            RdnReference::runPass(hc, hp);
            for (u32 o = 0; o < no; ++o) {
                const f32* cpu = reinterpret_cast<const f32*>(static_cast<std::uintptr_t>(m(outs[o][0])));
                const u64 off = outs[o][0];
                const u8* gpuBase =
                    off >= dn.outputBuffer().deviceAddress && off < dn.outputBuffer().deviceAddress + l.outputBytes
                        ? gOut + (off - dn.outputBuffer().deviceAddress)
                    : off >= dn.workBuffer().deviceAddress && off < dn.workBuffer().deviceAddress + l.workBytes
                        ? gWork + (off - dn.workBuffer().deviceAddress)
                        : gState + (off - dn.stateBuffer().deviceAddress);
                const f32* gpu = reinterpret_cast<const f32*>(gpuBase);
                const usize count = static_cast<usize>(outs[o][1] / 4u);
                f64 mx = 0.0;
                for (usize i = 0; i < count; ++i) {
                    mx = std::max(mx, static_cast<f64>(std::fabs(cpu[i])));
                }
                PassStat& ps = stats[p.args.pass];
                for (usize i = 0; i < count; ++i) {
                    const f64 c = cpu[i];
                    const f64 g = gpu[i];
                    const f64 scale = std::max(std::fabs(c), 1e-3 * mx);
                    const f64 e = scale > 0.0 ? std::fabs(g - c) / scale : std::fabs(g - c);
                    const bool bad = !(e <= kParityTol);
                    ps.maxErr = std::max(ps.maxErr, std::isfinite(e) ? e : 1e30);
                    ps.over += bad ? 1u : 0u;
                    ++ps.values;
                }
            }
        }
        // Whole chain against RdnReference.
        const usize n = static_cast<usize>(kW) * kH;
        const f32* gD = dn.mappedOutputD();
        const f32* gS = dn.mappedOutputS();
        for (usize i = 0; i < n; ++i) {
            for (u32 c = 0; c < 3u; ++c) {
                const f64 a = (&ref.outputD()[i].x)[c];
                const f64 b = (&ref.outputS()[i].x)[c];
                chainSe += (gD[i * 4u + c] - a) * (gD[i * 4u + c] - a) + (gS[i * 4u + c] - b) * (gS[i * 4u + c] - b);
                chainRef += a * a + b * b;
            }
        }
    }
    const char* names[FUSE_RDN_PASS_COUNT] = {"prepare",         "preblur",  "gradient.prepare",
                                              "gradient.atrous", "temporal", "mip",
                                              "historyfix",      "variance", "atrous"};
    for (u32 k = 0; k < FUSE_RDN_PASS_COUNT; ++k) {
        std::printf("passes: rdn.%-17s %8llu values, max rel err %.3g, over 1/1024: %llu\n", names[k],
                    static_cast<unsigned long long>(stats[k].values), stats[k].maxErr,
                    static_cast<unsigned long long>(stats[k].over));
        expect(stats[k].values > 0u, "every pass checked");
        expect(stats[k].over == 0u, "per-pass GPU == CPU within 1/1024");
    }
    const f64 chainRel = std::sqrt(chainSe / std::max(chainRef, 1e-30));
    std::printf("passes: whole chain (8 frames) GPU vs RdnReference relative RMS %.3g\n", chainRel);
    expect(chainRel <= 1e-2, "whole chain within 1e-2 relative RMS");
    std::printf("validation messages: %u\n", g_messages);
    expect(g_messages == 0u, "0 validation messages");
    dn.destroy();
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "rdn.", 4) == 0) {
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

int runZeroAlloc(Context& ctx, RdnKernelLanguage language, bool countAllocations) {
    constexpr u32 kW = 64;
    constexpr u32 kH = 40;
    if (!ensureInputs(ctx, kW, kH)) {
        std::fprintf(stderr, "FAIL: inputs\n");
        return 1;
    }
    RadianceDenoiser dn;
    RadianceDenoiserDesc desc{};
    desc.device = ctx.device.get();
    desc.allocator = ctx.allocator.get();
    desc.bindless = &ctx.bindless;
    desc.language = language;
    desc.hostVisibleOutput = true;
    if (!dn.init(desc)) {
        std::printf("SKIP: kernel not built\n");
        return kSkip;
    }
    dn.setSettings(testSettings());
    RdnSyntheticDesc sd{};
    sd.width = kW;
    sd.height = kH;
    std::vector<RdnSyntheticFrame> frames(4);
    for (u32 i = 0; i < 4u; ++i) {
        rdn_synthetic_frame(sd, i + 1u, 5u, frames[i]);
    }
    rg::Graph graph;
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long side = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    u32 rebuilds = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        const RdnSyntheticFrame& f = frames[frame % 4u];
        writeInputs(ctx, f);
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        t_allocations = 0;
        t_count = measure;
        RdnFrameDesc fd = frameDesc(ctx, f);
        fd.reset = frame % 37u == 0u;
        const bool begun = dn.beginFrame(ctx.serial, fd);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        graph.reset();
        const RdnGraphInputs in = importInputs(ctx, graph);
        const RdnGraphRefs refs = dn.importInto(graph);
        dn.addPasses(graph, refs, in);
        graph.addPass("test.readback", nullptr, nullptr).use(refs.output, rg::Access::HostRead);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle();
        dn.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(begun && result.ok && waited, "frame ok");
        expect(dn.stats().passes == dn.passCount() && dn.passCount() > 10u, "every pass runs in steady state");
        if (measure) {
            side += begin + inCallbacks;
            build += graphBuild;
        }
        if (frame == kWarmup) {
            rebuilds = dn.stats().rebuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(dn.stats().rebuilds == rebuilds && rebuilds == 1u, "no resource rebuild in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation off): %u steady-state frames (%s kernel)\n"
                    "  RadianceDenoiser::beginFrame + rdn.* pass callbacks: %llu operator-new calls\n"
                    "  graph build (test imports + denoiser imports and passes): %llu\n",
                    kTotal - kWarmup, dn.kernelLanguage(), side, build);
        expect(side == 0u, "the denoiser makes no steady-state heap allocations");
        expect(build == 0u, "graph build with the denoiser passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc (validated run): %u frames, validation messages: %u\n", kTotal, g_messages);
        expect(g_messages == 0u, "0 validation messages");
    }
    dn.destroy();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "passes";
    std::string backend = "set";
    std::string lang = "slang";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        if (key == "--mode") {
            mode = argv[i + 1];
        } else if (key == "--backend") {
            backend = argv[i + 1];
        } else if (key == "--language") {
            lang = argv[i + 1];
        }
    }
    const RdnKernelLanguage language = lang == "glsl" ? RdnKernelLanguage::Glsl : RdnKernelLanguage::Slang;
    int rc = 0;
    if (mode == "zero_alloc") {
        {
            Context ctx;
            rc = setup(ctx, backend == "buffer", true);
            if (rc == 0) {
                rc = runZeroAlloc(ctx, language, false);
            }
        }
        if (rc == 0) {
            Context ctx;
            rc = setup(ctx, backend == "buffer", false);
            if (rc == 0) {
                rc = runZeroAlloc(ctx, language, true);
            }
        }
    } else {
        Context ctx;
        rc = setup(ctx, backend == "buffer", true);
        if (rc == 0) {
            rc = runPasses(ctx, language);
        }
    }
    if (rc != 0) {
        return rc;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "FAIL: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS %s\n", mode.c_str());
    return 0;
}

#endif
