// Asset plan W0.7 layered materials: Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation;
// every validation message fails the run). CPU gates: test_rp_material_layers_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): materials.eval and / or materials.balls, then read-back copies. Both kernel
// languages built (Slang, GLSL) run the parity checks.
//
//   --mode parity      4096 random surfaces over the 11 test materials (every feature: UV / triplanar, stochastic,
//                      macro, 1-3 height / wet layers with every mask source, detail near / fading / far): the GPU
//                      evaluation == the CPU reference (ml_evaluate) per surface within tolerance (1e-4); Slang == GLSL
//   --mode stochastic  a 256 x 256 grid over 8 x 8 texture repeats evaluated on the GPU with plain and stochastic
//                      tiling: autocorrelation at one period (repetition) drops from ~1 to < 0.25 while the mean
//                      and the standard deviation stay within 3 % / 15 %
//   --mode triplanar   a path across a rounded cube edge evaluated on the GPU: the triplanar material has no step
//                      across the bevel larger than on the faces; the per-face UV unwrap does (the metric sees seams)
//   --mode golden      the material-ball golden scene (8 .fusemat balls, 256 x 128): two renders bit-identical, GPU ==
//                      CPU reference, == the committed golden PNG (tests/material_layers/material_balls.png); writes
//                      <build>/material_balls.png; FUSE_RP_ML_UPDATE_GOLDEN=1 rewrites the committed golden
//   --mode zero_alloc  64 steady-state frames (moving camera and sun; eval + balls + copy): 0 operator-new calls in
//                      beginFrame, the materials.* callbacks and the graph build
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include "test_rp_material_layers_common.hpp"

#include <fuse/renderer/material_layers/material_layers.hpp>
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
using namespace ml_test;
using fuse::u64;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr u32 kMaxSurfaces = 256u * 256u;

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
    Buffer surfaces{};
    Buffer results{};
    Buffer readback{};
    u8 surfacesQueue = rg::kNoQueue;
    u8 resultsQueue = rg::kNoQueue;
    u8 readbackQueue = rg::kNoQueue;
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        if (allocator != nullptr) {
            for (Buffer* b : {&surfaces, &results, &readback}) {
                if (b->handle != nullptr) {
                    allocator->destroyBuffer(*b);
                }
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

bool makeBuffer(Context& ctx, Buffer& b, u64 size, BufferUsage usage, MemoryUsage memory, const char* name) {
    BufferDesc d{};
    d.size = static_cast<usize>(size);
    d.usage = usage;
    d.memoryUsage = memory;
    d.name = name;
    return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr;
}

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
    instanceDesc.appName = "fuse_rp_material_layers";
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
    const MlCapabilities caps = queryMaterialLayerCapabilities(ctx.device.get());
    if (!caps.layers) {
        std::printf("SKIP: layered materials unsupported: %s\n", caps.reason);
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
    const auto storage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                  static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    if (!makeBuffer(ctx, ctx.surfaces, u64{kMaxSurfaces} * sizeof(MlSurface), storage, MemoryUsage::CpuToGpu,
                    "rp_ml.surfaces") ||
        !makeBuffer(ctx, ctx.results, u64{kMaxSurfaces} * sizeof(MlResult), storage, MemoryUsage::GpuToCpu,
                    "rp_ml.results") ||
        !makeBuffer(ctx, ctx.readback, u64{kGoldenWidth} * kGoldenHeight * 16u, BufferUsage::TransferDst,
                    MemoryUsage::GpuToCpu, "rp_ml.readback") ||
        ctx.surfaces.deviceAddress == 0u || ctx.results.deviceAddress == 0u) {
        std::fprintf(stderr, "FAIL: test buffers\n");
        return 1;
    }
    return 0;
}

struct Lang {
    MlKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[2] = {{MlKernelLanguage::Slang, "slang"}, {MlKernelLanguage::Glsl, "glsl"}};

MlFrameDesc sceneFrame() {
    MlFrameDesc f{};
    f.params = ml_ball_scene(kGoldenWidth, kGoldenHeight, kBallCount).params;
    return f;
}

bool initLayers(Context& ctx, MaterialLayers& layers, MlKernelLanguage language, const MlLibrary& lib) {
    MaterialLayersDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.language = language;
    return layers.init(d) && layers.setLibrary(lib);
}

/// One frame: optional eval of `count` surfaces (already in ctx.surfaces), optional balls + image copy.
bool runFrame(Context& ctx, MaterialLayers& layers, rg::Graph& graph, const MlFrameDesc& frame, u32 count, bool balls) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    if (!layers.beginFrame(ctx.serial, frame)) {
        return false;
    }
    graph.reset();
    const MaterialLayerRefs refs = layers.importInto(graph);
    if (count > 0u) {
        const rg::BufferRef in = graph.importBuffer(rg::ImportedBuffer{ctx.surfaces.handle, ctx.surfaces.desc.size,
                                                                       ctx.surfacesQueue, &ctx.surfacesQueue, "rp_ml.in"});
        const rg::BufferRef out = graph.importBuffer(rg::ImportedBuffer{ctx.results.handle, ctx.results.desc.size,
                                                                        ctx.resultsQueue, &ctx.resultsQueue, "rp_ml.out"});
        layers.addEval(graph, refs, in, ctx.surfaces.deviceAddress, out, ctx.results.deviceAddress, count);
    }
    if (balls) {
        layers.addBalls(graph, refs);
        const rg::BufferRef rb = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size,
                                                                       ctx.readbackQueue, &ctx.readbackQueue, "rp_ml.rb"});
        layers.addCopyImage(graph, refs, rb, 0u);
    }
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    layers.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    return result.ok && waited;
}

