// WP-9.1 neural Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation; every validation
// message fails the run). CPU gates: test_rp_neural_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): NeuralGpu imports its ring buffer (this frame's slot, written by the
// host in beginFrame), the test imports a host-visible input buffer and an output buffer, "neural.infer" runs the
// portable kernel, and a host-read pass declares the read-back. Both kernel languages built (Slang, GLSL) run
// every check.
//
//   --mode infer      the configuration matrix (identity / frequency / hash grid 2D + 3D, every activation, fp32 and
//                     fp16 parameters), 4096 samples each: GPU == NeuralNet::infer within 1e-5 (abs + rel); the
//                     configurations without sin / cos / exp must match bit for bit. Then 8 frames of online
//                     training (the CPU trainer updates the net between frames; each changed version is uploaded,
//                     the GPU follows it), then unchanged frames (no slot copy once every ring slot is current).
//                     The cooperative-matrix kernel is not dispatched (Lavapipe has no VK_KHR_cooperative_matrix).
//   --mode zero_alloc 64 steady-state frames: 0 operator-new calls in NeuralGpu::beginFrame, the neural.* pass
//                     callbacks and the whole graph build (validated run first; validation off for the count).
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, capability missing, no kernel built).
#include <fuse/renderer/neural/neural_gpu.hpp>
#include <fuse/renderer/neural/neural_mlp.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include "test_rp_neural_common.hpp"

