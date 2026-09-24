// WP-1.1 GPU scene Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization validation;
// every validation message fails the run). The CPU twin is test_rp_gpu_scene_cpu.cpp.
//
// Every frame: GpuScene::commit() records the dirty ranges into the UploadQueue, flush() submits
// them, then a render-graph (WP-0.3) frame runs the gpu_scene_readback kernel, which reaches the
// scene only through a 32-bit bindless handle (WP-0.4) and BDA pointers and writes what it read into
// a bindless storage buffer; a HostRead pass exposes it to the test.
//
//   --mode readback   meshes (incl. a WP-1.2 meshlet mesh uploaded to its own geometry buffer),
//                     materials, lights, 4096 instances, 8 frames of updates: raw words of every
//                     table and the header == CPU mirror, typed per-instance words (shader structs)
//                     == CPU expectation, geometry buffer == packMeshletGeometry blob. Slang and GLSL
//                     kernels both run when both are built and must agree.
//   --mode churn      60 frames of random add / remove / move / material swaps starting from a
//                     64-row table (several reallocations, retired buffers and bindless slots
//                     collected); GPU == mirror every frame.
//   --mode delta      1k / 10k / 100k instances x 1% / 10% moved per frame: bytes staged per frame ==
//                     48 x (moved now + moved last frame), copy regions recorded by UploadQueue ==
//                     commit ranges, GPU transforms == mirror.
//   --mode zero_alloc steady-state frames (updates, add/remove churn, commit, flush, graph build,
//                     execute, wait): 0 operator-new calls on the scene side; the whole frame
//                     including UploadQueue / executor is reported.
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, no shader built, backend unsupported).
#include <fuse/renderer/geometry/meshlet_builder.hpp>
#include <fuse/renderer/geometry/meshlet_format.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_meshlets.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <random>
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