bool evaluate(Context& ctx, MaterialLayers& layers, const std::vector<MlSurface>& surfaces, std::vector<MlResult>& out) {
    if (surfaces.size() > kMaxSurfaces) {
        return false;
    }
    std::memcpy(ctx.surfaces.mapped, surfaces.data(), surfaces.size() * sizeof(MlSurface));
    rg::Graph graph;
    if (!runFrame(ctx, layers, graph, sceneFrame(), static_cast<u32>(surfaces.size()), false)) {
        return false;
    }
    out.resize(surfaces.size());
    std::memcpy(out.data(), ctx.results.mapped, out.size() * sizeof(MlResult));
    return true;
}

bool renderBalls(Context& ctx, MaterialLayers& layers, std::vector<MlF4>& image) {
    rg::Graph graph;
    if (!runFrame(ctx, layers, graph, sceneFrame(), 0u, true)) {
        return false;
    }
    image.resize(static_cast<usize>(kGoldenWidth) * kGoldenHeight);
    std::memcpy(image.data(), ctx.readback.mapped, image.size() * sizeof(MlF4));
    return true;
}

struct Err {
    f64 albedo = 0, normal = 0, scalar = 0;
    u32 over = 0; ///< surfaces with any component off by more than 1e-3
};

Err compareResults(const std::vector<MlResult>& a, const std::vector<MlResult>& b) {
    Err e{};
    for (usize i = 0; i < a.size(); ++i) {
        f64 worst = 0.0;
        for (u32 c = 0; c < 3u; ++c) {
            const f64 da = std::fabs(static_cast<f64>(a[i].albedo[c]) - b[i].albedo[c]);
            const f64 dn = std::fabs(static_cast<f64>(a[i].normal[c]) - b[i].normal[c]);
            e.albedo = std::fmax(e.albedo, da);
            e.normal = std::fmax(e.normal, dn);
            worst = std::fmax(worst, std::fmax(da, dn));
        }
        const f64 ds = std::fmax(std::fmax(std::fabs(static_cast<f64>(a[i].roughness) - b[i].roughness),
                                           std::fabs(static_cast<f64>(a[i].metallic) - b[i].metallic)),
                                 std::fmax(std::fabs(static_cast<f64>(a[i].ao) - b[i].ao),
                                           std::fabs(static_cast<f64>(a[i].height) - b[i].height)));
        e.scalar = std::fmax(e.scalar, ds);
        worst = std::fmax(worst, ds);
        e.over += worst > 1e-3 ? 1u : 0u;
    }
    return e;
}

