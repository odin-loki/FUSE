// FUSE Relight RL-5.4 research track, GPU inference (Lavapipe; PE under Wine in the MinGW tree): the neural radiance
// cache trained on the CPU (NeuralRadianceCache: WP-9.1 NeuralTrainer on the hash grid's training records of the
// Cornell scene), evaluated on the GPU by the WP-9.1 portable kernel (NeuralGpu, "neural.infer") at every training
// record of the last frame, per kernel language:
//   parity   GPU radiance == NeuralNet::infer (the CPU reference) within 1e-4 relative on every query (the hash-grid
//            encoding and ReLU layers are the same IEEE operations; the portable kernel differs only in sin / cos / exp,
//            unused here), and once every ring slot holds the weights they are not uploaded again.
// Validation: under the Khronos layer with sync validation (native) or the host-injected layer (Wine,
// --external-validation with a negative control); any message fails. Exit 77 = skip.
#include "pt_test_scenes.hpp"

#include <relight_nrc/neural_radiance_cache.hpp>

#include <fuse/relight/options/option.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/render/pathtrace/radiance_cache.hpp>
#include <fuse/renderer/neural/neural_gpu.hpp>
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
constexpr u32 kW = 24u, kH = 24u;

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
    instanceDesc.appName = "rl_nrc_vk";
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
    d.name = "rl_nrc.staging";
    if (!ctx.allocator->createBuffer(d, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    return 0;
}

namespace nn = fuse::renderer::neural;
namespace rs = fuse::relight::research;

struct IoBuffers {
    Buffer inputs{};
    Buffer outputs{};
    u8 inputsQueue = rg::kNoQueue;
    u8 outputsQueue = rg::kNoQueue;
};

bool trainNet(rs::NeuralRadianceCache& nrc, std::vector<float>& queries) {
    pt::PtScene s = cornell(false);
    pt::PtCompiledScene c;
    std::string error;
    if (!c.compile(s, {}, &error)) {
        check(false, "compile: " + error);
        return false;
    }
    pt::RadianceCacheSettings cs;
    cs.capacity = 1u << 14;
    cs.cellPixels = 2.f;
    cs.trainStride = 2;
    pt::RadianceCacheCpu rc;
    rc.configure(cs);
    rs::NrcSettings ns = rs::NrcSettings::defaults();
    for (int a = 0; a < 3; ++a) {
        ns.boundsMin[a] = -2.6f;
        ns.boundsMax[a] = 2.6f;
    }
    if (!nrc.init(ns)) {
        return false;
    }
    pt::PtSettings st;
    st.maxBounces = 4;
    for (u32 f = 0; f < 24u; ++f) {
        if (!rc.frame(c, st, kW, kH, 3u, f) ||
            !nrc.trainFrame(rc.records().data(), u32(rc.records().size() / pt::kRcRecordWords))) {
            return false;
        }
    }
    queries.clear();
    const std::vector<pt::Word>& rec = rc.records();
    for (std::size_t i = 0; i + 2u < rec.size(); i += pt::kRcRecordWords) {
        if (!(rec[i].w > 0.5f)) {
            continue;
        }
        const float p[3] = {rec[i].x, rec[i].y, rec[i].z};
        const float n[3] = {rec[i + 1u].x, rec[i + 1u].y, rec[i + 1u].z};
        float x[3];
        nrc.encodeInput(p, n, x);
        queries.insert(queries.end(), x, x + 3);
    }
    return !queries.empty();
}

int runInference(Context& ctx, const std::string& language) {
    rs::NeuralRadianceCache nrc;
    std::vector<float> queries;
    if (!trainNet(nrc, queries)) {
        check(false, "CPU training of the neural cache");
        return 1;
    }
    const u32 count = static_cast<u32>(queries.size() / 3u);
    IoBuffers io;
    BufferDesc d{};
    d.size = queries.size() * sizeof(float);
    d.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                       static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    d.memoryUsage = MemoryUsage::CpuToGpu;
    d.name = "rl_nrc.inputs";
    if (!ctx.allocator->createBuffer(d, io.inputs) || io.inputs.mapped == nullptr) {
        check(false, "inputs buffer");
        return 1;
    }
    std::memcpy(io.inputs.mapped, queries.data(), queries.size() * sizeof(float));
    d.memoryUsage = MemoryUsage::GpuToCpu;
    d.name = "rl_nrc.outputs";
    if (!ctx.allocator->createBuffer(d, io.outputs) || io.outputs.mapped == nullptr) {
        ctx.allocator->destroyBuffer(io.inputs);
        check(false, "outputs buffer");
        return 1;
    }
    struct Lang {
        nn::NeuralKernelLanguage language;
        const char* name;
    };
    const Lang langs[] = {{nn::NeuralKernelLanguage::Slang, "slang"}, {nn::NeuralKernelLanguage::Glsl, "glsl"}};
    u32 ran = 0;
    for (const Lang& lang : langs) {
        if (language != "all" && language != lang.name) {
            continue;
        }
        nn::NeuralGpu gpu;
        nn::NeuralGpuDesc gd{};
        gd.device = ctx.device.get();
        gd.allocator = ctx.allocator.get();
        gd.bindless = &ctx.bindless;
        gd.language = lang.language;
        gd.framesInFlight = 3;
        if (!gpu.init(gd)) {
            std::printf("%s: neural kernel unavailable, skipped\n", lang.name);
            continue;
        }
        ++ran;
        rg::Graph graph;
        double worst = 0.0;
        u32 bad = 0;
        u32 uploadsBefore = 0;
        for (u32 frame = 0; frame < 6u; ++frame) {
            ++ctx.serial;
            ctx.bindless.setFrameSerial(ctx.serial);
            if (frame == 3u) { // every ring slot (3 frames in flight) holds this version
                uploadsBefore = gpu.stats().uploads;
            }
            bool ok = gpu.beginFrame(ctx.serial, nrc.net());
            graph.reset();
            const nn::NeuralGraphRefs refs = gpu.importInto(graph);
            const rg::BufferRef in = graph.importBuffer(rg::ImportedBuffer{io.inputs.handle, io.inputs.desc.size,
                                                                           io.inputsQueue, &io.inputsQueue,
                                                                           "rl_nrc.inputs"});
            const rg::BufferRef out = graph.importBuffer(rg::ImportedBuffer{io.outputs.handle, io.outputs.desc.size,
                                                                            io.outputsQueue, &io.outputsQueue,
                                                                            "rl_nrc.outputs"});
            ok = ok && gpu.addInferPass(graph, refs, in, 0u, io.inputs.deviceAddress, out, 0u, io.outputs.deviceAddress,
                                        count);
            graph.addPass("readback.host", nullptr, nullptr).use(out, rg::Access::HostRead);
            const rg::ExecuteResult result = ctx.executor->execute(graph);
            ok = ok && result.ok && ctx.executor->waitIdle();
            gpu.collectRetired(ctx.serial);
            ctx.bindless.collectRetired(ctx.serial);
            check(ok, "inference frame");
            const float* y = static_cast<const float*>(io.outputs.mapped);
            nn::NeuralScratch scratch;
            for (u32 i = 0; i < count && ok; ++i) {
                float ref[3];
                nrc.net().infer(&queries[std::size_t(i) * 3u], ref, scratch);
                for (u32 k = 0; k < 3u; ++k) {
                    const double e = std::fabs(double(y[i * 3u + k]) - double(ref[k])) / (1.0 + std::fabs(double(ref[k])));
                    worst = std::max(worst, e);
                    bad += e > 1e-4 ? 1u : 0u;
                }
            }
        }
        const u32 steadyUploads = gpu.stats().uploads - uploadsBefore;
        std::printf("%s: %u queries x 6 frames on the GPU (%u parameters): worst relative error %.2e, %u outside 1e-4; "
                    "steady-state weight uploads %u\n",
                    gpu.kernelLanguage(), count, nrc.net().paramCount(), worst, bad, steadyUploads);
        check(bad == 0u, "GPU inference == CPU NeuralNet::infer within 1e-4");
        check(steadyUploads == 0u, "unchanged weights are not uploaded again");
        ctx.executor->waitIdle();
        gpu.destroy();
    }
    ctx.allocator->destroyBuffer(io.inputs);
    ctx.allocator->destroyBuffer(io.outputs);
    return ran == 0u ? kSkip : 0;
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
        rc = runInference(ctx, language);
    }
    if (rc == kSkip) {
        return kSkip;
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
