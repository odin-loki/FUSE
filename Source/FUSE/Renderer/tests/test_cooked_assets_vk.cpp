// E06 (AP-RT-COOKED, UNI-U7-ASSET-1 render half) Lavapipe gates. VK_LAYER_KHRONOS_validation with synchronization
// validation: every validation / sync-validation message fails the run. CPU gates: test_cooked_assets_cpu.cpp.
//
//   --mode decode    BCn sampling: blocks of every format (BC1 / BC4 / BC5 / BC6H / BC7; random bits, so every BC7
//                    and BC6H mode incl. the reserved ones, plus the cook's encoder output), two mip levels, uploaded
//                    as VK_FORMAT_BC* images (cooked_texture_gpu.hpp) and texelFetch'ed by a compute kernel == the CPU
//                    decoder (BC7 / BC6H exact; BC1 / BC4 / BC5 within the implementation's interpolation precision,
//                    <= 2 / 255: Lavapipe interpolates BC4 / BC5 with 6-bit weights, e.g. 4/5 -> 51/64); the
//                    forced CPU-decode images return the CPU decoder's values exactly; ResourceManager::createCookedTexture
//                    (BC7 image when supported, RGBA8 when forced / unsupported).
//   --mode render    SceneRenderer frames of a sphere (T0, TAAU off): (A) cooked FMSH v2 (meshlets + DAG adopted) with
//                    a .fusemat whose albedo is a cooked BC7 and whose normal map is a cooked BC5, sampled as native BC
//                    images; (C) the RGBA8 / raw twin: the same source pixels as RGBA8 images (box-filtered mips) and
//                    the same streams through the meshlet builder. mean FLIP(A, C) <= 0.02; the textures are really
//                    sampled (FLIP(A, untextured twin) well above it).
//   --mode fallback  (B) = (A) with the BCn blocks decoded on the CPU (the no-textureCompressionBC path):
//                    FLIP(B, C) <= 0.02, and B vs A: mean FLIP <= 0.015, max |diff| <= 32 / 255, mean <= 0.5 / 255
//                    (Lavapipe: 0.009 / 18 / 0.12; the BC5 6-bit interpolation weights (<= 2 / 255 in the normal)
//                    through the specular highlight, and compressed-format filtering precision).
//   --mode e2e       glTF + PNG (vendor/assimp BoxTextured) -> fuse_cook (the tool) -> .fusemesh / .fusetex (+ a
//                    .fusemat) on disk -> VFS + AssetRegistry (job decode) -> CookedAssetRegistry (IRenderUploadSink)
//                    -> GpuScene mesh row + layered material row + bindless BC7 slot -> ECS MeshAssets -> rendered
//                    frame; the GPU material table and the frame are read back (row == the CPU mirror, logo sampled:
//                    FLIP against the RGBA8 twin of the PNG <= 0.02).
//
// Exit 77 = skip (stub build, no ICD / validation layer, no BC support for the native path, cook tool missing).
#include "cooked_assets/cooked_assets_test_content.hpp"

#include <fuse/asset/asset_registry.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/light.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/renderer/cooked_assets/bcn_decode.hpp>
#include <fuse/renderer/cooked_assets/cooked_asset_registry.hpp>
#include <fuse/renderer/cooked_assets/cooked_texture_gpu.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/material_layers/fusemat.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/scene_renderer/scene_renderer.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/upload_queue.hpp>

#if defined(FUSE_CA_HAS_FLIP)
#include <flip_metric.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

[[maybe_unused]] void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
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

namespace fs = std::filesystem;
using namespace fuse;
using namespace fuse::renderer;
using namespace fuse::renderer::frame;
using namespace fuse::renderer::scene_renderer;
namespace ca = fuse::renderer::cooked_assets;
namespace ml = fuse::renderer::material_layers;

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
constexpr u32 kRenderW = 160;
constexpr u32 kRenderH = 120;

u32 g_messages = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL onMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) == 0 ||
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
    UploadQueue upload;
    Buffer staging{};
    Buffer readback{};

    ~Context() {
        if (vkDevice != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(vkDevice);
        }
        executor.reset();
        upload.destroy();
        if (allocator != nullptr) {
            allocator->destroyBuffer(readback);
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

int setup(Context& ctx) {
    if (!layerAvailable(kValidationLayer)) {
        std::printf("SKIP: %s not installed (the gate needs synchronization validation)\n", kValidationLayer);
        return kSkip;
    }
    setenv("VK_INSTANCE_LAYERS", kValidationLayer, 1);
    setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
    setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
    setenv("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK", 1);
    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_cooked_assets_vk";
    instanceDesc.enableValidation = true;
    ctx.instance = VulkanInstance::create(instanceDesc);
    if (ctx.instance == nullptr || !ctx.instance->isValid()) {
        std::printf("SKIP: no Vulkan instance\n");
        return kSkip;
    }
    const VkInstance vkInstance = static_cast<VkInstance>(ctx.instance->nativeHandle());
    auto createMessenger =
        reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vkInstance, "vkCreateDebugUtilsMessengerEXT"));
    ctx.destroyMessenger =
        reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vkInstance, "vkDestroyDebugUtilsMessengerEXT"));
    if (createMessenger == nullptr) {
        std::printf("SKIP: VK_EXT_debug_utils unavailable\n");
        return kSkip;
    }
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = onMessage;
    createMessenger(vkInstance, &info, nullptr, &ctx.messenger);
    ctx.device = VulkanDevice::create(*ctx.instance, VulkanDeviceDesc{});
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
    BindlessDesc bdesc{};
    bdesc.backend = BindlessBackendPreference::DescriptorSet; // the frame composer gates' choice (Lavapipe)
    if (!ctx.bindless.init(*ctx.device, bdesc)) {
        std::fprintf(stderr, "FAIL: bindless init\n");
        return 1;
    }
    BufferDesc stagingDesc{};
    stagingDesc.size = kStagingBytes;
    stagingDesc.usage = BufferUsage::TransferSrc;
    stagingDesc.memoryUsage = MemoryUsage::CpuToGpu;
    stagingDesc.name = "cooked_assets_vk.staging";
    if (!ctx.allocator->createBuffer(stagingDesc, ctx.staging) || ctx.staging.mapped == nullptr ||
        !ctx.upload.init(ctx.device.get(), ctx.staging.handle, ctx.staging.mapped, kStagingBytes)) {
        std::fprintf(stderr, "FAIL: staging / UploadQueue\n");
        return 1;
    }
    BufferDesc readbackDesc{};
    readbackDesc.size = 4u * 1024u * 1024u;
    readbackDesc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::TransferDst) | static_cast<u32>(BufferUsage::Storage));
    readbackDesc.memoryUsage = MemoryUsage::GpuToCpu;
    readbackDesc.name = "cooked_assets_vk.readback";
    if (!ctx.allocator->createBuffer(readbackDesc, ctx.readback) || ctx.readback.mapped == nullptr) {
        std::fprintf(stderr, "FAIL: readback\n");
        return 1;
    }
    ctx.executor = rg::Executor::create(*ctx.device, ctx.allocator.get());
    if (ctx.executor == nullptr || !ctx.executor->isValid()) {
        std::fprintf(stderr, "FAIL: rg::Executor\n");
        return 1;
    }
    std::printf("device: %s, textureCompressionBC %s\n", ctx.device->info().deviceName.c_str(),
                ctx.device->info().textureCompressionBC ? "on" : "off");
    return 0;
}

