// FUSE Relight RL-4.4 Vulkan gates (Lavapipe): the Relight light set on the GPU (RelightLightsGpu, light_set_gpu.hpp).
//
// Every frame is one render graph (WP-0.3): RelightLightsGpu imports its ring (this frame's slot: the raw D3DLIGHT9
// records and the host part of the light table) and the WP-7.1 tree slot, "relight.lights.convert" converts the game
// lights ON THE GPU into the table, "relight.lights.sample" answers light-set queries (tree selection + the light's
// own sample + the density / radiance of an evaluation light), and a host-read pass declares the read-back.
//
//   convert     GPU conversion == the CPU conversion (the same single-source kernel, light_d3d_convert): kind and
//               flags exact, every float within 1e-4 relative (the device's cos / division rounding against RL-1.5's
//               double stableCos / discriminant); the ff_lit lights == the RL-1.5 capture reference (<= 1e-5);
//               the host part of the table bit-identical.
//   sample      GPU light-set sampler == the CPU light_set_sample kernel run on the GPU-converted table: selected
//               light and pmf bit for bit (the tree sampler is IEEE-exact), sampled position / direction / radiance /
//               pdf and the evaluation light's radiance / density within 1e-3 (transcendentals).
//   frames      build (game + authored + emissive + fallback) -> refits with moving lights -> a larger set (ring
//               reallocation) -> a smaller set.
//   zero_alloc  64 steady-state frames: no operator-new call in RelightLightSet (beginFrame .. build),
//               RelightLightsGpu::beginFrame, the graph build and the relight.lights.* / light_tree.* pass callbacks
//               (counted without an in-process validation layer: it allocates through operator new).
//
// Options: --language slang | glsl | all (default all built), --no-validation (Wine: no layer can be enumerated),
// --external-validation (the host loader injects the layer: a negative control proves it is live, then every
// message fails the run). Exit 77 = skip (stub build, no ICD / layer / capability / kernel).
#include <fuse/relight/render/lights/light_set.hpp>
#include <fuse/relight/render/lights/light_set_gpu.hpp>

#include "light_kernels.hpp"

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/scene/lights/legacy_light.hpp>
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

/// No allocation on the passing path (called inside the zero_alloc measurement).
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
namespace rl = fuse::relight::render::lights;
namespace lk = fuse::relight::lightk;
namespace lt = fuse::renderer::light_tree;
namespace tap = fuse::relight::tap;
namespace scene = fuse::relight::scene;
using lk::float3;
using lk::float4;
using lk::RlLight;

/// rtx.sceneScale belongs to scene/instances (linked in the runtime); the light set reads it by name every frame,
/// and a missing option costs a lookup string, so the test declares it like the runtime does.
struct BorrowedStandIns {
    FUSE_RELIGHT_OPTION("rtx", float, sceneScale, 1.f, "Test stand-in for the scene package's option.");
};

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr u32 kQueries = 4096u;

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

struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed * 0x9E3779B97F4A7C15ull + 1u) {}
    u32 next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(s >> 33);
    }
    float uniform() { return static_cast<float>(next() >> 7) * (1.f / 16777216.f); }
    float range(float a, float b) { return a + (b - a) * uniform(); }
};

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

