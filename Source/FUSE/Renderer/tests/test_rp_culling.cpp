// WP-1.3 instance culling Lavapipe gates (VK_LAYER_KHRONOS_validation with synchronization
// validation; every validation message fails the run). CPU gates: test_rp_culling_cpu.cpp.
//
// Every frame is one render graph (WP-0.3): GpuScene (WP-1.1) delta upload, InstanceCuller phase 1,
// a depth + instance-ID raster pass drawing the phase-1 args with vkCmdDrawIndexedIndirectCount, the
// single-pass Hi-Z build, phase 2 (vkCmdDispatchIndirect), the phase-2 raster pass and the Hi-Z
// build for the next frame. A second InstanceCuller with frustum and occlusion off draws every
// instance into separate targets: the no-occlusion reference image.
//
//   --mode parity      10k instances, 8 frames of camera and object motion. Hi-Z after each build ==
//                      CPU pyramid of the read-back depth (bit exact); per-instance GPU results ==
//                      CPU brute-force reference (build_hiz_reference + cull_reference; instances on
//                      a test boundary are excluded by the parity rule and counted, < 1%); the
//                      phase-1 / phase-2 draw args are exactly the Phase1Drawn / Phase2Drawn sets,
//                      each {36, 1, 0, 0, slot}; Slang and GLSL kernels agree word for word.
//   --mode camera_cut  wall + objects hidden behind it; camera teleports to the far side (history
//                      kept, then with cameraCut), then the wall teleports away. After phase 2, 0
//                      objects of the no-occlusion reference are missing, and the ID images match.
//   --mode submit_flat CPU time of the frame's graph build + compile + record (Executor::recordInline,
//                      never submitted, so no GPU work interferes) at 1k / 10k / 100k instances
//                      (interleaved, median of 200 rounds, validation off): within +-10%.
//   --mode zero_alloc  64 steady-state frames (10k instances, 1% moved): 0 operator-new calls on the
//                      cull side (InstanceCuller calls + the cull.* pass callbacks), validated run
//                      first (validation off for the count: the layer allocates through operator new).
//   --backend set | buffer   bindless descriptor-set backend / VK_EXT_descriptor_buffer (skip if absent)
//
// Exit 77 = skip (stub build, no ICD / validation layer, no kernel built, backend unsupported).
#include <fuse/renderer/culling/cull_reference.hpp>
#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
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
#include <memory>
#include <new>
#include <random>
#include <set>
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
using namespace fuse::renderer::culling;
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
constexpr usize kStagingBytes = 16u * 1024u * 1024u;
constexpr usize kReadbackBytes = 16u * 1024u * 1024u;
constexpr u32 kWidth = 256;
constexpr u32 kHeight = 192;
constexpr u32 kFormatR32Uint = 98u;