void* operator new(std::size_t size) {
    if (t_count) {
        ++t_allocations;
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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
using namespace fuse::renderer::gpu_scene;
using fuse::f32;
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
constexpr u32 kTypedWords = 40u;
constexpr u32 kHeaderTable = 6u;
constexpr u32 kGeometryTableBase = 8u;
constexpr usize kStagingBytes = 8u * 1024u * 1024u;
constexpr usize kReadbackBytes = 48u * 1024u * 1024u;

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

struct Kernel {
    const char* name = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    std::unique_ptr<rg::Executor> executor;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    std::vector<Kernel> kernels;
    BindlessDescriptors bindless;
    UploadQueue upload;
    Buffer staging{};
    Buffer readback{};
    BindlessSlotHandle readbackSlot{};
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (vkDevice != VK_NULL_HANDLE) {
            for (Kernel& k : kernels) {
                vkDestroyPipeline(vkDevice, k.pipeline, nullptr);
            }
            if (pipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(vkDevice, pipelineLayout, nullptr);
            }
        }
        if (allocator != nullptr) {
            if (readbackSlot.isValid()) {
                bindless.unregisterSlot(readbackSlot);
            }
            allocator->destroyBuffer(readback);
            allocator->destroyBuffer(staging);
        }
        if (device != nullptr) {
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

std::vector<char> readFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool addKernel(Context& ctx, const char* name, const char* path) {
    const std::vector<char> code = readFile(path);
    if (code.empty() || code.size() % 4u != 0u) {
        std::fprintf(stderr, "FAIL: %s not readable\n", path);
        return false;
    }
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = code.size();
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(ctx.vkDevice, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
        return false;
    }
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.flags = static_cast<VkPipelineCreateFlags>(ctx.bindless.pipelineCreateFlags());
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = ctx.pipelineLayout;
    Kernel kernel{name, VK_NULL_HANDLE};
    const VkResult created = vkCreateComputePipelines(ctx.vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &kernel.pipeline);
    vkDestroyShaderModule(ctx.vkDevice, module, nullptr);
    if (created != VK_SUCCESS) {
        return false;
    }
    ctx.kernels.push_back(kernel);
    return true;
}

int setup(Context& ctx, bool descriptorBuffer, bool validation = true) {
#if !defined(FUSE_RP_GPU_SCENE_SLANG_SPV) && !defined(FUSE_RP_GPU_SCENE_GLSL_SPV)
    (void)ctx;
    (void)descriptorBuffer;
    (void)validation;
    std::printf("SKIP: gpu_scene_readback kernel not built (neither slangc nor glslangValidator)\n");
    return kSkip;
#else
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
    instanceDesc.appName = "fuse_rp_gpu_scene";
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
    const RendererCaps& caps = ctx.device->info().caps;
    if (!caps.bufferDeviceAddress) {
        std::printf("SKIP: no bufferDeviceAddress\n");
        return kSkip;
    }
    if (descriptorBuffer && !caps.descriptorBuffer) {
        std::printf("SKIP: device has no VK_EXT_descriptor_buffer\n");
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    std::printf("device: %s | %s\n", ctx.device->info().deviceName.c_str(), caps.summary().c_str());

    BindlessDesc bdesc{};
    bdesc.backend = descriptorBuffer ? BindlessBackendPreference::DescriptorBuffer : BindlessBackendPreference::DescriptorSet;
    if (!ctx.bindless.init(*ctx.device, bdesc)) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    expect(ctx.bindless.backend() ==
               (descriptorBuffer ? BindlessBackend::DescriptorBuffer : BindlessBackend::DescriptorSet),
           "bindless backend as requested");
    std::printf("bindless backend: %s\n", bindlessBackendName(ctx.bindless.backend()));

    BufferDesc stagingDesc{};
    stagingDesc.size = kStagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "rp_gpu_scene.staging";
    BufferDesc readbackDesc{};
    readbackDesc.size = kReadbackBytes;
    readbackDesc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::Storage) |
                                                  static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    readbackDesc.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDesc.name = "rp_gpu_scene.readback";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.allocator->createBuffer(readbackDesc, ctx.readback) || ctx.readback.mapped == nullptr) {
        std::fprintf(stderr, "FAIL: staging / readback buffers\n");
        return 1;
    }
    ctx.readbackSlot = ctx.bindless.registerBufferSlot(ctx.readback, false);
    if (!ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: UploadQueue\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }

    VkDescriptorSetLayout setLayout = static_cast<VkDescriptorSetLayout>(ctx.bindless.layoutHandle());
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 * sizeof(u32)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(ctx.vkDevice, &layoutInfo, nullptr, &ctx.pipelineLayout) != VK_SUCCESS) {
        return 1;
    }
    ctx.kernels.reserve(2);
#if defined(FUSE_RP_GPU_SCENE_SLANG_SPV)
    if (!addKernel(ctx, "slang", FUSE_RP_GPU_SCENE_SLANG_SPV)) {
        return 1;
    }
#endif
#if defined(FUSE_RP_GPU_SCENE_GLSL_SPV)
    if (!addKernel(ctx, "glsl", FUSE_RP_GPU_SCENE_GLSL_SPV)) {
        return 1;
    }
#endif
    std::printf("readback kernels:");
    for (const Kernel& k : ctx.kernels) {
        std::printf(" %s", k.name);
    }
    std::printf("\n");
    return 0;
#endif
}

// --- readback frame (render graph) ---------------------------------------------------------------

struct Push {
    u32 scene = 0;
    u32 outBuffer = 0;
    u32 mode = 0;
    u32 table = 0;
    u32 wordCount = 0;
    u32 outOffset = 0;
    u32 pad0 = 0;
    u32 pad1 = 0;
};

struct Dispatches {
    Context* ctx = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    Push pushes[16] = {};
    u32 count = 0;
    u32 words = 0;
};

void recordDispatches(const rg::PassContext& pc, void* user) {
    const Dispatches& d = *static_cast<const Dispatches*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeline);
    d.ctx->bindless.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.ctx->pipelineLayout, 0);
    for (u32 i = 0; i < d.count; ++i) {
        const Push& p = d.pushes[i];
        vkCmdPushConstants(cmd, d.ctx->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push), &p);
        vkCmdDispatch(cmd, (p.wordCount + 63u) / 64u, 1, 1);
    }
}

/// Plans the readback of every table + header (raw), the typed words and optionally geometry.
void planReadback(const GpuScene& scene, u32 outHandle, bool typed, u32 geometryMesh, u32 geometryWords,
                  Dispatches& d) {
    d.count = 0;
    u32 offset = 0;
    auto add = [&](u32 mode, u32 table, u32 words) {
        if (words == 0u) {
            return;
        }
        Push& p = d.pushes[d.count++];
        p = Push{};
        p.scene = scene.headerHandle();
        p.outBuffer = outHandle;
        p.mode = mode;
        p.table = table;
        p.wordCount = words;
        p.outOffset = offset;
        offset += mode == 0u ? words : words * kTypedWords;
    };
    add(0u, kHeaderTable, sizeof(GpuSceneHeader) / 4u);
    for (u32 t = 0; t < kGpuSceneTableCount; ++t) {
        const TableBytes b = scene.tableBytes(static_cast<GpuSceneTable>(t));
        add(0u, t, b.count * b.stride / 4u);
    }
    if (typed) {
        add(1u, 0u, scene.instanceHighWater());
    }
    if (geometryMesh != kInvalidIndex) {
        add(0u, kGeometryTableBase + geometryMesh, geometryWords);
    }
    d.words = offset;
}

