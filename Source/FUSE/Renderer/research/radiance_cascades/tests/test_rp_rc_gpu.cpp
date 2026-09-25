// WP-6.5 radiance cascades (research): Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization
// validation; every validation message fails the run). CPU gates: test_rp_rc_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): the scene (f32x4 per texel) sits in a host-visible buffer the
// test writes between frames and imports (StorageRead), RadianceCascadesGpu adds rc.cascade x n + rc.gather,
// then rc.copy reads the f32x4 output back. Both kernel languages built (Slang, GLSL) run every check.
//
//   --mode parity      GPU == CPU reference (RcCpuSolver, the kernels' line-for-line twin) on the rooms scene
//                      (97 x 83, odd extent), the sealed leak box and the disk emitter, vanilla and bilinear fix,
//                      (s0, N0) = (1, 4) and (2, 8): >= 99.9% of pixels within 1e-5 relative (+ 1e-6 absolute),
//                      every pixel within 1e-3 relative (+ 1e-4 absolute: a sqrt / division ulp on the device can
//                      move one march sample of one bilinear-fix ray across a texel edge), frame means within
//                      1e-5 relative; the empty scene reproduces the sky within 1e-6 on the GPU too.
//   --mode zero_alloc  80 frames (64 measured) alternating scenes and merge modes at a fixed layout: 0 operator-new
//                      calls in beginFrame, the rc.* pass callbacks and the whole graph build; no layout rebuild
//                      (validated run first; validation off for the count).
//   --backend none | set | buffer   no bindless / bindless descriptor sets / VK_EXT_descriptor_buffer bound on the
//                      pipelines' layout (skip if absent).
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include <fuse/renderer/research/rc/rc_gpu.hpp>
#include <fuse/renderer/research/rc/rc_reference.hpp>
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
using namespace fuse::renderer::research::rc;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr u32 kMaxPixels = 128u * 128u;

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

BufferUsage usage2(BufferUsage a, BufferUsage b) { return static_cast<BufferUsage>(static_cast<u32>(a) | static_cast<u32>(b)); }

struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    std::unique_ptr<rg::Executor> executor;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    BindlessDescriptors bindless;
    bool useBindless = false;
    Buffer scene{};
    Buffer readback{};
    u8 sceneQueue = rg::kNoQueue;
    u8 readbackQueue = rg::kNoQueue;
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        if (allocator != nullptr) {
            for (Buffer* b : {&scene, &readback}) {
                if (b->handle != nullptr) {
                    allocator->destroyBuffer(*b);
                }
            }
        }
        if (device != nullptr && useBindless) {
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

int setup(Context& ctx, const std::string& backend, bool validation) {
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
    instanceDesc.appName = "fuse_rp_rc_gpu";
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
    if (backend == "buffer" && !ctx.device->info().caps.descriptorBuffer) {
        std::printf("SKIP: device has no VK_EXT_descriptor_buffer\n");
        return kSkip;
    }
    const RcCapabilities caps = queryRcCapabilities(ctx.device.get());
    if (!caps.rc) {
        std::printf("SKIP: radiance cascades unsupported: %s\n", caps.reason);
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s\n", ctx.device->info().deviceName.c_str());
    if (backend == "set" || backend == "buffer") {
        BindlessDesc bdesc{};
        bdesc.backend =
            backend == "buffer" ? BindlessBackendPreference::DescriptorBuffer : BindlessBackendPreference::DescriptorSet;
        if (!ctx.bindless.init(*ctx.device, bdesc)) {
            std::fprintf(stderr, "FAIL: bindless init\n");
            return 1;
        }
        ctx.useBindless = true;
        std::printf("bindless backend: %s\n", bindlessBackendName(ctx.bindless.backend()));
    } else {
        std::printf("bindless: none\n");
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    BufferDesc s{};
    s.size = static_cast<usize>(kMaxPixels) * 16u;
    s.usage = usage2(BufferUsage::Storage, BufferUsage::ShaderDeviceAddress);
    s.memoryUsage = MemoryUsage::CpuToGpu;
    s.name = "rp_rc.scene";
    BufferDesc r{};
    r.size = static_cast<usize>(kMaxPixels) * 16u;
    r.usage = BufferUsage::TransferDst;
    r.memoryUsage = MemoryUsage::GpuToCpu;
    r.name = "rp_rc.readback";
    if (!ctx.allocator->createBuffer(s, ctx.scene) || ctx.scene.mapped == nullptr || ctx.scene.deviceAddress == 0u ||
        !ctx.allocator->createBuffer(r, ctx.readback) || ctx.readback.mapped == nullptr) {
        std::fprintf(stderr, "FAIL: test buffers\n");
        return 1;
    }
    return 0;
}

bool initRc(Context& ctx, RadianceCascadesGpu& rc, RcKernelLanguage language) {
    RcGpuDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = ctx.useBindless ? &ctx.bindless : nullptr;
    d.language = language;
    return rc.init(d);
}

void buildGraph(Context& ctx, RadianceCascadesGpu& rc, rg::Graph& graph, bool readback) {
    graph.reset();
    const rg::BufferRef scene = graph.importBuffer(
        rg::ImportedBuffer{ctx.scene.handle, ctx.scene.desc.size, ctx.sceneQueue, &ctx.sceneQueue, "rp_rc.scene"});
    const RcGraphRefs refs = rc.importInto(graph);
    rc.addPasses(graph, refs, scene);
    if (readback) {
        const rg::BufferRef rb = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size,
                                                                       ctx.readbackQueue, &ctx.readbackQueue,
                                                                       "rp_rc.readback"});
        rc.addCopyOutput(graph, refs, rb, 0);
    }
}

bool runFrame(Context& ctx, RadianceCascadesGpu& rc, const RcScene& scene, const RcSettings& settings,
              std::vector<f32>& out) {
    std::memcpy(ctx.scene.mapped, scene.texels.data(), scene.texels.size() * sizeof(f32));
    ++ctx.serial;
    if (ctx.useBindless) {
        ctx.bindless.setFrameSerial(ctx.serial);
    }
    if (!rc.beginFrame(ctx.serial, settings, scene.width, scene.height, ctx.scene.deviceAddress)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return false;
    }
    rg::Graph graph;
    buildGraph(ctx, rc, graph, true);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    rc.collectRetired(ctx.serial);
    if (ctx.useBindless) {
        ctx.bindless.collectRetired(ctx.serial);
    }
    if (!result.ok || !waited) {
        std::fprintf(stderr, "  execute failed\n");
        return false;
    }
    out.resize(static_cast<usize>(scene.width) * scene.height * 4u);
    std::memcpy(out.data(), ctx.readback.mapped, out.size() * sizeof(f32));
    return true;
}

struct Lang {
    RcKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[] = {{RcKernelLanguage::Slang, "slang"}, {RcKernelLanguage::Glsl, "glsl"}};

// --- parity ----------------------------------------------------------------------------------------------
int runParity(Context& ctx) {
    u32 languages = 0;
    for (const Lang& lang : kLangs) {
        RadianceCascadesGpu rc;
        if (!initRc(ctx, rc, lang.language)) {
            std::printf("%s kernels: not built, skipped\n", lang.name);
            continue;
        }
        ++languages;
        struct SceneCase {
            const char* name;
            RcScene scene;
        };
        std::vector<SceneCase> scenes(3);
        scenes[0].name = "rooms 97x83";
        sceneRooms(scenes[0].scene, 97, 83, 1u);
        scenes[1].name = "leak box 64x64";
        std::vector<u8> inside;
        sceneLeakBox(scenes[1].scene, 64, 64, 2u, inside);
        scenes[2].name = "disk 72x56";
        sceneDisk(scenes[2].scene, 72, 56, 3.0f);
        RcCpuSolver cpu;
        std::vector<f32> ref;
        std::vector<f32> gpu;
        for (const SceneCase& sc : scenes) {
            struct Config {
                u32 s0, n0;
                bool fix;
            };
            for (const Config c : {Config{1, 4, false}, Config{1, 4, true}, Config{2, 8, false}, Config{2, 8, true}}) {
                RcSettings s{};
                s.probeSpacing0 = c.s0;
                s.dirs0 = c.n0;
                s.interval0 = static_cast<f32>(c.s0);
                s.bilinearFix = c.fix;
                s.sky[0] = 0.05f;
                s.sky[1] = 0.08f;
                s.sky[2] = 0.12f;
                const bool ran = runFrame(ctx, rc, sc.scene, s, gpu);
                expect(ran, "GPU frame");
                expect(cpu.solve(sc.scene, s, ref), "CPU solve");
                if (!ran) {
                    continue;
                }
                const usize n = ref.size() / 4u;
                usize exact = 0;
                usize close = 0;
                f64 worst = 0.0;
                f64 sumGpu = 0.0;
                f64 sumCpu = 0.0;
                for (usize i = 0; i < n; ++i) {
                    bool same = true;
                    bool nearOk = true; // not `near`: a <windows.h> macro under MSVC
                    for (u32 k = 0; k < 4u; ++k) {
                        const f64 a = gpu[i * 4u + k];
                        const f64 b = ref[i * 4u + k];
                        const f64 d = std::fabs(a - b);
                        same = same && a == b;
                        nearOk = nearOk && d <= 1e-5 * std::fabs(b) + 1e-6;
                        worst = std::max(worst, d / (std::fabs(b) + 0.1));
                        if (k < 3u) {
                            sumGpu += a;
                            sumCpu += b;
                        }
                    }
                    exact += same ? 1u : 0u;
                    close += nearOk ? 1u : 0u;
                }
                const f64 meanRel = sumCpu > 0.0 ? std::fabs(sumGpu - sumCpu) / sumCpu : std::fabs(sumGpu - sumCpu);
                std::printf("parity %s: %-14s s0 %u N0 %u %-12s %u cascades: bit-exact %.3f%%, within 1e-5 %.3f%%, "
                            "worst %.2e, frame mean rel %.2e\n",
                            lang.name, sc.name, c.s0, c.n0, c.fix ? "bilinear-fix" : "vanilla", rc.layout().cascades,
                            100.0 * static_cast<f64>(exact) / static_cast<f64>(n),
                            100.0 * static_cast<f64>(close) / static_cast<f64>(n), worst, meanRel);
                expect(static_cast<f64>(close) >= 0.999 * static_cast<f64>(n), "GPU == CPU: >= 99.9% pixels within 1e-5");
                expect(worst <= 1e-3, "GPU == CPU: every pixel within 1e-3 relative");
                expect(meanRel <= 1e-5, "GPU == CPU: frame mean within 1e-5");
            }
        }
        // Energy on the GPU: the empty scene under a uniform sky.
        RcScene empty;
        sceneEmpty(empty, 61, 45);
        for (const bool fix : {false, true}) {
            RcSettings s{};
            s.bilinearFix = fix;
            s.sky[0] = 1.0f;
            s.sky[1] = 0.5f;
            s.sky[2] = 0.25f;
            expect(runFrame(ctx, rc, empty, s, gpu), "GPU frame");
            f64 dev = 0.0;
            for (usize i = 0; i < gpu.size() / 4u; ++i) {
                for (u32 k = 0; k < 3u; ++k) {
                    dev = std::max(dev, std::fabs(static_cast<f64>(gpu[i * 4u + k]) - s.sky[k]) / s.sky[k]);
                }
            }
            std::printf("energy %s: empty 61x45 %s: max relative deviation from the sky %.3g\n", lang.name,
                        fix ? "bilinear-fix" : "vanilla", dev);
            expect(dev <= 1e-6, "GPU: empty scene reproduces the sky within 1e-6");
        }
        std::printf("%s: %u layout rebuilds over the parity frames\n", lang.name, rc.stats().layoutRebuilds);
        rc.destroy();
    }
    if (languages == 0u) {
        std::printf("SKIP: no radiance-cascades kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "rc.", 3) == 0) {
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
    RadianceCascadesGpu rc;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initRc(ctx, rc, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: no radiance-cascades kernel built\n");
        return kSkip;
    }
    std::vector<RcScene> scenes(4);
    for (u32 i = 0; i < 4u; ++i) {
        sceneRooms(scenes[i], 48, 40, i);
    }
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
        const RcScene& scene = scenes[frame % 4u];
        std::memcpy(ctx.scene.mapped, scene.texels.data(), scene.texels.size() * sizeof(f32));
        RcSettings s{};
        s.bilinearFix = (frame % 2u) == 0u;
        s.sky[0] = 0.02f * static_cast<f32>(frame % 5u);
        ++ctx.serial;
        if (ctx.useBindless) {
            ctx.bindless.setFrameSerial(ctx.serial);
        }
        t_allocations = 0;
        t_count = measure;
        const bool begun = rc.beginFrame(ctx.serial, s, scene.width, scene.height, ctx.scene.deviceAddress);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, rc, graph, false);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle();
        rc.collectRetired(ctx.serial);
        if (ctx.useBindless) {
            ctx.bindless.collectRetired(ctx.serial);
        }
        expect(begun && result.ok && waited, "frame ok");
        expect(rc.stats().passes == rc.layout().cascades + 1u, "every pass runs");
        if (measure) {
            side += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
        if (frame == kWarmup) {
            rebuilds = rc.stats().layoutRebuilds;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(rc.stats().layoutRebuilds == rebuilds && rebuilds == 1u, "no layout rebuild in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s kernels; %u rc.cascade + rc.gather; merge mode, sky and scene\n"
                    "  changing)\n"
                    "  beginFrame + rc.* pass callbacks: %llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (scene import + rc imports and passes): %llu\n",
                    kTotal - kWarmup, rc.kernelLanguage(), rc.layout().cascades, side, callbacks, build);
        expect(side == 0u, "the radiance-cascade passes make no steady-state heap allocations");
        expect(build == 0u, "graph build with the radiance-cascade passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok (%s)\n", kTotal,
                    rc.kernelLanguage());
    }
    rc.destroy();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "parity";
    std::string backend = "none";
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
        const int setupRc = setup(ctx, backend, true);
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
        const int setupRc = setup(counted, backend, false);
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