u32 g_messages = 0;
bool g_quietMessages = false; ///< negative control: count, do not print

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) ==
            0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    ++g_messages;
    if (g_messages <= 20u && !g_quietMessages) {
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

std::vector<char> readFile(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// --- math (column-major, Vulkan clip space, forward depth) ----------------------------------------
struct Mat4 {
    f32 m[16] = {};
};

Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (u32 c = 0; c < 4; ++c) {
        for (u32 row = 0; row < 4; ++row) {
            f32 s = 0.f;
            for (u32 k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = s;
        }
    }
    return r;
}

Mat4 perspective(f32 fovY, f32 aspect, f32 zNear, f32 zFar) {
    const f32 f = 1.f / std::tan(fovY * 0.5f);
    Mat4 p{};
    p.m[0] = f / aspect;
    p.m[5] = -f;
    p.m[10] = zFar / (zNear - zFar);
    p.m[11] = -1.f;
    p.m[14] = zNear * zFar / (zNear - zFar);
    return p;
}

Mat4 lookAt(const f32 eye[3], const f32 at[3]) {
    f32 f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    const f32 fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (f32& v : f) {
        v /= fl;
    }
    const f32 up[3] = {0.f, 1.f, 0.f};
    f32 s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    const f32 sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    for (f32& v : s) {
        v /= sl;
    }
    const f32 u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    Mat4 v{};
    v.m[0] = s[0];
    v.m[4] = s[1];
    v.m[8] = s[2];
    v.m[1] = u[0];
    v.m[5] = u[1];
    v.m[9] = u[2];
    v.m[2] = -f[0];
    v.m[6] = -f[1];
    v.m[10] = -f[2];
    v.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    v.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    v.m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
    v.m[15] = 1.f;
    return v;
}

Mat4 camera(f32 ex, f32 ey, f32 ez, f32 ax, f32 ay, f32 az) {
    const f32 eye[3] = {ex, ey, ez};
    const f32 at[3] = {ax, ay, az};
    return mul(perspective(1.1f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 120.f), lookAt(eye, at));
}

GpuTransform boxTransform(f32 x, f32 y, f32 z, f32 sx, f32 sy, f32 sz) {
    GpuTransform t{};
    t.rows[0][0] = sx;
    t.rows[1][1] = sy;
    t.rows[2][2] = sz;
    t.rows[0][3] = x;
    t.rows[1][3] = y;
    t.rows[2][3] = z;
    return t;
}

// --- Vulkan context -------------------------------------------------------------------------------
struct RasterPipeline {
    const char* name = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct Target {
    Texture depth{};
    Texture id{};
    BindlessSlotHandle depthSlot{};
    u32 depthLayout = 0;
    u32 idLayout = 0;
    u8 depthQueue = rg::kNoQueue;
    u8 idQueue = rg::kNoQueue;
};

struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    std::unique_ptr<rg::Executor> executor;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkPipelineLayout rasterLayout = VK_NULL_HANDLE;
    std::vector<RasterPipeline> raster; ///< per language: "slang", "glsl"
    BindlessDescriptors bindless;
    UploadQueue upload;
    Buffer staging{};
    Buffer readback{};
    Buffer vertices{};
    Buffer indices{};
    Target main{};
    Target reference{};
    u64 serial = 0;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (vkDevice != VK_NULL_HANDLE) {
            for (RasterPipeline& p : raster) {
                vkDestroyPipeline(vkDevice, p.pipeline, nullptr);
            }
            if (rasterLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(vkDevice, rasterLayout, nullptr);
            }
        }
        if (allocator != nullptr) {
            for (Target* t : {&main, &reference}) {
                if (t->depthSlot.isValid()) {
                    bindless.unregisterSlot(t->depthSlot);
                }
                allocator->destroyImage(t->depth);
                allocator->destroyImage(t->id);
            }
            allocator->destroyBuffer(readback);
            allocator->destroyBuffer(staging);
            allocator->destroyBuffer(vertices);
            allocator->destroyBuffer(indices);
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

VkShaderModule loadModule(Context& ctx, const char* path) {
    const std::vector<char> code = readFile(path);
    if (code.empty() || code.size() % 4u != 0u) {
        std::fprintf(stderr, "FAIL: %s not readable\n", path);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(ctx.vkDevice, &info, nullptr, &module);
    return module;
}

bool addRaster(Context& ctx, const char* name, const char* vsPath, const char* fsPath) {
    VkShaderModule vs = loadModule(ctx, vsPath);
    VkShaderModule fs = loadModule(ctx, fsPath);
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) {
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0, 12, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attribute{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attribute;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamics[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamics;
    const VkFormat colorFormat = VK_FORMAT_R32_UINT;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;
    rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.flags = static_cast<VkPipelineCreateFlags>(ctx.bindless.pipelineCreateFlags());
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = ctx.rasterLayout;
    RasterPipeline p{name, VK_NULL_HANDLE};
    const VkResult result = vkCreateGraphicsPipelines(ctx.vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &p.pipeline);
    vkDestroyShaderModule(ctx.vkDevice, vs, nullptr);
    vkDestroyShaderModule(ctx.vkDevice, fs, nullptr);
    if (result != VK_SUCCESS) {
        return false;
    }
    ctx.raster.push_back(p);
    return true;
}

bool createTarget(Context& ctx, Target& t, bool sampledDepth, const char* name) {
    TextureDesc depth{};
    depth.width = kWidth;
    depth.height = kHeight;
    depth.format = GpuFormat::D32Sfloat;
    depth.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::DepthStencilAttachment) |
                                          static_cast<u32>(ImageUsage::TransferSrc) |
                                          (sampledDepth ? static_cast<u32>(ImageUsage::Sampled) : 0u));
    depth.name = name;
    TextureDesc id{};
    id.width = kWidth;
    id.height = kHeight;
    id.format = static_cast<GpuFormat>(kFormatR32Uint);
    id.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::ColorAttachment) |
                                       static_cast<u32>(ImageUsage::TransferSrc));
    id.name = name;
    if (!ctx.allocator->createImage(depth, t.depth) || !ctx.allocator->createImage(id, t.id)) {
        return false;
    }
    if (sampledDepth) {
        t.depthSlot = ctx.bindless.registerTextureSlot(t.depth, false);
    }
    return true;
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
    instanceDesc.appName = "fuse_rp_culling";
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
    if (!caps.bufferDeviceAddress || !caps.drawIndirectCount || !caps.dynamicRendering) {
        std::printf("SKIP: device lacks bufferDeviceAddress / drawIndirectCount / dynamicRendering\n");
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
    std::printf("device: %s\n", ctx.device->info().deviceName.c_str());
    BindlessDesc bdesc{};
    bdesc.backend = descriptorBuffer ? BindlessBackendPreference::DescriptorBuffer : BindlessBackendPreference::DescriptorSet;
    if (!ctx.bindless.init(*ctx.device, bdesc)) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    std::printf("bindless backend: %s\n", bindlessBackendName(ctx.bindless.backend()));

    BufferDesc stagingDesc{};
    stagingDesc.size = kStagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "rp_culling.staging";
    BufferDesc readbackDesc{};
    readbackDesc.size = kReadbackBytes;
    readbackDesc.usage = BufferUsage::TransferDst;
    readbackDesc.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDesc.name = "rp_culling.readback";
    // Unit cube [-1, 1]^3: 8 vertices, 12 triangles (the GpuMesh below says triangleCount 12).
    const f32 cube[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                            {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    const u32 cubeIndices[36] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
                                 3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2};
    BufferDesc vbDesc{};
    vbDesc.size = sizeof(cube);
    vbDesc.usage = BufferUsage::Vertex;
    vbDesc.memoryUsage = MemoryUsage::CpuToGpu;
    vbDesc.name = "rp_culling.cube_vertices";
    BufferDesc ibDesc{};
    ibDesc.size = sizeof(cubeIndices);
    ibDesc.usage = BufferUsage::Index;
    ibDesc.memoryUsage = MemoryUsage::CpuToGpu;
    ibDesc.name = "rp_culling.cube_indices";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.allocator->createBuffer(readbackDesc, ctx.readback) || ctx.readback.mapped == nullptr ||
        !ctx.allocator->createBuffer(vbDesc, ctx.vertices) || ctx.vertices.mapped == nullptr ||
        !ctx.allocator->createBuffer(ibDesc, ctx.indices) || ctx.indices.mapped == nullptr) {
        std::fprintf(stderr, "FAIL: buffers\n");
        return 1;
    }
    std::memcpy(ctx.vertices.mapped, cube, sizeof(cube));
    std::memcpy(ctx.indices.mapped, cubeIndices, sizeof(cubeIndices));
    if (!ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: UploadQueue\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    if (!createTarget(ctx, ctx.main, true, "rp_culling.main") || !createTarget(ctx, ctx.reference, false, "rp_culling.ref")) {
        std::fprintf(stderr, "FAIL: render targets\n");
        return 1;
    }
    VkDescriptorSetLayout setLayout = static_cast<VkDescriptorSetLayout>(ctx.bindless.layoutHandle());
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 80};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(ctx.vkDevice, &layoutInfo, nullptr, &ctx.rasterLayout) != VK_SUCCESS) {
        return 1;
    }
    ctx.raster.reserve(2);
#if defined(FUSE_RP_CULLING_SLANG_VS) && defined(FUSE_RP_CULLING_SLANG_FS)
    if (!addRaster(ctx, "slang", FUSE_RP_CULLING_SLANG_VS, FUSE_RP_CULLING_SLANG_FS)) {
        std::fprintf(stderr, "FAIL: slang raster pipeline\n");
        return 1;
    }
#endif
#if defined(FUSE_RP_CULLING_GLSL_VS) && defined(FUSE_RP_CULLING_GLSL_FS)
    if (!addRaster(ctx, "glsl", FUSE_RP_CULLING_GLSL_VS, FUSE_RP_CULLING_GLSL_FS)) {
        std::fprintf(stderr, "FAIL: glsl raster pipeline\n");
        return 1;
    }
#endif
    if (ctx.raster.empty()) {
        std::printf("SKIP: test raster shaders not built (neither slangc nor glslangValidator)\n");
        return kSkip;
    }
    return 0;
}

GpuSceneDesc gpuDesc(Context& ctx, u32 capacity) {
    GpuSceneDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.upload = &ctx.upload;
    d.bindless = &ctx.bindless;
    d.instanceCapacity = capacity;
    return d;
}

/// One kernel language under test: a two-phase culler, a reference culler (no culling at all) and
/// the matching raster pipeline.
struct Rig {
    const char* language = nullptr;
    VkPipeline raster = VK_NULL_HANDLE;
    InstanceCuller culler;
    InstanceCuller reference;
};

bool initRig(Context& ctx, Rig& rig, CullKernelLanguage language, u32 capacity) {
    InstanceCullerDesc desc{};
    desc.device = ctx.device.get();
    desc.allocator = ctx.allocator.get();
    desc.bindless = &ctx.bindless;
    desc.instanceCapacity = capacity;
    desc.language = language;
    if (!rig.culler.init(desc) || !rig.reference.init(desc) || !rig.culler.setResolution(kWidth, kHeight)) {
        return false;
    }
    rig.language = rig.culler.kernelLanguage();
    for (const RasterPipeline& p : ctx.raster) {
        if (std::strcmp(p.name, rig.language) == 0) {
            rig.raster = p.pipeline;
        }
    }
    if (rig.raster == VK_NULL_HANDLE) {
        rig.raster = ctx.raster.front().pipeline;
    }
    return true;
}

// --- per-frame graph ------------------------------------------------------------------------------
struct DrawRecord {
    Context* ctx = nullptr;
    const InstanceCuller* culler = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    CullPhase phase = CullPhase::Phase1;
    rg::TextureRef depth;
    rg::TextureRef id;
    bool clear = true;
    struct {
        f32 viewProj[16];
        u32 scene;
        u32 pad[3];
    } push{};
};

void recordDraw(const rg::PassContext& pc, void* user) {
    const DrawRecord& d = *static_cast<const DrawRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = static_cast<VkImageView>(pc.imageView(d.id));
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = d.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color.uint32[0] = 0xFFFFFFFFu;
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = static_cast<VkImageView>(pc.imageView(d.depth));
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = d.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil.depth = 1.f;
    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = {kWidth, kHeight};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline);
    d.ctx->bindless.bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d.ctx->rasterLayout, 0);
    const VkViewport viewport{0.f, 0.f, static_cast<f32>(kWidth), static_cast<f32>(kHeight), 0.f, 1.f};
    const VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const VkBuffer vb = static_cast<VkBuffer>(d.ctx->vertices.handle);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
    vkCmdBindIndexBuffer(cmd, static_cast<VkBuffer>(d.ctx->indices.handle), 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cmd, d.ctx->rasterLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(d.push), &d.push);
    d.culler->recordDraws(cmd, d.phase);
    vkCmdEndRendering(cmd);
}

/// Byte layout of the readback buffer.
struct ReadbackLayout {
    u64 depth1 = 0, depth2 = 0, id = 0, refId = 0, hiz1 = 0, hiz2 = 0, results = 0, counts = 0, args = 0, end = 0;
    u64 hizMip[kMaxHizMips] = {};
};

ReadbackLayout makeLayout(const InstanceCuller& culler) {
    ReadbackLayout l{};
    const u64 image = static_cast<u64>(kWidth) * kHeight * 4u;
    u64 o = 0;
    l.depth1 = o;
    o += image;
    l.depth2 = o;
    o += image;
    l.id = o;
    o += image;
    l.refId = o;
    o += image;
    u64 hizBytes = 0;
    for (u32 m = 0; m < culler.hizMipCount(); ++m) {
        l.hizMip[m] = hizBytes;
        const u64 dim = std::max<u64>(1u, culler.hizDim() >> m);
        hizBytes += dim * dim * 4u;
    }
    l.hiz1 = o;
    o += hizBytes;
    l.hiz2 = o;
    o += hizBytes;
    l.results = o;
    o += culler.resultsBytes();
    l.counts = o;
    o += kCountWords * 4u;
    l.args = o;
    o += culler.argsBytes();
    l.end = o;
    return l;
}

struct CopyRecord {
    enum Kind : u8 { Depth, Id, Hiz, Buffer } kind = Depth;
    Context* ctx = nullptr;
    const InstanceCuller* culler = nullptr;
    rg::TextureRef image;
    rg::BufferRef buffer;
    rg::BufferRef dst;
    u64 dstOffset = 0;
    u64 bytes = 0;
    const ReadbackLayout* layout = nullptr;
};

void recordCopy(const rg::PassContext& pc, void* user) {
    const CopyRecord& c = *static_cast<const CopyRecord*>(user);
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    const VkBuffer dst = static_cast<VkBuffer>(pc.buffer(c.dst));
    if (c.kind == CopyRecord::Buffer) {
        const VkBufferCopy region{0, c.dstOffset, c.bytes};
        vkCmdCopyBuffer(cmd, static_cast<VkBuffer>(pc.buffer(c.buffer)), dst, 1, &region);
        return;
    }
    if (c.kind == CopyRecord::Hiz) {
        VkBufferImageCopy regions[kMaxHizMips]{};
        const u32 mips = c.culler->hizMipCount();
        for (u32 m = 0; m < mips; ++m) {
            const u32 dim = std::max(1u, c.culler->hizDim() >> m);
            regions[m].bufferOffset = c.dstOffset + c.layout->hizMip[m];
            regions[m].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            regions[m].imageExtent = {dim, dim, 1};
        }
        vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.image)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, mips,
                               regions);
        return;
    }
    VkBufferImageCopy region{};
    region.bufferOffset = c.dstOffset;
    region.imageSubresource = {c.kind == CopyRecord::Depth ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT)
                                                           : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
                               0, 0, 1};
    region.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, static_cast<VkImage>(pc.image(c.image)), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
}

