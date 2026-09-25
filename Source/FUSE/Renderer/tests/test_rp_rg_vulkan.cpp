// WP-0.3 Vulkan gates for render graph v2 (Lavapipe, VK_LAYER_KHRONOS_validation with
// synchronization validation on; every validation message fails the run).
//
//   --mode hazards  (a) 12-pass synthetic graph with RAW / WAR / WAW hazards on images (incl. mip
//                   subresources) and buffers (incl. byte ranges): transfer + compute work, 3
//                   frames, readback equals the CPU expectation, 0 validation messages. Negative
//                   control: the same graph with barrier recording suppressed must trip sync
//                   validation (proves the gate is live).
//   --mode alias    (c) transients with disjoint lifetimes (2 images + 2 buffers) share one VMA
//                   block (vmaCalculateStatistics delta, same VkDeviceMemory/offset); readbacks are
//                   correct and identical to a run with aliasing disabled.
//   --mode async    (d) with --layer-dir: VK_LAYER_FUSE_split_transfer_family (compute family on)
//                   below the validation layer; graphics -> async compute -> transfer -> graphics
//                   with queue-family ownership transfers and timeline waits, 3 frames, results
//                   equal the single-queue run, 0 validation messages.
//   --mode radix    gpu_radix_sort through its render graph passes (u32 and u64 keys, > 32 passes)
//                   under sync validation, sorted output equals std::stable_sort.
//   --mode rtstage  rg::kStageRayTracing (WP-7.3 follow-up): compute write -> raygen storage read, transfer
//                   clear -> raygen sampled read, raygen read -> compute write (WAR), raygen write -> copy;
//                   planned barriers name RAY_TRACING_SHADER, readback correct, 0 validation messages over 3
//                   frames; SBT from the allocator (BufferUsage::ShaderBindingTable). Negative controls: with
//                   barriers suppressed sync validation fires; the same accesses declared at the compute stage
//                   must trip it on vkCmdTraceRaysKHR when the layer tracks trace rays (1.3.275 does not:
//                   reported, not enforced). A T2-capped device drops the SBT usage silently. Skip (77)
//                   without VK_KHR_ray_tracing_pipeline enabled.
//
// Exit 77 = skip (stub build, no ICD / validation layer, test shader not built).
#include <fuse/renderer/compute/gpu_radix_sort.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rg/sync_model.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

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
using fuse::u32;
using fuse::u64;
using fuse::u8;

#if defined(_WIN32)
[[maybe_unused]] int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kSplitLayer = "VK_LAYER_FUSE_split_transfer_family";
constexpr u32 kR32Uint = VK_FORMAT_R32_UINT;

// --- validation capture -----------------------------------------------------------------------

struct MessageLog {
    u32 count = 0;
    bool quiet = false;
    std::vector<std::string> ids;
    u32 traceRaysHazards = 0; ///< SYNC-HAZARD messages naming vkCmdTraceRaysKHR (--mode rtstage)
};
MessageLog g_log;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) ==
            0 ||
        (type & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    ++g_log.count;
    const char* id = data != nullptr && data->pMessageIdName != nullptr ? data->pMessageIdName : "(no id)";
    g_log.ids.emplace_back(id);
    if (std::strstr(id, "SYNC-HAZARD") != nullptr && data->pMessage != nullptr &&
        std::strstr(data->pMessage, "vkCmdTraceRaysKHR") != nullptr) {
        ++g_log.traceRaysHazards;
    }
    if (!g_log.quiet) {
        std::fprintf(stderr, "VALIDATION MESSAGE: %s\n  %s\n", id,
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

// --- context ------------------------------------------------------------------------------------

struct Context {
    std::unique_ptr<VulkanInstance> instance;
    std::unique_ptr<VulkanDevice> device;
    std::unique_ptr<GpuAllocator> allocator;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    bool validation = false;
    VkDevice vkDevice = VK_NULL_HANDLE;
    // rg_test_add.comp
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
            if (pool != VK_NULL_HANDLE) {
                vkDestroyDescriptorPool(vkDevice, pool, nullptr);
            }
            if (pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(vkDevice, pipeline, nullptr);
            }
            if (pipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(vkDevice, pipelineLayout, nullptr);
            }
            if (setLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(vkDevice, setLayout, nullptr);
            }
        }
        allocator.reset();
        device.reset();
        if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr && instance != nullptr) {
            destroyMessenger(static_cast<VkInstance>(instance->nativeHandle()), messenger, nullptr);
        }
        instance.reset();
    }
};

/// Returns 0 on success, kSkip when the environment cannot run the gate, 1 on failure.
int setupContext(Context& ctx, const char* layerDir, bool splitCompute) {
    ctx.validation = layerAvailable(kValidationLayer);
    std::string layers = ctx.validation ? kValidationLayer : "";
    if (layerDir != nullptr) {
        std::string path = layerDir;
        const char* existing = std::getenv("VK_ADD_LAYER_PATH");
        if (existing != nullptr && existing[0] != '\0') {
            path += ":";
            path += existing;
        }
        setenv("VK_ADD_LAYER_PATH", path.c_str(), 1);
        layers += layers.empty() ? "" : ":";
        layers += kSplitLayer; // application -> driver: validation sees the split device
    }
    setenv("FUSE_SPLIT_LAYER_COMPUTE_FAMILY", splitCompute ? "1" : "0", 1);
    setenv("VK_INSTANCE_LAYERS", layers.c_str(), 1);
    if (ctx.validation) {
        setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
        setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
        setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);
    } else {
        std::printf("SKIP: %s not installed (the gate needs synchronization validation)\n", kValidationLayer);
        return kSkip;
    }
    if (layerDir != nullptr && !layerAvailable(kSplitLayer)) {
        std::fprintf(stderr, "FAIL: %s not found under %s\n", kSplitLayer, layerDir);
        return 1;
    }

    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_rp_rg_vulkan";
    instanceDesc.enableValidation = true;
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
    if (createMessenger != nullptr) {
        VkDebugUtilsMessengerCreateInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = onMessage;
        createMessenger(vkInstance, &info, nullptr, &ctx.messenger);
    }
    ctx.device = VulkanDevice::create(*ctx.instance);
    if (ctx.device == nullptr || !ctx.device->isValid()) {
        std::printf("SKIP: no Vulkan device\n");
        return kSkip;
    }
    ctx.vkDevice = static_cast<VkDevice>(ctx.device->nativeHandle());
    ctx.allocator = GpuAllocator::create(*ctx.device);
    if (ctx.allocator == nullptr || !ctx.allocator->isValid()) {
        std::fprintf(stderr, "FAIL: GpuAllocator\n");
        return 1;
    }
    return 0;
}

int setupComputePipeline(Context& ctx) {
#if defined(FUSE_RG_TEST_SHADER)
    std::ifstream file(FUSE_RG_TEST_SHADER, std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (code.empty() || code.size() % 4u != 0u) {
        std::printf("SKIP: %s not readable\n", FUSE_RG_TEST_SHADER);
        return kSkip;
    }
    VkDescriptorSetLayoutBinding bindings[2]{};
    for (u32 i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 2;
    setInfo.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx.vkDevice, &setInfo, nullptr, &ctx.setLayout);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &ctx.setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    vkCreatePipelineLayout(ctx.vkDevice, &layoutInfo, nullptr, &ctx.pipelineLayout);
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = code.size();
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(ctx.vkDevice, &moduleInfo, nullptr, &module);
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = ctx.pipelineLayout;
    const VkResult created = vkCreateComputePipelines(ctx.vkDevice, VK_NULL_HANDLE, 1, &info, nullptr, &ctx.pipeline);
    vkDestroyShaderModule(ctx.vkDevice, module, nullptr);
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 32;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    vkCreateDescriptorPool(ctx.vkDevice, &poolInfo, nullptr, &ctx.pool);
    return created == VK_SUCCESS ? 0 : 1;
#else
    (void)ctx;
    std::printf("SKIP: rg_test_add.comp not built (glslangValidator missing)\n");
    return kSkip;
#endif
}

struct HostBuffer {
    Buffer buffer{};
    GpuAllocator* allocator = nullptr;
    ~HostBuffer() {
        if (allocator != nullptr && buffer.handle != nullptr) {
            allocator->destroyBuffer(buffer);
        }
    }
};

bool createReadback(Context& ctx, HostBuffer& out, u64 size, const char* name) {
    BufferDesc desc{};
    desc.size = static_cast<fuse::usize>(size);
    desc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::TransferDst) |
                                          static_cast<u32>(BufferUsage::Storage));
    desc.memoryUsage = MemoryUsage::GpuToCpu;
    desc.name = name;
    out.allocator = ctx.allocator.get();
    return ctx.allocator->createBuffer(desc, out.buffer) && out.buffer.mapped != nullptr;
}