f32 halfToFloat(u16 h) { return ca::half_bits_to_float(h); }

// ================================================================================================================
// --mode decode
struct FetchKernel {
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    ~FetchKernel() {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        vkDestroySampler(device, sampler, nullptr);
    }
};

struct FetchPush {
    u32 width;
    u32 height;
    s32 lod;
    u32 offset;
};

struct FetchJob {
    FetchKernel* kernel = nullptr;
    VkDescriptorSet set = VK_NULL_HANDLE;
    FetchPush push{};
    rg::BufferRef out;
};

void recordFetch(const rg::PassContext& pc, void* user) {
    const FetchJob& j = *static_cast<const FetchJob*>(user);
    const VkCommandBuffer cmd = static_cast<VkCommandBuffer>(pc.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, j.kernel->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, j.kernel->layout, 0, 1, &j.set, 0, nullptr);
    vkCmdPushConstants(cmd, j.kernel->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FetchPush), &j.push);
    vkCmdDispatch(cmd, (j.push.width + 7u) / 8u, (j.push.height + 7u) / 8u, 1u);
}

bool createFetchKernel(Context& ctx, FetchKernel& k) {
#if defined(FUSE_CA_FETCH_SPV)
    std::ifstream in(FUSE_CA_FETCH_SPV, std::ios::binary);
    const std::vector<char> spv((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (spv.empty() || spv.size() % 4u != 0u) {
        return false;
    }
    k.device = ctx.vkDevice;
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo sl{};
    sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sl.bindingCount = 2;
    sl.pBindings = b;
    if (vkCreateDescriptorSetLayout(k.device, &sl, nullptr, &k.setLayout) != VK_SUCCESS) {
        return false;
    }
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FetchPush)};
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &k.setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(k.device, &pl, nullptr, &k.layout) != VK_SUCCESS) {
        return false;
    }
    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = spv.size();
    sm.pCode = reinterpret_cast<const u32*>(spv.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(k.device, &sm, nullptr, &module) != VK_SUCCESS) {
        return false;
    }
    VkComputePipelineCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = module;
    cp.stage.pName = "main";
    cp.layout = k.layout;
    const VkResult r = vkCreateComputePipelines(k.device, VK_NULL_HANDLE, 1, &cp, nullptr, &k.pipeline);
    vkDestroyShaderModule(k.device, module, nullptr);
    if (r != VK_SUCCESS) {
        return false;
    }
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64}};
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 64;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(k.device, &dp, nullptr, &k.pool) != VK_SUCCESS) {
        return false;
    }
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 16.f;
    return vkCreateSampler(k.device, &si, nullptr, &k.sampler) == VK_SUCCESS;
#else
    (void)ctx;
    (void)k;
    return false;
#endif
}