/// Negative control for a layer the process cannot enumerate (Wine: the host loader injects it): an invalid sampler
/// (mipLodBias above maxSamplerLodBias, VUID-VkSamplerCreateInfo-mipLodBias-01069) must produce a message.
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
            std::printf("SKIP: %s not installed (the gate needs synchronization validation; --no-validation runs "
                        "without)\n",
                        kValidationLayer);
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
    instanceDesc.appName = "rl_lights_vk";
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
            std::printf("SKIP: no validation layer reached this process (the loader injected none; negative control "
                        "silent)\n");
            return kSkip;
        }
        std::printf("external validation layer live (negative control: %u message(s), not counted)\n", control);
    }
    const lt::LightTreeCapabilities caps = lt::queryLightTreeCapabilities(ctx.device.get());
    if (!caps.gpu) {
        std::printf("SKIP: light tree GPU path unsupported: %s\n", caps.reason);
        return kSkip;
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
    auto buffer = [&](Buffer& b, u64 size, MemoryUsage memory, const char* name) {
        BufferDesc d{};
        d.size = static_cast<usize>(size);
        d.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                           static_cast<u32>(BufferUsage::ShaderDeviceAddress));
        d.memoryUsage = memory;
        d.name = name;
        return ctx.allocator->createBuffer(d, b) && b.mapped != nullptr && b.deviceAddress != 0u;
    };
    if (!buffer(ctx.queries, u64(kQueries) * lk::kRlQueryWords * sizeof(float4), MemoryUsage::CpuToGpu,
                "rl_lights.queries") ||
        !buffer(ctx.results, u64(kQueries) * lk::kRlResultWords * sizeof(float4), MemoryUsage::GpuToCpu,
                "rl_lights.results")) {
        std::fprintf(stderr, "FAIL: query / result buffers\n");
        return 1;
    }
    return 0;
}

bool initGpu(Context& ctx, rl::RelightLightsGpu& gpu, rl::LightKernelLanguage language) {
    rl::RelightLightsGpuDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.language = language;
    d.framesInFlight = 3;
    d.initialLights = 256;
    return gpu.init(d);
}

// --- scene --------------------------------------------------------------------------------------------------------

tap::Light d3dLight(u32 index, u32 type) {
    tap::Light l;
    l.index = index;
    l.enabled = true;
    l.type = type;
    return l;
}

tap::Vec3 appNormalize(float x, float y, float z) {
    const float l = std::sqrt(x * x + y * y + z * z);
    return {x / l, y / l, z / l};
}

/// ff_lit's lights (Tests/relight/apps/scenes/ff_lit.cpp), as in test_lights_cpu.cpp.
std::vector<tap::Light> ffLitLights() {
    std::vector<tap::Light> v;
    tap::Light dir = d3dLight(0, scene::d3dlight::DIRECTIONAL);
    dir.diffuse = {0.8f, 0.8f, 0.75f, 1.f};
    dir.direction = appNormalize(0.4f, -0.6f, 0.7f);
    v.push_back(dir);
    tap::Light pt = d3dLight(1, scene::d3dlight::POINT);
    pt.diffuse = {1.0f, 0.3f, 0.2f, 1.f};
    pt.position = {-1.6f, 1.0f, -1.0f};
    pt.range = 6.0f;
    pt.attenuation0 = 0.2f;
    pt.attenuation1 = 0.3f;
    pt.attenuation2 = 0.05f;
    v.push_back(pt);
    tap::Light spot = d3dLight(2, scene::d3dlight::SPOT);
    spot.diffuse = {0.2f, 0.4f, 1.0f, 1.f};
    spot.position = {1.5f, 2.0f, -1.5f};
    spot.direction = appNormalize(-0.3f, -1.0f, 0.6f);
    spot.range = 10.0f;
    spot.attenuation0 = 1.0f;
    spot.theta = 0.35f;
    spot.phi = 0.9f;
    spot.falloff = 1.0f;
    v.push_back(spot);
    tap::Light off = d3dLight(3, scene::d3dlight::POINT);
    off.enabled = false;
    off.diffuse = {0.0f, 1.0f, 0.0f, 1.f};
    off.position = {0.0f, 0.0f, -2.0f};
    off.range = 100.0f;
    off.attenuation0 = 1.0f;
    v.push_back(off);
    return v;
}