/// Commit + flush + one RG frame with `kernel`; returns false on submission failure.
bool gpuFrame(Context& ctx, GpuScene& scene, rg::Graph& graph, Dispatches& d, GpuSceneCommitStats* commitOut,
              bool skipFlush = false) {
    const GpuSceneCommitStats stats = scene.commit();
    if (commitOut != nullptr) {
        *commitOut = stats;
    }
    if (!skipFlush) {
        ctx.upload.flush();
    }
    graph.reset();
    const GpuSceneGraphRefs refs = scene.importInto(graph);
    const rg::BufferRef out = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, kReadbackBytes, rg::kNoQueue,
                                                                    nullptr, "rp_gpu_scene.readback"});
    rg::PassBuilder pass = graph.addPass("gpu_scene.readback", &recordDispatches, &d, rg::QueueClass::Graphics);
    GpuScene::useAll(pass, refs, rg::Access::StorageRead, rg::kStageCompute);
    pass.use(out, rg::Access::StorageWrite, rg::BufferRange{0, static_cast<u64>(d.words) * 4u}, rg::kStageCompute);
    graph.addPass("gpu_scene.host_read", nullptr, nullptr).use(out, rg::Access::HostRead);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && (skipFlush || ctx.upload.waitAll());
    return stats.ok && result.ok && waited;
}

const u32* readbackWords(Context& ctx) { return static_cast<const u32*>(ctx.readback.mapped); }

/// Raw words of header + tables must equal the mirror; typed words must equal the CPU expectation.
bool compareWithMirror(const GpuScene& scene, const Dispatches& d, const u32* words, const char* label) {
    bool ok = true;
    for (u32 i = 0; i < d.count; ++i) {
        const Push& p = d.pushes[i];
        const u32* got = words + p.outOffset;
        if (p.mode == 0u && p.table == kHeaderTable) {
            ok = std::memcmp(got, &scene.header(), sizeof(GpuSceneHeader)) == 0 && ok;
            if (!ok) {
                std::fprintf(stderr, "  %s: header differs\n", label);
            }
        } else if (p.mode == 0u && p.table < kGpuSceneTableCount) {
            const TableBytes b = scene.tableBytes(static_cast<GpuSceneTable>(p.table));
            if (std::memcmp(got, b.data, static_cast<usize>(p.wordCount) * 4u) != 0) {
                u32 first = 0;
                const u32* mirror = reinterpret_cast<const u32*>(b.data);
                while (first < p.wordCount && got[first] == mirror[first]) {
                    ++first;
                }
                std::fprintf(stderr, "  %s: table %u differs at word %u (row %u): gpu 0x%08x mirror 0x%08x\n", label,
                             p.table, first, first * 4u / b.stride, got[first], mirror[first]);
                ok = false;
            }
        } else if (p.mode == 1u) {
            const u32 meshCount = scene.meshCount();
            const u32 materialCount = scene.materialCount();
            const TableBytes materials = scene.tableBytes(GpuSceneTable::Materials);
            for (u32 s = 0; s < p.wordCount; ++s) {
                u32 expected[kTypedWords];
                const GpuInstance& inst = scene.instance(s);
                std::memcpy(expected, &inst, sizeof(GpuInstance));
                std::memcpy(expected + 8, &scene.transform(s), sizeof(GpuTransform));
                std::memcpy(expected + 20, &scene.prevTransform(s), sizeof(GpuTransform));
                for (u32 k = 32; k < 38; ++k) {
                    expected[k] = kInvalidIndex;
                }
                if (inst.mesh < meshCount) {
                    const GpuMesh& m = scene.mesh(inst.mesh);
                    expected[32] = m.meshletCount;
                    std::memcpy(&expected[33], &m.boundsRadius, 4);
                    expected[34] = m.geometryHandle;
                    expected[35] = static_cast<u32>(m.meshlets & 0xFFFFFFFFu);
                }
                if (inst.material < materialCount) {
                    const u32* row = reinterpret_cast<const u32*>(materials.data + static_cast<usize>(inst.material) * 128u);
                    expected[36] = row[0];
                    expected[37] = row[19];
                }
                expected[38] = kGpuSceneMagic;
                expected[39] = scene.header().liveInstances;
                if (std::memcmp(expected, got + s * kTypedWords, sizeof(expected)) != 0) {
                    std::fprintf(stderr, "  %s: typed words of instance %u differ\n", label, s);
                    ok = false;
                    break;
                }
            }
        }
    }
    return ok;
}