struct FrameOptions {
    Mat4 viewProj{};
    bool occlusion = true;
    bool cameraCut = false;
    bool withReference = true;
    bool readback = true;
};

/// Everything a frame's graph references (kept alive until execute()).
struct FrameState {
    DrawRecord draws[3];
    CopyRecord copies[12];
    u32 copyCount = 0;
    ReadbackLayout layout{};
};

rg::TextureRef importTarget(rg::Graph& graph, Texture& image, u32& layout, u8& queue, u32 format, const char* name) {
    rg::ImportedImage i{};
    i.image = image.image;
    i.view = image.view;
    i.format = format;
    i.width = kWidth;
    i.height = kHeight;
    i.initialLayout = layout;
    i.initialQueue = queue;
    i.layoutTracker = &layout;
    i.queueTracker = &queue;
    i.name = name;
    return graph.importImage(i);
}

/// Builds the frame's graph (no scene updates, no execution). Returns the refs of the culler.
CullGraphRefs buildFrame(Context& ctx, GpuScene& scene, Rig& rig, rg::Graph& graph, const FrameOptions& opt, FrameState& fs) {
    graph.reset();
    fs.copyCount = 0;
    const GpuSceneGraphRefs sceneRefs = scene.importInto(graph);
    const CullGraphRefs cull = rig.culler.importInto(graph);
    const rg::TextureRef depth =
        importTarget(graph, ctx.main.depth, ctx.main.depthLayout, ctx.main.depthQueue, VK_FORMAT_D32_SFLOAT, "main.depth");
    const rg::TextureRef id =
        importTarget(graph, ctx.main.id, ctx.main.idLayout, ctx.main.idQueue, kFormatR32Uint, "main.id");
    const rg::BufferRef vb = graph.importBuffer(rg::ImportedBuffer{ctx.vertices.handle, ctx.vertices.desc.size,
                                                                   rg::kNoQueue, nullptr, "cube.vertices"});
    const rg::BufferRef ib = graph.importBuffer(rg::ImportedBuffer{ctx.indices.handle, ctx.indices.desc.size,
                                                                   rg::kNoQueue, nullptr, "cube.indices"});
    rg::BufferRef readback{};
    if (opt.readback) {
        fs.layout = makeLayout(rig.culler);
        readback = graph.importBuffer(
            rg::ImportedBuffer{ctx.readback.handle, kReadbackBytes, rg::kNoQueue, nullptr, "rp_culling.readback"});
    }
    const u32 sceneHandle = scene.headerHandle();
    auto addDraw = [&](const char* name, DrawRecord& d, const InstanceCuller& culler, const CullGraphRefs& refs,
                       CullPhase phase, rg::TextureRef dDepth, rg::TextureRef dId, bool clear) {
        d.ctx = &ctx;
        d.culler = &culler;
        d.pipeline = rig.raster;
        d.phase = phase;
        d.depth = dDepth;
        d.id = dId;
        d.clear = clear;
        std::memcpy(d.push.viewProj, opt.viewProj.m, sizeof(d.push.viewProj));
        d.push.scene = sceneHandle;
        rg::PassBuilder pass = graph.addPass(name, &recordDraw, &d);
        culler.useDraws(pass, refs, phase);
        GpuScene::useAll(pass, sceneRefs, rg::Access::StorageRead, rg::kStageVertex);
        pass.use(vb, rg::Access::VertexRead).use(ib, rg::Access::IndexRead);
        pass.use(dDepth, rg::Access::DepthAttachmentWrite).use(dId, rg::Access::ColorAttachmentWrite);
    };
    auto addCopy = [&](const char* name, CopyRecord::Kind kind, rg::TextureRef image, rg::BufferRef buffer, u64 dstOffset,
                       u64 bytes) {
        CopyRecord& c = fs.copies[fs.copyCount++];
        c = CopyRecord{};
        c.kind = kind;
        c.ctx = &ctx;
        c.culler = &rig.culler;
        c.image = image;
        c.buffer = buffer;
        c.dst = readback;
        c.dstOffset = dstOffset;
        c.bytes = bytes;
        c.layout = &fs.layout;
        rg::PassBuilder pass = graph.addPass(name, &recordCopy, &c);
        if (kind == CopyRecord::Buffer) {
            pass.use(buffer, rg::Access::TransferSrc, rg::BufferRange{0, bytes});
        } else {
            pass.use(image, rg::Access::TransferSrc);
        }
        pass.use(readback, rg::Access::TransferDst, rg::BufferRange{dstOffset, kind == CopyRecord::Buffer ? bytes : 0u});
    };

    if (opt.withReference) {
        const CullGraphRefs refRefs = rig.reference.importInto(graph);
        const rg::TextureRef refDepth = importTarget(graph, ctx.reference.depth, ctx.reference.depthLayout,
                                                     ctx.reference.depthQueue, VK_FORMAT_D32_SFLOAT, "ref.depth");
        const rg::TextureRef refId =
            importTarget(graph, ctx.reference.id, ctx.reference.idLayout, ctx.reference.idQueue, kFormatR32Uint, "ref.id");
        rig.reference.addPhase1(graph, refRefs, sceneRefs, sceneHandle);
        addDraw("ref.draw", fs.draws[2], rig.reference, refRefs, CullPhase::Phase1, refDepth, refId, true);
        if (opt.readback) {
            addCopy("readback.ref_id", CopyRecord::Id, refId, {}, fs.layout.refId, 0);
        }
    }
    rig.culler.addPhase1(graph, cull, sceneRefs, sceneHandle);
    addDraw("draw.phase1", fs.draws[0], rig.culler, cull, CullPhase::Phase1, depth, id, true);
    if (opt.readback) {
        addCopy("readback.depth1", CopyRecord::Depth, depth, {}, fs.layout.depth1, 0);
    }
    rig.culler.addHizBuild(graph, cull, depth, ctx.bindless.shaderHandle(ctx.main.depthSlot));
    if (opt.readback) {
        addCopy("readback.hiz1", CopyRecord::Hiz, cull.hiz, {}, fs.layout.hiz1, 0);
    }
    rig.culler.addPhase2(graph, cull, sceneRefs, sceneHandle);
    addDraw("draw.phase2", fs.draws[1], rig.culler, cull, CullPhase::Phase2, depth, id, false);
    rig.culler.addHizBuild(graph, cull, depth, ctx.bindless.shaderHandle(ctx.main.depthSlot));
    if (opt.readback) {
        addCopy("readback.hiz2", CopyRecord::Hiz, cull.hiz, {}, fs.layout.hiz2, 0);
        addCopy("readback.depth2", CopyRecord::Depth, depth, {}, fs.layout.depth2, 0);
        addCopy("readback.id", CopyRecord::Id, id, {}, fs.layout.id, 0);
        addCopy("readback.results", CopyRecord::Buffer, {}, cull.results, fs.layout.results, rig.culler.resultsBytes());
        addCopy("readback.counts", CopyRecord::Buffer, {}, cull.counts, fs.layout.counts, kCountWords * 4u);
        addCopy("readback.args", CopyRecord::Buffer, {}, cull.args, fs.layout.args, rig.culler.argsBytes());
        graph.addPass("readback.host", nullptr, nullptr).use(readback, rg::Access::HostRead);
    }
    return cull;
}