// --- parity ------------------------------------------------------------------------------------------------------
int runParity(Context& ctx) {
    MlLibrary lib;
    if (!buildLibrary(FUSE_RP_ML_FIXTURE_DIR, lib)) {
        expect(false, "golden library");
        return 1;
    }
    const std::vector<MlSurface> surfaces = randomSurfaces(4096u, 17u);
    std::vector<MlResult> cpu(surfaces.size());
    for (usize i = 0; i < surfaces.size(); ++i) {
        cpu[i] = ml_evaluate(lib.view(), surfaces[i]);
    }
    std::vector<MlResult> gpu[2];
    u32 ran = 0;
    for (u32 l = 0; l < 2u; ++l) {
        MaterialLayers layers;
        if (!initLayers(ctx, layers, kLangs[l].language, lib)) {
            std::printf("parity: %s kernels not built, skipped\n", kLangs[l].name);
            continue;
        }
        if (!evaluate(ctx, layers, surfaces, gpu[l])) {
            expect(false, "eval frame");
            return 1;
        }
        const Err e = compareResults(gpu[l], cpu);
        std::printf("parity (%s): %zu surfaces, max |GPU - CPU| albedo %.3g normal %.3g scalars %.3g; %u surfaces > 1e-3\n",
                    kLangs[l].name, surfaces.size(), e.albedo, e.normal, e.scalar, e.over);
        expectLe(e.albedo, 1e-4, "GPU layered evaluation == CPU reference (albedo)");
        expectLe(e.normal, 1e-4, "GPU layered evaluation == CPU reference (normal)");
        expectLe(e.scalar, 1e-4, "GPU layered evaluation == CPU reference (roughness / metallic / AO / height)");
        expectLe(e.over, surfaces.size() / 200u, "at most 0.5 % of the surfaces differ by more than 1e-3");
        vkDeviceWaitIdle(ctx.vkDevice);
        layers.destroy();
        ++ran;
    }
    if (ran == 0u) {
        std::printf("SKIP: no kernel built\n");
        return kSkip;
    }
    if (ran == 2u) {
        const Err e = compareResults(gpu[0], gpu[1]);
        std::printf("parity: Slang vs GLSL max albedo %.3g normal %.3g scalars %.3g\n", e.albedo, e.normal, e.scalar);
        expectLe(std::fmax(e.albedo, std::fmax(e.normal, e.scalar)), 1e-4, "Slang == GLSL");
    }
    return 0;
}

// --- stochastic ---------------------------------------------------------------------------------------------------
int runStochastic(Context& ctx) {
    MlLibrary lib;
    MaterialLayers layers;
    if (!buildLibrary(FUSE_RP_ML_FIXTURE_DIR, lib) || !initLayers(ctx, layers, MlKernelLanguage::Auto, lib)) {
        std::printf("SKIP: no kernel built\n");
        return kSkip;
    }
    constexpr u32 kSize = 256u;
    constexpr u32 kRepeats = 8u;
    std::vector<MlResult> plain, stoch;
    if (!evaluate(ctx, layers, tilingGrid(kSize, kRepeats, kMatPlainTiling), plain) ||
        !evaluate(ctx, layers, tilingGrid(kSize, kRepeats, kMatStochasticTiling), stoch)) {
        expect(false, "eval frames");
        return 1;
    }
    const std::vector<f32> lp = luminance(plain), ls = luminance(stoch);
    const f64 acPlain = ml_autocorrelation(lp, kSize, kSize, kSize / kRepeats);
    const f64 acStoch = ml_autocorrelation(ls, kSize, kSize, kSize / kRepeats);
    f64 mp = 0, sp = 0, ms = 0, ss = 0;
    meanStd(lp, mp, sp);
    meanStd(ls, ms, ss);
    std::printf("stochastic (%s): autocorrelation at one period plain %.4f -> stochastic %.4f; mean %.4f -> %.4f, "
                "std %.4f -> %.4f\n",
                layers.kernelLanguage(), acPlain, acStoch, mp, ms, sp, ss);
    expect(acPlain > 0.99, "plain tiling repeats (autocorrelation ~ 1 at one period)");
    expectLe(acStoch, 0.25, "stochastic tiling removes the visible repetition");
    expectLe(std::fabs(ms - mp) / mp, 0.03, "stochastic tiling preserves the mean");
    expectLe(std::fabs(ss / sp - 1.0), 0.15, "stochastic tiling preserves the standard deviation");
    // Second-order check: the stochastic grid vs its CPU reference.
    std::vector<MlResult> cpu;
    for (const MlSurface& s : tilingGrid(kSize, kRepeats, kMatStochasticTiling)) {
        cpu.push_back(ml_evaluate(lib.view(), s));
    }
    const Err e = compareResults(stoch, cpu);
    expectLe(e.albedo, 1e-4, "stochastic grid GPU == CPU");
    vkDeviceWaitIdle(ctx.vkDevice);
    return 0;
}