std::vector<u32> readWords(Context& ctx, const HostBuffer& buffer, u64 offsetBytes, u32 words) {
    std::vector<u32> out(words, 0u);
    ctx.allocator->readMapped(buffer.buffer, out.data(), words * sizeof(u32), static_cast<fuse::usize>(offsetBytes));
    return out;
}

bool allEqual(const std::vector<u32>& words, u32 value) {
    return std::all_of(words.begin(), words.end(), [&](u32 w) { return w == value; });
}

// --- pass callbacks -----------------------------------------------------------------------------

struct FillPass {
    rg::BufferRef buffer;
    u32 value = 0;
};
void fillFn(const rg::PassContext& c, void* user) {
    const auto* p = static_cast<const FillPass*>(user);
    vkCmdFillBuffer(static_cast<VkCommandBuffer>(c.commandBuffer), static_cast<VkBuffer>(c.buffer(p->buffer)), 0,
                    VK_WHOLE_SIZE, p->value);
}

struct AddPass {
    Context* ctx = nullptr;
    rg::BufferRef src, dst;
    u32 count = 0;
    u32 add = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;
};
void addFn(const rg::PassContext& c, void* user) {
    auto* p = static_cast<AddPass*>(user);
    VkDescriptorBufferInfo infos[2] = {{static_cast<VkBuffer>(c.buffer(p->src)), 0, VK_WHOLE_SIZE},
                                       {static_cast<VkBuffer>(c.buffer(p->dst)), 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2]{};
    for (u32 i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = p->set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(p->ctx->vkDevice, 2, writes, 0, nullptr);
    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(c.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->ctx->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->ctx->pipelineLayout, 0, 1, &p->set, 0, nullptr);
    const u32 push[2] = {p->count, p->add};
    vkCmdPushConstants(cmd, p->ctx->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(cmd, (p->count + 63u) / 64u, 1, 1);
}

struct ClearPass {
    rg::TextureRef image;
    u32 mip = 0;
    VkClearColorValue color{};
};
void clearFn(const rg::PassContext& c, void* user) {
    const auto* p = static_cast<const ClearPass*>(user);
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, p->mip, 1, 0, 1};
    vkCmdClearColorImage(static_cast<VkCommandBuffer>(c.commandBuffer), static_cast<VkImage>(c.image(p->image)),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &p->color, 1, &range);
}

struct BufferToImagePass {
    rg::BufferRef src;
    rg::TextureRef dst;
    u32 mip = 0;
    u32 extent = 0;
};
void bufferToImageFn(const rg::PassContext& c, void* user) {
    const auto* p = static_cast<const BufferToImagePass*>(user);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, p->mip, 0, 1};
    region.imageExtent = {p->extent, p->extent, 1};
    vkCmdCopyBufferToImage(static_cast<VkCommandBuffer>(c.commandBuffer), static_cast<VkBuffer>(c.buffer(p->src)),
                           static_cast<VkImage>(c.image(p->dst)), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

struct ImageCopyPass {
    rg::TextureRef src, dst;
    u32 extent = 0;
};
void imageCopyFn(const rg::PassContext& c, void* user) {
    const auto* p = static_cast<const ImageCopyPass*>(user);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {p->extent, p->extent, 1};
    vkCmdCopyImage(static_cast<VkCommandBuffer>(c.commandBuffer), static_cast<VkImage>(c.image(p->src)),
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkImage>(c.image(p->dst)),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

struct ImageToBufferPass {
    rg::TextureRef src;
    rg::BufferRef dst;
    u32 mip = 0;
    u32 extent = 0;
    u64 offset = 0;
};
void imageToBufferFn(const rg::PassContext& c, void* user) {
    const auto* p = static_cast<const ImageToBufferPass*>(user);
    VkBufferImageCopy region{};
    region.bufferOffset = p->offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, p->mip, 0, 1};
    region.imageExtent = {p->extent, p->extent, 1};
    vkCmdCopyImageToBuffer(static_cast<VkCommandBuffer>(c.commandBuffer), static_cast<VkImage>(c.image(p->src)),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkBuffer>(c.buffer(p->dst)), 1, &region);
}

struct BufferCopyPass {
    rg::BufferRef src, dst;
    u64 srcOffset = 0, dstOffset = 0, size = 0;
};
void bufferCopyFn(const rg::PassContext& c, void* user) {
    const auto* p = static_cast<const BufferCopyPass*>(user);
    VkBufferCopy region{p->srcOffset, p->dstOffset, p->size};
    vkCmdCopyBuffer(static_cast<VkCommandBuffer>(c.commandBuffer), static_cast<VkBuffer>(c.buffer(p->src)),
                    static_cast<VkBuffer>(c.buffer(p->dst)), 1, &region);
}

VkDescriptorSet allocateSet(Context& ctx) {
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = ctx.pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &ctx.setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(ctx.vkDevice, &info, &set);
    return set;
}

// --- (a) hazards ----------------------------------------------------------------------------------

constexpr u32 kWords = 256; // B1 / B2 elements
constexpr u32 kExtent = 16; // I1 mip 0 / I2

struct HazardGraph {
    FillPass fill1, fill2;
    AddPass add1, add2;
    ClearPass clear1, clear2;
    BufferToImagePass toMip1;
    ImageCopyPass copyI1I2;
    ImageToBufferPass outI2, outMip1;
    BufferCopyPass outB2;
};

void buildHazardGraph(rg::Graph& g, HazardGraph& h, Context& ctx, VkBuffer out) {
    g.reset();
    const rg::BufferRef b1 = g.createBuffer({kWords * 4u, 0, "rg.b1"});
    const rg::BufferRef b2 = g.createBuffer({kWords * 4u, 0, "rg.b2"});
    rg::ImageDesc i1Desc;
    i1Desc.width = kExtent;
    i1Desc.height = kExtent;
    i1Desc.mipLevels = 2;
    i1Desc.format = kR32Uint;
    i1Desc.name = "rg.i1";
    const rg::TextureRef i1 = g.createImage(i1Desc);
    rg::ImageDesc i2Desc = i1Desc;
    i2Desc.mipLevels = 1;
    i2Desc.name = "rg.i2";
    const rg::TextureRef i2 = g.createImage(i2Desc);
    const rg::BufferRef outRef = g.importBuffer({out, 3072, rg::kNoQueue, nullptr, "rg.out"});

    h.fill1 = {b1, 1u};
    h.fill2 = {b1, 5u};
    h.add1 = {&ctx, b1, b2, kWords, 10u, allocateSet(ctx)};
    h.add2 = {&ctx, b1, b2, kWords, 10u, allocateSet(ctx)};
    h.clear1 = {i1, 0, {}};
    h.clear1.color.uint32[0] = 7u;
    h.clear2 = {i1, 0, {}};
    h.clear2.color.uint32[0] = 9u;
    h.toMip1 = {b2, i1, 1, kExtent / 2u};
    h.copyI1I2 = {i1, i2, kExtent};
    h.outI2 = {i2, outRef, 0, kExtent, 0};
    h.outMip1 = {i1, outRef, 1, kExtent / 2u, 1024};
    h.outB2 = {b2, outRef, 0, 2048, kWords * 4u};

    constexpr u8 kCs = rg::kStageCompute;
    g.addPass("01.fill_b1", fillFn, &h.fill1).use(b1, rg::Access::TransferDst);
    g.addPass("02.add_b1_to_b2", addFn, &h.add1)
        .use(b1, rg::Access::StorageRead, {}, kCs) // RAW b1
        .use(b2, rg::Access::StorageWrite, {}, kCs);
    g.addPass("03.refill_b1", fillFn, &h.fill2).use(b1, rg::Access::TransferDst); // WAR b1
    g.addPass("04.clear_i1_mip0", clearFn, &h.clear1).use(i1, rg::Access::TransferDst, {0, 1, 0, 1});
    g.addPass("05.b2_to_i1_mip1", bufferToImageFn, &h.toMip1)
        .use(b2, rg::Access::TransferSrc, {0, 256}) // RAW b2 (byte range)
        .use(i1, rg::Access::TransferDst, {1, 1, 0, 1});
    g.addPass("06.clear_i1_mip0_again", clearFn, &h.clear2).use(i1, rg::Access::TransferDst, {0, 1, 0, 1}); // WAW
    g.addPass("07.copy_i1_to_i2", imageCopyFn, &h.copyI1I2)
        .use(i1, rg::Access::TransferSrc, {0, 1, 0, 1}) // RAW i1 mip 0
        .use(i2, rg::Access::TransferDst);
    g.addPass("08.add_b1_to_b2_again", addFn, &h.add2)
        .use(b1, rg::Access::StorageRead, {}, kCs)   // RAW b1 (refill)
        .use(b2, rg::Access::StorageWrite, {}, kCs); // WAR (pass 5 read) + WAW (pass 2)
    g.addPass("09.i2_to_out", imageToBufferFn, &h.outI2)
        .use(i2, rg::Access::TransferSrc)
        .use(outRef, rg::Access::TransferDst, {0, 1024});
    g.addPass("10.i1_mip1_to_out", imageToBufferFn, &h.outMip1)
        .use(i1, rg::Access::TransferSrc, {1, 1, 0, 1})
        .use(outRef, rg::Access::TransferDst, {1024, 256});
    g.addPass("11.b2_to_out", bufferCopyFn, &h.outB2)
        .use(b2, rg::Access::TransferSrc)
        .use(outRef, rg::Access::TransferDst, {2048, 1024});
    g.addPass("12.host_read", nullptr, nullptr).use(outRef, rg::Access::HostRead);
}

int runHazards(Context& ctx) {
    HostBuffer out;
    if (!createReadback(ctx, out, 3072, "rg.out")) {
        std::fprintf(stderr, "FAIL: readback buffer\n");
        return 1;
    }
    auto executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    expect(executor->isValid(), "executor created");
    if (!executor->isValid()) {
        std::fprintf(stderr, "executor: %s\n", executor->message().c_str());
        return 1;
    }
    std::printf("executor: %s, debug labels %s\n", executor->message().c_str(), executor->debugLabels() ? "on" : "off");
    rg::Graph graph;
    HazardGraph h;
    for (u32 frame = 0; frame < 3; ++frame) {
        std::memset(out.buffer.mapped, 0, 3072);
        vkResetDescriptorPool(ctx.vkDevice, ctx.pool, 0);
        buildHazardGraph(graph, h, ctx, static_cast<VkBuffer>(out.buffer.handle));
        const rg::ExecuteResult result = executor->execute(graph);
        expect(result.ok, "hazard graph executed");
        expect(executor->waitIdle(), "hazard graph retired");
        const rg::CompileStats& stats = graph.stats();
        expect(stats.executedPasses == 12u && result.executedPasses == 12u, "12 passes executed");
        expect(stats.passesWithBarriers >= 11u, "(almost) every pass carries its own derived barrier batch");
        expect(result.barrierCalls >= 11u, "one vkCmdPipelineBarrier[2] per pass batch");
        if (executor->debugLabels()) {
            expect(result.debugLabels == 12u, "one debug label per pass");
        }
        if (frame > 0u) {
            expect(!result.transientsRebuilt, "steady-state frame reuses the transient heap");
        }
        expect(allEqual(readWords(ctx, out, 0, kExtent * kExtent), 9u), "I2 = second clear of I1 mip 0 (9)");
        expect(allEqual(readWords(ctx, out, 1024, 64), 11u), "I1 mip 1 = first B2 (1 + 10)");
        expect(allEqual(readWords(ctx, out, 2048, kWords), 15u), "B2 = refilled B1 + 10 (15)");
        std::printf("frame %u: %u passes, %u barrier calls (%u image, %u buffer barriers), %u transients in %u "
                    "allocations\n",
                    frame, result.executedPasses, result.barrierCalls, result.imageBarriers, result.bufferBarriers,
                    executor->transientStats().transients, executor->transientStats().allocations);
    }
    const u32 clean = g_log.count;
    expect(clean == 0u, "sync validation clean on the 12-pass hazard graph");

    // Negative control: identical graph, no barriers recorded -> synchronization validation fires.
    rg::ExecutorDesc noBarriers{};
    noBarriers.debugSkipBarriers = true;
    auto unsafe = rg::Executor::create(*ctx.device, ctx.allocator.get(), noBarriers);
    g_log.quiet = true;
    const u32 before = g_log.count;
    vkResetDescriptorPool(ctx.vkDevice, ctx.pool, 0);
    buildHazardGraph(graph, h, ctx, static_cast<VkBuffer>(out.buffer.handle));
    unsafe->execute(graph);
    unsafe->waitIdle();
    u32 hazards = 0;
    for (u32 i = before; i < g_log.ids.size(); ++i) {
        if (g_log.ids[i].find("SYNC-HAZARD") != std::string::npos) {
            ++hazards;
        }
    }
    std::printf("negative control (barriers suppressed): %u messages, %u SYNC-HAZARD\n", g_log.count - before, hazards);
    expect(hazards > 0u, "negative control: sync validation reports hazards without the graph's barriers");
    unsafe.reset();
    g_log.quiet = false;
    g_log.count = clean;
    return 0;
}

// --- (c) aliasing -------------------------------------------------------------------------------

struct AliasGraph {
    ClearPass clearA, clearB;
    ImageToBufferPass outA, outB;
    FillPass fillC, fillD;
    BufferCopyPass outC, outD;
};

constexpr u32 kAliasExtent = 64;
constexpr u64 kAliasImageBytes = kAliasExtent * kAliasExtent * 4u;
constexpr u64 kAliasBufferBytes = 4096;
constexpr u64 kAliasOutBytes = 2 * kAliasImageBytes + 2 * kAliasBufferBytes;

struct AliasRefs {
    rg::TextureRef a, b;
    rg::BufferRef c, d;
};

AliasRefs buildAliasGraph(rg::Graph& g, AliasGraph& h, VkBuffer out) {
    g.reset();
    rg::ImageDesc desc;
    desc.width = kAliasExtent;
    desc.height = kAliasExtent;
    desc.format = VK_FORMAT_R8G8B8A8_UNORM;
    desc.name = "rg.alias_a";
    AliasRefs refs;
    refs.a = g.createImage(desc);
    desc.name = "rg.alias_b";
    refs.b = g.createImage(desc);
    refs.c = g.createBuffer({kAliasBufferBytes, 0, "rg.alias_c"});
    refs.d = g.createBuffer({kAliasBufferBytes, 0, "rg.alias_d"});
    const rg::BufferRef outRef = g.importBuffer({out, kAliasOutBytes, rg::kNoQueue, nullptr, "rg.alias_out"});
    h.clearA = {refs.a, 0, {}};
    h.clearA.color.float32[0] = 1.f;
    h.clearA.color.float32[3] = 1.f;
    h.clearB = {refs.b, 0, {}};
    h.clearB.color.float32[1] = 1.f;
    h.clearB.color.float32[3] = 1.f;
    h.outA = {refs.a, outRef, 0, kAliasExtent, 0};
    h.outB = {refs.b, outRef, 0, kAliasExtent, kAliasImageBytes};
    h.fillC = {refs.c, 0xC0FFEEu};
    h.fillD = {refs.d, 0xD00Du};
    h.outC = {refs.c, outRef, 0, 2 * kAliasImageBytes, kAliasBufferBytes};
    h.outD = {refs.d, outRef, 0, 2 * kAliasImageBytes + kAliasBufferBytes, kAliasBufferBytes};
    g.addPass("clear_a", clearFn, &h.clearA).use(refs.a, rg::Access::TransferDst);
    g.addPass("a_to_out", imageToBufferFn, &h.outA)
        .use(refs.a, rg::Access::TransferSrc)
        .use(outRef, rg::Access::TransferDst, {0, kAliasImageBytes});
    g.addPass("clear_b", clearFn, &h.clearB).use(refs.b, rg::Access::TransferDst);
    g.addPass("b_to_out", imageToBufferFn, &h.outB)
        .use(refs.b, rg::Access::TransferSrc)
        .use(outRef, rg::Access::TransferDst, {kAliasImageBytes, kAliasImageBytes});
    g.addPass("fill_c", fillFn, &h.fillC).use(refs.c, rg::Access::TransferDst);
    g.addPass("c_to_out", bufferCopyFn, &h.outC)
        .use(refs.c, rg::Access::TransferSrc)
        .use(outRef, rg::Access::TransferDst, {2 * kAliasImageBytes, kAliasBufferBytes});
    g.addPass("fill_d", fillFn, &h.fillD).use(refs.d, rg::Access::TransferDst);
    g.addPass("d_to_out", bufferCopyFn, &h.outD)
        .use(refs.d, rg::Access::TransferSrc)
        .use(outRef, rg::Access::TransferDst, {2 * kAliasImageBytes + kAliasBufferBytes, kAliasBufferBytes});
    g.addPass("host_read", nullptr, nullptr).use(outRef, rg::Access::HostRead);
    return refs;
}

std::vector<u8> runAlias(Context& ctx, bool aliasing, rg::TransientStats& statsOut, bool& sharedOut) {
    HostBuffer out;
    std::vector<u8> bytes;
    if (!createReadback(ctx, out, kAliasOutBytes, "rg.alias_out")) {
        expect(false, "alias readback buffer");
        return bytes;
    }
    rg::ExecutorDesc desc{};
    desc.enableAliasing = aliasing;
    auto executor = rg::Executor::create(*ctx.device, ctx.allocator.get(), desc);
    rg::Graph graph;
    AliasGraph h;
    for (u32 frame = 0; frame < 2; ++frame) {
        std::memset(out.buffer.mapped, 0, kAliasOutBytes);
        const AliasRefs refs = buildAliasGraph(graph, h, static_cast<VkBuffer>(out.buffer.handle));
        const rg::ExecuteResult result = executor->execute(graph);
        expect(result.ok && executor->waitIdle(), "alias graph executed");
        rg::TransientMemory a{}, b{}, c{}, d{};
        executor->transientMemory(graph, refs.a, a);
        executor->transientMemory(graph, refs.b, b);
        executor->transientMemory(graph, refs.c, c);
        executor->transientMemory(graph, refs.d, d);
        sharedOut = a.deviceMemory != nullptr && a.deviceMemory == b.deviceMemory && a.offset == b.offset &&
                    c.deviceMemory == a.deviceMemory && d.deviceMemory == a.deviceMemory;
        expect(graph.lifetime(refs.a).last < graph.lifetime(refs.b).first, "A dies before B is born");
        if (frame == 1u) {
            expect(!result.transientsRebuilt, "second frame reuses the transient heap");
        }
    }
    statsOut = executor->transientStats();
    bytes.resize(kAliasOutBytes);
    ctx.allocator->readMapped(out.buffer, bytes.data(), bytes.size());
    return bytes;
}

bool checkAliasBytes(const std::vector<u8>& bytes) {
    if (bytes.size() != kAliasOutBytes) {
        return false;
    }
    for (u64 i = 0; i < kAliasExtent * kAliasExtent; ++i) {
        const u8* a = &bytes[i * 4u];
        const u8* b = &bytes[kAliasImageBytes + i * 4u];
        if (a[0] != 255 || a[1] != 0 || a[2] != 0 || a[3] != 255 || b[0] != 0 || b[1] != 255 || b[2] != 0 ||
            b[3] != 255) {
            return false;
        }
    }
    for (u64 w = 0; w < kAliasBufferBytes / 4u; ++w) {
        u32 c = 0, d = 0;
        std::memcpy(&c, &bytes[2 * kAliasImageBytes + w * 4u], 4);
        std::memcpy(&d, &bytes[2 * kAliasImageBytes + kAliasBufferBytes + w * 4u], 4);
        if (c != 0xC0FFEEu || d != 0xD00Du) {
            return false;
        }
    }
    return true;
}

int runAliasMode(Context& ctx) {
    rg::TransientStats aliased{}, separate{};
    bool shared = false;
    bool sharedOff = true;
    const std::vector<u8> withAliasing = runAlias(ctx, true, aliased, shared);
    const std::vector<u8> withoutAliasing = runAlias(ctx, false, separate, sharedOff);
    std::printf("aliasing on : %u transients, %u allocations (VMA +%u), %u aliased, %llu B required, %llu B allocated\n",
                aliased.transients, aliased.allocations, aliased.vmaAllocationDelta, aliased.aliasedResources,
                static_cast<unsigned long long>(aliased.bytesRequired),
                static_cast<unsigned long long>(aliased.bytesAllocated));
    std::printf("aliasing off: %u transients, %u allocations (VMA +%u), %llu B allocated\n", separate.transients,
                separate.allocations, separate.vmaAllocationDelta,
                static_cast<unsigned long long>(separate.bytesAllocated));
    expect(aliased.transients == 4u && aliased.allocations == 1u && aliased.aliasedResources == 3u,
           "4 transients with disjoint lifetimes share 1 block");
#if defined(FUSE_RHI_ALLOCATOR_VMA)
    expect(aliased.vmaAllocationDelta == 1u, "VMA statistics: the transient heap is one allocation");
    expect(separate.vmaAllocationDelta == 4u, "VMA statistics: aliasing off -> one allocation per transient");
#endif
    expect(shared, "A and B (and C, D) are bound to the same VkDeviceMemory at the same offset");
    expect(!sharedOff && separate.allocations == 4u, "aliasing off: separate memory");
    expect(aliased.bytesAllocated < separate.bytesAllocated, "aliasing reduces transient memory");
    expect(checkAliasBytes(withAliasing), "aliased readbacks correct");
    expect(withAliasing == withoutAliasing, "aliased output identical to the non-aliased run");
    expect(g_log.count == 0u, "sync validation clean with aliased transients");
    return 0;
}

// --- (d) async ------------------------------------------------------------------------------------

struct AsyncGraph {
    FillPass fillX;
    ClearPass clearI;
    AddPass addXY;
    ImageToBufferPass iToZ;
    BufferCopyPass yToW, wToOut, zToOut;
};

void buildAsyncGraph(rg::Graph& g, AsyncGraph& h, Context& ctx, VkBuffer out) {
    g.reset();
    const rg::BufferRef x = g.createBuffer({kWords * 4u, 0, "rg.async_x"});
    const rg::BufferRef y = g.createBuffer({kWords * 4u, 0, "rg.async_y"});
    const rg::BufferRef z = g.createBuffer({kWords * 4u, 0, "rg.async_z"});
    const rg::BufferRef w = g.createBuffer({kWords * 4u, 0, "rg.async_w"});
    rg::ImageDesc desc;
    desc.width = kExtent;
    desc.height = kExtent;
    desc.format = kR32Uint;
    desc.name = "rg.async_i";
    const rg::TextureRef img = g.createImage(desc);
    const rg::BufferRef outRef = g.importBuffer({out, 2048, rg::kNoQueue, nullptr, "rg.async_out"});
    h.fillX = {x, 3u};
    h.clearI = {img, 0, {}};
    h.clearI.color.uint32[0] = 4u;
    h.addXY = {&ctx, x, y, kWords, 10u, allocateSet(ctx)};
    h.iToZ = {img, z, 0, kExtent, 0};
    h.yToW = {y, w, 0, 0, kWords * 4u};
    h.wToOut = {w, outRef, 0, 0, kWords * 4u};
    h.zToOut = {z, outRef, 0, 1024, kWords * 4u};
    using rg::QueueClass;
    g.addPass("g.fill_x", fillFn, &h.fillX, QueueClass::Graphics).use(x, rg::Access::TransferDst);
    g.addPass("g.clear_i", clearFn, &h.clearI, QueueClass::Graphics).use(img, rg::Access::TransferDst);
    g.addPass("c.add_x_to_y", addFn, &h.addXY, QueueClass::AsyncCompute)
        .use(x, rg::Access::StorageRead)
        .use(y, rg::Access::StorageWrite);
    g.addPass("c.i_to_z", imageToBufferFn, &h.iToZ, QueueClass::AsyncCompute)
        .use(img, rg::Access::TransferSrc)
        .use(z, rg::Access::TransferDst);
    g.addPass("t.y_to_w", bufferCopyFn, &h.yToW, QueueClass::Transfer)
        .use(y, rg::Access::TransferSrc)
        .use(w, rg::Access::TransferDst);
    g.addPass("g.w_to_out", bufferCopyFn, &h.wToOut, QueueClass::Graphics)
        .use(w, rg::Access::TransferSrc)
        .use(outRef, rg::Access::TransferDst, {0, 1024});
    g.addPass("g.z_to_out", bufferCopyFn, &h.zToOut, QueueClass::Graphics)
        .use(z, rg::Access::TransferSrc)
        .use(outRef, rg::Access::TransferDst, {1024, 1024});
    g.addPass("g.host_read", nullptr, nullptr, QueueClass::Graphics).use(outRef, rg::Access::HostRead);
}

std::vector<u32> runAsync(Context& ctx, bool multiQueue, rg::CompileStats& statsOut, rg::ExecuteResult& resultOut,
                          bool timelineEdges = false) {
    HostBuffer out;
    std::vector<u32> words;
    if (!createReadback(ctx, out, 2048, "rg.async_out")) {
        expect(false, "async readback buffer");
        return words;
    }
    rg::ExecutorDesc desc{};
    desc.enableAsyncCompute = multiQueue;
    desc.enableTransferQueue = multiQueue;
    desc.timelineCrossQueueWaits = timelineEdges;
    auto executor = rg::Executor::create(*ctx.device, ctx.allocator.get(), desc);
    if (multiQueue) {
        expect(executor->queueAvailable(rg::QueueClass::AsyncCompute), "split layer: async compute queue available");
        expect(executor->queueAvailable(rg::QueueClass::Transfer), "split layer: transfer queue available");
        std::printf("families: graphics %u, async compute %u, transfer %u\n",
                    executor->queueFamily(rg::QueueClass::Graphics), executor->queueFamily(rg::QueueClass::AsyncCompute),
                    executor->queueFamily(rg::QueueClass::Transfer));
    }
    rg::Graph graph;
    AsyncGraph h;
    for (u32 frame = 0; frame < 3; ++frame) {
        std::memset(out.buffer.mapped, 0, 2048);
        vkResetDescriptorPool(ctx.vkDevice, ctx.pool, 0);
        buildAsyncGraph(graph, h, ctx, static_cast<VkBuffer>(out.buffer.handle));
        resultOut = executor->execute(graph);
        expect(resultOut.ok, "async graph executed");
        // Frame N+1 is recorded while frame N may still run (frames in flight); only the last
        // frame is waited for explicitly.
        if (frame == 2u) {
            expect(executor->waitIdle(), "async graph retired");
        } else {
            executor->waitIdle(); // descriptor pool is reset per frame
        }
        statsOut = graph.stats();
    }
    words = readWords(ctx, out, 0, 512);
    return words;
}

int runAsyncMode(Context& ctx) {
    rg::CompileStats multi{}, single{}, timeline{};
    rg::ExecuteResult multiResult{}, singleResult{}, timelineResult{};
    const std::vector<u32> multiWords = runAsync(ctx, true, multi, multiResult);
    const u32 multiMessages = g_log.count;
    const std::vector<u32> singleWords = runAsync(ctx, false, single, singleResult);
    std::printf("multi-queue : %u batches, %u submissions, %u ownership transfers, %u cross-queue edges (%u binary "
                "semaphore waits), %u timeline waits, %u queue fallbacks\n",
                multi.batchCount, multiResult.submissions, multi.ownershipTransfers, multi.crossQueueWaits,
                multiResult.binaryWaits, multiResult.timelineWaits, multi.queueFallbacks);
    std::printf("single-queue: %u batches, %u submissions, %u ownership transfers\n", single.batchCount,
                singleResult.submissions, single.ownershipTransfers);
    expect(multi.batchCount == 4u && multiResult.submissions == 4u, "graphics, compute, transfer, graphics batches");
    expect(multi.ownershipTransfers >= 5u, "x, img (g->c), y (c->t), w (t->g), z (c->g) ownership transfers");
    expect(multiResult.binaryWaits == 4u, "4 cross-queue edges (g->c, c->t, t->g, c->g)");
    expect(multiResult.timelineWaits >= 2u, "cross-frame timeline waits on the other queues");
    expect(multi.queueFallbacks == 0u, "no queue fallbacks");
    expect(single.batchCount == 1u && single.ownershipTransfers == 0u, "single queue: 1 batch, no transfers");
    expect(multiWords.size() == 512u && std::all_of(multiWords.begin(), multiWords.begin() + 256, [](u32 v) { return v == 13u; }),
           "y = x + 10 (13) after compute -> transfer -> graphics");
    expect(multiWords.size() == 512u && std::all_of(multiWords.begin() + 256, multiWords.end(), [](u32 v) { return v == 4u; }),
           "z = cleared image (4) after graphics -> compute -> graphics");
    expect(multiWords == singleWords, "multi-queue result equals the single-queue result");
    expect(multiMessages == 0u && g_log.count == 0u,
           "sync validation clean across queues (QFOT + semaphores + timeline pacing)");

    // Timeline-semaphore edges (ExecutorDesc::timelineCrossQueueWaits): functional check only —
    // synchronization validation before layer 1.3.290 does not follow timeline waits, so its
    // messages are reported but not gated here.
    const u32 before = g_log.count;
    g_log.quiet = true;
    const std::vector<u32> timelineWords = runAsync(ctx, true, timeline, timelineResult, true);
    g_log.quiet = false;
    std::printf("timeline edges: %u timeline waits, %u validation messages (not gated: syncval timeline support)\n",
                timelineResult.timelineWaits, g_log.count - before);
    g_log.count = before;
    expect(timelineResult.binaryWaits == 0u && timelineResult.timelineWaits >= 4u, "timeline edges used");
    expect(timelineWords == singleWords, "timeline-edge multi-queue result equals the single-queue result");
    return 0;
}

// --- radix ----------------------------------------------------------------------------------------

int runRadix(Context& ctx) {
    auto sorter = GpuRadixSort::create(*ctx.device, {nullptr, ctx.allocator.get(), "fuse.rg.radix"});
    if (sorter == nullptr || !sorter->isValid()) {
        std::printf("SKIP: radix sort unavailable (%s)\n", sorter != nullptr ? sorter->message().c_str() : "null");
        return kSkip;
    }
    u32 seed = 12345u;
    auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return seed;
    };
    {
        const u32 n = 70000;
        std::vector<u32> keys(n), values(n);
        for (u32 i = 0; i < n; ++i) {
            keys[i] = next();
            values[i] = i;
        }
        std::vector<std::pair<u32, u32>> ref(n);
        for (u32 i = 0; i < n; ++i) {
            ref[i] = {keys[i], values[i]};
        }
        std::stable_sort(ref.begin(), ref.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        GpuRadixSortStats stats{};
        expect(sorter->sort(keys.data(), values.data(), n, 32, &stats), "u32 sort through the graph");
        bool ok = true;
        for (u32 i = 0; i < n && ok; ++i) {
            ok = keys[i] == ref[i].first && values[i] == ref[i].second;
        }
        expect(ok, "u32 graph sort equals std::stable_sort");
        std::printf("u32 x %u: %u graph passes, %u barrier calls, %u dispatches\n", n, sorter->lastGraphPassCount(),
                    sorter->lastGraphBarrierCalls(), stats.dispatches);
        expect(sorter->lastGraphBarrierCalls() > 0u, "graph derived the sort's barriers");
    }
    {
        const u32 n = 20000;
        std::vector<u64> keys(n);
        std::vector<u32> values(n);
        for (u32 i = 0; i < n; ++i) {
            keys[i] = (static_cast<u64>(next()) << 32u) | next();
            values[i] = i;
        }
        std::vector<std::pair<u64, u32>> ref(n);
        for (u32 i = 0; i < n; ++i) {
            ref[i] = {keys[i], values[i]};
        }
        std::stable_sort(ref.begin(), ref.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        expect(sorter->sort64(keys.data(), values.data(), n, 64), "u64 sort through the graph");
        bool ok = true;
        for (u32 i = 0; i < n && ok; ++i) {
            ok = keys[i] == ref[i].first && values[i] == ref[i].second;
        }
        expect(ok, "u64 graph sort equals std::stable_sort");
        std::printf("u64 x %u: %u graph passes, %u barrier calls\n", n, sorter->lastGraphPassCount(),
                    sorter->lastGraphBarrierCalls());
        expect(sorter->lastGraphPassCount() > 32u, "u64 sort graph exceeds the old 32-pass cap");
    }
    expect(g_log.count == 0u, "sync validation clean on the graph-driven radix sort");
    return 0;
}

// --- ray-tracing stage (WP-7.3 follow-up) ---------------------------------------------------------

constexpr u32 kRtWords = 256;
constexpr u32 kRtExtent = 16;

/// Raygen-only ray-tracing pipeline (rg_test_rt.rgen) + its SBT from the allocator.
struct RtRig {
    Context* ctx = nullptr;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    Buffer sbt{};
    VkStridedDeviceAddressRegionKHR raygen{};
    PFN_vkCmdTraceRaysKHR traceRays = nullptr;

    ~RtRig() {
        if (ctx == nullptr || ctx->vkDevice == VK_NULL_HANDLE) {
            return;
        }
        vkDeviceWaitIdle(ctx->vkDevice);
        if (sbt.handle != nullptr) {
            ctx->allocator->destroyBuffer(sbt);
        }
        if (pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(ctx->vkDevice, pool, nullptr);
        }
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(ctx->vkDevice, pipeline, nullptr);
        }
        if (layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(ctx->vkDevice, layout, nullptr);
        }
        if (setLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(ctx->vkDevice, setLayout, nullptr);
        }
    }
};

int setupRtRig(Context& ctx, RtRig& rig) {
#if defined(FUSE_RG_TEST_RT_SHADER) && defined(VK_KHR_ray_tracing_pipeline)
    rig.ctx = &ctx;
    if (!ctx.device->info().caps.rayTracingPipeline) {
        std::printf("SKIP: VK_KHR_ray_tracing_pipeline not enabled on this device\n");
        return kSkip;
    }
    std::ifstream file(FUSE_RG_TEST_RT_SHADER, std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (code.empty() || code.size() % 4u != 0u) {
        std::printf("SKIP: %s not readable\n", FUSE_RG_TEST_RT_SHADER);
        return kSkip;
    }
    auto createRt = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(ctx.vkDevice, "vkCreateRayTracingPipelinesKHR"));
    auto getHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        vkGetDeviceProcAddr(ctx.vkDevice, "vkGetRayTracingShaderGroupHandlesKHR"));
    rig.traceRays = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(vkGetDeviceProcAddr(ctx.vkDevice, "vkCmdTraceRaysKHR"));
    if (createRt == nullptr || getHandles == nullptr || rig.traceRays == nullptr) {
        std::fprintf(stderr, "FAIL: ray-tracing pipeline entry points missing although the feature is enabled\n");
        return 1;
    }
    VkDescriptorSetLayoutBinding bindings[3]{};
    const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
    for (u32 i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = types[i];
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 3;
    setInfo.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx.vkDevice, &setInfo, nullptr, &rig.setLayout);
    VkPushConstantRange range{VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, 8};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &rig.setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    vkCreatePipelineLayout(ctx.vkDevice, &layoutInfo, nullptr, &rig.layout);
    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = code.size();
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(ctx.vkDevice, &moduleInfo, nullptr, &module);
    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stage.module = module;
    stage.pName = "main";
    VkRayTracingShaderGroupCreateInfoKHR group{};
    group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    group.generalShader = 0;
    group.closestHitShader = VK_SHADER_UNUSED_KHR;
    group.anyHitShader = VK_SHADER_UNUSED_KHR;
    group.intersectionShader = VK_SHADER_UNUSED_KHR;
    VkRayTracingPipelineCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    info.stageCount = 1;
    info.pStages = &stage;
    info.groupCount = 1;
    info.pGroups = &group;
    info.maxPipelineRayRecursionDepth = 0;
    info.layout = rig.layout;
    const VkResult created = createRt(ctx.vkDevice, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &rig.pipeline);
    vkDestroyShaderModule(ctx.vkDevice, module, nullptr);
    if (created != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL: vkCreateRayTracingPipelinesKHR (%d)\n", static_cast<int>(created));
        return 1;
    }

    // SBT through the allocator: BufferUsage::ShaderBindingTable (+ device address), host-visible, one raygen record.
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
    rtProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(static_cast<VkPhysicalDevice>(ctx.device->nativePhysicalDevice()), &props);
    const u64 handleSize = rtProps.shaderGroupHandleSize;
    const u64 baseAlign = (std::max)(static_cast<u64>(rtProps.shaderGroupBaseAlignment), u64{1});
    const u64 record = (handleSize + baseAlign - 1u) / baseAlign * baseAlign;
    u8 handle[64] = {};
    if (handleSize == 0u || handleSize > sizeof(handle) ||
        getHandles(ctx.vkDevice, rig.pipeline, 0, 1, static_cast<size_t>(handleSize), handle) != VK_SUCCESS) {
        std::fprintf(stderr, "FAIL: shader group handle\n");
        return 1;
    }
    BufferDesc sbtDesc{};
    sbtDesc.size = static_cast<fuse::usize>(record + baseAlign);
    sbtDesc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::ShaderBindingTable) |
                                             static_cast<u32>(BufferUsage::ShaderDeviceAddress));
    sbtDesc.memoryUsage = MemoryUsage::CpuToGpu;
    sbtDesc.name = "rg.rt_sbt";
    const bool sbtOk = ctx.allocator->createBuffer(sbtDesc, rig.sbt) && rig.sbt.mapped != nullptr && rig.sbt.deviceAddress != 0u;
    expect(sbtOk, "SBT buffer from the allocator: mapped, device address");
    if (!sbtOk) {
        return 1;
    }
    const u64 aligned = (rig.sbt.deviceAddress + baseAlign - 1u) / baseAlign * baseAlign;
    std::memcpy(static_cast<u8*>(rig.sbt.mapped) + (aligned - rig.sbt.deviceAddress), handle, static_cast<size_t>(handleSize));
    rig.raygen = {aligned, record, record};
    std::printf("SBT: handle %llu B, base alignment %llu B, allocator buffer %llu B at 0x%llx\n",
                static_cast<unsigned long long>(handleSize), static_cast<unsigned long long>(baseAlign),
                static_cast<unsigned long long>(sbtDesc.size), static_cast<unsigned long long>(rig.sbt.deviceAddress));

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 8;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    vkCreateDescriptorPool(ctx.vkDevice, &poolInfo, nullptr, &rig.pool);
    return 0;
#else
    (void)ctx;
    (void)rig;
    std::printf("SKIP: rg_test_rt.rgen not built or VK_KHR_ray_tracing_pipeline headers missing\n");
    return kSkip;
#endif
}

struct TracePass {
    RtRig* rig = nullptr;
    rg::BufferRef src, dst;
    rg::TextureRef image;
    u32 count = 0;
    u32 add = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;
};
void traceFn(const rg::PassContext& c, void* user) {
#if defined(VK_KHR_ray_tracing_pipeline)
    auto* p = static_cast<TracePass*>(user);
    const VkDevice device = p->rig->ctx->vkDevice;
    VkDescriptorBufferInfo buffers[2] = {{static_cast<VkBuffer>(c.buffer(p->src)), 0, VK_WHOLE_SIZE},
                                         {static_cast<VkBuffer>(c.buffer(p->dst)), 0, VK_WHOLE_SIZE}};
    VkDescriptorImageInfo image{VK_NULL_HANDLE, static_cast<VkImageView>(c.imageView(p->image)),
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[3]{};
    for (u32 i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = p->set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i < 2u ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[i].pBufferInfo = i < 2u ? &buffers[i] : nullptr;
        writes[i].pImageInfo = i < 2u ? nullptr : &image;
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(c.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, p->rig->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, p->rig->layout, 0, 1, &p->set, 0, nullptr);
    const u32 push[2] = {p->count, p->add};
    vkCmdPushConstants(cmd, p->rig->layout, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 0, sizeof(push), push);
    const VkStridedDeviceAddressRegionKHR empty{0, 0, 0};
    p->rig->traceRays(cmd, &p->rig->raygen, &empty, &empty, &empty, p->count, 1, 1);
#else
    (void)c;
    (void)user;
#endif
}

struct RtGraph {
    FillPass fill;
    AddPass add1, add2;
    ClearPass clear;
    TracePass trace;
    BufferCopyPass outDst, outB;
    u32 traceIndex = 0;
    u32 warIndex = 0;
    rg::BufferRef b, dst;
    rg::TextureRef image;
};

/// 01 fill src = 1 (transfer) -> 02 compute b = src + 10 -> 03 clear image = 7 (transfer) -> 04 raygen: dst = b + image
/// + 100 (StorageRead b, SampledRead image, StorageWrite dst at `traceStages`) -> 05 compute b = src + 20 (WAR on b)
/// -> 06/07 copies to the readback -> 08 host read. Expected: dst = 118, b = 21.
void buildRtGraph(rg::Graph& g, RtGraph& h, Context& ctx, RtRig& rig, VkBuffer out, u8 traceStages) {
    g.reset();
    const rg::BufferRef src = g.createBuffer({kRtWords * 4u, 0, "rg.rt_src"});
    h.b = g.createBuffer({kRtWords * 4u, 0, "rg.rt_b"});
    h.dst = g.createBuffer({kRtWords * 4u, 0, "rg.rt_dst"});
    rg::ImageDesc imageDesc;
    imageDesc.width = kRtExtent;
    imageDesc.height = kRtExtent;
    imageDesc.format = kR32Uint;
    imageDesc.name = "rg.rt_image";
    h.image = g.createImage(imageDesc);
    const rg::BufferRef outRef = g.importBuffer({out, 2048, rg::kNoQueue, nullptr, "rg.rt_out"});

    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool = rig.pool;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &rig.setLayout;
    VkDescriptorSet traceSet = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(ctx.vkDevice, &info, &traceSet);

    h.fill = {src, 1u};
    h.add1 = {&ctx, src, h.b, kRtWords, 10u, allocateSet(ctx)};
    h.add2 = {&ctx, src, h.b, kRtWords, 20u, allocateSet(ctx)};
    h.clear = {h.image, 0, {}};
    h.clear.color.uint32[0] = 7u;
    h.trace = {&rig, h.b, h.dst, h.image, kRtWords, 100u, traceSet};
    h.outDst = {h.dst, outRef, 0, 0, kRtWords * 4u};
    h.outB = {h.b, outRef, 0, 1024, kRtWords * 4u};

    constexpr u8 kCs = rg::kStageCompute;
    g.addPass("rt.01.fill_src", fillFn, &h.fill).use(src, rg::Access::TransferDst);
    g.addPass("rt.02.compute_b", addFn, &h.add1)
        .use(src, rg::Access::StorageRead, {}, kCs)
        .use(h.b, rg::Access::StorageWrite, {}, kCs);
    g.addPass("rt.03.clear_image", clearFn, &h.clear).use(h.image, rg::Access::TransferDst);
    h.traceIndex = g.addPass("rt.04.trace", traceFn, &h.trace)
                       .use(h.b, rg::Access::StorageRead, {}, traceStages)       // RAW: compute write -> RT read
                       .use(h.image, rg::Access::SampledRead, {}, traceStages)   // RAW + layout: transfer -> RT sample
                       .use(h.dst, rg::Access::StorageWrite, {}, traceStages)
                       .index();
    h.warIndex = g.addPass("rt.05.compute_b_again", addFn, &h.add2)
                     .use(src, rg::Access::StorageRead, {}, kCs)
                     .use(h.b, rg::Access::StorageWrite, {}, kCs) // WAR: RT read -> compute write
                     .index();
    g.addPass("rt.06.dst_to_out", bufferCopyFn, &h.outDst)
        .use(h.dst, rg::Access::TransferSrc) // RAW: RT write -> transfer read
        .use(outRef, rg::Access::TransferDst, {0, 1024});
    g.addPass("rt.07.b_to_out", bufferCopyFn, &h.outB)
        .use(h.b, rg::Access::TransferSrc)
        .use(outRef, rg::Access::TransferDst, {1024, 1024});
    g.addPass("rt.08.host_read", nullptr, nullptr).use(outRef, rg::Access::HostRead);
}

/// Stages of the planned barriers on `id` in front of `pass` (buffer or image), OR-ed.
void barrierStages(const rg::Graph& g, u32 pass, u32 id, bool image, u64& src, u64& dst, u32& newLayout) {
    src = dst = 0;
    newLayout = 0;
    const rg::BarrierRange& r = g.passBarriers(pass);
    if (image) {
        for (u32 i = r.imageBegin; i < r.imageBegin + r.imageCount; ++i) {
            const rg::ImageBarrier& b = g.imageBarriers()[i];
            if (b.resource == id) {
                src |= b.srcStages;
                dst |= b.dstStages;
                newLayout = b.newLayout;
            }
        }
        return;
    }
    for (u32 i = r.bufferBegin; i < r.bufferBegin + r.bufferCount; ++i) {
        const rg::BufferBarrier& b = g.bufferBarriers()[i];
        if (b.resource == id) {
            src |= b.srcStages;
            dst |= b.dstStages;
        }
    }
}

/// A device capped at T2 (no RT pipeline): BufferUsage::ShaderBindingTable is dropped, the buffer is created and
/// validation stays quiet.
void checkSbtUsageDroppedBelowT3(Context& ctx) {
    VulkanDeviceDesc desc{};
    desc.maxTier = RenderTier::T2;
    auto device = VulkanDevice::create(*ctx.instance, desc);
    if (device == nullptr || !device->isValid()) {
        std::printf("T2-capped device unavailable, SBT drop check skipped\n");
        return;
    }
    expect(!device->info().caps.rayTracingPipeline, "T2 cap: ray-tracing pipeline not enabled");
    const u32 before = g_log.count;
    {
        auto allocator = GpuAllocator::create(*device);
        expect(allocator != nullptr && allocator->isValid(), "T2 allocator");
        if (allocator != nullptr && allocator->isValid()) {
            BufferDesc d{};
            d.size = 256;
            d.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::ShaderBindingTable) |
                                               static_cast<u32>(BufferUsage::ShaderDeviceAddress));
            d.memoryUsage = MemoryUsage::CpuToGpu;
            d.name = "rg.t2_sbt";
            Buffer b{};
            expect(allocator->createBuffer(d, b) && b.deviceAddress != 0u,
                   "T2: ShaderBindingTable usage dropped, buffer created with a device address");
            if (b.handle != nullptr) {
                allocator->destroyBuffer(b);
            }
        }
    }
    const u32 afterAllocator = g_log.count;
    // Informational: the same usage bit passed raw (what the allocator avoids) on this device.
    u32 raw = 0;
    {
#if defined(VK_KHR_ray_tracing_pipeline)
        g_log.quiet = true;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = 256;
        info.usage = VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const VkDevice vkDevice = static_cast<VkDevice>(device->nativeHandle());
        VkBuffer buffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(vkDevice, &info, nullptr, &buffer) == VK_SUCCESS) {
            vkDestroyBuffer(vkDevice, buffer, nullptr);
        }
        raw = g_log.count - afterAllocator;
        g_log.quiet = false;
#endif
    }
    device.reset();
    std::printf("T2-capped device: SBT usage through the allocator: %u validation message(s); raw "
                "VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR: %u message(s)\n",
                afterAllocator - before, raw);
    expect(afterAllocator == before, "T2: no validation message from the SBT usage through the allocator");
    g_log.count = afterAllocator;
}