bool beginCull(Context& ctx, GpuScene& scene, Rig& rig, const FrameOptions& opt) {
    CullFrameDesc frame{};
    std::memcpy(frame.viewProj, opt.viewProj.m, sizeof(frame.viewProj));
    frame.instanceCount = scene.instanceHighWater();
    frame.occlusion = opt.occlusion;
    frame.cameraCut = opt.cameraCut;
    bool ok = rig.culler.beginFrame(ctx.serial, frame);
    if (opt.withReference) {
        CullFrameDesc all = frame;
        all.frustum = false;
        all.occlusion = false;
        ok = rig.reference.beginFrame(ctx.serial, all) && ok;
    }
    return ok;
}

/// Commit + flush + graph + execute + wait.
bool runFrame(Context& ctx, GpuScene& scene, Rig& rig, rg::Graph& graph, const FrameOptions& opt, FrameState& fs) {
    const GpuSceneCommitStats stats = scene.commit();
    ctx.upload.flush();
    if (!beginCull(ctx, scene, rig, opt)) {
        return false;
    }
    buildFrame(ctx, scene, rig, graph, opt, fs);
    const rg::ExecuteResult result = ctx.executor->execute(graph);
    const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
    rig.culler.collectRetired(ctx.serial);
    rig.reference.collectRetired(ctx.serial);
    scene.collectRetired(ctx.serial);
    return stats.ok && result.ok && waited;
}

void beginSceneFrame(Context& ctx, GpuScene& scene) {
    ++ctx.serial;
    ctx.bindless.setFrameSerial(ctx.serial);
    scene.beginFrame(ctx.serial);
}

// --- frame analysis -------------------------------------------------------------------------------
struct FrameResult {
    std::vector<u32> results;
    std::vector<u32> phase1;
    std::vector<u32> phase2;
    u32 counts[kCountWords] = {};
    std::vector<f32> hiz2[kMaxHizMips];
    u32 missing = 0;        ///< reference-visible instances not drawn
    u32 refVisible = 0;
    u32 pixelMismatch = 0;
    u32 occluded = 0;
    u32 frustumCulled = 0;
};

const u8* rb(Context& ctx, u64 offset) { return static_cast<const u8*>(ctx.readback.mapped) + offset; }

void copyPyramid(Context& ctx, const InstanceCuller& culler, u64 base, const ReadbackLayout& l, HizPyramid& out) {
    out.dim0 = culler.hizDim();
    out.mipCount = culler.hizMipCount();
    for (u32 m = 0; m < out.mipCount; ++m) {
        const u32 dim = std::max(1u, out.dim0 >> m);
        out.levels[m].resize(static_cast<usize>(dim) * dim);
        std::memcpy(out.levels[m].data(), rb(ctx, base + l.hizMip[m]), out.levels[m].size() * 4u);
    }
}