/// Runs the readback with every built kernel; kernels must agree word for word.
bool verifyFrame(Context& ctx, GpuScene& scene, rg::Graph& graph, bool typed, u32 geometryMesh,
                 const std::vector<u8>* geometryBlob, const char* label, GpuSceneCommitStats* commitOut = nullptr) {
    bool ok = true;
    std::vector<u32> first;
    for (usize k = 0; k < ctx.kernels.size(); ++k) {
        Dispatches d{};
        d.ctx = &ctx;
        d.pipeline = ctx.kernels[k].pipeline;
        const u32 geometryWords = geometryBlob != nullptr ? static_cast<u32>(geometryBlob->size() / 4u) : 0u;
        planReadback(scene, ctx.bindless.shaderHandle(ctx.readbackSlot), typed, geometryMesh, geometryWords, d);
        if (static_cast<usize>(d.words) * 4u > kReadbackBytes) {
            std::fprintf(stderr, "FAIL: readback buffer too small\n");
            return false;
        }
        std::memset(ctx.readback.mapped, 0xEE, static_cast<usize>(d.words) * 4u);
        if (!gpuFrame(ctx, scene, graph, d, k == 0 ? commitOut : nullptr)) {
            std::fprintf(stderr, "FAIL: %s: GPU frame failed\n", label);
            return false;
        }
        const u32* words = readbackWords(ctx);
        std::string what = std::string(label) + "/" + ctx.kernels[k].name;
        ok = compareWithMirror(scene, d, words, what.c_str()) && ok;
        if (geometryBlob != nullptr) {
            const Push& g = d.pushes[d.count - 1u];
            if (std::memcmp(words + g.outOffset, geometryBlob->data(), geometryBlob->size()) != 0) {
                std::fprintf(stderr, "  %s: geometry buffer differs from packMeshletGeometry\n", what.c_str());
                ok = false;
            }
        }
        if (k == 0) {
            first.assign(words, words + d.words);
        } else if (std::memcmp(first.data(), words, static_cast<usize>(d.words) * 4u) != 0) {
            std::fprintf(stderr, "  %s: Slang and GLSL kernels disagree\n", label);
            ok = false;
        }
    }
    return ok;
}

// --- scene helpers -------------------------------------------------------------------------------

GpuTransform translation(f32 x, f32 y, f32 z) {
    GpuTransform t{};
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
}

bool buildSphere(geometry::MeshletMesh& out) {
    const u32 rings = 16, segments = 24;
    std::vector<f32> pos;
    std::vector<u32> idx;
    for (u32 r = 0; r <= rings; ++r) {
        const f32 phi = 3.14159265f * static_cast<f32>(r) / static_cast<f32>(rings);
        for (u32 s = 0; s <= segments; ++s) {
            const f32 theta = 6.2831853f * static_cast<f32>(s) / static_cast<f32>(segments);
            pos.push_back(std::sin(phi) * std::cos(theta));
            pos.push_back(std::cos(phi));
            pos.push_back(std::sin(phi) * std::sin(theta));
        }
    }
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = r * (segments + 1) + s, b = a + segments + 1;
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    geometry::MeshletSource src{};
    src.positions = pos.data();
    src.vertex_count = static_cast<u32>(pos.size() / 3u);
    src.indices = idx.data();
    src.index_count = static_cast<u32>(idx.size());
    return geometry::build_meshlets(src, geometry::MeshletBuildOptions{}, out, nullptr);
}

GpuSceneDesc gpuDesc(Context& ctx, u32 instances, GpuSceneScatter scatter = GpuSceneScatter::Auto) {
    GpuSceneDesc d{};
    d.scatter = scatter;
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = instances;
    return d;
}

void beginFrame(Context& ctx, GpuScene& scene) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    scene.beginFrame(ctx.serial);
}

/// Every submitted frame has completed (verifyFrame waits): reclaim retired buffers and slots.
void collect(Context& ctx, GpuScene& scene) {
    scene.collectRetired(ctx.serial);
    ctx.bindless.collectRetired(ctx.serial);
}

// --- modes ---------------------------------------------------------------------------------------