tap::Light randomD3dLight(Rng& rng, u32 index) {
    tap::Light l = d3dLight(index, 1u + rng.next() % 3u);
    const float scale = rng.range(0.05f, 3.f);
    l.diffuse = {rng.range(0.f, 1.f) * scale, rng.range(0.f, 1.f) * scale, rng.range(0.f, 1.f) * scale, 1.f};
    l.position = {rng.range(-40.f, 40.f), rng.range(-10.f, 20.f), rng.range(-40.f, 40.f)};
    l.direction = {rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), rng.range(-1.f, 1.f)};
    l.range = rng.range(0.5f, 200.f);
    switch (rng.next() % 4u) {
    case 0:
        l.attenuation0 = rng.range(0.f, 2.f);
        break;
    case 1:
        l.attenuation0 = rng.range(0.f, 1.f);
        l.attenuation1 = rng.range(0.001f, 1.f);
        break;
    case 2:
        l.attenuation0 = rng.range(0.f, 1.f);
        l.attenuation1 = rng.range(0.f, 0.5f);
        l.attenuation2 = rng.range(0.0001f, 0.3f);
        break;
    default:
        l.attenuation0 = rng.range(300.f, 1000.f);
        break;
    }
    l.phi = rng.range(0.05f, 3.1f);
    l.theta = rng.range(0.f, l.phi);
    l.falloff = rng.next() % 3u == 0u ? 1.f : rng.range(0.f, 4.f);
    return l;
}

struct Scene {
    std::vector<tap::Light> game;
    std::vector<RlLight> authored;
    std::vector<float> positions;
    std::vector<u32> indices;
    bool fallback = false;
};

Scene makeScene(u32 randomGame, u32 grid, u64 seed) {
    Scene s;
    Rng rng(seed);
    s.game = ffLitLights();
    for (u32 i = 0; i < randomGame; ++i) {
        s.game.push_back(randomD3dLight(rng, 8u + i));
    }
    s.authored.push_back(rl::makeSphereLight(float3(0.5f, 3.f, -0.3f), 0.7f, float3(2.f, 1.5f, 1.f)));
    RlLight shaped = rl::makeSphereLight(float3(-1.f, 4.f, 0.5f), 0.4f, float3(5.f, 5.f, 4.f));
    rl::setShaping(shaped, float3(0.2f, -1.f, 0.1f), 0.9f, 0.2f, 2.f);
    s.authored.push_back(shaped);
    RlLight rect = rl::makeRectLight(float3(0.f, 5.f, 0.f), float3(0.8f, 0.f, 0.f), float3(0.f, 0.f, 0.5f),
                                     float3(3.f, 3.f, 3.f));
    rl::setShaping(rect, float3(0.f, -1.f, 0.f), 1.2f, 0.1f, 1.f);
    s.authored.push_back(rect);
    s.authored.push_back(rl::makeDiskLight(float3(3.f, 2.f, 1.f), float3(0.5f, 0.f, 0.f), float3(0.f, -0.3f, 0.4f),
                                           float3(2.f, 2.f, 2.f), true));
    s.authored.push_back(rl::makeCylinderLight(float3(-3.f, 1.2f, 0.4f), float3(0.9f, 0.1f, 0.f), 0.25f,
                                               float3(3.f, 2.f, 1.f)));
    s.authored.push_back(rl::makeDistantLight(float3(0.3f, -1.f, 0.2f), 0.12f, float3(2.f, 2.f, 1.5f)));
    // Emissive grid (lit side down) at y = 6.
    for (u32 z = 0; z <= grid; ++z) {
        for (u32 x = 0; x <= grid; ++x) {
            s.positions.push_back(-4.f + 8.f * float(x) / float(grid));
            s.positions.push_back(6.f);
            s.positions.push_back(-4.f + 8.f * float(z) / float(grid));
        }
    }
    for (u32 z = 0; z < grid; ++z) {
        for (u32 x = 0; x < grid; ++x) {
            const u32 a = z * (grid + 1u) + x;
            const u32 b = a + 1u;
            const u32 c = a + grid + 1u;
            const u32 d = c + 1u;
            s.indices.insert(s.indices.end(), {a, b, d, a, d, c});
        }
    }
    s.fallback = true;
    return s;
}

void fillSet(rl::RelightLightSet& set, const Scene& s) {
    set.beginFrame();
    set.addGameLights(s.game.data(), static_cast<u32>(s.game.size()));
    for (usize i = 0; i < s.authored.size(); ++i) {
        set.addLight(s.authored[i], 100u + i);
    }
    rl::EmissiveMesh mesh;
    mesh.positions = s.positions.data();
    mesh.vertexCount = static_cast<u32>(s.positions.size() / 3u);
    mesh.indices = s.indices.data();
    mesh.indexCount = static_cast<u32>(s.indices.size());
    mesh.index32 = true;
    mesh.radiance = float3(0.5f, 0.4f, 0.3f);
    mesh.key = 7;
    set.addEmissiveTriangles(mesh);
    if (s.fallback) {
        set.addFallbackLight(rl::FallbackLight{2u, float3(0.3f, 0.3f, 0.3f), float3(0.1f, -1.f, 0.2f), 0.5f});
    }
}