bool samePyramid(const HizPyramid& a, const HizPyramid& b) {
    if (a.mipCount != b.mipCount || a.dim0 != b.dim0) {
        return false;
    }
    for (u32 m = 0; m < a.mipCount; ++m) {
        if (a.levels[m].size() != b.levels[m].size() ||
            std::memcmp(a.levels[m].data(), b.levels[m].data(), a.levels[m].size() * 4u) != 0) {
            return false;
        }
    }
    return true;
}

/// Reads back and checks the internal consistency of a frame: Hi-Z == CPU pyramid of the depth,
/// draws == results, command contents, reference visibility.
void analyseFrame(Context& ctx, Rig& rig, GpuScene& scene, const FrameState& fs, FrameResult& out, const char* label) {
    const ReadbackLayout& l = fs.layout;
    const u32 n = scene.instanceHighWater();
    out.results.resize(n);
    std::memcpy(out.results.data(), rb(ctx, l.results), static_cast<usize>(n) * 4u);
    std::memcpy(out.counts, rb(ctx, l.counts), sizeof(out.counts));
    // Hi-Z builds against the CPU reference kernel.
    HizPyramid gpu1, gpu2, cpu1, cpu2;
    copyPyramid(ctx, rig.culler, l.hiz1, l, gpu1);
    copyPyramid(ctx, rig.culler, l.hiz2, l, gpu2);
    build_hiz_reference(reinterpret_cast<const f32*>(rb(ctx, l.depth1)), kWidth, kHeight, cpu1);
    build_hiz_reference(reinterpret_cast<const f32*>(rb(ctx, l.depth2)), kWidth, kHeight, cpu2);
    if (!samePyramid(gpu1, cpu1) || !samePyramid(gpu2, cpu2)) {
        std::fprintf(stderr, "  %s: GPU Hi-Z differs from the CPU pyramid of the read-back depth\n", label);
        ++g_failures;
    }
    for (u32 m = 0; m < gpu2.mipCount; ++m) {
        out.hiz2[m] = gpu2.levels[m];
    }
    // Draw args == result sets.
    const auto* args = reinterpret_cast<const DrawIndexedIndirectCommand*>(rb(ctx, l.args));
    const u32 cap = rig.culler.capacity();
    out.phase1.clear();
    out.phase2.clear();
    bool argsOk = out.counts[kCountPhase1Draws] <= cap && out.counts[kCountPhase2Draws] <= cap;
    for (u32 k = 0; argsOk && k < out.counts[kCountPhase1Draws]; ++k) {
        out.phase1.push_back(args[k].firstInstance);
    }
    for (u32 k = 0; argsOk && k < out.counts[kCountPhase2Draws]; ++k) {
        out.phase2.push_back(args[cap + k].firstInstance);
    }
    for (u32 k = 0; argsOk && k < out.counts[kCountPhase1Draws] + out.counts[kCountPhase2Draws]; ++k) {
        const DrawIndexedIndirectCommand& d = k < out.counts[kCountPhase1Draws]
                                                  ? args[k]
                                                  : args[cap + k - out.counts[kCountPhase1Draws]];
        argsOk = d.indexCount == 36u && d.instanceCount == 1u && d.firstIndex == 0u && d.vertexOffset == 0 &&
                 d.firstInstance < n;
    }
    std::vector<u32> expect1, expect2;
    for (u32 i = 0; i < n; ++i) {
        out.occluded += out.results[i] == kResultOccluded;
        out.frustumCulled += out.results[i] == kResultFrustumCulled;
        if (out.results[i] == kResultPhase1Drawn) {
            expect1.push_back(i);
        } else if (out.results[i] == kResultPhase2Drawn) {
            expect2.push_back(i);
        }
    }
    std::vector<u32> got1 = out.phase1, got2 = out.phase2;
    std::sort(got1.begin(), got1.end());
    std::sort(got2.begin(), got2.end());
    if (!argsOk || got1 != expect1 || got2 != expect2) {
        std::fprintf(stderr, "  %s: draw args (%u + %u) do not match the result words (%zu + %zu)\n", label,
                     out.counts[kCountPhase1Draws], out.counts[kCountPhase2Draws], expect1.size(), expect2.size());
        ++g_failures;
    }
    // Reference: every instance with a pixel in the no-occlusion image must have been drawn.
    const u32* refId = reinterpret_cast<const u32*>(rb(ctx, l.refId));
    const u32* id = reinterpret_cast<const u32*>(rb(ctx, l.id));
    std::vector<u8> seen(n, 0u);
    for (u32 p = 0; p < kWidth * kHeight; ++p) {
        if (refId[p] < n) {
            seen[refId[p]] = 1u;
        }
        out.pixelMismatch += refId[p] != id[p];
    }
    for (u32 i = 0; i < n; ++i) {
        if (seen[i] != 0u) {
            ++out.refVisible;
            if (out.results[i] != kResultPhase1Drawn && out.results[i] != kResultPhase2Drawn) {
                ++out.missing;
            }
        }
    }
}

// --- parity ---------------------------------------------------------------------------------------
struct ParityScene {
    std::vector<InstanceHandle> handles;
    std::vector<u32> movers;
};

void buildParityScene(GpuScene& scene, ParityScene& ps, u32 count, u32 seed) {
    GpuMesh cube{};
    cube.boundsRadius = std::sqrt(3.f);
    cube.triangleCount = 12;
    scene.addMesh(cube);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<f32> u(-1.f, 1.f);
    // Occluders: a few large slabs across the view.
    for (u32 w = 0; w < 6; ++w) {
        InstanceDesc d{};
        d.mesh = 0;
        d.transform = boxTransform(-12.f + 5.f * static_cast<f32>(w), u(rng) * 2.f, -14.f - 3.f * static_cast<f32>(w % 3),
                                   2.2f, 3.5f + u(rng), 0.3f);
        ps.handles.push_back(scene.addInstance(d));
    }
    for (u32 i = static_cast<u32>(ps.handles.size()); i < count; ++i) {
        InstanceDesc d{};
        d.mesh = 0;
        const f32 s = 0.08f + 0.25f * (u(rng) * 0.5f + 0.5f);
        d.transform = boxTransform(u(rng) * 26.f, u(rng) * 12.f, -3.f - 60.f * (u(rng) * 0.5f + 0.5f), s, s, s);
        if (i % 211u == 13u) {
            d.flags &= ~static_cast<u32>(kInstanceVisible);
        }
        ps.handles.push_back(scene.addInstance(d));
        if (i % 29u == 3u) {
            ps.movers.push_back(i);
        }
    }
}

