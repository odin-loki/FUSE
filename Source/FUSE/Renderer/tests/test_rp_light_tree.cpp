// WP-7.1 light tree Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation; every
// validation message fails the run). CPU gates: test_rp_light_tree_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): LightTreeGpu imports its ring buffer (this frame's slot, written by
// the host in beginFrame), the test imports a host-visible query buffer and a result buffer, "light_tree.sample"
// answers the queries, and a host-read pass declares the read-back. Both kernel languages built (Slang, GLSL)
// run every check.
//
//   --mode sample     frames over the tree's life: build (mixed kinds + directional), 3 refits with moving lights,
//                     rebuild with the 10.7k-triangle scene (ring reallocation), rebuild with fewer lights,
//                     unchanged frames (no slot copy once every ring slot is current). 4096 queries per frame
//                     (surface and volume points, evalLight = a random light or the sampled one): every GPU result
//                     field == LightTree::sample / LightTree::pmf on the CPU, bit for bit.
//   --mode zero_alloc 64 steady-state frames (tree refitted every frame, 4096 queries): 0 operator-new calls in
//                     LightTreeGpu::beginFrame, the light_tree.* pass callbacks and the whole graph build
//                     (validated run first; validation off for the count).
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include <fuse/renderer/light_tree/light_tree.hpp>
#include <fuse/renderer/light_tree/light_tree_gpu.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include "test_rp_light_tree_scene.hpp"