#include <algorithm>
#include <bit>
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
using namespace fuse::renderer::neural;
using fuse::f32;
using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::u8;
using fuse::usize;
using nn_test::Rng;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr u32 kSamples = 4096u;

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
    Buffer inputs{};
    Buffer outputs{};
    u8 inputsQueue = rg::kNoQueue;
    u8 outputsQueue = rg::kNoQueue;
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        if (allocator != nullptr) {
            for (Buffer* b : {&inputs, &outputs}) {
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
    instanceDesc.appName = "fuse_rp_neural";
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
    const NeuralCapabilities caps = queryNeuralCapabilities(ctx.device.get());
    if (!caps.gpu) {
        std::printf("SKIP: neural inference unsupported: %s\n", caps.reason);
        return kSkip;
    }
    std::printf("cooperative matrix: device %s, kernel %s\n", ctx.device->info().caps.cooperativeMatrix ? "yes" : "no",
                caps.coopMatrixKernelBuilt ? "built" : "not built");
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
    if (!buffer(ctx.inputs, kSamples * kNnMaxRawInputs * sizeof(f32), MemoryUsage::CpuToGpu, "rp_nn.inputs") ||
        !buffer(ctx.outputs, kSamples * kNnMaxWidth * sizeof(f32), MemoryUsage::GpuToCpu, "rp_nn.outputs")) {
        std::fprintf(stderr, "FAIL: input / output buffers\n");
        return 1;
    }
    return 0;
}

struct Lang {
    NeuralKernelLanguage language;
    const char* name;
};
constexpr Lang kLangs[] = {{NeuralKernelLanguage::Slang, "slang"}, {NeuralKernelLanguage::Glsl, "glsl"}};

bool initGpu(Context& ctx, NeuralGpu& gpu, NeuralKernelLanguage language) {
    NeuralGpuDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.language = language;
    d.framesInFlight = 3;
    d.initialSlotBytes = 16u * 1024u; // small: the larger configurations reallocate the ring
    return gpu.init(d);
}

void buildGraph(Context& ctx, NeuralGpu& gpu, rg::Graph& graph, u32 count) {
    graph.reset();
    const NeuralGraphRefs refs = gpu.importInto(graph);
    const rg::BufferRef inputs = graph.importBuffer(
        rg::ImportedBuffer{ctx.inputs.handle, ctx.inputs.desc.size, ctx.inputsQueue, &ctx.inputsQueue, "rp_nn.inputs"});
    const rg::BufferRef outputs = graph.importBuffer(rg::ImportedBuffer{ctx.outputs.handle, ctx.outputs.desc.size,
                                                                        ctx.outputsQueue, &ctx.outputsQueue,
                                                                        "rp_nn.outputs"});
    const bool added = gpu.addInferPass(graph, refs, inputs, 0u, ctx.inputs.deviceAddress, outputs, 0u,
                                        ctx.outputs.deviceAddress, count);
    expect(added, "infer pass added");
    graph.addPass("readback.host", nullptr, nullptr).use(outputs, rg::Access::HostRead);
}

bool usesTranscendentals(const NeuralNetConfig& c) {
    return c.encoding == NeuralEncoding::Frequency || c.hiddenActivation == NeuralActivation::Sigmoid ||
           c.outputActivation == NeuralActivation::Sigmoid;
}

struct Compare {
    u32 mismatches = 0; ///< outside tolerance
    u32 inexact = 0;    ///< not bit-identical
    f64 maxAbs = 0;
};

/// Runs one frame on the inputs already in ctx.inputs and compares with the CPU reference.
Compare runFrame(Context& ctx, NeuralGpu& gpu, rg::Graph& graph, const NeuralNet& net, u32 count) {
    Compare c{};
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    if (!gpu.beginFrame(ctx.serial, net)) {
        std::fprintf(stderr, "  beginFrame failed\n");
        c.mismatches = count;
        return c;
    }
    buildGraph(ctx, gpu, graph, count);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle();
    gpu.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
    if (!result.ok || !waited) {
        std::fprintf(stderr, "  execute failed\n");
        c.mismatches = count;
        return c;
    }
    const f32* x = static_cast<const f32*>(ctx.inputs.mapped);
    const f32* y = static_cast<const f32*>(ctx.outputs.mapped);
    const u32 in = net.header().inputDims;
    const u32 out = net.header().outputDims;
    NeuralScratch scratch;
    f32 ref[kNnMaxWidth];
    for (u32 s = 0; s < count; ++s) {
        net.infer(x + static_cast<usize>(s) * in, ref, scratch);
        for (u32 o = 0; o < out; ++o) {
            const f32 g = y[static_cast<usize>(s) * out + o];
            const f64 d = std::fabs(static_cast<f64>(g) - ref[o]);
            c.maxAbs = std::max(c.maxAbs, std::isnan(d) ? 1e30 : d);
            if (std::bit_cast<u32>(g) != std::bit_cast<u32>(ref[o])) {
                ++c.inexact;
            }
            if (!(d <= 1e-5 + 1e-5 * std::fabs(static_cast<f64>(ref[o])))) {
                if (c.mismatches < 3u) {
                    std::fprintf(stderr, "  sample %u out %u: GPU %a CPU %a\n", s, o, static_cast<f64>(g),
                                 static_cast<f64>(ref[o]));
                }
                ++c.mismatches;
            }
        }
    }
    return c;
}

void writeInputs(Context& ctx, const NeuralNetConfig& c, Rng& rng, u32 count) {
    std::vector<f32> x;
    nn_test::randomInputs(rng, c, count, x);
    std::memcpy(ctx.inputs.mapped, x.data(), x.size() * sizeof(f32));
}

int runInfer(Context& ctx) {
    u32 languages = 0;
    for (const Lang& lang : kLangs) {
        NeuralGpu gpu;
        if (!initGpu(ctx, gpu, lang.language)) {
            std::printf("%s: kernel not built, skipped\n", lang.name);
            continue;
        }
        ++languages;
        expect(!gpu.coopMatrixReady(), "cooperative-matrix pipeline not created (not allowed / not supported)");
        rg::Graph graph;
        Rng rng(500);
        u32 configs = 0;
        for (const nn_test::NamedConfig& nc : nn_test::configMatrix()) {
            NeuralNet net;
            expect(net.init(nc.config, 90u + configs), "init");
            nn_test::perturb(net, 91u + configs);
            writeInputs(ctx, nc.config, rng, kSamples);
            const Compare c = runFrame(ctx, gpu, graph, net, kSamples);
            const bool exactWanted = !usesTranscendentals(nc.config);
            std::printf("  %s %-36s max |d| %.3g, %u outside tolerance, %u not bit-exact%s\n", gpu.kernelLanguage(),
                        nc.name, c.maxAbs, c.mismatches, c.inexact, exactWanted ? " (must be exact)" : "");
            expect(c.mismatches == 0u, "GPU inference == CPU reference within tolerance");
            if (exactWanted) {
                expect(c.inexact == 0u, "GPU == CPU bit for bit without transcendentals");
            }
            ++configs;
        }
        // Online training: the CPU trainer updates the net, the GPU follows every version.
        NeuralNet net;
        net.init(nn_test::imageFitConfig(NeuralPrecision::F16), 95);
        NeuralTrainer trainer;
        trainer.init(net, NeuralAdamDesc{}, 256);
        std::vector<f32> tx(512), tt(768);
        u32 trainMismatch = 0;
        for (u32 frame = 0; frame < 8u; ++frame) {
            for (u32 i = 0; i < 256u; ++i) {
                tx[i * 2u] = rng.uniform();
                tx[i * 2u + 1u] = rng.uniform();
                nn_test::targetImage(tx[i * 2u], tx[i * 2u + 1u], &tt[i * 3u]);
            }
            trainer.step(net, tx.data(), tt.data(), 256);
            writeInputs(ctx, net.config(), rng, kSamples);
            const Compare c = runFrame(ctx, gpu, graph, net, kSamples);
            trainMismatch += c.inexact;
        }
        std::printf("  %s online training, 8 frames: %u values not bit-exact\n", gpu.kernelLanguage(), trainMismatch);
        expect(trainMismatch == 0u, "GPU follows every trained version (bit for bit)");
        const u32 before = gpu.stats().uploads;
        for (u32 frame = 0; frame < 6u; ++frame) {
            runFrame(ctx, gpu, graph, net, 256);
        }
        // 3 slots: the first 3 unchanged frames may copy (older versions), the last 3 must not.
        const u32 copies = gpu.stats().uploads - before;
        std::printf("  %s unchanged frames: %u slot copies over 6 frames, %u ring reallocations\n",
                    gpu.kernelLanguage(), copies, gpu.stats().reallocations);
        expect(copies <= 3u, "no slot copy once every ring slot holds the current version");
        expect(gpu.stats().reallocations > 0u, "a larger network reallocates the ring");
        ctx.executor->waitIdle();
        gpu.destroy();
    }
    if (languages == 0u) {
        std::printf("SKIP: no neural kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- zero_alloc ------------------------------------------------------------------------------------
thread_local bool t_inPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "neural.", 7) == 0) {
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
    NeuralGpu gpu;
    bool ok = false;
    for (const Lang& lang : kLangs) {
        if (initGpu(ctx, gpu, lang.language)) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        std::printf("SKIP: no neural kernel built\n");
        return kSkip;
    }
    NeuralNet net;
    net.init(nn_test::imageFitConfig(NeuralPrecision::F16), 97);
    NeuralTrainer trainer;
    trainer.init(net, NeuralAdamDesc{}, 128);
    std::vector<f32> tx(256), tt(384);
    rg::Graph graph;
    Rng rng(98);
    writeInputs(ctx, net.config(), rng, kSamples);
    constexpr u32 kWarmup = 16;
    constexpr u32 kTotal = 80;
    unsigned long long side = 0, callbacks = 0, build = 0, train = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    u32 reallocations = 0;
    for (u32 frame = 0; frame < kTotal; ++frame) {
        const bool measure = countAllocations && frame >= kWarmup;
        for (u32 i = 0; i < 128u; ++i) {
            tx[i * 2u] = rng.uniform();
            tx[i * 2u + 1u] = rng.uniform();
            nn_test::targetImage(tx[i * 2u], tx[i * 2u + 1u], &tt[i * 3u]);
        }
        ++ctx.serial;
        ctx.bindless.setFrameSerial(ctx.serial);
        t_allocations = 0;
        t_count = measure;
        const bool stepped = trainer.step(net, tx.data(), tt.data(), 128) >= 0.0f; // new version every frame
        t_count = false;
        const unsigned long long trainAllocs = t_allocations;
        t_allocations = 0;
        t_count = measure;
        const bool begun = gpu.beginFrame(ctx.serial, net);
        t_count = false;
        const unsigned long long begin = t_allocations;
        t_allocations = 0;
        t_count = measure;
        buildGraph(ctx, gpu, graph, kSamples);
        t_count = false;
        const unsigned long long graphBuild = t_allocations;
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle();
        gpu.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(stepped && begun && result.ok && waited, "frame ok");
        if (measure) {
            side += begin + inCallbacks;
            callbacks += inCallbacks;
            build += graphBuild;
            train += trainAllocs;
        }
        if (frame == kWarmup) {
            reallocations = gpu.stats().reallocations;
        }
    }
    ctx.executor->setPassHooks(rg::PassHooks{});
    expect(gpu.stats().reallocations == reallocations, "no ring reallocation in steady state");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%s kernel; Adam step + upload of the new version every frame, %u "
                    "inferences)\n"
                    "  NeuralTrainer::step: %llu operator-new calls\n"
                    "  NeuralGpu::beginFrame (slot copy) + neural.* pass callbacks: %llu (callbacks %llu)\n"
                    "  whole graph build (imports + infer pass + host read-back): %llu\n",
                    kTotal - kWarmup, gpu.kernelLanguage(), kSamples, train, side, callbacks, build);
        expect(train == 0u, "training step makes no steady-state heap allocation");
        expect(side == 0u, "the neural upload and pass make no steady-state heap allocations");
        expect(build == 0u, "graph build with the neural pass makes no steady-state heap allocations");
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
    std::string mode = "infer";
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
        if (mode == "infer") {
            rc = runInfer(ctx);
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