int runParity(Context& ctx) {
    constexpr u32 kInstances = 10000;
    constexpr u32 kFrames = 8;
    std::vector<CullKernelLanguage> languages = {CullKernelLanguage::Slang, CullKernelLanguage::Glsl};
    std::vector<std::vector<std::vector<u32>>> perLanguage; // [language][frame] results
    for (const CullKernelLanguage language : languages) {
        Rig rig;
        if (!initRig(ctx, rig, language, kInstances)) {
            std::printf("  language %s: not built, skipped\n", language == CullKernelLanguage::Slang ? "slang" : "glsl");
            continue;
        }
        if ((language == CullKernelLanguage::Slang) != (std::strcmp(rig.language, "slang") == 0)) {
            continue; // Auto fallback picked the other language: already covered
        }
        GpuScene scene;
        if (!scene.init(gpuDesc(ctx, kInstances + 64u)) || !scene.gpuEnabled()) {
            std::fprintf(stderr, "FAIL: GpuScene GPU init\n");
            return 1;
        }
        ParityScene ps;
        rg::Graph graph;
        FrameState fs;
        HizPyramid prevHiz;
        bool prevValid = false;
        std::vector<std::vector<u32>> frames;
        u32 totalAmbiguous = 0, totalCompared = 0, totalOccluded = 0, totalPhase2 = 0, totalMissing = 0;
        std::printf("  kernels: %s\n", rig.language);
        std::printf("  %5s %8s %8s %8s %8s %8s %9s %8s %8s\n", "frame", "phase1", "phase2", "occluded", "frustum", "cand",
                    "ambiguous", "missing", "pixdiff");
        for (u32 frame = 0; frame < kFrames; ++frame) {
            beginSceneFrame(ctx, scene);
            if (frame == 0) {
                buildParityScene(scene, ps, kInstances, 1234);
            } else {
                for (usize k = 0; k < ps.movers.size(); ++k) {
                    const u32 i = ps.movers[k];
                    const GpuTransform& cur = scene.transform(i);
                    GpuTransform next = cur;
                    next.rows[0][3] += 0.35f * std::sin(static_cast<f32>(frame + k));
                    next.rows[1][3] += 0.25f * std::cos(static_cast<f32>(frame * 3 + k));
                    scene.setTransform(ps.handles[i], next);
                }
            }
            FrameOptions opt{};
            const f32 t = static_cast<f32>(frame);
            opt.viewProj = camera(0.4f * t, 0.5f, 6.f - 0.3f * t, 0.3f * t - 1.f, 0.f, -30.f);
            if (!runFrame(ctx, scene, rig, graph, opt, fs)) {
                std::fprintf(stderr, "FAIL: frame %u failed\n", frame);
                return 1;
            }
            FrameResult r;
            char label[64];
            std::snprintf(label, sizeof(label), "%s frame %u", rig.language, frame);
            analyseFrame(ctx, rig, scene, fs, r, label);
            // CPU brute-force reference on the same inputs.
            HizPyramid curHiz;
            copyPyramid(ctx, rig.culler, fs.layout.hiz1, fs.layout, curHiz);
            const CullConstants& c = rig.culler.constants();
            expect(rig.culler.historyValid() == prevValid, "history flag follows the previous frame's builds");
            CullParityReference ref;
            cull_reference_parity(scene_spans(scene), c, prevValid ? prevHiz.view() : cull_kernel::HizLevels{},
                                  curHiz.view(), ref, fuse::kernel::Backend::CpuParallel);
            u32 differ = 0;
            for (u32 i = 0; i < scene.instanceHighWater(); ++i) {
                if (ref.ambiguous[i] != 0u) {
                    continue;
                }
                ++totalCompared;
                if (ref.results[i] != r.results[i]) {
                    if (differ < 5u) {
                        std::fprintf(stderr, "  %s: instance %u GPU %u != CPU %u\n", label, i, r.results[i], ref.results[i]);
                    }
                    ++differ;
                }
            }
            expect(differ == 0u, "GPU results == CPU brute-force reference (non-boundary instances)");
            expect(ref.ambiguousCount * 100u < scene.instanceHighWater(), "boundary instances < 1%");
            expect(r.missing == 0u, "no reference-visible instance missing after phase 2");
            expect(r.pixelMismatch == 0u, "two-phase ID image == no-occlusion ID image");
            totalAmbiguous += ref.ambiguousCount;
            totalOccluded += r.occluded;
            totalPhase2 += r.counts[kCountPhase2Draws];
            totalMissing += r.missing;
            std::printf("  %5u %8u %8u %8u %8u %8u %9u %8u %8u\n", frame, r.counts[kCountPhase1Draws],
                        r.counts[kCountPhase2Draws], r.occluded, r.frustumCulled, r.counts[kCountCandidates],
                        ref.ambiguousCount, r.missing, r.pixelMismatch);
            prevHiz = HizPyramid{};
            copyPyramid(ctx, rig.culler, fs.layout.hiz2, fs.layout, prevHiz);
            prevValid = true;
            frames.push_back(r.results);
        }
        expect(totalOccluded > 0u && totalPhase2 > 0u, "occlusion culls and phase 2 draws (not vacuous)");
        if (perLanguage.empty()) {
            // Negative control: the same frame recorded without the graph's barriers must make
            // synchronization validation fire (proves the layer watches these passes).
            rg::ExecutorDesc unsafe{};
            unsafe.debugSkipBarriers = true;
            std::unique_ptr<rg::Executor> safe = std::move(ctx.executor);
            ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get(), unsafe);
            const u32 before = g_messages;
            g_quietMessages = true;
            beginSceneFrame(ctx, scene);
            FrameOptions opt{};
            opt.viewProj = camera(0.f, 0.5f, 6.f, -1.f, 0.f, -30.f);
            const bool ran = runFrame(ctx, scene, rig, graph, opt, fs);
            const u32 hazards = g_messages - before;
            g_messages = before;
            g_quietMessages = false;
            ctx.executor.reset();
            ctx.executor = std::move(safe);
            expect(ran && hazards > 0u, "negative control: without barriers sync validation reports hazards");
            std::printf("  negative control (no barriers): %u validation message(s), as expected\n", hazards);
        }
        std::printf("  %s: %u instance decisions compared, %u boundary exclusions, %u occluded, %u phase-2 draws, %u missing\n",
                    rig.language, totalCompared, totalAmbiguous, totalOccluded, totalPhase2, totalMissing);
        perLanguage.push_back(std::move(frames));
        scene.destroy();
        rig.culler.destroy();
        rig.reference.destroy();
    }
    if (perLanguage.empty()) {
        std::printf("SKIP: no culling kernel built\n");
        return kSkip;
    }
    if (perLanguage.size() == 2u) {
        u32 differ = 0;
        for (u32 f = 0; f < kFrames; ++f) {
            differ += perLanguage[0][f] != perLanguage[1][f] ? 1u : 0u;
        }
        expect(differ == 0u, "Slang and GLSL kernels produce identical results every frame");
        std::printf("  slang == glsl: %u / %u frames identical\n", kFrames - differ, kFrames);
    }
    return 0;
}