int runReadback(Context& ctx) {
    GpuScene scene;
    if (!scene.init(gpuDesc(ctx, 4096)) || !scene.gpuEnabled()) {
        std::fprintf(stderr, "FAIL: GpuScene GPU init\n");
        return 1;
    }
    expect(scene.headerHandle() != 0u && scene.headerAddress() != 0u, "header has a bindless handle and a BDA");
    std::printf("scatter kernel: %s\n", scene.scatterKernel());
    u32 scatteredFrames = 0;
    geometry::MeshletMesh sphere;
    if (!buildSphere(sphere)) {
        std::fprintf(stderr, "FAIL: meshlet build\n");
        return 1;
    }
    std::vector<u8> blob;
    packMeshletGeometry(sphere, blob);
    const u32 sphereMesh = scene.addMeshletMesh(sphere);
    expect(sphereMesh == 0u && scene.mesh(0).meshlets != 0u && scene.mesh(0).geometryHandle != 0u,
           "meshlet mesh uploaded with BDA streams and a bindless handle");
    for (u32 m = 1; m < 8; ++m) {
        GpuMesh mesh{};
        mesh.meshletCount = m * 3u;
        mesh.boundsRadius = static_cast<f32>(m) * 0.5f;
        scene.addMesh(mesh);
    }
    for (u32 i = 0; i < 40; ++i) {
        GpuMaterial mat{};
        mat.baseColor = {static_cast<f32>(i) * 0.01f, 0.5f, 0.25f, 1.f};
        mat.flags = i * 3u + 1u;
        scene.setMaterial(i, mat);
    }
    std::vector<LightHandle> lights;
    for (u32 i = 0; i < 100; ++i) {
        GpuLight l{};
        l.type = static_cast<u32>(i % 3u == 0u ? GpuLightType::Spot : GpuLightType::Point);
        l.position[0] = static_cast<f32>(i);
        l.intensity = 1.f + static_cast<f32>(i);
        lights.push_back(scene.addLight(l));
    }
    std::mt19937 rng(99);
    std::vector<InstanceHandle> handles;
    rg::Graph graph;
    for (u32 frame = 0; frame < 8; ++frame) {
        beginFrame(ctx, scene);
        if (frame == 0) {
            for (u32 i = 0; i < 4096; ++i) {
                InstanceDesc d{};
                d.mesh = i % 9u; // mesh 8 does not exist: typed readback reports invalid
                d.material = i % 41u;
                d.transform = translation(static_cast<f32>(i), 1.f, 2.f);
                d.userData = i * 7u;
                handles.push_back(scene.addInstance(d));
            }
        } else {
            for (u32 i = 0; i < 200; ++i) {
                const InstanceHandle h = handles[rng() % handles.size()];
                scene.setTransform(h, translation(static_cast<f32>(frame), static_cast<f32>(rng() % 100u), 3.f));
            }
            for (u32 i = 0; i < 20; ++i) {
                scene.setInstanceMaterial(handles[rng() % handles.size()], rng() % 40u);
            }
            GpuLight l = scene.light(lights[frame].slot);
            l.intensity += 10.f;
            scene.setLight(lights[frame], l);
            scene.removeLight(lights[frame + 50u]);
        }
        GpuSceneCommitStats stats{};
        const bool ok = verifyFrame(ctx, scene, graph, true, frame == 0 ? sphereMesh : kInvalidIndex,
                                    frame == 0 ? &blob : nullptr, "readback", &stats);
        expect(ok, "GPU readback equals the CPU mirror bit for bit");
        std::printf("readback frame %u: %u live instances, %u lights, commit %llu bytes, %u ranges, %u copies, "
                    "%u scatter jobs, %s\n",
                    frame, scene.liveInstances(), scene.header().liveLights, static_cast<unsigned long long>(stats.bytes),
                    stats.ranges, stats.copies, stats.scatterJobs, ok ? "GPU == mirror" : "MISMATCH");
        scatteredFrames += stats.scatterJobs > 0u ? 1u : 0u;
        if (frame > 0) {
            const u32 moved = stats.tables[static_cast<u32>(GpuSceneTable::Transforms)].dirtyRows;
            expect(stats.bytes < 64u * 1024u && moved <= 200u, "steady frames upload deltas only");
        }
        collect(ctx, scene);
    }
    expect(std::strcmp(scene.scatterKernel(), "off") == 0 || scatteredFrames >= 7u,
           "fragmented deltas took the scatter path");
    scene.destroy();

    // Negative control: a delta committed but never flushed must show up as a mismatch (the
    // comparison can fail), and flushing it repairs the GPU copy.
    GpuScene control;
    if (!control.init(gpuDesc(ctx, 64, GpuSceneScatter::Off))) {
        return 1;
    }
    control.addMesh(GpuMesh{});
    beginFrame(ctx, control);
    InstanceDesc id{};
    id.mesh = 0;
    const InstanceHandle h = control.addInstance(id);
    expect(verifyFrame(ctx, control, graph, false, kInvalidIndex, nullptr, "control.setup"), "control scene uploaded");
    beginFrame(ctx, control);
    control.setTransform(h, translation(9.f, 9.f, 9.f));
    Dispatches d{};
    d.ctx = &ctx;
    d.pipeline = ctx.kernels.front().pipeline;
    planReadback(control, ctx.bindless.shaderHandle(ctx.readbackSlot), false, kInvalidIndex, 0u, d);
    expect(gpuFrame(ctx, control, graph, d, nullptr, true), "control frame ran");
    const bool staleDetected = !compareWithMirror(control, d, readbackWords(ctx), "control(expected mismatch)");
    expect(staleDetected, "negative control: an unflushed delta is detected");
    ctx.upload.flush();
    ctx.upload.waitAll();
    expect(verifyFrame(ctx, control, graph, false, kInvalidIndex, nullptr, "control.flushed"),
           "flushing the delta repairs the GPU copy");
    std::printf("negative control: unflushed delta %s, repaired after flush\n", staleDetected ? "detected" : "MISSED");
    control.destroy();
    return 0;
}

