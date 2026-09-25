// FUSE Relight RL-5.6 Vulkan gates (Lavapipe): the terrain bake on the GPU (TerrainBakerGpu, terrain_baker_gpu.hpp:
// "relight.terrain.clear" + one "relight.terrain.layer" pass per layer on RG v2) against the CPU reference
// (TerrainBaker) on the synthetic terrain of the golden gate.
//
//   parity       per kernel language: the GPU bake equals the CPU bake (max abs difference <= 1e-5; the RGBA8 hash equals
//                the CPU golden), after a content change (re-pack, re-bake) as well; an unchanged frame is not re-packed.
//   zero_alloc   32 steady-state re-bakes (transform change each frame): no operator-new call in prepare, the graph
//                build and the pass callbacks; no reallocation.
// Validation: every run is under the Khronos layer with sync validation (native) or the host-injected layer (Wine,
// --external-validation with a negative control); any message fails. Exit 77 = skip.
#include "terrain_test_scene.hpp"

#include <fuse/relight/terrain/terrain_baker.hpp>
#include <fuse/relight/terrain/terrain_baker_gpu.hpp>

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
using namespace terrain_test;


constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr usize kStagingBytes = 16u * 1024u * 1024u;

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
    instanceDesc.appName = "rl_terrain_vk";
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
    d.name = "rl_terrain.staging";
    if (!ctx.allocator->createBuffer(d, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    return 0;
}


struct Lang {
    tb::TerrainKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[] = {{tb::TerrainKernelLanguage::Slang, "slang"}, {tb::TerrainKernelLanguage::Glsl, "glsl"}};

bool initGpu(Context& ctx, tb::TerrainBakerGpu& g, tb::TerrainKernelLanguage language) {
    tb::TerrainBakerGpuDesc d;
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.language = language;
    return g.init(d);
}

bool runBake(Context& ctx, tb::TerrainBakerGpu& g, rg::Graph& graph, Terrain& t, bool* repacked,
             unsigned long long* allocs = nullptr) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    t_allocations = 0;
    t_count = allocs != nullptr;
    graph.reset();
    rg::BufferRef out{};
    bool ok = g.prepare(t.desc, t.layers, repacked) && g.addPasses(graph, &out);
    if (ok) {
        rg::PassBuilder rb = graph.addPass("readback.host", nullptr, nullptr);
        rb.use(out, rg::Access::HostRead);
    }
    t_count = false;
    if (allocs != nullptr) {
        *allocs += t_allocations;
    }
    if (!ok) {
        std::fprintf(stderr, "  bake setup failed: %s\n", g.reason());
        return false;
    }
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    ctx.bindless.collectRetired(ctx.serial);
    g.collectRetired();
    return result.ok && waited;
}

double compare(const tb::TerrainBakerGpu& g, const std::vector<float>& cpu, std::size_t texels, u64* hash) {
    const float* p = g.mappedOutput();
    std::vector<float> gpu(p, p + texels * 4u);
    double worst = 0.0;
    for (std::size_t i = 0; i < texels * 4u; ++i) {
        worst = std::max(worst, double(std::fabs(gpu[i] - cpu[i])));
    }
    *hash = hashRgba8(gpu, texels);
    return worst;
}

int runParity(Context& ctx, const std::string& language) {
    u32 languages = 0;
    for (const Lang& lang : kLangs) {
        if (language != "all" && language != lang.name) {
            continue;
        }
        tb::TerrainBakerGpu g;
        if (!initGpu(ctx, g, lang.language)) {
            if (std::strstr(g.reason(), "no terrain kernel") != nullptr) {
                std::printf("%s: kernel not built, skipped\n", lang.name);
                continue;
            }
            std::printf("SKIP: GPU terrain bake unavailable: %s\n", g.reason());
            return kSkip;
        }
        ++languages;
        rg::Graph graph;
        Terrain t;
        makeTerrain(t);
        tb::TerrainBaker cpu;
        std::vector<float> ref;
        const std::size_t texels = std::size_t(t.desc.width) * t.desc.height;
        bool repacked = false;
        check(runBake(ctx, g, graph, t, &repacked), "parity: GPU bake");
        cpu.bake(t.desc, t.layers, ref);
        u64 hash = 0;
        const double e0 = compare(g, ref, texels, &hash);
        bool repackedAgain = true;
        check(runBake(ctx, g, graph, t, &repackedAgain), "parity: GPU bake (unchanged)");
        t.layers[3].uvTransform[2] = 0.3f;
        t.texRoad[2] = 0.75f;
        bool repackedChanged = false;
        check(runBake(ctx, g, graph, t, &repackedChanged), "parity: GPU bake (changed)");
        cpu.bake(t.desc, t.layers, ref);
        u64 hash1 = 0;
        const double e1 = compare(g, ref, texels, &hash1);
        std::printf("%s kernels: 96 x 64 bake of 5 layers: max |GPU - CPU| %.2e (RGBA8 hash 0x%016llx, golden %s); "
                    "unchanged frame re-packed %d; after a change %.2e (re-packed %d)\n",
                    g.kernelLanguage(), e0, (unsigned long long)hash, hash == kGolden ? "equal" : "DIFFERS",
                    repackedAgain ? 1 : 0, e1, repackedChanged ? 1 : 0);
        check(repacked && e0 <= 1e-5 && hash == kGolden, "parity: GPU bake == CPU bake (golden)");
        check(!repackedAgain, "parity: unchanged inputs are not re-packed");
        check(repackedChanged && e1 <= 1e-5, "parity: GPU bake == CPU bake after a change");
        ctx.executor->waitIdle();
        g.destroy();
    }
    if (languages == 0u) {
        std::printf("SKIP: no terrain kernel built\n");
        return kSkip;
    }
    return 0;
}

thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "relight.terrain.", 16) == 0) {
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
    tb::TerrainBakerGpu g;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initGpu(ctx, g, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: GPU terrain bake unavailable: %s\n", g.reason());
        return kSkip;
    }
    Terrain t;
    makeTerrain(t);
    rg::Graph graph;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    unsigned long long setup = 0, callbacks = 0;
    u32 reallocations = 0;
    for (u32 frame = 0; frame < 40u; ++frame) {
        const bool measure = countAllocations && frame >= 8u;
        t.layers[3].uvTransform[2] = 0.01f * float(frame);
        bool repacked = false;
        unsigned long long a = 0;
        t_allocations = 0;
        check(runBake(ctx, g, graph, t, &repacked, measure ? &a : nullptr), "zero_alloc: bake");
        const unsigned long long cb = t_allocations;
        if (measure) {
            setup += a;
            callbacks += cb;
        }
        if (frame == 8u) {
            reallocations = g.stats().reallocations;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    check(g.stats().reallocations == reallocations, "zero_alloc: no reallocation in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (%s kernels, 32 steady-state re-bakes): prepare + graph build %llu operator-new calls, "
                    "relight.terrain.* callbacks %llu\n",
                    g.kernelLanguage(), setup, callbacks);
        check(setup == 0u && callbacks == 0u, "steady-state terrain bakes make no heap allocation");
    } else {
        std::printf("zero_alloc frames under validation: 40 bakes ok (%s)\n", g.kernelLanguage());
    }
    ctx.executor->waitIdle();
    g.destroy();
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