// --- camera cut -----------------------------------------------------------------------------------
int runCameraCut(Context& ctx) {
    u32 ran = 0;
    for (const CullKernelLanguage language : {CullKernelLanguage::Slang, CullKernelLanguage::Glsl}) {
        Rig rig;
        if (!initRig(ctx, rig, language, 4096) ||
            (language == CullKernelLanguage::Slang) != (std::strcmp(rig.language, "slang") == 0)) {
            continue;
        }
        ++ran;
        GpuScene scene;
        if (!scene.init(gpuDesc(ctx, 4096)) || !scene.gpuEnabled()) {
            std::fprintf(stderr, "FAIL: GpuScene GPU init\n");
            return 1;
        }
        rg::Graph graph;
        FrameState fs;
        GpuMesh cube{};
        cube.boundsRadius = std::sqrt(3.f);
        cube.triangleCount = 12;
        InstanceHandle wall{};
        std::mt19937 rng(77);
        std::uniform_real_distribution<f32> u(-1.f, 1.f);
        // Camera A at z = +8 looking down -z; wall at z = -10; hidden objects in z in [-40, -12];
        // visible objects in front of the wall. Camera B at z = -60 looking back at the wall.
        const Mat4 cameraA = camera(0.f, 0.f, 8.f, 0.f, 0.f, -30.f);
        const Mat4 cameraB = camera(0.f, 0.f, -60.f, 0.f, 0.f, 0.f);
        struct Step {
            const char* name;
            const Mat4* view;
            bool cut;
            bool moveWall;
        };
        const Step steps[] = {{"A", &cameraA, false, false},        {"A", &cameraA, false, false},
                              {"A", &cameraA, false, false},        {"teleport->B", &cameraB, false, false},
                              {"B", &cameraB, false, false},        {"cut->A", &cameraA, true, false},
                              {"A", &cameraA, false, false},        {"wall teleports", &cameraA, false, true},
                              {"A", &cameraA, false, false}};
        std::printf("  kernels: %s\n", rig.language);
        std::printf("  %-16s %8s %8s %8s %8s %8s %8s\n", "step", "refvis", "phase1", "phase2", "occluded", "missing",
                    "pixdiff");
        u32 totalMissing = 0;
        u32 teleportPhase2 = 0;
        u32 wallPhase2 = 0;
        for (u32 s = 0; s < std::size(steps); ++s) {
            beginSceneFrame(ctx, scene);
            if (s == 0) {
                scene.addMesh(cube);
                InstanceDesc w{};
                w.mesh = 0;
                w.transform = boxTransform(0.f, 0.f, -10.f, 30.f, 20.f, 0.5f);
                wall = scene.addInstance(w);
                for (u32 i = 0; i < 1500; ++i) {
                    InstanceDesc d{};
                    d.mesh = 0;
                    const f32 sz = 0.2f + 0.4f * (u(rng) * 0.5f + 0.5f);
                    const bool hidden = i % 3u != 0u;
                    const f32 z = hidden ? -12.f - 28.f * (u(rng) * 0.5f + 0.5f) : -2.f - 6.f * (u(rng) * 0.5f + 0.5f);
                    d.transform = boxTransform(u(rng) * 9.f, u(rng) * 6.f, z, sz, sz, sz);
                    scene.addInstance(d);
                }
            }
            if (steps[s].moveWall) {
                scene.setTransform(wall, boxTransform(0.f, 200.f, -10.f, 30.f, 20.f, 0.5f), true);
            }
            FrameOptions opt{};
            opt.viewProj = *steps[s].view;
            opt.cameraCut = steps[s].cut;
            if (!runFrame(ctx, scene, rig, graph, opt, fs)) {
                std::fprintf(stderr, "FAIL: step %u failed\n", s);
                return 1;
            }
            FrameResult r;
            char label[64];
            std::snprintf(label, sizeof(label), "%s step %u (%s)", rig.language, s, steps[s].name);
            analyseFrame(ctx, rig, scene, fs, r, label);
            expect(!steps[s].cut || !rig.culler.historyValid(), "camera cut drops the history");
            expect(r.missing == 0u, "camera cut: 0 visible objects missing after phase 2");
            expect(r.pixelMismatch == 0u, "camera cut: ID image == no-occlusion reference");
            totalMissing += r.missing;
            if (s == 3u) {
                teleportPhase2 = r.counts[kCountPhase2Draws];
            }
            if (steps[s].moveWall) {
                wallPhase2 = r.counts[kCountPhase2Draws];
            }
            std::printf("  %-16s %8u %8u %8u %8u %8u %8u\n", steps[s].name, r.refVisible, r.counts[kCountPhase1Draws],
                        r.counts[kCountPhase2Draws], r.occluded, r.missing, r.pixelMismatch);
            if (s == 2u) {
                expect(r.occluded > 500u, "camera A: the wall occludes the objects behind it");
            }
        }
        expect(teleportPhase2 > 100u, "teleport: phase 2 draws the objects phase 1 rejected with the stale Hi-Z");
        expect(wallPhase2 > 100u, "wall teleport: disoccluded objects are drawn in phase 2");
        std::printf("  %s: %u missing over %zu steps; teleport phase-2 draws %u, wall-teleport phase-2 draws %u\n",
                    rig.language, totalMissing, std::size(steps), teleportPhase2, wallPhase2);
        scene.destroy();
        rig.culler.destroy();
        rig.reference.destroy();
    }
    if (ran == 0u) {
        std::printf("SKIP: no culling kernel built\n");
        return kSkip;
    }
    return 0;
}

// --- submit_flat ----------------------------------------------------------------------------------
int runSubmitFlat(Context& ctx) {
    const u32 sizes[3] = {1000, 10000, 100000};
    struct Setup {
        GpuScene scene;
        Rig rig;
        rg::Graph graph;
        FrameState fs;
        std::vector<double> buildUs;
        std::vector<double> frameUs;
    };
    std::vector<std::unique_ptr<Setup>> setups;
    for (const u32 n : sizes) {
        auto s = std::make_unique<Setup>();
        if (!initRig(ctx, s->rig, CullKernelLanguage::Auto, n) || !s->scene.init(gpuDesc(ctx, n + 64u))) {
            std::fprintf(stderr, "FAIL: init %u\n", n);
            return 1;
        }
        beginSceneFrame(ctx, s->scene);
        GpuMesh cube{};
        cube.boundsRadius = std::sqrt(3.f);
        cube.triangleCount = 12;
        s->scene.addMesh(cube);
        std::mt19937 rng(n);
        std::uniform_real_distribution<f32> u(-1.f, 1.f);
        for (u32 i = 0; i < n; ++i) {
            InstanceDesc d{};
            d.mesh = 0;
            const f32 sz = 0.05f + 0.1f * (u(rng) * 0.5f + 0.5f);
            d.transform = boxTransform(u(rng) * 30.f, u(rng) * 15.f, -3.f - 80.f * (u(rng) * 0.5f + 0.5f), sz, sz, sz);
            s->scene.addInstance(d);
        }
        s->scene.commit();
        ctx.upload.flush();
        ctx.upload.waitAll();
        setups.push_back(std::move(s));
    }
    // CPU record cost only: the graph is recorded into a command buffer that is never submitted, so
    // no GPU work (lavapipe runs on the same cores) overlaps or precedes a measurement. One real
    // frame per size first checks that the pipeline runs at that size.
    for (auto& s : setups) {
        beginSceneFrame(ctx, s->scene);
        s->scene.commit();
        ctx.upload.flush();
        FrameOptions opt{};
        opt.viewProj = camera(0.f, 0.f, 5.f, 0.f, 0.f, -30.f);
        opt.withReference = false;
        opt.readback = false;
        beginCull(ctx, s->scene, s->rig, opt);
        buildFrame(ctx, s->scene, s->rig, s->graph, opt, s->fs);
        const rg::ExecuteResult result = ctx.executor->execute(s->graph);
        expect(result.ok && ctx.executor->waitIdle() && ctx.upload.waitAll(), "executed frame ok");
    }
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = ctx.device->queues().graphicsFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandBufferCount = 1;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    if (vkCreateCommandPool(ctx.vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS ||
        (allocInfo.commandPool = pool, vkAllocateCommandBuffers(ctx.vkDevice, &allocInfo, &cmd)) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL: command pool\n");
        return 1;
    }
    constexpr u32 kWarmup = 10;
    constexpr u32 kRounds = 200;
    for (u32 round = 0; round < kWarmup + kRounds; ++round) {
        for (usize j = 0; j < setups.size(); ++j) {
            Setup& s = *setups[(j + round) % setups.size()];
            ++ctx.serial;
            FrameOptions opt{};
            opt.viewProj = camera(0.f, 0.f, 5.f, 0.1f * static_cast<f32>(round % 3), 0.f, -30.f);
            opt.withReference = false;
            opt.readback = false;
            vkResetCommandPool(ctx.vkDevice, pool, 0);
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &begin);
            const auto t0 = std::chrono::steady_clock::now();
            beginCull(ctx, s.scene, s.rig, opt);
            buildFrame(ctx, s.scene, s.rig, s.graph, opt, s.fs);
            const auto t1 = std::chrono::steady_clock::now();
            const rg::ExecuteResult result = ctx.executor->recordInline(s.graph, cmd);
            const auto t2 = std::chrono::steady_clock::now();
            vkEndCommandBuffer(cmd);
            expect(result.ok, "record ok");
            if (round >= kWarmup) {
                s.buildUs.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
                s.frameUs.push_back(std::chrono::duration<double, std::micro>(t2 - t0).count());
            }
        }
    }
    vkDestroyCommandPool(ctx.vkDevice, pool, nullptr);
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    std::printf("submit_flat (validation off, median of %u interleaved rounds, recorded, not submitted):\n", kRounds);
    std::printf("  %8s %14s %22s %8s\n", "instances", "graph build us", "build+compile+record us", "ratio");
    const double base = median(setups[0]->frameUs);
    bool flat = true;
    for (usize k = 0; k < setups.size(); ++k) {
        const double b = median(setups[k]->buildUs);
        const double f = median(setups[k]->frameUs);
        const double ratio = f / base;
        flat = flat && ratio >= 0.9 && ratio <= 1.1;
        std::printf("  %8u %14.1f %22.1f %8.3f\n", sizes[k], b, f, ratio);
    }
    expect(flat, "CPU record time flat from 1k to 100k instances (+-10%)");
    for (auto& s : setups) {
        s->scene.destroy();
        s->rig.culler.destroy();
        s->rig.reference.destroy();
    }
    return 0;
}