void moveScene(Scene& s, float d) {
    for (tap::Light& l : s.game) {
        l.position.y += d;
    }
    for (RlLight& l : s.authored) {
        l.position.x += d;
    }
    for (usize i = 1; i < s.positions.size(); i += 3) {
        s.positions[i] += d * 0.1f;
    }
}

void writeQueries(Context& ctx, const rl::RelightLightSet& set, Rng& rng) {
    float4* q = static_cast<float4*>(ctx.queries.mapped);
    const u32 lights = set.lightCount();
    for (u32 i = 0; i < kQueries; ++i) {
        float4* x = q + i * lk::kRlQueryWords;
        const float3 p(rng.range(-8.f, 8.f), rng.range(-2.f, 4.f), rng.range(-8.f, 8.f));
        float3 n(0.f);
        if ((i % 3u) != 0u) {
            n = lk::normalize(float3(rng.range(-0.5f, 0.5f), 1.f, rng.range(-0.5f, 0.5f)));
        }
        x[0] = float4(p.x, p.y, p.z, rng.uniform());
        x[1] = float4(n.x, n.y, n.z, rng.uniform());
        u32 eval = kQueries; // none
        if ((i % 4u) != 0u && lights > 0u) {
            eval = rng.next() % (lights + 1u); // lights: out of range, ignored
        }
        float3 wi = lk::normalize(float3(rng.range(-1.f, 1.f), rng.range(-1.f, 1.f), rng.range(-1.f, 1.f)));
        if (eval < lights && (i % 2u) == 0u) {
            // Towards the evaluation light: a direction that usually hits it.
            const lk::RlLightSample s = lk::rlLightSample(set.light(eval), p, rng.uniform(), rng.uniform());
            if ((s.flags & lk::kRlSampleValid) != 0u) {
                wi = s.wi;
            }
        }
        x[2] = float4(wi.x, wi.y, wi.z, rng.uniform());
        x[3] = float4(eval < kQueries ? float(eval) : -1.f, 0.f, 0.f, 0.f);
    }
}

/// Imports the buffers; adds the convert pass, the sample pass and the host read-back declaration.
void buildGraph(Context& ctx, rl::RelightLightsGpu& gpu, rg::Graph& graph, u32 count) {
    graph.reset();
    const rl::RelightLightsGraphRefs refs = gpu.importInto(graph);
    const rg::BufferRef queries = graph.importBuffer(rg::ImportedBuffer{
        ctx.queries.handle, ctx.queries.desc.size, ctx.queriesQueue, &ctx.queriesQueue, "rl_lights.queries"});
    const rg::BufferRef results = graph.importBuffer(rg::ImportedBuffer{
        ctx.results.handle, ctx.results.desc.size, ctx.resultsQueue, &ctx.resultsQueue, "rl_lights.results"});
    check(gpu.addConvertPass(graph, refs), "convert pass added");
    check(gpu.addSamplePass(graph, refs, queries, 0u, ctx.queries.deviceAddress, results, 0u,
                            ctx.results.deviceAddress, count),
          "sample pass added");
    graph.addPass("readback.host", nullptr, nullptr)
        .use(refs.ring, rg::Access::HostRead, refs.tableRange)
        .use(results, rg::Access::HostRead);
}

bool runGraph(Context& ctx, rl::RelightLightsGpu& gpu, rg::Graph& graph) {
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    return result.ok && waited;
}

float relErr(float a, float b) {
    if (a == b) {
        return 0.f;
    }
    if (!std::isfinite(a) || !std::isfinite(b)) {
        return 1e30f;
    }
    return std::fabs(a - b) / std::max(std::fabs(b), 1e-3f);
}