int runChurn(Context& ctx) {
    GpuScene scene;
    // The churn gate covers the GLSL scatter twin (readback / delta use Auto = Slang when built).
    if (!scene.init(gpuDesc(ctx, 64, GpuSceneScatter::Glsl)) || !scene.gpuEnabled()) {
        std::fprintf(stderr, "FAIL: GpuScene GPU init\n");
        return 1;
    }
    for (u32 m = 0; m < 4; ++m) {
        GpuMesh mesh{};
        mesh.meshletCount = m + 1u;
        scene.addMesh(mesh);
    }
    for (u32 i = 0; i < 8; ++i) {
        GpuMaterial mat{};
        mat.flags = i;
        scene.setMaterial(i, mat);
    }
    std::mt19937 rng(4242);
    std::vector<InstanceHandle> live;
    std::vector<InstanceHandle> dead;
    rg::Graph graph;
    u32 reallocations = 0;
    u32 retired = 0;
    for (u32 frame = 0; frame < 60; ++frame) {
        beginFrame(ctx, scene);
        const u32 adds = frame < 30 ? 20u + rng() % 60u : rng() % 30u;
        const u32 removes = live.empty() ? 0u : rng() % std::min<u32>(40u, static_cast<u32>(live.size()));
        for (u32 i = 0; i < removes; ++i) {
            const usize k = rng() % live.size();
            expect(scene.removeInstance(live[k]), "remove");
            dead.push_back(live[k]);
            live[k] = live.back();
            live.pop_back();
        }
        for (u32 i = 0; i < adds; ++i) {
            InstanceDesc d{};
            d.mesh = rng() % 5u;
            d.material = rng() % 9u;
            d.transform = translation(static_cast<f32>(rng() % 1000u), static_cast<f32>(frame), 0.f);
            live.push_back(scene.addInstance(d));
        }
        for (u32 i = 0; i < 25 && !live.empty(); ++i) {
            const InstanceHandle h = live[rng() % live.size()];
            scene.setTransform(h, translation(static_cast<f32>(frame), static_cast<f32>(i), 1.f), (rng() % 5u) == 0u);
            if (rng() % 3u == 0u) {
                scene.setInstanceMesh(h, rng() % 5u);
            }
        }
        for (u32 i = 0; i < 4 && !dead.empty(); ++i) {
            expect(!scene.setTransform(dead[rng() % dead.size()], GpuTransform{}), "stale handle rejected");
        }
        GpuSceneCommitStats stats{};
        expect(verifyFrame(ctx, scene, graph, true, kInvalidIndex, nullptr, "churn", &stats),
               "churn: GPU equals the CPU mirror after every frame");
        for (const TableCommitStats& t : stats.tables) {
            reallocations += t.reallocated ? 1u : 0u;
        }
        retired += scene.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        expect(scene.liveInstances() == live.size(), "live instance count");
    }
    std::printf("churn: 60 frames, %zu live, high water %u, %u table reallocations, %u retired buffers destroyed, "
                "scatter kernel %s\n",
                live.size(), scene.instanceHighWater(), reallocations, retired, scene.scatterKernel());
    expect(reallocations >= 3u && retired >= 3u, "churn exercised growth and retirement");
    scene.destroy();
    return 0;
}

struct DeltaCase {
    u32 n;
    u32 percent;
    GpuSceneScatter scatter;
};