#include <algorithm>
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
using namespace fuse::renderer::light_tree;
using fuse::f32;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using lt_test::Rng;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr u32 kQueries = 4096u;

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
    Buffer queries{};
    Buffer results{};
    u8 queriesQueue = rg::kNoQueue;
    u8 resultsQueue = rg::kNoQueue;
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        if (allocator != nullptr) {
            for (Buffer* b : {&queries, &results}) {
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
    instanceDesc.appName = "fuse_rp_light_tree";
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
    const LightTreeCapabilities caps = queryLightTreeCapabilities(ctx.device.get());
    if (!caps.gpu) {
        std::printf("SKIP: light tree unsupported: %s\n", caps.reason);
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
    auto buffer = [&](Buffer& b, u64 size, MemoryUsage memory, const char* name) {
        BufferDesc d{};
        d.size = static_cast<usize>(size);
        d.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                           static_cast<u32>(BufferUsage::ShaderDeviceAddress));
        d.memoryUsage = memory;
        d.name = name;
        return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr && b.deviceAddress != 0u;
    };
    if (!buffer(ctx.queries, kQueries * sizeof(LightTreeQuery), MemoryUsage::CpuToGpu, "rp_lt.queries") ||
        !buffer(ctx.results, kQueries * sizeof(LightTreeSample), MemoryUsage::GpuToCpu, "rp_lt.results")) {
        std::fprintf(stderr, "FAIL: query / result buffers\n");
        return 1;
    }
    return 0;
}

struct Lang {
    LightTreeKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[] = {{LightTreeKernelLanguage::Slang, "slang"}, {LightTreeKernelLanguage::Glsl, "glsl"}};

bool initGpu(Context& ctx, LightTreeGpu& gpu, LightTreeKernelLanguage language) {
    LightTreeGpuDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.language = language;
    d.framesInFlight = 3;
    d.initialLights = 1024;
    return gpu.init(d);
}

/// Imports the ring, queries and results; adds the sample pass and the host read-back declaration.
void buildGraph(Context& ctx, LightTreeGpu& gpu, rg::Graph& graph, u32 count) {
    graph.reset();
    const LightTreeGraphRefs refs = gpu.importInto(graph);
    const rg::BufferRef queries = graph.importBuffer(
        rg::ImportedBuffer{ctx.queries.handle, ctx.queries.desc.size, ctx.queriesQueue, &ctx.queriesQueue, "rp_lt.queries"});
    const rg::BufferRef results = graph.importBuffer(
        rg::ImportedBuffer{ctx.results.handle, ctx.results.desc.size, ctx.resultsQueue, &ctx.resultsQueue, "rp_lt.results"});
    const bool added = gpu.addSamplePass(graph, refs, queries, 0u, ctx.queries.deviceAddress, results, 0u,
                                         ctx.results.deviceAddress, count);
    expect(added, "sample pass added");
    graph.addPass("readback.host", nullptr, nullptr).use(results, rg::Access::HostRead);
}

void writeQueries(Context& ctx, const LightTree& tree, Rng& rng, f32 extent) {
    LightTreeQuery* q = static_cast<LightTreeQuery*>(ctx.queries.mapped);
    const u32 lights = static_cast<u32>(tree.emitters().size());
    for (u32 i = 0; i < kQueries; ++i) {
        LightTreeQuery& x = q[i];
        x = LightTreeQuery{};
        x.position[0] = rng.range(-extent, extent);
        x.position[1] = rng.range(-extent, extent);
        x.position[2] = rng.range(-extent, extent);
        if ((i % 2u) == 0u) {
            f32 n[3];
            lt_test::unitVector(rng, n);
            std::memcpy(x.normal, n, sizeof(n));
        }
        x.u0 = rng.uniform();
        x.u1 = rng.uniform();
        x.u2 = rng.uniform();
        // Edge values of the random numbers now and then.
        if ((i % 97u) == 0u) {
            x.u0 = 0.f;
            x.u1 = 0.5f;
            x.u2 = 0.5f;
        } else if ((i % 89u) == 0u) {
            x.u0 = kLtOneMinusEpsilon;
            x.u1 = kLtOneMinusEpsilon;
            x.u2 = 0.f;
        }
        x.evalLight = (i % 3u) == 0u ? kLtInvalid : (lights > 0u ? static_cast<u32>(rng.next() % (lights + 2u)) : 0u);
    }
}

bool sameBits(const LightTreeSample& a, const LightTreeSample& b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

/// Runs one frame and compares every result with the CPU sampler. Returns the number of mismatches.
u32 runFrame(Context& ctx, LightTreeGpu& gpu, rg::Graph& graph, const LightTree& tree, Rng& rng, f32 extent,
             u32& kinds) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    writeQueries(ctx, tree, rng, extent);
    if (!gpu.beginFrame(ctx.serial, tree)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        return kQueries;
    }
    buildGraph(ctx, gpu, graph, kQueries);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (!result.ok || !waited) {
        std::fprintf(stderr, "  execute failed\n");
        return kQueries;
    }
    const LightTreeQuery* q = static_cast<const LightTreeQuery*>(ctx.queries.mapped);
    const LightTreeSample* r = static_cast<const LightTreeSample*>(ctx.results.mapped);
    u32 mismatches = 0;
    for (u32 i = 0; i < kQueries; ++i) {
        const f32 p[3] = {q[i].position[0], q[i].position[1], q[i].position[2]};
        const f32 n[3] = {q[i].normal[0], q[i].normal[1], q[i].normal[2]};
        LightTreeSample cpu = tree.sample(p, n, q[i].u0, q[i].u1, q[i].u2);
        cpu.evalPmf = q[i].evalLight != kLtInvalid ? tree.pmf(p, n, q[i].evalLight) : 0.f;
        if (!sameBits(cpu, r[i])) {
            if (mismatches < 5u) {
                std::fprintf(stderr,
                             "  query %u: GPU light %u pmf %a pdf %a kind %u pos (%a %a %a) eval %a vs CPU light %u pmf %a "
                             "pdf %a kind %u pos (%a %a %a) eval %a\n",
                             i, r[i].light, r[i].pmf, r[i].pdfArea, r[i].kind, r[i].position[0], r[i].position[1],
                             r[i].position[2], r[i].evalPmf, cpu.light, cpu.pmf, cpu.pdfArea, cpu.kind, cpu.position[0],
                             cpu.position[1], cpu.position[2], cpu.evalPmf);
            }
            ++mismatches;
        }
        if (cpu.kind < 32u) {
            kinds |= 1u << cpu.kind;
        }
    }
    return mismatches;
}

int runSample(Context& ctx) {
    u32 languages = 0;
    for (const Lang& lang : kLangs) {
        LightTreeGpu gpu;
        if (!initGpu(ctx, gpu, lang.language)) {
            std::printf("%s: kernel not built, skipped\n", lang.name);
            continue;
        }
        ++languages;
        std::vector<LightTreeLight> mixed = lt_test::mixedScene(1);
        const std::vector<LightTreeLight> triangles = lt_test::triangleScene(2, 71);
        LightTree tree;
        tree.build(mixed);
        rg::Graph graph;
        Rng rng(100);
        Rng move(200);
        u32 kinds = 0;
        u32 total = 0;
        struct Step {
            const char* what;
            u32 mismatches;
            u32 uploads;
        };
        std::vector<Step> steps;
        auto frame = [&](const char* what, f32 extent) {
            const u32 before = gpu.stats().uploads;
            const u32 m = runFrame(ctx, gpu, graph, tree, rng, extent, kinds);
            total += m;
            steps.push_back({what, m, gpu.stats().uploads - before});
        };
        frame("build (mixed)", 12.f);
        for (u32 k = 0; k < 3u; ++k) {
            for (LightTreeLight& l : mixed) {
                const f32 d = move.range(-0.3f, 0.3f);
                l.position[1] += d;
                if (l.kind == kLtKindTriangle) {
                    l.u[1] += d;
                    l.v[1] += d;
                }
                l.intensity *= move.range(0.9f, 1.1f);
            }
            expect(tree.refit(mixed), "refit");
            frame("refit (moved)", 12.f);
        }
        const u32 reallocBefore = gpu.stats().reallocations;
        tree.build(triangles);
        frame("rebuild (10.7k triangles)", 8.f);
        expect(gpu.stats().reallocations > reallocBefore, "the larger tree reallocates the ring");
        std::vector<LightTreeLight> fewer(mixed.begin(), mixed.begin() + 300);
        tree.build(fewer);
        frame("rebuild (300 lights)", 12.f);
        for (u32 k = 0; k < 6u; ++k) {
            frame("unchanged", 12.f);
        }
        std::printf("%s kernel, %u queries per frame:\n", gpu.kernelLanguage(), kQueries);
        for (const Step& s : steps) {
            std::printf("  %-26s %u mismatches, %u slot copies\n", s.what, s.mismatches, s.uploads);
        }
        // Frames 6..8 of the unchanged run: every slot is current, no copy.
        u32 lateCopies = 0;
        for (usize i = steps.size() - 3u; i < steps.size(); ++i) {
            lateCopies += steps[i].uploads;
        }
        expect(lateCopies == 0u, "no slot copy once every ring slot holds the current tree");
        const u32 wanted = (1u << kLtKindPoint) | (1u << kLtKindSpot) | (1u << kLtKindRect) | (1u << kLtKindDisk) |
                           (1u << kLtKindTriangle) | (1u << kLtKindDirectional);
        expect((kinds & wanted) == wanted, "every light kind sampled");
        expect(total == 0u, "GPU samples == CPU sampler, bit for bit");
        std::printf("  %s: %u mismatches over %zu frames, %u ring reallocations\n", gpu.kernelLanguage(), total,
                    steps.size(), gpu.stats().reallocations);
        ctx.executor->waitIdle();
        gpu.destroy();
    }
    if (languages == 0u) {
        std::printf("SKIP: no light tree kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "light_tree.", 11) == 0) {
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
    LightTreeGpu gpu;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initGpu(ctx, gpu, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: no light tree kernel built\n");
        return kSkip;
    }
    std::vector<LightTreeLight> lights = lt_test::mixedScene(3);
    LightTree tree;
    tree.build(lights);
    rg::Graph graph;
    Rng rng(300);
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long side = 0, callbacks = 0, build = 0, refit = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    u32 reallocations = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        for (LightTreeLight& l : lights) {
            l.position[0] += 0.001f;
            if (l.kind == kLtKindTriangle) {
                l.u[0] += 0.001f;
                l.v[0] += 0.001f;
            }
        }
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        writeQueries(ctx, tree, rng, 12.f);
        t_allocations = 0;
        t_count = measure;
        const bool refitted = tree.refit(lights);
        t_count = false;
        const unsigned long long refitAllocs = t_allocations;
        t_allocations = 0;
        t_count = measure;
        const bool begun = gpu.beginFrame(ctx.serial, tree);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, gpu, graph, kQueries);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle();
        gpu.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(refitted && begun && result.ok && waited, "frame ok");
        if (measure) {
            side += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
            refit += refitAllocs;
        }
        if (frame == kWarmup) {
            reallocations = gpu.stats().reallocations;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(gpu.stats().reallocations == reallocations, "no ring reallocation in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s kernel; %zu lights refitted every frame, %u queries)\n"
                    "  LightTree::refit: %llu operator-new calls\n"
                    "  LightTreeGpu::beginFrame (slot copy) + light_tree.* pass callbacks: %llu (callbacks %llu)\n"
                    "  whole graph build (imports + sample pass + host read-back): %llu\n",
                    kTotal - kWarmup, gpu.kernelLanguage(), lights.size(), kQueries, refit, side, callbacks, build);
        expect(refit == 0u, "refit makes no steady-state heap allocation");
        expect(side == 0u, "the light tree upload and pass make no steady-state heap allocations");
        expect(build == 0u, "graph build with the light tree pass makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: %u frames ok (%s)\n", kTotal,
                    gpu.kernelLanguage());
    }
    ctx.executor->waitIdle();
    gpu.destroy();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "sample";
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
        if (mode == "sample") {
            rc = runSample(ctx);
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