struct FrameReport {
    u32 lights = 0;
    u32 game = 0;
    float convertErr = 0.f;
    u32 convertBad = 0;
    u32 hostBad = 0;
    u32 selectBad = 0;
    float sampleErr = 0.f;
    u32 sampleBad = 0;
    u32 evalHits = 0;
};

/// One frame: set -> GPU -> compare.
FrameReport runFrame(Context& ctx, rl::RelightLightsGpu& gpu, rg::Graph& graph, rl::RelightLightSet& set,
                     const Scene& scene, Rng& rng) {
    FrameReport rep;
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    fillSet(set, scene);
    check(set.build(), "set build");
    writeQueries(ctx, set, rng);
    if (!gpu.beginFrame(ctx.serial, set)) {
        check(false, "RelightLightsGpu::beginFrame");
        return rep;
    }
    buildGraph(ctx, gpu, graph, kQueries);
    if (!runGraph(ctx, gpu, graph)) {
        check(false, "graph execution");
        return rep;
    }
    rep.lights = set.lightCount();
    rep.game = set.gameLightCount();
    // Convert: the game part of the GPU table vs the CPU conversion.
    const float4* gpuTable = static_cast<const float4*>(gpu.mappedTable());
    std::vector<float4> table(gpuTable, gpuTable + std::size_t(rep.lights) * lk::kRlLightWords);
    for (u32 i = 0; i < rep.game; ++i) {
        const RlLight g = lk::rlLightUnpack(table.data() + i * lk::kRlLightWords);
        const RlLight c = set.light(i);
        const float* gf = reinterpret_cast<const float*>(table.data() + i * lk::kRlLightWords);
        const float* cf = reinterpret_cast<const float*>(set.table().data() + i * lk::kRlLightWords);
        float worst = 0.f;
        for (u32 k = 0; k < lk::kRlLightWords * 4u; ++k) {
            worst = std::max(worst, relErr(gf[k], cf[k]));
        }
        rep.convertErr = std::max(rep.convertErr, worst);
        if (g.kind != c.kind || g.flags != c.flags || worst > 1e-4f) {
            if (rep.convertBad < 3u) {
                std::fprintf(stderr, "  game light %u: kind %u/%u flags %u/%u worst relative error %g\n", i, g.kind,
                             c.kind, g.flags, c.flags, double(worst));
            }
            ++rep.convertBad;
        }
    }
    const usize gameWords = std::size_t(rep.game) * lk::kRlLightWords;
    if (std::memcmp(table.data() + gameWords, set.table().data() + gameWords,
                    (table.size() - gameWords) * sizeof(float4)) != 0) {
        ++rep.hostBad;
    }
    // Sample: CPU light_set_sample on the GPU-converted table.
    lk::SetSampleParams prm{};
    prm.tree = set.tree().view();
    prm.table = kernel::Span<const float4>{table.data(), static_cast<u32>(table.size())};
    prm.lightCount = rep.lights;
    const float4* q = static_cast<const float4*>(ctx.queries.mapped);
    const float4* r = static_cast<const float4*>(ctx.results.mapped);
    for (u32 i = 0; i < kQueries; ++i) {
        float4 cpu[lk::kRlResultWords];
        lk::rlSetSampleOne(prm, q + i * lk::kRlQueryWords, cpu);
        const float4* gpuR = r + i * lk::kRlResultWords;
        // Selection (IEEE-exact tree sampler): light index, pmf, the evaluation light's pmf factor.
        if (std::memcmp(&gpuR[0].w, &cpu[0].w, 4) != 0 || std::memcmp(&gpuR[2].w, &cpu[2].w, 4) != 0) {
            if (rep.selectBad < 3u) {
                std::fprintf(stderr, "  query %u: GPU light %g pmf %a vs CPU light %g pmf %a\n", i, double(gpuR[0].w),
                             double(gpuR[2].w), double(cpu[0].w), double(cpu[2].w));
            }
            ++rep.selectBad;
            continue;
        }
        const float* gf = reinterpret_cast<const float*>(gpuR);
        const float* cf = reinterpret_cast<const float*>(cpu);
        float worst = 0.f;
        for (u32 k = 0; k < lk::kRlResultWords * 4u; ++k) {
            worst = std::max(worst, relErr(gf[k], cf[k]));
        }
        if (cpu[3].w > 0.f) {
            ++rep.evalHits;
        }
        rep.sampleErr = std::max(rep.sampleErr, worst);
        if (worst > 1e-3f) {
            if (rep.sampleBad < 3u) {
                std::fprintf(stderr, "  query %u (light %g): worst relative error %g\n", i, double(cpu[0].w),
                             double(worst));
                for (u32 k = 0; k < lk::kRlResultWords; ++k) {
                    std::fprintf(stderr, "    r%u GPU (%g %g %g %g) CPU (%g %g %g %g)\n", k, double(gpuR[k].x),
                                 double(gpuR[k].y), double(gpuR[k].z), double(gpuR[k].w), double(cpu[k].x),
                                 double(cpu[k].y), double(cpu[k].z), double(cpu[k].w));
                }
            }
            ++rep.sampleBad;
        }
    }
    return rep;
}