// --- triplanar ----------------------------------------------------------------------------------------------------
int runTriplanar(Context& ctx) {
    MlLibrary lib;
    MaterialLayers layers;
    if (!buildLibrary(FUSE_RP_ML_FIXTURE_DIR, lib) || !initLayers(ctx, layers, MlKernelLanguage::Auto, lib)) {
        std::printf("SKIP: no kernel built\n");
        return kSkip;
    }
    const EdgePath tri = edgePath(kMatTriplanar, 0.002f);
    const EdgePath uv = edgePath(kMatPlainTiling, 0.002f);
    std::vector<MlResult> rt, ru;
    if (!evaluate(ctx, layers, tri.surfaces, rt) || !evaluate(ctx, layers, uv.surfaces, ru)) {
        expect(false, "eval frames");
        return 1;
    }
    f64 tIn = 0, tOut = 0, uIn = 0, uOut = 0;
    edgeSteps(rt, tri.bevelBegin, tri.bevelEnd, tIn, tOut);
    edgeSteps(ru, uv.bevelBegin, uv.bevelEnd, uIn, uOut);
    std::printf("triplanar (%s): largest albedo step across the bevel / on the faces: triplanar %.4f / %.4f, "
                "UV unwrap %.4f / %.4f\n",
                layers.kernelLanguage(), tIn, tOut, uIn, uOut);
    expectLe(tIn, 1.5 * tOut + 1e-3, "triplanar: continuous across the cube edge");
    expect(uIn > 2.5 * uOut, "the per-face UV unwrap shows the seam the triplanar path removes");
    vkDeviceWaitIdle(ctx.vkDevice);
    return 0;
}

// --- golden -------------------------------------------------------------------------------------------------------
int runGolden(Context& ctx) {
    MlLibrary lib;
    if (!buildLibrary(FUSE_RP_ML_FIXTURE_DIR, lib)) {
        expect(false, "golden library");
        return 1;
    }
    const MlBallScene scene = ml_ball_scene(kGoldenWidth, kGoldenHeight, kBallCount);
    std::vector<MlF4> cpuImage;
    ml_render_balls(scene.params, lib.view(), scene.balls.data(), cpuImage);
    std::vector<u8> cpuRgba;
    ml_to_srgb8(cpuImage, cpuRgba);
    std::vector<u8> primary;
    u32 ran = 0;
    for (u32 l = 0; l < 2u; ++l) {
        MaterialLayers layers;
        if (!initLayers(ctx, layers, kLangs[l].language, lib)) {
            continue;
        }
        std::vector<MlF4> a, b;
        if (!renderBalls(ctx, layers, a) || !renderBalls(ctx, layers, b)) {
            expect(false, "ball frames");
            return 1;
        }
        expect(std::memcmp(a.data(), b.data(), a.size() * sizeof(MlF4)) == 0, "two renders are bit-identical");
        std::vector<u8> rgba;
        ml_to_srgb8(a, rgba);
        const ImageDiff d = compareRgba(rgba, cpuRgba, 2u);
        std::printf("golden (%s): GPU vs CPU reference: %u pixels > 2 LSB, mean %.4f LSB, worst %u\n", kLangs[l].name,
                    d.over, d.mean, d.worst);
        expectLe(d.over, kGoldenWidth * kGoldenHeight / 500u, "GPU render == CPU reference (<= 0.2 % pixels > 2 LSB)");
        expectLe(d.mean, 0.25, "GPU render == CPU reference (mean)");
        u32 hit = 0;
        for (const MlF4& p : a) {
            hit += p.w > 0.5f ? 1u : 0u;
        }
        expect(hit > kGoldenWidth * kGoldenHeight / 5u, "the balls cover the frame");
        if (primary.empty()) {
            primary = rgba;
        }
        vkDeviceWaitIdle(ctx.vkDevice);
        layers.destroy();
        ++ran;
    }
    if (ran == 0u) {
        std::printf("SKIP: no kernel built\n");
        return kSkip;
    }
    const std::vector<u8> png = ml_png_encode(kGoldenWidth, kGoldenHeight, primary);
    expect(ml_png_encode(kGoldenWidth, kGoldenHeight, primary) == png, "PNG bytes deterministic");
    const std::string outPath = std::string(FUSE_RP_ML_OUTPUT_DIR) + "/" + kGoldenPng;
    expect(writeBytes(outPath, png), "write the render");
    std::printf("golden: wrote %s (%zu bytes)\n", outPath.c_str(), png.size());
    const std::string goldenPath = std::string(FUSE_RP_ML_FIXTURE_DIR) + "/" + kGoldenPng;
    const char* update = std::getenv("FUSE_RP_ML_UPDATE_GOLDEN");
    if (update != nullptr && std::strcmp(update, "1") == 0) {
        expect(writeBytes(goldenPath, png), "update the committed golden");
        std::printf("golden: UPDATED %s\n", goldenPath.c_str());
        return 0;
    }
    std::vector<u8> goldenBytes, golden;
    u32 w = 0, h = 0;
    if (!readBytes(goldenPath, goldenBytes) || !ml_png_decode(goldenBytes, w, h, golden) || w != kGoldenWidth ||
        h != kGoldenHeight) {
        std::fprintf(stderr, "FAIL: golden %s missing or unreadable\n", goldenPath.c_str());
        ++g_failures;
        return 0;
    }
    const ImageDiff d = compareRgba(primary, golden, 2u);
    std::printf("golden: render vs committed golden: %u pixels > 2 LSB, mean %.4f LSB, worst %u (bit-exact: %s)\n",
                d.over, d.mean, d.worst, primary == golden ? "yes" : "no");
    expectLe(d.over, kGoldenWidth * kGoldenHeight / 500u, "render == golden (<= 0.2 % pixels > 2 LSB)");
    expectLe(d.mean, 0.25, "render == golden (mean)");
    return 0;
}