VkDescriptorSet fetchSet(FetchKernel& k, const Texture& image, const Buffer& out) {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = k.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &k.setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(k.device, &ai, &set) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo ii{k.sampler, static_cast<VkImageView>(image.view), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bi{static_cast<VkBuffer>(out.handle), 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = set;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &ii;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = set;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &bi;
    vkUpdateDescriptorSets(k.device, 2, w, 0, nullptr);
    return set;
}

u64 g_rng = 0x9E3779B97F4A7C15ull;
u32 nextRandom() {
    g_rng ^= g_rng << 13u;
    g_rng ^= g_rng >> 7u;
    g_rng ^= g_rng << 17u;
    return static_cast<u32>(g_rng >> 16u);
}

/// A 2-level 32 x 32 cooked texture of `format`: random blocks (every mode, reserved ones included), with the cook's
/// encoder output in the first blocks when the cook library is linked.
asset::CookedTexture decodeTexture(asset::BcFormat format) {
    asset::CookedTexture t;
    t.format = format;
    t.compression = asset::bc_format_name(format);
    t.width = 32u;
    t.height = 32u;
    t.layers = 1u;
    t.srgb = false;
    const u32 bb = asset::bc_block_bytes(format);
    for (u32 l = 0; l < 2u; ++l) {
        asset::CookedTexture::Level level;
        level.width = 32u >> l;
        level.height = 32u >> l;
        level.blocks.resize(static_cast<usize>(asset::bc_block_count(level.width, level.height)) * bb);
        for (u8& b : level.blocks) {
            b = static_cast<u8>(nextRandom() & 255u);
        }
        t.levels.push_back(std::move(level));
    }
    // Make sure the reserved BC7 mode (first byte 0) and every BC7 mode bit appear.
    if (format == asset::BcFormat::BC7) {
        for (u32 m = 0; m <= 8u; ++m) {
            t.levels[0].blocks[m * 16u] = static_cast<u8>(m < 8u ? (1u << m) | (t.levels[0].blocks[m * 16u] & ~((2u << m) - 1u)) : 0u);
        }
    }
#if defined(FUSE_CA_HAS_COOK)
    cook::BcSourceImage img;
    img.width = 16u;
    img.height = 16u;
    const std::vector<u32> words = ca_test::makeAlbedo(16u);
    for (const u32 w : words) {
        for (u32 c = 0; c < 4u; ++c) {
            img.rgba8.push_back(static_cast<u8>((w >> (8u * c)) & 255u));
        }
        for (u32 c = 0; c < 3u; ++c) {
            img.rgba16f.push_back(cook::float_to_half(static_cast<f32>(((w >> (8u * c)) & 255u) / 32.0)));
        }
        img.rgba16f.push_back(cook::float_to_half(1.f));
    }
    std::vector<u8> encoded;
    if (cook::encode_bc_image(format, img, encoded)) {
        std::memcpy(t.levels[1].blocks.data(), encoded.data(), std::min(encoded.size(), t.levels[1].blocks.size()));
    }
#endif
    return t;
}

int runDecode(Context& ctx) {
    FetchKernel kernel;
    if (!createFetchKernel(ctx, kernel)) {
        std::fprintf(stderr, "FAIL: fetch kernel (bc_fetch.comp.spv) not built / not loadable\n");
        return 1;
    }
    bool nativeAny = false;
    for (const asset::BcFormat f : {asset::BcFormat::BC1, asset::BcFormat::BC4, asset::BcFormat::BC5,
                                    asset::BcFormat::BC6H, asset::BcFormat::BC7}) {
        const asset::CookedTexture tex = decodeTexture(f);
        ca::DecodedTexture cpu;
        expect(ca::decode_cooked_texture(tex, cpu), "CPU decode");
        const u32 texels = 32u * 32u + 16u * 16u;
        BufferDesc od{};
        od.size = static_cast<usize>(texels) * 16u * 2u;
        od.usage = BufferUsage::Storage;
        od.memoryUsage = MemoryUsage::GpuToCpu;
        od.name = "cooked_assets_vk.fetch";
        Buffer out{};
        if (!ctx.allocator->createBuffer(od, out) || out.mapped == nullptr) {
            expect(false, "fetch output buffer");
            continue;
        }
        ca::CookedTextureGpu images[2];
        bool made[2] = {false, false};
        for (u32 path = 0; path < 2u; ++path) {
            ca::CookedTextureUploadDesc ud{};
            ud.device = ctx.device.get();
            ud.allocator = ctx.allocator.get();
            ud.upload = &ctx.upload;
            ud.forceCpuDecode = path == 1u;
            ud.name = path == 0u ? "decode.native" : "decode.cpu";
            std::string error;
            made[path] = ca::upload_cooked_texture(ud, tex, images[path], &error);
            expect(made[path], "upload " + std::string(asset::bc_format_name(f)) + ": " + error);
        }
        const bool native = made[0] && images[0].path == ca::CookedTexturePath::Native;
        nativeAny = nativeAny || native;
        expect(!made[1] || images[1].path == ca::CookedTexturePath::CpuDecode, "forced CPU decode path");
        ctx.upload.flush();
        rg::Graph graph;
        const rg::BufferRef outRef =
            graph.importBuffer(rg::ImportedBuffer{out.handle, out.desc.size, rg::kNoQueue, nullptr, "fetch.out"});
        FetchJob jobs[4];
        u32 jobCount = 0;
        for (u32 path = 0; path < 2u; ++path) {
            if (!made[path]) {
                continue;
            }
            const VkDescriptorSet set = fetchSet(kernel, images[path].image, out);
            for (u32 l = 0; l < 2u; ++l) {
                FetchJob& j = jobs[jobCount++];
                j.kernel = &kernel;
                j.set = set;
                j.push = FetchPush{32u >> l, 32u >> l, static_cast<s32>(l), path * texels + (l == 0u ? 0u : 32u * 32u)};
                j.out = outRef;
                graph.addPass("fetch", &recordFetch, &j)
                    .use(outRef, rg::Access::StorageWrite,
                         rg::BufferRange{u64{j.push.offset} * 16u, u64{j.push.width} * j.push.height * 16u}, rg::kStageCompute);
            }
        }
        graph.addPass("fetch.host", nullptr, nullptr).use(outRef, rg::Access::HostRead);
        expect(ctx.executor->execute(graph).ok, "fetch graph executes");
        (void)ctx.executor->waitIdle();
        (void)ctx.upload.waitAll();

        const f32* gpu = static_cast<const f32*>(out.mapped);
        f64 maxNative = 0.0;
        f64 maxCpu = 0.0;
        for (u32 path = 0; path < 2u; ++path) {
            if (!made[path]) {
                continue;
            }
            for (u32 i = 0; i < texels; ++i) {
                const u32 l = i < 1024u ? 0u : 1u;
                const u32 k = l == 0u ? i : i - 1024u;
                for (u32 c = 0; c < 4u; ++c) {
                    const f32 g = gpu[(path * texels + i) * 4u + c];
                    f64 diff = 0.0;
                    if (f == asset::BcFormat::BC6H) {
                        u16 h = 0;
                        std::memcpy(&h, cpu.bytes.data() + cpu.levelOffsets[l] + k * 8u + c * 2u, 2u);
                        const f32 want = halfToFloat(h);
                        // In half ulps of the expected value.
                        const f32 ulp = std::max(std::fabs(want) * std::ldexp(1.f, -10), std::ldexp(1.f, -24));
                        diff = std::fabs(static_cast<f64>(g) - want) / ulp;
                    } else {
                        const f64 want = cpu.bytes[cpu.levelOffsets[l] + k * 4u + c];
                        diff = std::fabs(static_cast<f64>(g) * 255.0 - want);
                    }
                    (path == 0u ? maxNative : maxCpu) = std::max(path == 0u ? maxNative : maxCpu, diff);
                }
            }
        }
        const bool exactFormat = f == asset::BcFormat::BC7 || f == asset::BcFormat::BC6H;
        std::printf("decode %-4s: native %s max |gpu - cpu| = %.4f %s, cpu-decoded image max = %.4f\n",
                    asset::bc_format_name(f), native ? "BC" : "(unsupported)", maxNative,
                    f == asset::BcFormat::BC6H ? "half ulp" : "/255", maxCpu);
        if (native) {
            expect(exactFormat ? maxNative <= 1e-3 : maxNative <= 2.0,
                   std::string("GPU BCn decode == CPU decoder: ") + asset::bc_format_name(f));
        }
        expect(maxCpu <= 1e-3, std::string("CPU-decoded upload samples the decoder's values: ") + asset::bc_format_name(f));
        for (ca::CookedTextureGpu& img : images) {
            if (img.image.image != nullptr) {
                ctx.allocator->destroyImage(img.image);
            }
        }
        ctx.allocator->destroyBuffer(out);
        vkResetDescriptorPool(ctx.vkDevice, kernel.pool, 0);
    }
    // ResourceManager::createCookedTexture: the legacy B2.3 handle API over the same upload.
    {
        ResourceManager rm;
        ResourceManager::Desc rd{};
        rd.stagingRingBytes = 1u << 20;
        expect(rm.init(*ctx.device, ctx.bindless, rd), "ResourceManager init");
        const asset::CookedTexture tex = decodeTexture(asset::BcFormat::BC7);
        bool native = false;
        const TextureHandle h = rm.createCookedTexture(tex, false, &native);
        const Texture* t = rm.getTexture(h);
        expect(h.isValid() && t != nullptr && t->bindlessIndex != UINT32_MAX && t->desc.mipLevels == 2u,
               "createCookedTexture: bindless texture with both levels");
        expect(native == ca::device_supports_bc_format(ctx.device.get(), GpuFormat::Bc7Unorm) &&
                   (t == nullptr || t->desc.format == (native ? GpuFormat::Bc7Unorm : GpuFormat::R8G8B8A8Unorm)),
               "createCookedTexture: BC7 image when supported, RGBA8 otherwise");
        bool cpuNative = true;
        const TextureHandle h2 = rm.createCookedTexture(tex, true, &cpuNative);
        expect(h2.isValid() && !cpuNative && rm.getTexture(h2)->desc.format == GpuFormat::R8G8B8A8Unorm,
               "createCookedTexture(forceCpuDecode): RGBA8");
        expect(rm.waitAllUploads(), "cooked uploads complete");
        rm.destroyTexture(h);
        rm.destroyTexture(h2);
        rm.destroy();
    }
    if (!nativeAny) {
        std::printf("SKIP: the device samples no BC format (CPU decode verified)\n");
        return g_failures == 0 ? kSkip : 1;
    }
    return 0;
}

// ================================================================================================================
// Scenes
struct Copy {
    rg::TextureRef src;
    rg::BufferRef dst;
    u64 offset = 0;
    u32 width = 0;
    u32 height = 0;
};

void recordCopy(const rg::PassContext& pc, void* user) {
    const Copy& c = *static_cast<const Copy*>(user);
    VkBufferImageCopy region{};
    region.bufferOffset = c.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {c.width, c.height, 1};
    vkCmdCopyImageToBuffer(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkImage>(pc.image(c.src)),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
}

struct BufferCopy {
    rg::BufferRef src;
    rg::BufferRef dst;
    u64 bytes = 0;
    u64 dstOffset = 0;
};

void recordBufferCopy(const rg::PassContext& pc, void* user) {
    const BufferCopy& c = *static_cast<const BufferCopy*>(user);
    VkBufferCopy region{0, c.dstOffset, c.bytes};
    vkCmdCopyBuffer(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkBuffer>(pc.buffer(c.src)),
                    static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
}

FrameComposerDesc composerTemplate() {
    FrameComposerDesc d{};
    d.clusters = ClusterDesc{};
    d.vsmClipmap.levels = 12;
    d.vsmClipmap.firstLevelExtent = 4.f;
    d.vsmClipmap.markRadiusTexels = 2.5f;
    d.vsmClipmap.texelsPerPixel = 2.f;
    d.vsmPoolPagesX = 16;
    d.vsmPoolPagesY = 16;
    DDGIDesc& v = d.ddgiVolume;
    v.grid_origin = {-3.f, -1.f, -3.f};
    v.probe_spacing = {1.5f, 1.5f, 1.5f};
    v.grid_dims = {5u, 3u, 5u};
    v.rays_per_probe = 64;
    v.probes_per_frame = 75;
    v.irradiance_res = 6;
    v.depth_res = 8;
    v.hysteresis = 0.9f;
    v.max_ray_distance = 20.f;
    d.maxSplats = 64;
    d.maxSplatEntries = 1u << 14;
    return d;
}

FrameSettings frameSettings() {
    FrameSettings fs{};
    fs.vsmFilter.pcfRadius = 1;
    fs.ssfxSettings.ssgi_params.sample_sqrt = 2;
    fs.atmosphere.sizes.transWidth = 64;
    fs.atmosphere.sizes.transHeight = 32;
    fs.atmosphere.sizes.skyWidth = 64;
    fs.atmosphere.sizes.skyHeight = 48;
    fs.atmosphere.sizes.apWidth = 16;
    fs.atmosphere.sizes.apHeight = 16;
    fs.atmosphere.sizes.apDepth = 16;
    fs.atmosphere.sampling.transSteps = 32;
    fs.atmosphere.sampling.msDirSqrt = 4;
    fs.atmosphere.sampling.skySteps = 16;
    fs.fogSettings.gridX = 24;
    fs.fogSettings.gridY = 16;
    fs.fogSettings.gridZ = 24;
    fs.fogSettings.farPlane = 30.f;
    fs.fogSettings.medium.density = 0.01f;
    fs.clouds = false;
    fs.splats = false;
    fs.restir = false;
    fs.frameGen = false;
    fs.upscaler = FrameUpscaler::Taau;
    return fs;
}

SceneRendererDesc rendererDesc(Context& ctx) {
    SceneRendererDesc d{};
    d.device = ctx.device.get();
    d.allocator = ctx.allocator.get();
    d.bindless = &ctx.bindless;
    d.upload = &ctx.upload;
    d.width = kRenderW;
    d.height = kRenderH;
    d.tier = SceneTier::T0;
    d.instanceCapacity = 64;
    d.meshCapacity = 8;
    d.materialCapacity = 16;
    d.lightCapacity = 16;
    d.entityCapacity = 64;
    d.composer = composerTemplate();
    return d;
}

ecs::mat4 sunMatrix() {
    const f32 toSun[3] = {0.35f, 0.8f, 0.5f};
    const f32 l = std::sqrt(toSun[0] * toSun[0] + toSun[1] * toSun[1] + toSun[2] * toSun[2]);
    const f32 z[3] = {toSun[0] / l, toSun[1] / l, toSun[2] / l};
    f32 x[3] = {z[2], 0.f, -z[0]};
    const f32 xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    x[0] /= xl;
    x[2] /= xl;
    const f32 y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    ecs::mat4 m = ecs::mat4::identity();
    for (u32 a = 0; a < 3u; ++a) {
        m.data[0 + a] = x[a];
        m.data[4 + a] = y[a];
        m.data[8 + a] = z[a];
    }
    m.data[13] = 10.f;
    return m;
}

/// ECS: one entity with Transform + Mesh + MeshAssets (asset ids), a sun.
struct World {
    ecs::Registry registry;
    ecs::EntityID object{};
};

void buildWorld(World& w, asset::AssetId mesh, asset::AssetId material, f32 scale) {
    w.registry.init(16);
    w.object = w.registry.create();
    ecs::Transform t{};
    t.local_to_world = ecs::mat4::identity();
    t.local_to_world.data[0] = scale;
    t.local_to_world.data[5] = scale;
    t.local_to_world.data[10] = scale;
    w.registry.add<ecs::Transform>(w.object, t);
    w.registry.add<ecs::Mesh>(w.object, ecs::Mesh{});
    ecs::MeshAssets ma{};
    ma.mesh = mesh;
    ma.material = material;
    w.registry.add<ecs::MeshAssets>(w.object, ma);
    const ecs::EntityID sun = w.registry.create();
    ecs::Transform st{};
    st.local_to_world = sunMatrix();
    w.registry.add<ecs::Transform>(sun, st);
    ecs::DirectionalLight d{};
    d.color = ecs::vec3{1.f, 0.97f, 0.9f, 0.f};
    d.intensity = 3.f;
    w.registry.add<ecs::DirectionalLight>(sun, d);
}

FrameCamera camera(const f32 eye[3]) {
    FrameCamera c{};
    for (u32 a = 0; a < 3u; ++a) {
        c.eye[a] = eye[a];
        c.target[a] = 0.f;
    }
    c.fovY = 0.8f;
    c.nearPlane = 0.05f;
    c.farPlane = 40.f;
    return c;
}

/// Linear RGBA16F frame -> sRGB RGBA8 (clamped), what FLIP compares.
std::vector<u8> toSrgb8(const std::vector<u8>& half, u32 w, u32 h) {
    std::vector<u8> out(static_cast<usize>(w) * h * 4u);
    for (usize i = 0; i < static_cast<usize>(w) * h; ++i) {
        for (u32 c = 0; c < 4u; ++c) {
            u16 bits = 0;
            std::memcpy(&bits, half.data() + i * 8u + c * 2u, 2u);
            f32 v = halfToFloat(bits);
            v = std::isfinite(v) ? std::clamp(v, 0.f, 1.f) : 0.f;
            const f32 s = c < 3u ? (v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f) : 1.f;
            out[i * 4u + c] = static_cast<u8>(std::lround(s * 255.f));
        }
    }
    return out;
}

f64 flipMean(const std::vector<u8>& a, const std::vector<u8>& b, u32 w, u32 h) {
#if defined(FUSE_CA_HAS_FLIP)
    const content_golden::FlipResult r = content_golden::computeFlip(a.data(), b.data(), w, h);
    return r.valid ? r.mean : 1.0;
#else
    // Without the vendored FLIP: mean absolute sRGB difference (a stricter proxy on smooth content).
    f64 sum = 0.0;
    for (usize i = 0; i < a.size(); ++i) {
        sum += std::fabs(static_cast<f64>(a[i]) - b[i]) / 255.0;
    }
    return a.empty() ? 1.0 : sum / static_cast<f64>(a.size());
#endif
}

void writePpm(const std::string& name, const std::vector<u8>& rgba, u32 w, u32 h) {
    std::ofstream out(std::string(FUSE_CA_OUTPUT_DIR) + "/" + name, std::ios::binary | std::ios::trunc);
    out << "P6\n" << w << " " << h << "\n255\n";
    for (usize i = 0; i < static_cast<usize>(w) * h; ++i) {
        out.write(reinterpret_cast<const char*>(&rgba[i * 4u]), 3);
    }
}

/// Renders `frames` frames of `world` and returns the last output as sRGB RGBA8.
std::vector<u8> renderFrames(Context& ctx, SceneRenderer& renderer, ca::CookedAssetRegistry& cooked, World& world,
                             const FrameCamera& cam, u32 frames, u64& serial) {
    std::vector<u8> image;
    rg::Graph graph;
    for (u32 f = 0; f < frames; ++f) {
        expect(cooked.commit(), "layered table commit");
        cooked.syncEntities(world.registry);
        graph.reset();
        SceneFrameDesc sfd{};
        sfd.serial = ++serial;
        sfd.frameIndex = f;
        sfd.resetHistory = f == 0u;
        const FrameGraphOutputs outs = renderer.renderScene(world.registry, cam, graph, sfd);
        expect(outs.output.valid(), "frame recorded");
        const rg::BufferRef rb =
            graph.importBuffer(rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size, rg::kNoQueue, nullptr, "readback"});
        Copy c{outs.output, rb, 0, renderer.composer().outputWidth(), renderer.composer().outputHeight()};
        graph.addPass("readback.copy", &recordCopy, &c)
            .use(outs.output, rg::Access::TransferSrc)
            .use(rb, rg::Access::TransferDst, rg::BufferRange{0, u64{c.width} * c.height * 8u});
        graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
        expect(ctx.executor->execute(graph).ok, "frame executes");
        (void)ctx.executor->waitIdle();
        (void)ctx.upload.waitAll();
        renderer.collectRetired(sfd.serial);
        cooked.collectRetired(sfd.serial);
        if (f + 1u == frames) {
            const u8* p = static_cast<const u8*>(ctx.readback.mapped);
            image = toSrgb8(std::vector<u8>(p, p + u64{c.width} * c.height * 8u), c.width, c.height);
        }
    }
    return image;
}

// --- render / fallback -------------------------------------------------------------------------------------------
enum class Variant { Native, CpuDecode, Raw, Untextured };

const char* variantName(Variant v) {
    switch (v) {
    case Variant::Native:
        return "cooked (native BC)";
    case Variant::CpuDecode:
        return "cooked (CPU decode)";
    case Variant::Raw:
        return "RGBA8 / raw";
    case Variant::Untextured:
        return "untextured";
    }
    return "?";
}

struct SphereContent {
    asset::CookedMesh raw;
    asset::CookedMesh cookedMesh;
    asset::CookedTexture albedo;
    asset::CookedTexture normal;
    std::vector<u32> albedoPixels;
    std::vector<u32> normalPixels;
};

ml::FuseMat sphereMaterial(bool textured) {
    ml::FuseMat m{};
    m.name = "test/cooked/sphere";
    m.albedo[0] = 1.f;
    m.albedo[1] = 1.f;
    m.albedo[2] = 1.f;
    m.roughness = 0.55f;
    m.normalStrength = 1.f;
    if (textured) {
        m.textures.albedo = "sphere_albedo";
        m.textures.normal = "sphere_normal";
    } else {
        m.albedo[0] = 0.55f;
        m.albedo[1] = 0.45f;
        m.albedo[2] = 0.35f;
    }
    return m;
}

std::vector<u8> renderSphere(Context& ctx, const SphereContent& content, Variant variant, ca::CookedAssetStats* statsOut) {
    SceneRenderer renderer;
    if (!renderer.initialize(rendererDesc(ctx))) {
        expect(false, std::string("SceneRenderer::initialize: ") + renderer.reason());
        return {};
    }
    renderer.setFrameSettings(frameSettings());
    ca::CookedAssetRegistry cooked;
    ca::CookedAssetRegistryDesc cd{};
    cd.device = ctx.device.get();
    cd.allocator = ctx.allocator.get();
    cd.bindless = &ctx.bindless;
    cd.firstMaterialRow = 0u;
    cd.materialCapacity = 8u;
    cd.forceCpuBcDecode = variant == Variant::CpuDecode;
    expect(cooked.init(renderer, cd), "CookedAssetRegistry init");
    std::string error;
    const asset::AssetId meshId = asset::asset_id_of("game:/mesh/sphere.fusemesh");
    if (variant == Variant::Raw || variant == Variant::Untextured) {
        expect(cooked.addRgba8Texture("sphere_albedo", 256u, 256u, true, content.albedoPixels) != ml::kMlNoTexture,
               "RGBA8 albedo");
        // The W0.7 normal slot packs roughness / AO factors in B / A: the RGBA8 twin of a BC5 normal map has 1, 1.
        std::vector<u32> nra = content.normalPixels;
        for (u32& w : nra) {
            w = (w & 0x0000FFFFu) | 0xFFFF0000u;
        }
        expect(cooked.addRgba8Texture("sphere_normal", 256u, 256u, false, nra) != ml::kMlNoTexture, "RGBA8 normal");
        expect(cooked.addMesh(meshId, content.raw, &error) != ca::CookedAssetRegistry::kInvalid, "raw mesh: " + error);
    } else {
        expect(cooked.addCookedTexture("sphere_albedo", content.albedo) != ml::kMlNoTexture, "cooked BC7 albedo");
        expect(cooked.addCookedTexture("sphere_normal", content.normal) != ml::kMlNoTexture, "cooked BC5 normal");
        expect(cooked.addMesh(meshId, content.cookedMesh, &error) != ca::CookedAssetRegistry::kInvalid,
               "cooked mesh: " + error);
        expect(cooked.meshData(meshId) != nullptr && cooked.meshData(meshId)->path == ca::CookedMeshPath::Adopted &&
                   cooked.meshData(meshId)->hasDag,
               "FMSH v2 meshlets + DAG adopted");
    }
    const u32 row = cooked.addMaterial(sphereMaterial(variant != Variant::Untextured), &error);
    expect(row == 0u, "material row 0: " + error);
    World world;
    // AssetId of the material is not needed: the entity gets its row through the mesh-assets sync of the mesh only.
    buildWorld(world, meshId, asset::AssetId{}, 1.f);
    world.registry.get<ecs::Mesh>(world.object)->material_id = row;
    const f32 eye[3] = {0.f, 0.25f, 1.45f};
    u64 serial = 0;
    std::vector<u8> image = renderFrames(ctx, renderer, cooked, world, camera(eye), 3u, serial);
    if (statsOut != nullptr) {
        *statsOut = cooked.stats();
    }
    expect(renderer.meshes().gpuMesh(cooked.meshEngineId(meshId)) != MeshRegistry::kInvalidMesh, "mesh row present");
    expect(renderer.composer().layeredMaterialsHandle() == cooked.tableHandle() && cooked.tableHandle() != 0u,
           "composer samples the registry's layered table");
    (void)ctx.executor->waitIdle();
    cooked.destroy();
    renderer.destroy();
    ctx.bindless.collectRetired(~0ull);
    return image;
}

bool makeSphereContent(SphereContent& c) {
#if defined(FUSE_CA_HAS_COOK)
    c.raw = ca_test::makeSphere(32u, 48u, 0.5f);
    c.albedoPixels = ca_test::makeAlbedo(256u);
    c.normalPixels = ca_test::makeNormalMap(256u);
    std::string error;
    cook::TextureCookOptions ao; // BC7, sRGB, mips
    cook::TextureCookOptions no;
    no.format = asset::BcFormat::BC5;
    no.normal_map = true;
    if (!ca_test::cookTexture(c.albedoPixels, 256u, 256u, 1u, ao, c.albedo, nullptr, &error) ||
        !ca_test::cookTexture(c.normalPixels, 256u, 256u, 1u, no, c.normal, nullptr, &error) ||
        !ca_test::cookMesh(c.raw, true, true, c.cookedMesh, nullptr, &error)) {
        std::fprintf(stderr, "FAIL: cook: %s\n", error.c_str());
        return false;
    }
    return c.albedo.format == asset::BcFormat::BC7 && c.normal.format == asset::BcFormat::BC5;
#else
    (void)c;
    return false;
#endif
}

int runRender(Context& ctx, bool fallback) {
#if !defined(FUSE_CA_HAS_COOK)
    std::printf("SKIP: the cook library (Tools/FUSE/Cook) is not in this build\n");
    return kSkip;
#else
    if (!cook::mesh_meshlets_available()) {
        std::printf("SKIP: the cook's meshlet builder is not in this build\n");
        return kSkip;
    }
    SphereContent content;
    if (!makeSphereContent(content)) {
        return 1;
    }
    const bool bcSupported = ca::device_supports_bc_format(ctx.device.get(), GpuFormat::Bc7Srgb) &&
                             ca::device_supports_bc_format(ctx.device.get(), GpuFormat::Bc5Unorm);
    if (!fallback && !bcSupported) {
        std::printf("SKIP: the device samples no BC7 / BC5 (run --mode fallback)\n");
        return kSkip;
    }
    ca::CookedAssetStats st{};
    const std::vector<u8> raw = renderSphere(ctx, content, Variant::Raw, &st);
    writePpm("cooked_assets_raw.ppm", raw, kRenderW, kRenderH);
    if (!fallback) {
        const std::vector<u8> native = renderSphere(ctx, content, Variant::Native, &st);
        expect(st.nativeBcTextures == 2u && st.adoptedMeshes == 1u && st.dagMeshes == 1u,
               "native: 2 BC images, adopted mesh + DAG");
        const std::vector<u8> flat = renderSphere(ctx, content, Variant::Untextured, nullptr);
        writePpm("cooked_assets_native.ppm", native, kRenderW, kRenderH);
        const f64 fr = flipMean(native, raw, kRenderW, kRenderH);
        const f64 ff = flipMean(native, flat, kRenderW, kRenderH);
        std::printf("render: mean FLIP %s vs %s = %.5f (gate <= 0.02); vs %s = %.5f (textures sampled)\n",
                    variantName(Variant::Native), variantName(Variant::Raw), fr, variantName(Variant::Untextured), ff);
        expect(!native.empty() && fr <= 0.02, "cooked BC7 + BC5 + FMSH == RGBA8 / raw path (FLIP <= 0.02)");
        expect(ff > 0.04 && ff > 4.0 * fr, "the textures change the image (untextured twin far away)");
    } else {
        const std::vector<u8> cpu = renderSphere(ctx, content, Variant::CpuDecode, &st);
        expect(st.nativeBcTextures == 0u && st.textures == 2u, "fallback: 2 CPU-decoded images");
        writePpm("cooked_assets_cpu_decode.ppm", cpu, kRenderW, kRenderH);
        const f64 fr = flipMean(cpu, raw, kRenderW, kRenderH);
        std::printf("fallback: mean FLIP %s vs %s = %.5f (gate <= 0.02)\n", variantName(Variant::CpuDecode),
                    variantName(Variant::Raw), fr);
        expect(!cpu.empty() && fr <= 0.02, "CPU-decoded cooked assets == RGBA8 / raw path (FLIP <= 0.02)");
        if (bcSupported) {
            const std::vector<u8> native = renderSphere(ctx, content, Variant::Native, &st);
            const f64 fn = flipMean(native, cpu, kRenderW, kRenderH);
            u32 maxDiff = 0;
            f64 sumDiff = 0.0;
            for (usize i = 0; i < native.size() && native.size() == cpu.size(); ++i) {
                const u32 d = native[i] > cpu[i] ? native[i] - cpu[i] : cpu[i] - native[i];
                maxDiff = std::max(maxDiff, d);
                sumDiff += d;
            }
            const f64 meanDiff = native.empty() ? 255.0 : sumDiff / static_cast<f64>(native.size());
            std::printf("fallback: %s vs %s: mean FLIP %.5f (gate <= 0.015), max |diff| %u / 255, mean %.4f / 255\n",
                        variantName(Variant::Native), variantName(Variant::CpuDecode), fn, maxDiff, meanDiff);
            // The decoded values agree (decode gate: BC7 exact, BC5 <= 2 / 255); what remains is the implementation's
            // filtering precision on compressed vs RGBA8 images and the BC5 interpolation rounding through the
            // specular term: a few LSBs in a fraction of a percent of the pixels.
            expect(fn <= 0.015 && maxDiff <= 32u && meanDiff <= 0.5,
                   "native BC sampling == CPU-decoded fallback (FLIP <= 0.015, max 32 / 255, mean <= 0.5 / 255)");
        }
    }
    return 0;
#endif
}

// --- e2e ---------------------------------------------------------------------------------------------------------
struct SchedulerScope {
    explicit SchedulerScope(u32 workers) {
        jobs::JobScheduler::instance().shutdown();
        jobs::JobScheduler::instance().initialize(workers);
    }
    ~SchedulerScope() { jobs::JobScheduler::instance().shutdown(); }
};

void writeFile(const fs::path& path, const std::vector<u8>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

int runE2e(Context& ctx, const char* cookTool) {
#if !defined(FUSE_CA_HAS_COOK)
    (void)cookTool;
    std::printf("SKIP: the cook library (Tools/FUSE/Cook) is not in this build\n");
    return kSkip;
#else
    if (cookTool == nullptr) {
        std::printf("SKIP: no fuse_cook tool given (--cook <path>)\n");
        return kSkip;
    }
    const fs::path src = fs::path(FUSE_SOURCE_DIR) / "vendor/assimp/test/models/glTF2/BoxTextured-glTF";
    if (!fs::exists(src / "BoxTextured.gltf")) {
        std::printf("SKIP: %s not found\n", (src / "BoxTextured.gltf").string().c_str());
        return kSkip;
    }
    const fs::path dir = fs::path(FUSE_CA_TMP_DIR) / "e2e";
    fs::remove_all(dir);
    fs::create_directories(dir / "src");
    for (const char* name : {"BoxTextured.gltf", "BoxTextured0.bin", "CesiumLogoFlat.png"}) {
        fs::copy_file(src / name, dir / "src" / name, fs::copy_options::overwrite_existing);
    }
    const fs::path cookedDir = dir / "cooked";
    fs::create_directories(cookedDir / "mesh");
    fs::create_directories(cookedDir / "tex");
    const std::string prefix = "cd \"" + dir.string() + "\" && \"" + cookTool + "\"";
    const std::string meshCmd = prefix + " --mesh --input \"" + (dir / "src" / "BoxTextured.gltf").string() +
                                "\" --output \"" + (cookedDir / "mesh" / "box.fusemesh").string() + "\"";
    const std::string texCmd = prefix + " --texture --input \"" + (dir / "src" / "CesiumLogoFlat.png").string() +
                               "\" --output \"" + (cookedDir / "tex" / "logo.fusetex").string() + "\"";
    std::printf("cook: %s\ncook: %s\n", meshCmd.c_str(), texCmd.c_str());
    const int meshRc = std::system(meshCmd.c_str());
    const int texRc = std::system(texCmd.c_str());
    if (meshRc != 0 || texRc != 0) {
        expect(false, "fuse_cook cooks BoxTextured.gltf and CesiumLogoFlat.png");
        return 1;
    }
    ml::FuseMat mat{};
    mat.name = "test/cooked/box";
    mat.albedo[0] = mat.albedo[1] = mat.albedo[2] = 1.f;
    mat.roughness = 0.6f;
    mat.textures.albedo = "logo"; // the cook id of game:/tex/logo.fusetex (its stem)
    writeFile(cookedDir / "mat" / "box.fusemat", ml::write_fusemat_binary(mat));

    // The RGBA8 twin of the PNG (stb through the cook library).
    cook::TextureSource png;
    std::string error;
    expect(cook::load_texture_source((dir / "src" / "CesiumLogoFlat.png").string(), png, nullptr, &error),
           "PNG decodes: " + error);
    std::vector<u32> pngWords(static_cast<usize>(png.width) * png.height);
    for (usize i = 0; i < pngWords.size() && png.rgba8.size() >= pngWords.size() * 4u; ++i) {
        pngWords[i] = ca_test::packRgba(png.rgba8[i * 4u], png.rgba8[i * 4u + 1u], png.rgba8[i * 4u + 2u], png.rgba8[i * 4u + 3u]);
    }

    std::vector<u8> images[2];
    for (u32 pass = 0; pass < 2u; ++pass) {
        const bool cookedPass = pass == 0u;
        SceneRenderer renderer;
        if (!renderer.initialize(rendererDesc(ctx))) {
            expect(false, std::string("SceneRenderer::initialize: ") + renderer.reason());
            return 1;
        }
        renderer.setFrameSettings(frameSettings());
        ca::CookedAssetRegistry cooked;
        ca::CookedAssetRegistryDesc cd{};
        cd.device = ctx.device.get();
        cd.allocator = ctx.allocator.get();
        cd.bindless = &ctx.bindless;
        cd.firstMaterialRow = 4u;
        cd.materialCapacity = 4u;
        expect(cooked.init(renderer, cd), "CookedAssetRegistry init");
        World world;
        u64 serial = 0;
        {
            SchedulerScope scheduler(2);
            io::VirtualFileSystem vfs;
            vfs.mount(io::MountKind::Game, cookedDir.string(), "game:");
            asset::AssetRegistry assets(vfs);
            const asset::AssetId meshId = assets.acquire("game:/mesh/box.fusemesh");
            const asset::AssetId matId = assets.acquire("game:/mat/box.fusemat");
            asset::AssetId texId{};
            if (cookedPass) {
                texId = assets.acquire("game:/tex/logo.fusetex");
            } else {
                expect(cooked.addRgba8Texture("logo", png.width, png.height, true, pngWords) != ml::kMlNoTexture,
                       "RGBA8 twin of the PNG");
            }
            expect(assets.pumpUntilIdle(cooked, 30000), "cooked assets load");
            expect(assets.state(meshId) == asset::AssetLoadState::Ready && assets.state(matId) == asset::AssetLoadState::Ready &&
                       (!cookedPass || assets.state(texId) == asset::AssetLoadState::Ready),
                   "Ready: " + assets.error(meshId) + assets.error(matId) + (cookedPass ? assets.error(texId) : ""));
            buildWorld(world, meshId, matId, 1.f);
            const u32 row = cooked.materialRow(matId);
            expect(row == 4u && assets.gpuResource(matId) == u64{row} + 1u, "material row 4 (gpu_resource = row + 1)");
            const u32 tex = cooked.findTexture("logo");
            expect(tex != ml::kMlNoTexture && ctx.bindless.validateShaderHandle(cooked.textureHandle(tex)),
                   "logo texture has a live bindless slot");
            if (cookedPass) {
                expect(assets.texture(texId) != nullptr && assets.texture(texId)->format == asset::BcFormat::BC7,
                       "fuse_cook wrote BC7");
                expect(cooked.textureIsNative(tex) ==
                           ca::device_supports_bc_format(ctx.device.get(), GpuFormat::Bc7Srgb),
                       "BC7 sampled natively when the device supports it");
                const ca::CookedMeshletResult* md = cooked.meshData(meshId);
                std::printf("e2e: mesh %s (%s), texture %ux%u %s %s, %zu levels\n",
                            md != nullptr ? ca::cooked_mesh_path_name(md->path) : "?",
                            md != nullptr && !md->note.empty() ? md->note.c_str() : "FMSH meshlet table",
                            assets.texture(texId)->width, assets.texture(texId)->height,
                            assets.texture(texId)->compression.c_str(), cooked.textureIsNative(tex) ? "native" : "decoded",
                            assets.texture(texId)->levels.size());
            }
            const ml::MlMaterial* tm = cooked.tableMaterial(cooked.materialTableIndex(row));
            expect(tm != nullptr && tm->albedoTex == tex, ".fusemat albedo slot -> the logo's table index");

            const f32 eye[3] = {1.35f, 1.1f, 1.8f};
            images[pass] = renderFrames(ctx, renderer, cooked, world, camera(eye), 3u, serial);
            expect(renderer.meshes().gpuMesh(cooked.meshEngineId(meshId)) != MeshRegistry::kInvalidMesh,
                   "GpuScene mesh row for the cooked mesh");
            const ecs::Mesh* em = world.registry.get<ecs::Mesh>(world.object);
            expect(em != nullptr && em->vertex_buffer.index() == cooked.meshEngineId(meshId) && em->material_id == row,
                   "ECS Mesh from MeshAssets");

            // GPU readback of the scene's material table: the row is the layered row the CPU mirror holds.
            if (cookedPass) {
                const Buffer& table = renderer.scene().tableBuffer(gpu_scene::GpuSceneTable::Materials);
                const u64 bytes = u64{row + 1u} * sizeof(gpu_scene::GpuMaterial);
                rg::Graph graph;
                const rg::BufferRef srcRef = graph.importBuffer(
                    rg::ImportedBuffer{table.handle, table.desc.size, rg::kNoQueue, nullptr, "materials"});
                const rg::BufferRef rb = graph.importBuffer(
                    rg::ImportedBuffer{ctx.readback.handle, ctx.readback.desc.size, rg::kNoQueue, nullptr, "readback"});
                BufferCopy bc{srcRef, rb, bytes, 0};
                graph.addPass("materials.readback", &recordBufferCopy, &bc)
                    .use(srcRef, rg::Access::TransferSrc, rg::BufferRange{0, bytes})
                    .use(rb, rg::Access::TransferDst, rg::BufferRange{0, bytes});
                graph.addPass("readback.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
                expect(table.handle != nullptr && ctx.executor->execute(graph).ok, "material table readback");
                (void)ctx.executor->waitIdle();
                gpu_scene::GpuMaterial g{};
                std::memcpy(&g, static_cast<const u8*>(ctx.readback.mapped) + u64{row} * sizeof(g), sizeof(g));
                const gpu_scene::TableBytes mirror = renderer.scene().tableBytes(gpu_scene::GpuSceneTable::Materials);
                expect(gpu_scene::gpu_material_layered(g) &&
                           gpu_scene::gpu_material_layered_index(g) == cooked.materialTableIndex(row) &&
                           std::memcmp(&g, mirror.data + u64{row} * mirror.stride, sizeof(g)) == 0,
                       "GPU material row == CPU mirror (layered, table index)");
            }
            assets.unloadAll();
            assets.drainRenderUploads(cooked);
            expect(cooked.stats().meshes == 0u && cooked.stats().materials == 0u &&
                       cooked.stats().textures == (cookedPass ? 0u : 1u),
                   "unload releases the GPU copies");
            vfs.waitIdle(5000);
        }
        (void)ctx.executor->waitIdle();
        cooked.destroy();
        renderer.destroy();
        ctx.bindless.collectRetired(~0ull);
    }
    writePpm("cooked_assets_e2e.ppm", images[0], kRenderW, kRenderH);
    const f64 f = flipMean(images[0], images[1], kRenderW, kRenderH);
    std::printf("e2e: mean FLIP cooked (fuse_cook BC7 + FMSH) vs RGBA8 PNG twin = %.5f (gate <= 0.02)\n", f);
    expect(!images[0].empty() && f <= 0.02, "cooked glTF + PNG render == the RGBA8 twin (FLIP <= 0.02)");
    fs::remove_all(dir);
    return 0;
#endif
}

} // namespace

int main(int argc, char** argv) {
    std::string mode = "decode";
    const char* cookTool = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (a == "--cook" && i + 1 < argc) {
            cookTool = argv[++i];
        }
    }
    int rc = 0;
    {
        Context ctx;
        rc = setup(ctx);
        if (rc != 0) {
            return rc;
        }
        if (mode == "decode") {
            rc = runDecode(ctx);
        } else if (mode == "render") {
            rc = runRender(ctx, false);
        } else if (mode == "fallback") {
            rc = runRender(ctx, true);
        } else if (mode == "e2e") {
            rc = runE2e(ctx, cookTool);
        } else {
            std::fprintf(stderr, "unknown --mode %s\n", mode.c_str());
            return 2;
        }
        (void)ctx.executor->waitIdle();
    }
    if (g_messages != 0u) {
        std::fprintf(stderr, "FAIL: %u validation / sync-validation message(s)\n", g_messages);
        return 1;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_cooked_assets_vk %s: %d failure(s)\n", mode.c_str(), g_failures);
        return 1;
    }
    std::printf("fuse_cooked_assets_vk %s: %s (0 validation messages)\n", mode.c_str(), rc == kSkip ? "skipped" : "passed");
    return rc;
}

#endif