int runRtStage(Context& ctx) {
    RtRig rig;
    const int rc = setupRtRig(ctx, rig);
    if (rc != 0) {
        return rc;
    }
    HostBuffer out;
    if (!createReadback(ctx, out, 2048, "rg.rt_out")) {
        std::fprintf(stderr, "FAIL: readback buffer\n");
        return 1;
    }
    auto executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (!executor->isValid()) {
        std::fprintf(stderr, "executor: %s\n", executor->message().c_str());
        return 1;
    }
    rg::Graph graph;
    RtGraph h;
    const u64 rt = rg::vkc::kStageRayTracingShader;
    const u64 cs = rg::vkc::kStageComputeShader;
    for (u32 frame = 0; frame < 3; ++frame) {
        std::memset(out.buffer.mapped, 0, 2048);
        vkResetDescriptorPool(ctx.vkDevice, ctx.pool, 0);
        vkResetDescriptorPool(ctx.vkDevice, rig.pool, 0);
        buildRtGraph(graph, h, ctx, rig, static_cast<VkBuffer>(out.buffer.handle), rg::kStageRayTracing);
        const rg::ExecuteResult result = executor->execute(graph);
        expect(result.ok, "ray-tracing graph executed");
        expect(executor->waitIdle(), "ray-tracing graph retired");
        expect(result.executedPasses == 8u, "8 passes executed");
        u64 src = 0, dst = 0;
        u32 layout = 0;
        barrierStages(graph, h.traceIndex, h.b.id, false, src, dst, layout);
        expect(src == cs && dst == rt, "compute write -> RT read: COMPUTE_SHADER -> RAY_TRACING_SHADER");
        barrierStages(graph, h.traceIndex, h.image.id, true, src, dst, layout);
        expect(src == rg::vkc::kStageTransfer && dst == rt && layout == rg::vkc::kLayoutShaderReadOnly,
               "transfer clear -> RT sample: TRANSFER -> RAY_TRACING_SHADER, SHADER_READ_ONLY_OPTIMAL");
        barrierStages(graph, h.warIndex, h.b.id, false, src, dst, layout);
        expect((src & rt) != 0u && dst == cs, "RT read -> compute write (WAR): RAY_TRACING_SHADER -> COMPUTE_SHADER");
        barrierStages(graph, h.warIndex + 1u, h.dst.id, false, src, dst, layout);
        expect(src == rt && dst == rg::vkc::kStageTransfer, "RT write -> copy: RAY_TRACING_SHADER -> TRANSFER");
        expect(allEqual(readWords(ctx, out, 0, kRtWords), 118u), "raygen output = (1 + 10) + 7 + 100");
        expect(allEqual(readWords(ctx, out, 1024, kRtWords), 21u), "b rewritten after the RT read = 1 + 20");
        std::printf("frame %u: %u passes, %u barrier calls (%u image, %u buffer barriers)\n", frame, result.executedPasses,
                    result.barrierCalls, result.imageBarriers, result.bufferBarriers);
    }
    const u32 clean = g_log.count;
    expect(clean == 0u, "sync validation clean: compute / transfer -> RT-stage reads and RT -> compute / transfer");

    // Negative controls. (1) Is synchronization validation live for vkCmdTraceRaysKHR at all? The graph with no
    // barriers recorded must trip it on the raygen's reads. (2) If it is: the raygen accesses declared at the compute
    // stage (barriers that miss RAY_TRACING_SHADER) must trip it too. VK_LAYER_KHRONOS_validation 1.3.275 (Ubuntu
    // 24.04) does not track ray-tracing pipeline descriptor accesses (it reports only the dispatch / transfer
    // hazards), so there the RT-stage ordering rests on the planned-barrier checks above; (2) is enforced as soon as
    // the layer covers trace rays.
    g_log.quiet = true;
    u32 before = g_log.count;
    g_log.traceRaysHazards = 0;
    {
        rg::ExecutorDesc noBarriers{};
        noBarriers.debugSkipBarriers = true;
        auto unsafe = rg::Executor::create(*ctx.device, ctx.allocator.get(), noBarriers);
        vkResetDescriptorPool(ctx.vkDevice, ctx.pool, 0);
        vkResetDescriptorPool(ctx.vkDevice, rig.pool, 0);
        buildRtGraph(graph, h, ctx, rig, static_cast<VkBuffer>(out.buffer.handle), rg::kStageRayTracing);
        unsafe->execute(graph);
        unsafe->waitIdle();
    }
    u32 hazards = 0;
    for (u32 i = before; i < g_log.ids.size(); ++i) {
        hazards += g_log.ids[i].find("SYNC-HAZARD") != std::string::npos ? 1u : 0u;
    }
    const bool rtTracked = g_log.traceRaysHazards > 0u;
    std::printf("negative control 1 (barriers suppressed): %u messages, %u SYNC-HAZARD, %u on vkCmdTraceRaysKHR -> sync "
                "validation %s ray-tracing pipeline accesses\n",
                g_log.count - before, hazards, g_log.traceRaysHazards, rtTracked ? "tracks" : "does NOT track");
    expect(hazards > 0u, "negative control 1: sync validation fires without the graph's barriers");
    before = g_log.count;
    g_log.traceRaysHazards = 0;
    vkResetDescriptorPool(ctx.vkDevice, ctx.pool, 0);
    vkResetDescriptorPool(ctx.vkDevice, rig.pool, 0);
    buildRtGraph(graph, h, ctx, rig, static_cast<VkBuffer>(out.buffer.handle), rg::kStageCompute);
    executor->execute(graph);
    executor->waitIdle();
    std::printf("negative control 2 (raygen accesses declared at the compute stage): %u messages, %u SYNC-HAZARD on "
                "vkCmdTraceRaysKHR%s\n",
                g_log.count - before, g_log.traceRaysHazards, rtTracked ? "" : " (not enforced: layer does not track trace rays)");
    if (rtTracked) {
        expect(g_log.traceRaysHazards > 0u, "negative control 2: compute-stage barriers miss the raygen's reads");
    }
    g_log.quiet = false;
    g_log.count = clean;
    executor.reset();

    checkSbtUsageDroppedBelowT3(ctx);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "hazards";
    const char* layerDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (std::strcmp(argv[i], "--layer-dir") == 0 && i + 1 < argc) {
            layerDir = argv[++i];
        }
    }
    const bool async = mode == "async";
    if (async && layerDir == nullptr) {
        std::fprintf(stderr, "FAIL: --mode async needs --layer-dir\n");
        return 1;
    }
    Context ctx;
    const int setup = setupContext(ctx, async ? layerDir : nullptr, async);
    if (setup != 0) {
        return setup;
    }
    std::printf("device: %s | %s\n", ctx.device->info().deviceName.c_str(),
                ctx.device->info().caps.valid ? ctx.device->info().caps.summary().c_str() : "no caps");
    int rc = 0;
    if (mode == "hazards" || mode == "async" || mode == "rtstage") {
        rc = setupComputePipeline(ctx);
        if (rc != 0) {
            return rc;
        }
    }
    if (mode == "hazards") {
        rc = runHazards(ctx);
    } else if (mode == "alias") {
        rc = runAliasMode(ctx);
    } else if (mode == "async") {
        rc = runAsyncMode(ctx);
    } else if (mode == "radix") {
        rc = runRadix(ctx);
    } else if (mode == "rtstage") {
        rc = runRtStage(ctx);
    } else {
        std::fprintf(stderr, "unknown --mode %s\n", mode.c_str());
        return 1;
    }
    if (rc != 0) {
        return rc;
    }
    if (g_log.count != 0u) {
        std::fprintf(stderr, "FAIL: %u validation message(s)\n", g_log.count);
        ++g_failures;
    }
    if (g_failures == 0) {
        std::printf("fuse_rp_rg_vulkan --mode %s: all checks passed\n", mode.c_str());
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_rp_rg_vulkan --mode %s: %d failure(s)\n", mode.c_str(), g_failures);
    return EXIT_FAILURE;
}

#endif