int runDelta(Context& ctx) {
    std::printf("  %8s %5s %8s %7s %12s %10s %8s %10s %10s\n", "N", "dirty", "changed", "path", "bytes/frame",
                "bytes/chg", "ranges", "vkCopies", "commit us");
    const DeltaCase cases[] = {
        {1000u, 1u, GpuSceneScatter::Auto},   {10000u, 1u, GpuSceneScatter::Auto},  {100000u, 1u, GpuSceneScatter::Auto},
        {1000u, 10u, GpuSceneScatter::Auto},  {10000u, 10u, GpuSceneScatter::Auto}, {100000u, 10u, GpuSceneScatter::Auto},
        // Direct copies for comparison (one vkCmdCopyBuffer region per range; 100k x 10% is ~9k regions
        // and very slow under synchronization validation, so it is left out).
        {1000u, 1u, GpuSceneScatter::Off},    {10000u, 1u, GpuSceneScatter::Off},   {100000u, 1u, GpuSceneScatter::Off},
        {10000u, 10u, GpuSceneScatter::Off},
    };
    for (const DeltaCase& c : cases) {
        const u32 n = c.n;
        const u32 percent = c.percent;
        {
            GpuScene scene;
            if (!scene.init(gpuDesc(ctx, n, c.scatter)) || !scene.gpuEnabled()) {
                std::fprintf(stderr, "FAIL: GpuScene GPU init\n");
                return 1;
            }
            scene.addMesh(GpuMesh{});
            scene.setMaterial(0, GpuMaterial{});
            std::vector<InstanceHandle> handles;
            handles.reserve(n);
            rg::Graph graph;
            beginFrame(ctx, scene);
            for (u32 i = 0; i < n; ++i) {
                InstanceDesc d{};
                d.mesh = 0;
                d.material = 0;
                d.transform = translation(static_cast<f32>(i), 0.f, 0.f);
                handles.push_back(scene.addInstance(d));
            }
            expect(verifyFrame(ctx, scene, graph, false, kInvalidIndex, nullptr, "delta.populate"), "populate readback");
            const u32 changed = std::max(1u, n * percent / 100u);
            std::mt19937 rng(n + percent);
            std::vector<u32> pick(n);
            u64 bytes = 0;
            u32 ranges = 0;
            u64 copies = 0;
            double commitUs = 0.0;
            for (u32 frame = 1; frame <= 4; ++frame) {
                beginFrame(ctx, scene);
                for (u32 i = 0; i < n; ++i) {
                    pick[i] = i;
                }
                for (u32 i = 0; i < changed; ++i) {
                    std::swap(pick[i], pick[i + rng() % (n - i)]);
                    const u32 k = pick[i];
                    scene.setTransform(handles[k], translation(static_cast<f32>(k), static_cast<f32>(frame), 1.f));
                }
                const u64 copiesBefore = ctx.upload.stats().recordedCopies;
                const auto t0 = std::chrono::steady_clock::now();
                const GpuSceneCommitStats stats = scene.commit(); // the frame's upload work
                const auto t1 = std::chrono::steady_clock::now();
                const u64 frameCopies = ctx.upload.stats().recordedCopies - copiesBefore;
                expect(stats.ok, "commit ok");
                if (frame >= 2) {
                    // Rows: this frame's moves (cur) + last frame's moves (prev), 48 bytes each, plus a
                    // 4-byte destination index per row on the scatter path.
                    const TableCommitStats& cur = stats.tables[static_cast<u32>(GpuSceneTable::Transforms)];
                    const TableCommitStats& prev = stats.tables[static_cast<u32>(GpuSceneTable::PrevTransforms)];
                    expect(cur.uploadedRows == changed && prev.uploadedRows == changed,
                           "rows uploaded == moved now (cur) + moved last frame (prev)");
                    const u64 perRow = sizeof(GpuTransform) + (cur.scattered ? sizeof(u32) : 0u);
                    expect(stats.bytes == perRow * 2u * changed, "bytes == (48 [+4 scatter index]) x rows");
                    expect(frameCopies == stats.copies, "UploadQueue copy regions == commit copies");
                    bytes = stats.bytes;
                    ranges = stats.ranges;
                    copies = frameCopies;
                    commitUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
                }
                // Verify on the GPU (commit() inside is a no-op now: everything is clean).
                expect(verifyFrame(ctx, scene, graph, false, kInvalidIndex, nullptr, "delta"),
                       "delta: GPU equals the CPU mirror");
                collect(ctx, scene);
            }
            std::printf("  %8u %4u%% %8u %7s %12llu %10.1f %8u %10llu %10.1f\n", n, percent, changed,
                        c.scatter == GpuSceneScatter::Off ? "copy" : "auto", static_cast<unsigned long long>(bytes),
                        static_cast<double>(bytes) / static_cast<double>(changed), ranges,
                        static_cast<unsigned long long>(copies), commitUs);
            scene.destroy();
        }
    }
    return 0;
}