// --- zero_alloc -----------------------------------------------------------------------------------
thread_local bool t_inCullPass = false;

void hookBegin(const rg::PassContext&, const char* name, void*) {
    if (name != nullptr && std::strncmp(name, "cull.", 5) == 0) {
        t_inCullPass = true;
        t_count = true;
    }
}

void hookEnd(const rg::PassContext&, const char*, void*) {
    if (t_inCullPass) {
        t_inCullPass = false;
        t_count = false;
    }
}

int runZeroAlloc(Context& ctx, bool countAllocations) {
    constexpr u32 kInstances = 10000;
    Rig rig;
    GpuScene scene;
    if (!initRig(ctx, rig, CullKernelLanguage::Auto, kInstances) || !scene.init(gpuDesc(ctx, kInstances + 64u))) {
        std::fprintf(stderr, "FAIL: init\n");
        return 1;
    }
    ParityScene ps;
    rg::Graph graph;
    FrameState fs;
    constexpr u32 kWarmup = 16;
    constexpr u32 kFrames = 80;
    unsigned long long cullSide = 0;
    unsigned long long callbacks = 0;
    unsigned long long frameTotal = 0;
    if (countAllocations) {
        ctx.executor->setPassHooks(rg::PassHooks{&hookBegin, &hookEnd, nullptr});
    }
    for (u32 frame = 0; frame < kFrames; ++frame) {
        // The last frame reads back (a different graph shape): verified, not counted.
        const bool measure = countAllocations && frame >= kWarmup && frame + 1u < kFrames;
        beginSceneFrame(ctx, scene);
        if (frame == 0) {
            buildParityScene(scene, ps, kInstances, 99);
        }
        for (u32 k = 0; k < kInstances / 100u; ++k) {
            const u32 i = ps.movers[(frame * 7u + k) % ps.movers.size()];
            GpuTransform next = scene.transform(i);
            next.rows[1][3] += 0.01f;
            scene.setTransform(ps.handles[i], next);
        }
        scene.commit();
        ctx.upload.flush();
        FrameOptions opt{};
        opt.viewProj = camera(0.05f * static_cast<f32>(frame % 10), 0.5f, 6.f, -1.f, 0.f, -30.f);
        opt.withReference = false;
        opt.readback = (frame + 1u == kFrames);
        // Cull side: every InstanceCuller call of the frame.
        t_allocations = 0;
        t_count = measure;
        beginCull(ctx, scene, rig, opt);
        t_count = false;
        const unsigned long long begin = t_allocations;
        // Graph build: the culler's import / add* / useDraws calls happen inside; count them all.
        t_allocations = 0;
        t_count = measure;
        buildFrame(ctx, scene, rig, graph, opt, fs);
        t_count = false;
        const unsigned long long build = t_allocations;
        // Execute: the hooks count only inside the cull.* pass callbacks.
        t_allocations = 0;
        const rg::ExecuteResult result = ctx.executor->execute(graph);
        const unsigned long long inCallbacks = t_allocations;
        const bool waited = ctx.executor->waitIdle() && ctx.upload.waitAll();
        rig.culler.collectRetired(ctx.serial);
        scene.collectRetired(ctx.serial);
        expect(result.ok && waited, "frame ok");
        if (measure) {
            cullSide += begin + inCallbacks;
            callbacks += inCallbacks;
            frameTotal += build;
        }
    }
    FrameResult r;
    analyseFrame(ctx, rig, scene, fs, r, "zero_alloc last frame");
    ctx.executor->setPassHooks(rg::PassHooks{});
    if (countAllocations) {
        std::printf("zero_alloc (validation layer off: it is C++ and allocates through operator new):\n"
                    "  %u steady-state frames (%u instances, 1%% moved)\n"
                    "  cull side (InstanceCuller::beginFrame + cull.* pass callbacks): %llu operator-new calls "
                    "(callbacks %llu)\n"
                    "  whole graph build (scene + cull imports, cull passes, test draw / copy passes): %llu\n",
                    kFrames - kWarmup - 1u, kInstances, cullSide, callbacks, frameTotal);
        expect(cullSide == 0u, "cull side makes no steady-state heap allocations");
        expect(frameTotal == 0u, "graph build with the cull passes makes no steady-state heap allocations");
    } else {
        std::printf("zero_alloc frames under validation + sync validation: phase-1 %u, phase-2 %u draws\n",
                    r.counts[kCountPhase1Draws], r.counts[kCountPhase2Draws]);
    }
    scene.destroy();
    rig.culler.destroy();
    rig.reference.destroy();
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
    const bool validated = mode != "submit_flat";
    int rc = 0;
    {
        Context ctx;
        const int setupRc = setup(ctx, backend == "buffer", validated);
        if (setupRc != 0) {
            return setupRc;
        }
        if (mode == "parity") {
            rc = runParity(ctx);
        } else if (mode == "camera_cut") {
            rc = runCameraCut(ctx);
        } else if (mode == "submit_flat") {
            rc = runSubmitFlat(ctx);
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