// --- zero_alloc ---------------------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "materials.", 10) == 0) {
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
    MlLibrary lib;
    MaterialLayers layers;
    if (!buildLibrary(FUSE_RP_ML_FIXTURE_DIR, lib) || !initLayers(ctx, layers, MlKernelLanguage::Auto, lib)) {
        std::printf("SKIP: no kernel built\n");
        return kSkip;
    }
    const std::vector<MlSurface> surfaces = randomSurfaces(1024u, 3u);
    std::memcpy(ctx.surfaces.mapped, surfaces.data(), surfaces.size() * sizeof(MlSurface));
    constexpr u32 kWarmup = 8;
    constexpr u32 kTotal = 72;
    unsigned long long side = 0, callbacks = 0, build = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    rg::Graph graph;
    u32 rebuilds = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        MlFrameDesc f = sceneFrame();
        f.params.camPos[0] = 0.02f * static_cast<f32>(frame);
        f.params.sunDir[0] = 0.5f - 0.004f * static_cast<f32>(frame);
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        t_allocations = 0;
        t_count = measure;
        const bool begun = layers.beginFrame(ctx.serial, f);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        graph.reset();
        const MaterialLayerRefs refs = layers.importInto(graph);
        const rg::BufferRef in = graph.importBuffer(rg::ImportedBuffer{ctx.surfaces.handle, ctx.surfaces.desc.size,
                                                                       ctx.surfacesQueue, &ctx.surfacesQueue, "rp_ml.in"});
        const rg::BufferRef out = graph.importBuffer(rg::ImportedBuffer{ctx.results.handle, ctx.results.desc.size,
                                                                        ctx.resultsQueue, &ctx.resultsQueue, "rp_ml.out"});
        const rg::BufferRef rb = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size,
                                                                       ctx.readbackQueue, &ctx.readbackQueue, "rp_ml.rb"});
        layers.addEval(graph, refs, in, ctx.surfaces.deviceAddress, out, ctx.results.deviceAddress,
                       static_cast<u32>(surfaces.size()));
        layers.addBalls(graph, refs);
        layers.addCopyImage(graph, refs, rb, 0u);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle();
        layers.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(begun && result.ok && waited, "frame ok");
        if (measure) {
            side += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
        }
        if (frame == kWarmup) {
            rebuilds = layers.stats().imageRebuilds + layers.stats().libraryUploads;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(layers.stats().imageRebuilds + layers.stats().libraryUploads == rebuilds && rebuilds == 2u,
           "no buffer rebuild in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s kernels; eval of 1024 surfaces + balls 256 x 128 + copy; camera and\n"
                    "  sun moving)\n"
                    "  beginFrame + materials.* pass callbacks: %llu operator-new calls (callbacks %llu)\n"
                    "  whole graph build (imports and passes): %llu\n",
                    kTotal - kWarmup, layers.kernelLanguage(), side, callbacks, build);
        expect(side == 0u, "the material passes make no steady-state heap allocations");
        expect(build == 0u, "graph build with the material passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok (%s)\n", kTotal,
                    layers.kernelLanguage());
    }
    vkDeviceWaitIdle(ctx.vkDevice);
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
        } else if (mode == "stochastic") {
            rc = runStochastic(ctx);
        } else if (mode == "triplanar") {
            rc = runTriplanar(ctx);
        } else if (mode == "golden") {
            rc = runGolden(ctx);
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