int runZeroAlloc(Context& ctx, bool countAllocations) {
    constexpr u32 kInstances = 10000;
    GpuScene scene;
    GpuSceneDesc desc = gpuDesc(ctx, kInstances + 256u);
    desc.mergeGapBytes = 64u;
    if (!scene.init(desc) || !scene.gpuEnabled()) {
        std::fprintf(stderr, "FAIL: GpuScene GPU init\n");
        return 1;
    }
    scene.addMesh(GpuMesh{});
    scene.setMaterial(0, GpuMaterial{});
    std::vector<InstanceHandle> handles;
    handles.reserve(kInstances);
    std::vector<InstanceHandle> extra;
    extra.reserve(64);
    rg::Graph graph;
    Dispatches d{};
    d.ctx = &ctx;
    d.pipeline = ctx.kernels.front().pipeline;
    std::mt19937 rng(17);
    unsigned long long sceneAllocs = 0;
    unsigned long long frameAllocs = 0;
    constexpr u32 kWarmup = 24;
    constexpr u32 kFrames = 88;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        const bool measure = frame >= kWarmup;
        // --- scene side: updates, churn, commit, graph build -------------------------------------
        t_allocations = 0;
        t_count = measure;
        beginFrame(ctx, scene);
        if (frame == 0) {
            for (u32 i = 0; i < kInstances; ++i) {
                InstanceDesc id{};
                id.mesh = 0;
                id.material = 0;
                id.transform = translation(static_cast<f32>(i), 0.f, 0.f);
                handles.push_back(scene.addInstance(id));
            }
        }
        for (u32 i = 0; i < kInstances / 100u; ++i) {
            scene.setTransform(handles[rng() % kInstances], translation(static_cast<f32>(frame), static_cast<f32>(i), 0.f));
        }
        for (u32 i = 0; i < 8; ++i) {
            InstanceDesc id{};
            id.transform = translation(static_cast<f32>(frame), 0.f, 1.f);
            extra.push_back(scene.addInstance(id));
        }
        while (extra.size() > 32u) {
            scene.removeInstance(extra.front());
            extra.erase(extra.begin());
        }
        const GpuSceneCommitStats stats = scene.commit();
        planReadback(scene, ctx.bindless.shaderHandle(ctx.readbackSlot), false, kInvalidIndex, 0u, d);
        graph.reset();
        const GpuSceneGraphRefs refs = scene.importInto(graph);
        const rg::BufferRef out = graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, kReadbackBytes,
                                                                        rg::kNoQueue, nullptr, "rp_gpu_scene.readback"});
        rg::PassBuilder pass = graph.addPass("gpu_scene.readback", &recordDispatches, &d, rg::QueueClass::Graphics);
        GpuScene::useAll(pass, refs, rg::Access::StorageRead, rg::kStageCompute);
        pass.use(out, rg::Access::StorageWrite, rg::BufferRange{0, static_cast<u64>(d.words) * 4u}, rg::kStageCompute);
        graph.addPass("gpu_scene.host_read", nullptr, nullptr).use(out, rg::Access::HostRead);
        const unsigned long long sceneSide = t_allocations;
        // --- submission side: UploadQueue flush, RG execute, wait --------------------------------
        ctx.upload.flush();
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        scene.collectRetired(ctx.serial);
        ctx.bindless.collectRetired(ctx.serial);
        t_count = false;
        expect(stats.ok && result.ok && waited, "frame ok");
        if (measure) {
            sceneAllocs += sceneSide;
            frameAllocs += t_allocations;
        }
    }
    expect(compareWithMirror(scene, d, readbackWords(ctx), "zero_alloc"), "zero_alloc: GPU equals mirror");
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u instances, 1%% moved + 8 adds / 8 removes per frame)\n"
                    "  scene side (beginFrame, updates, churn, commit incl. UploadQueue stage/record, graph build): "
                    "%llu operator-new calls\n"
                    "  whole frame (+ UploadQueue::flush, rg::Executor::execute, waits): %llu operator-new calls\n",
                    kFrames - kWarmup, kInstances, sceneAllocs, frameAllocs);
        expect(sceneAllocs == 0u, "scene side makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: GPU == mirror\n");
    }
    scene.destroy();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "readback";
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
        const int setupRc = setup(ctx, backend == "buffer");
        if (setupRc != 0) {
            return setupRc;
        }
        if (mode == "readback") {
            rc = runReadback(ctx);
        } else if (mode == "churn") {
            rc = runChurn(ctx);
        } else if (mode == "delta") {
            rc = runDelta(ctx);
        } else if (mode == "zero_alloc") {
            rc = runZeroAlloc(ctx, false); // same frames, validated (the allocation count follows)
        } else {
            std::fprintf(stderr, "unknown --mode %s\n", mode.c_str());
            return 2;
        }
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