struct Lang {
    rl::LightKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[] = {{rl::LightKernelLanguage::Slang, "slang"}, {rl::LightKernelLanguage::Glsl, "glsl"}};

int runParity(Context& ctx, const std::string& language) {
    u32 languages = 0;
    for (const Lang& lang : kLangs) {
        if (language != "all" && language != lang.name) {
            continue;
        }
        rl::RelightLightsGpu gpu;
        if (!initGpu(ctx, gpu, lang.language)) {
            std::printf("%s: kernels not built, skipped\n", lang.name);
            continue;
        }
        ++languages;
        rl::RelightLightSet set;
        rg::Graph graph;
        Rng rng(11);
        Scene scene = makeScene(120, 8, 1);
        struct Step {
            const char* what;
            FrameReport rep;
        };
        std::vector<Step> steps;
        steps.push_back({"build", runFrame(ctx, gpu, graph, set, scene, rng)});
        for (u32 k = 0; k < 3u; ++k) {
            moveScene(scene, 0.25f);
            steps.push_back({"refit (moved)", runFrame(ctx, gpu, graph, set, scene, rng)});
        }
        check(set.stats().refits >= 3u, "moving lights refit the tree");
        const u32 reallocBefore = gpu.stats().reallocations;
        Scene big = makeScene(1500, 24, 2);
        steps.push_back({"rebuild (large)", runFrame(ctx, gpu, graph, set, big, rng)});
        check(gpu.stats().reallocations > reallocBefore, "the larger set reallocates the ring");
        Scene small = makeScene(10, 2, 3);
        small.fallback = false;
        steps.push_back({"rebuild (small)", runFrame(ctx, gpu, graph, set, small, rng)});
        std::printf("%s kernels, %u queries per frame:\n", gpu.kernelLanguage(), kQueries);
        for (const Step& s : steps) {
            const FrameReport& r = s.rep;
            std::printf("  %-16s %5u lights (%4u game): convert max rel err %.2e (%u bad), host part %s, selection %u "
                        "mismatches, sample max rel err %.2e (%u bad, %u eval hits)\n",
                        s.what, r.lights, r.game, double(r.convertErr), r.convertBad, r.hostBad == 0u ? "==" : "DIFFERS",
                        r.selectBad, double(r.sampleErr), r.sampleBad, r.evalHits);
            check(r.lights > 0u && r.convertBad == 0u, "GPU D3D light conversion == CPU conversion (1e-4)");
            check(r.hostBad == 0u, "host part of the GPU table == the CPU table");
            check(r.selectBad == 0u, "GPU light selection / pmf == CPU bit for bit");
            check(r.sampleBad == 0u, "GPU light samples == CPU light samples (1e-3)");
            check(r.evalHits > 0u, "evaluation queries hit lights");
        }
        // ff_lit on the GPU == the RL-1.5 capture reference.
        Scene ff;
        ff.game = ffLitLights();
        ff.fallback = false;
        runFrame(ctx, gpu, graph, set, ff, rng);
        const float4* t = static_cast<const float4*>(gpu.mappedTable());
        const float expected[3][3] = {{0.800000011920929f, 0.800000011920929f, 0.75f},
                                      {0.12710805237293243f, 0.03813241794705391f, 0.025421610102057457f},
                                      {0.00397887360304594f, 0.00795774720609188f, 0.019894367083907127f}};
        for (u32 i = 0; i < 3u; ++i) {
            const RlLight g = lk::rlLightUnpack(t + i * lk::kRlLightWords);
            for (u32 c = 0; c < 3u; ++c) {
                check(relErr(g.radiance[static_cast<int>(c)], expected[i][c]) <= 1e-5f,
                      "GPU ff_lit light " + std::to_string(i) + " radiance == RL-1.5 capture reference");
            }
        }
        const RlLight spot = lk::rlLightUnpack(t + 2u * lk::kRlLightWords);
        check(relErr(spot.cosCone, 0.9004471302032471f) <= 1e-5f && relErr(spot.softness, 0.08427941799163818f) <= 1e-4f &&
                  spot.focus == 1.f && spot.radius == 4.f,
              "GPU ff_lit spot shaping == RL-1.5 capture reference");
        std::printf("  ff_lit on the GPU == RL-1.5 capture reference (radiance <= 1e-5, spot shaping)\n");
        std::printf("  %s: %u ring reallocations, %llu bytes uploaded\n", gpu.kernelLanguage(), gpu.stats().reallocations,
                    static_cast<unsigned long long>(gpu.stats().uploadBytes));
        ctx.executor->waitIdle();
        gpu.destroy();
    }
    if (languages == 0u) {
        std::printf("SKIP: no Relight light kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- zero_alloc -----------------------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && (std::strncmp(name, "relight.lights.", 15) == 0 || std::strncmp(name, "light_tree.", 11) == 0)) {
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
    rl::RelightLightsGpu gpu;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initGpu(ctx, gpu, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: no Relight light kernel built\n");
        return kSkip;
    }
    Scene scene = makeScene(200, 8, 4);
    rl::RelightLightSet set;
    set.reserve(1024, 256);
    rg::Graph graph;
    Rng rng(12);
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long setAllocs = 0, begin = 0, graphBuild = 0, callbacks = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    u32 reallocations = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        moveScene(scene, 0.01f);
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        t_allocations = 0;
        t_count = measure;
        fillSet(set, scene);
        const bool built = set.build();
        t_count = false;
        const unsigned long long a0 = t_allocations;
        writeQueries(ctx, set, rng);
        t_allocations = 0;
        t_count = measure;
        const bool begun = gpu.beginFrame(ctx.serial, set);
        t_count = false;
        const unsigned long long a1 = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, gpu, graph, kQueries);
        t_count = false;
        const unsigned long long a2 = t_allocations;
        t_allocations = 0;
        const bool ran = runGraph(ctx, gpu, graph);
        const unsigned long long a3 = t_allocations;
        check(built && begun && ran, "frame ok");
        if (measure) {
            setAllocs += a0;
            begin += a1;
            graphBuild += a2;
            callbacks += a3;
        }
        if (frame == kWarmup) {
            reallocations = gpu.stats().reallocations;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    check(gpu.stats().reallocations == reallocations, "no ring reallocation in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (%s kernels, %u lights, %u steady-state frames, %u queries):\n"
                    "  RelightLightSet frame (beginFrame .. build): %llu operator-new calls\n"
                    "  RelightLightsGpu::beginFrame (ring + tree slot): %llu\n"
                    "  graph build (imports + convert + sample + read-back): %llu\n"
                    "  relight.lights.* / light_tree.* pass callbacks: %llu\n",
                    gpu.kernelLanguage(), set.lightCount(), kTotal - kWarmup, kQueries, setAllocs, begin, graphBuild,
                    callbacks);
        check(setAllocs == 0u && begin == 0u && graphBuild == 0u && callbacks == 0u,
              "steady-state light frames make no heap allocation");
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
            rc = runZeroAlloc(ctx, validation == Validation::External || validation == Validation::Off);
        }
    }
    if (rc == kSkip) {
        return kSkip;
    }
    if (rc == 0 && validation == Validation::Layer) {
        // The in-process layer allocates through operator new: count in a context without it.
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
