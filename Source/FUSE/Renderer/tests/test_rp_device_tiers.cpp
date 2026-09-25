// WP-0.1 gate (docs/unification/RENDERER-EXECUTION.md): Vulkan 1.3 device + renderer tier detection.
//
//  * Lavapipe (mesh, AS, ray query, RT pipeline; no cooperative matrix) reports T2.
//  * FUSE_RENDER_TIER_MAX / VulkanDeviceDesc::maxTier cap the tier and keep higher-tier features
//    off; VK_LAYER_FUSE_mask_features (tests/vk_layer_mask_features.cpp, loaded *below*
//    VK_LAYER_KHRONOS_validation) hides extensions / feature bits so the hardware tier drops to T1
//    or T0, and a missing T0 requirement rejects the device (FUSE_VK_ALLOW_1_2 escape accepted).
//  * Selection policy: the Lavapipe ICD is registered twice (two real devices, as in
//    test_b2_physical_device_selection) and masked per device: a device missing a T0 requirement
//    loses, a higher (capped) tier wins among equal device types.
//  * Query back: the mask layer records the VkDeviceCreateInfo that reaches the driver; every
//    RendererCaps bit must match the feature bit actually passed down (and the extension list).
//  * Enabled features work: sync2 barriers + vkQueueSubmit2, dynamic-rendering clear, BDA query,
//    readback checked. Zero validation warnings / errors throughout (sync validation on).
//
// Pure tier logic (render_tier.hpp) is also checked in the stub build, which then exits 77 (skip).
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/render_tier.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

#if defined(__linux__)
#include <climits>
#include <dlfcn.h>
#include <fstream>
#include <link.h>
#include <sstream>
#endif

namespace {

using fuse::u32;
using fuse::u64;
using fuse::u8;
using namespace fuse::renderer;

constexpr int kSkip = 77;
int g_failures = 0;

void expectTrue(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

u64 allFeatures() {
    return (u64{1} << kRenderFeatureCount) - 1;
}

void checkTierLogic() {
    RenderTier tier = RenderTier::T3;
    expectTrue(parseRenderTier("T0", tier) && tier == RenderTier::T0, "parse T0");
    expectTrue(parseRenderTier("t2", tier) && tier == RenderTier::T2, "parse t2");
    expectTrue(parseRenderTier("1", tier) && tier == RenderTier::T1, "parse 1");
    expectTrue(!parseRenderTier("T4", tier) && tier == RenderTier::T1, "reject T4, out untouched");
    expectTrue(!parseRenderTier("", tier) && !parseRenderTier("T", tier) && !parseRenderTier(nullptr, tier) &&
                   !parseRenderTier("T12", tier),
               "reject malformed tiers");
    expectTrue(std::strcmp(renderTierName(RenderTier::T3), "T3") == 0, "tier name");

    const u64 all = allFeatures();
    auto without = [all](std::initializer_list<RenderFeature> features) {
        u64 mask = all;
        for (RenderFeature f : features) {
            mask &= ~renderFeatureBit(f);
        }
        return mask;
    };
    expectTrue(renderTierFromMask(all) == RenderTier::T3, "all features -> T3");
    expectTrue(renderTierFromMask(without({RenderFeature::CooperativeMatrix})) == RenderTier::T2,
               "no cooperative matrix -> T2 (Lavapipe)");
    expectTrue(renderTierFromMask(without({RenderFeature::RayQuery})) == RenderTier::T1, "no ray query -> T1");
    expectTrue(renderTierFromMask(without({RenderFeature::MeshShader})) == RenderTier::T0, "no mesh -> T0");
    expectTrue(renderTierFromMask(without({RenderFeature::TaskShader})) == RenderTier::T0, "no task -> T0");
    expectTrue(renderTierFromMask(without({RenderFeature::DescriptorBuffer, RenderFeature::ShaderObject,
                                           RenderFeature::DeviceGeneratedCommands,
                                           RenderFeature::ShaderImageInt64Atomics})) == RenderTier::T3,
               "optional features do not affect the tier");
    expectTrue(renderTierFromMask(without({RenderFeature::ShaderInt64})) == RenderTier::T0, "T0 incomplete -> T0");
    expectTrue(renderMissingT0(without({RenderFeature::ShaderInt64, RenderFeature::Synchronization2})) ==
                   "synchronization2,shaderInt64",
               "missing T0 names in feature order");
    expectTrue(renderMissingT0(all).empty(), "nothing missing");

    const u64 aboveT0 = renderFeaturesAboveTier(RenderTier::T0);
    expectTrue((aboveT0 & renderFeatureBit(RenderFeature::MeshShader)) != 0 &&
                   (aboveT0 & renderFeatureBit(RenderFeature::RayQuery)) != 0 &&
                   (aboveT0 & renderFeatureBit(RenderFeature::DescriptorBuffer)) == 0 &&
                   (aboveT0 & renderT0RequiredMask()) == 0,
               "features above T0");
    const u64 aboveT2 = renderFeaturesAboveTier(RenderTier::T2);
    expectTrue(aboveT2 == (renderFeatureBit(RenderFeature::RayTracingPipeline) |
                           renderFeatureBit(RenderFeature::CooperativeMatrix)),
               "features above T2 are the T3 set");
    expectTrue(renderFeaturesAboveTier(RenderTier::T3) == 0, "nothing above T3");

    RendererCaps caps{};
    caps.setEnabledMask(without({RenderFeature::CooperativeMatrix}));
    expectTrue(caps.meshShader && caps.rayTracingPipeline && !caps.cooperativeMatrix && caps.synchronization2,
               "per-feature bools follow the mask");
    expectTrue(caps.fallbackFor(RenderFeature::MeshShader) == nullptr, "no fallback for an enabled feature");
    expectTrue(caps.fallbackFor(RenderFeature::CooperativeMatrix) != nullptr, "fallback named for a missing one");
    for (u32 i = 0; i < kRenderFeatureCount; ++i) {
        const RenderFeatureInfo& info = renderFeatureInfo(static_cast<RenderFeature>(i));
        expectTrue(info.name != nullptr && info.fallback != nullptr && info.fallback[0] != '\0',
                   "feature info complete");
    }
    caps.apiVersion = (1u << 22) | (2u << 12);
    expectTrue(caps.missingT0() == "Vulkan 1.3", "missingT0 names the API version");
}

#if defined(FUSE_VULKAN_BACKEND) && (defined(__linux__) || defined(_WIN32))

#if defined(__linux__)
constexpr bool kMaskLayerAvailable = true;
#else
// Windows PE (Relight RL-0.7: Wine + Lavapipe): the mask layer and the second ICD are Linux-only, so
// only the unmasked scenarios run (tier report, tier caps, optional features, feature exercise).
constexpr bool kMaskLayerAvailable = false;
int setenv(const char* name, const char* value, int /*overwrite*/) {
    return _putenv_s(name, value);
}
int unsetenv(const char* name) {
    return _putenv_s(name, "");
}
#endif

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
constexpr const char* kMaskLayer = "VK_LAYER_FUSE_mask_features";
constexpr u64 kTimeoutNs = 5000000000ull;

std::string g_layerLibrary;

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

#if defined(__linux__)
std::string readFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// Absolute path of the ICD library named by `manifestPath` (see test_b2_physical_device_selection).
std::string resolveIcdLibrary(const std::string& manifestPath) {
    const std::string json = readFile(manifestPath);
    const size_t key = json.find("\"library_path\"");
    if (key == std::string::npos) {
        return {};
    }
    const size_t open = json.find('"', json.find(':', key) + 1);
    const size_t close = open == std::string::npos ? std::string::npos : json.find('"', open + 1);
    if (close == std::string::npos) {
        return {};
    }
    std::string library = json.substr(open + 1, close - open - 1);
    if (library.find('/') != std::string::npos && library[0] != '/') {
        const size_t slash = manifestPath.rfind('/');
        library = (slash == std::string::npos ? std::string(".") : manifestPath.substr(0, slash)) + "/" + library;
    }
    void* handle = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        return {};
    }
    std::string resolved;
    link_map* map = nullptr;
    if (dlinfo(handle, RTLD_DI_LINKMAP, &map) == 0 && map != nullptr && map->l_name != nullptr) {
        char real[PATH_MAX];
        if (realpath(map->l_name, real) != nullptr) {
            resolved = real;
        }
    }
    dlclose(handle);
    return resolved;
}

/// Two ICD manifests for the same library (distinct path strings) -> two real VkPhysicalDevices.
int registerIcdTwice(const std::string& dir) {
    const char* icds = std::getenv("VK_ICD_FILENAMES");
    if (icds == nullptr || icds[0] == '\0') {
        icds = std::getenv("VK_DRIVER_FILES");
    }
    std::string manifest = icds != nullptr && icds[0] != '\0' ? icds : "/usr/share/vulkan/icd.d/lvp_icd.json";
    manifest = manifest.substr(0, manifest.find(':'));
    const std::string library = resolveIcdLibrary(manifest);
    if (library.empty()) {
        std::printf("SKIP: cannot resolve the ICD library of %s\n", manifest.c_str());
        return kSkip;
    }
    const size_t slash = library.rfind('/');
    const std::string alias = library.substr(0, slash) + "/." + library.substr(slash);
    const std::string paths[2] = {library, alias};
    std::string list;
    for (int i = 0; i < 2; ++i) {
        const std::string path = dir + "/fuse_mask_icd_" + std::to_string(i) + ".json";
        std::ofstream out(path);
        out << "{\n    \"file_format_version\": \"1.0.1\",\n    \"ICD\": {\n        \"library_path\": \"" << paths[i]
            << "\",\n        \"api_version\": \"1.3.0\"\n    }\n}\n";
        if (!out) {
            std::fprintf(stderr, "FAIL: cannot write %s\n", path.c_str());
            return 1;
        }
        list += (i == 0 ? "" : ":") + path;
    }
    setenv("VK_ICD_FILENAMES", list.c_str(), 1);
    unsetenv("VK_DRIVER_FILES");
    return 0;
}
#endif // __linux__

/// What the mask layer saw reach the driver at the last vkCreateDevice.
struct DeviceCreateRecord {
    std::vector<std::string> extensions;
    std::vector<std::string> features;
    bool valid = false;

    static bool contains(const std::vector<std::string>& list, const char* name) {
        for (const std::string& entry : list) {
            if (entry == name) {
                return true;
            }
        }
        return false;
    }
    bool hasExtension(const char* name) const { return contains(extensions, name); }
    bool hasFeature(const char* name) const { return contains(features, name); }
};

#if defined(__linux__)
std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        if (end > start) {
            out.push_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return out;
}
#endif

/// Reads the record from the mask layer instance the loader has loaded (instance must be alive).
DeviceCreateRecord lastDeviceCreate() {
    DeviceCreateRecord record{};
#if defined(__linux__)
    void* handle = dlopen(g_layerLibrary.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (handle == nullptr) {
        return record;
    }
    using Fn = const char* (*)();
    auto fn = reinterpret_cast<Fn>(dlsym(handle, "fuseMaskFeaturesLastDeviceCreate"));
    if (fn != nullptr) {
        const std::string text = fn();
        const size_t semi = text.find(';');
        if (text.rfind("ext=", 0) == 0 && semi != std::string::npos && text.compare(semi + 1, 5, "feat=") == 0) {
            record.extensions = splitList(text.substr(4, semi - 4));
            record.features = splitList(text.substr(semi + 6));
            record.valid = true;
        }
    }
    dlclose(handle);
#endif
    return record;
}

u32 findMemoryType(VkPhysicalDevice physical, u32 bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0 && (memory.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

/// sync2 barriers + vkQueueSubmit2, a dynamic-rendering clear, a BDA query and a readback.
bool exerciseT0Features(const VulkanDevice& device) {
    const auto physical = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());
    const auto vk = static_cast<VkDevice>(device.nativeHandle());
    const auto queue = static_cast<VkQueue>(device.queues().graphics);
    constexpr u32 kSize = 4;
    constexpr VkDeviceSize kBytes = kSize * kSize * 4;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool ok = true;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {kSize, kSize, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ok = vkCreateImage(vk, &imageInfo, nullptr, &image) == VK_SUCCESS;
    if (ok) {
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(vk, image, &req);
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = findMemoryType(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        ok = alloc.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(vk, &alloc, nullptr, &imageMemory) == VK_SUCCESS &&
             vkBindImageMemory(vk, image, imageMemory, 0) == VK_SUCCESS;
    }
    if (ok) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageInfo.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ok = vkCreateImageView(vk, &viewInfo, nullptr, &view) == VK_SUCCESS;
    }
    if (ok) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = kBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ok = vkCreateBuffer(vk, &bufferInfo, nullptr, &buffer) == VK_SUCCESS;
    }
    if (ok) {
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(vk, buffer, &req);
        VkMemoryAllocateFlagsInfo flags{};
        flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.pNext = &flags;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = findMemoryType(physical, req.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        ok = alloc.memoryTypeIndex != UINT32_MAX &&
             vkAllocateMemory(vk, &alloc, nullptr, &bufferMemory) == VK_SUCCESS &&
             vkBindBufferMemory(vk, buffer, bufferMemory, 0) == VK_SUCCESS &&
             vkMapMemory(vk, bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS;
    }
    if (ok) {
        std::memset(mapped, 0, kBytes);
        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = buffer;
        expectTrue(vkGetBufferDeviceAddress(vk, &addressInfo) != 0, "bufferDeviceAddress returns an address");

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = device.queues().graphicsFamily;
        ok = vkCreateCommandPool(vk, &poolInfo, nullptr, &pool) == VK_SUCCESS;
    }
    if (ok) {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        ok = vkAllocateCommandBuffers(vk, &alloc, &cmd) == VK_SUCCESS &&
             vkCreateFence(vk, &fenceInfo, nullptr, &fence) == VK_SUCCESS;
    }
    if (ok) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        VkImageMemoryBarrier2 toAttachment{};
        toAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        toAttachment.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        toAttachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toAttachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toAttachment.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toAttachment.image = image;
        toAttachment.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &toAttachment;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.25f, 0.5f, 0.75f, 1.0f}};
        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, {kSize, kSize}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &rendering);
        vkCmdEndRendering(cmd);

        VkImageMemoryBarrier2 toTransfer = toAttachment;
        toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toTransfer.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        dep.pImageMemoryBarriers = &toTransfer;
        vkCmdPipelineBarrier2(cmd, &dep);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kSize, kSize, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);

        VkBufferMemoryBarrier2 toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toHost.buffer = buffer;
        toHost.size = VK_WHOLE_SIZE;
        VkDependencyInfo hostDep{};
        hostDep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        hostDep.bufferMemoryBarrierCount = 1;
        hostDep.pBufferMemoryBarriers = &toHost;
        vkCmdPipelineBarrier2(cmd, &hostDep);
        ok = vkEndCommandBuffer(cmd) == VK_SUCCESS;
    }
    if (ok) {
        VkCommandBufferSubmitInfo cmdInfo{};
        cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmdInfo.commandBuffer = cmd;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cmdInfo;
        ok = vkQueueSubmit2(queue, 1, &submit, fence) == VK_SUCCESS &&
             vkWaitForFences(vk, 1, &fence, VK_TRUE, kTimeoutNs) == VK_SUCCESS;
    }
    if (ok) {
        const auto* bytes = static_cast<const u8*>(mapped);
        const int expected[4] = {64, 128, 191, 255};
        for (VkDeviceSize i = 0; i < kBytes && ok; ++i) {
            const int diff = static_cast<int>(bytes[i]) - expected[i % 4];
            ok = diff >= -1 && diff <= 1;
        }
        expectTrue(ok, "dynamic-rendering clear read back through sync2 barriers");
    }
    vkDeviceWaitIdle(vk);
    if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(vk, fence, nullptr);
    }
    if (pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vk, pool, nullptr);
    }
    if (buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(vk, buffer, nullptr);
    }
    if (bufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(vk, bufferMemory, nullptr);
    }
    if (view != VK_NULL_HANDLE) {
        vkDestroyImageView(vk, view, nullptr);
    }
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(vk, image, nullptr);
    }
    if (imageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(vk, imageMemory, nullptr);
    }
    return ok;
}

struct Scenario {
    const char* what;
    const char* mask = "";          ///< FUSE_MASK_FEATURES
    const char* tierEnv = nullptr;  ///< FUSE_RENDER_TIER_MAX (nullptr = unset)
    bool allow12Env = false;        ///< FUSE_VK_ALLOW_1_2=1
    VulkanDeviceDesc desc{};
    int expectIndex = 0;            ///< -1 = creation must fail
    RenderTier expectTier = RenderTier::T2;
    RenderTier expectHardwareTier = RenderTier::T2;
    bool expectMeetsT0 = true;
    std::vector<RenderFeature> mustHave{};
    std::vector<RenderFeature> mustLack{};
    const char* messageContains = nullptr; ///< failure message / selection summary substring
    bool exercise = false;
};

void setOrUnset(const char* name, const char* value) {
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
}

std::vector<std::string> g_capsSummaries;

void runScenario(const Scenario& s) {
    const std::string tag = std::string("[") + s.what + "] ";
    setenv("FUSE_MASK_FEATURES", s.mask, 1);
    setOrUnset("FUSE_RENDER_TIER_MAX", s.tierEnv);
    setOrUnset("FUSE_VK_ALLOW_1_2", s.allow12Env ? "1" : nullptr);
    resetVulkanValidationCounters();
    {
        VulkanInstanceDesc instanceDesc{};
        instanceDesc.appName = "fuse_rp_device_tiers";
        instanceDesc.enableValidation = true;
        auto instance = VulkanInstance::create(instanceDesc);
        if (instance == nullptr || !instance->isValid()) {
            expectTrue(false, tag + "instance creation");
            return;
        }
        expectTrue(instance->info().apiVersion >= VK_API_VERSION_1_3, tag + "instance requests Vulkan >= 1.3");
        auto device = VulkanDevice::create(*instance, s.desc);
        const VulkanDeviceInfo& info = device->info();
        if (s.messageContains != nullptr) {
            const std::string haystack = info.message + " | " + info.selection;
            expectTrue(haystack.find(s.messageContains) != std::string::npos,
                       tag + "message/selection mentions '" + s.messageContains + "': " + haystack);
        }
        if (s.expectIndex < 0) {
            expectTrue(!device->isValid(), tag + "device creation must fail: " + info.selection);
            expectTrue(!info.caps.valid, tag + "no caps without a device");
        } else if (!device->isValid()) {
            expectTrue(false, tag + "device creation failed: " + info.message);
        } else {
            const RendererCaps& caps = info.caps;
            g_capsSummaries.push_back(tag + caps.summary());
            expectTrue(info.physicalDeviceIndex == static_cast<u32>(s.expectIndex),
                       tag + "selected #" + std::to_string(s.expectIndex) + ": " + info.selection);
            expectTrue(caps.valid, tag + "caps valid");
            expectTrue(caps.tier == s.expectTier, tag + "tier " + renderTierName(s.expectTier) + ", got " +
                                                      renderTierName(caps.tier));
            expectTrue(caps.hardwareTier == s.expectHardwareTier,
                       tag + "hardware tier " + renderTierName(s.expectHardwareTier) + ", got " +
                           renderTierName(caps.hardwareTier));
            expectTrue(caps.meetsT0 == s.expectMeetsT0, tag + "meetsT0");
            expectTrue(!s.expectMeetsT0 || caps.missingT0().empty(), tag + "nothing missing at T0");
            expectTrue(s.expectMeetsT0 || !caps.missingT0().empty(), tag + "missingT0 names the gap");
            expectTrue((caps.enabledMask & ~caps.supportedMask) == 0, tag + "enabled subset of supported");
            expectTrue((caps.enabledMask & renderFeaturesAboveTier(caps.tierCap)) == 0,
                       tag + "nothing above the tier cap enabled");
            expectTrue(caps.apiVersion == info.apiVersion, tag + "caps apiVersion");
            expectTrue(!s.expectMeetsT0 || info.apiVersion >= VK_API_VERSION_1_3, tag + "device API >= 1.3");
            expectTrue(!caps.cooperativeMatrix, tag + "Lavapipe has no cooperative matrix (never T3)");
            for (RenderFeature f : s.mustHave) {
                expectTrue(caps.has(f), tag + "has " + renderFeatureInfo(f).name);
            }
            for (RenderFeature f : s.mustLack) {
                expectTrue(!caps.has(f), tag + "lacks " + renderFeatureInfo(f).name);
                expectTrue(caps.fallbackFor(f) != nullptr, tag + "fallback for " + renderFeatureInfo(f).name);
            }
            expectTrue(info.dynamicRendering == caps.dynamicRendering &&
                           info.timelineSemaphore == caps.timelineSemaphore &&
                           info.bufferDeviceAddress == caps.bufferDeviceAddress,
                       tag + "legacy VulkanDeviceInfo bools agree with caps");

            // Query back: what reached the driver (recorded by the mask layer below validation).
            const DeviceCreateRecord record = lastDeviceCreate();
            expectTrue(record.valid || !kMaskLayerAvailable, tag + "mask layer recorded vkCreateDevice");
            if (record.valid) {
                for (u32 i = 0; i < kRenderFeatureCount; ++i) {
                    const auto f = static_cast<RenderFeature>(i);
                    if (f == RenderFeature::DeviceGeneratedCommands) {
                        continue; // not in the layer's table (headers may predate it)
                    }
                    const char* name = renderFeatureInfo(f).name;
                    expectTrue(caps.has(f) == record.hasFeature(name),
                               tag + name + (caps.has(f) ? " reported but not passed to vkCreateDevice"
                                                         : " passed to vkCreateDevice but not reported"));
                }
                const bool meshExt = record.hasExtension(VK_EXT_MESH_SHADER_EXTENSION_NAME);
                expectTrue(meshExt == (caps.meshShader || caps.taskShader), tag + "VK_EXT_mesh_shader iff mesh");
                expectTrue(record.hasExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ==
                               caps.accelerationStructure,
                           tag + "VK_KHR_acceleration_structure iff AS");
                expectTrue(record.hasExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) ==
                               caps.accelerationStructure,
                           tag + "VK_KHR_deferred_host_operations iff AS");
                expectTrue(record.hasExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME) == caps.rayQuery,
                           tag + "VK_KHR_ray_query iff rayQuery");
                expectTrue(record.hasExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == caps.rayTracingPipeline,
                           tag + "VK_KHR_ray_tracing_pipeline iff RT pipeline");
                expectTrue(record.hasExtension(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME) == caps.descriptorBuffer,
                           tag + "VK_EXT_descriptor_buffer iff descriptorBuffer");
                expectTrue(record.hasExtension(VK_EXT_SHADER_OBJECT_EXTENSION_NAME) == caps.shaderObject,
                           tag + "VK_EXT_shader_object iff shaderObject");
                expectTrue(record.hasExtension(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME) ==
                               caps.shaderImageInt64Atomics,
                           tag + "VK_EXT_shader_image_atomic_int64 iff image int64 atomics");
            }
            const auto vk = static_cast<VkDevice>(device->nativeHandle());
            if (caps.meshShader) {
                expectTrue(vkGetDeviceProcAddr(vk, "vkCmdDrawMeshTasksEXT") != nullptr, tag + "mesh entry point");
            }
            if (caps.accelerationStructure) {
                expectTrue(vkGetDeviceProcAddr(vk, "vkCreateAccelerationStructureKHR") != nullptr,
                           tag + "AS entry point");
            }
            if (caps.rayTracingPipeline) {
                expectTrue(vkGetDeviceProcAddr(vk, "vkCmdTraceRaysKHR") != nullptr, tag + "RT entry point");
            }
            if (s.exercise) {
                expectTrue(exerciseT0Features(*device), tag + "sync2 / dynamic rendering / BDA exercise");
            }
        }
        device.reset();
        instance.reset();
    }
    const VulkanValidationCounters counters = vulkanValidationCounters();
    expectTrue(counters.errors == 0 && counters.warnings == 0,
               tag + "zero validation messages (errors " + std::to_string(counters.errors) + ", warnings " +
                   std::to_string(counters.warnings) + "): " + counters.lastError);
}

int run(const char* layerDir) {
#if defined(__linux__)
    if (layerDir == nullptr) {
        std::fprintf(stderr, "FAIL: --layer-dir required\n");
        return 1;
    }
    if (const int status = registerIcdTwice(layerDir); status != 0) {
        return status;
    }
    g_layerLibrary = std::string(layerDir) + "/libVkLayer_fuse_mask_features.so";
    std::string path = layerDir;
    const char* existing = std::getenv("VK_ADD_LAYER_PATH");
    if (existing != nullptr && existing[0] != '\0') {
        path += ":";
        path += existing;
    }
    setenv("VK_ADD_LAYER_PATH", path.c_str(), 1);
    setenv("NODEVICE_SELECT", "1", 1); // keep enumeration order (see test_b2_physical_device_selection)
    const bool validation = layerAvailable(kValidationLayer);
    std::string layers = validation ? std::string(kValidationLayer) + ":" : std::string();
    layers += kMaskLayer; // application -> driver: validation sees the masked device
    setenv("VK_INSTANCE_LAYERS", layers.c_str(), 1);
    if (validation) {
        setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
        setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
    } else {
        std::printf("note: %s not installed — running without validation\n", kValidationLayer);
    }
    if (!layerAvailable(kMaskLayer)) {
        std::fprintf(stderr, "FAIL: %s not found under %s\n", kMaskLayer, layerDir);
        return 1;
    }
#else
    (void)layerDir;
    const bool validation = layerAvailable(kValidationLayer);
    if (validation) {
        setenv("VK_INSTANCE_LAYERS", kValidationLayer, 1);
        setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
        setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
    } else {
        std::printf("note: %s not available — running without validation\n", kValidationLayer);
    }
#endif
    {
        VulkanInstanceDesc probeDesc{};
        probeDesc.enableValidation = false;
        auto probe = VulkanInstance::create(probeDesc);
        if (probe == nullptr || !probe->isValid()) {
            std::printf("SKIP: no Vulkan instance\n");
            return kSkip;
        }
        if (probe->info().apiVersion < VK_API_VERSION_1_3) {
            std::printf("SKIP: Vulkan loader older than 1.3\n");
            return kSkip;
        }
    }

    using F = RenderFeature;
    const std::vector<F> t0 = {F::Synchronization2,  F::DynamicRendering,     F::Maintenance4,
                               F::TimelineSemaphore, F::DescriptorIndexing,   F::BufferDeviceAddress,
                               F::DrawIndirectCount, F::MultiDrawIndirect,    F::ShaderDrawParameters,
                               F::ShaderInt64,       F::ShaderBufferInt64Atomics};
    auto with = [&t0](std::initializer_list<F> extra) {
        std::vector<F> out = t0;
        out.insert(out.end(), extra.begin(), extra.end());
        return out;
    };
    VulkanDeviceDesc noPreference{};
    noPreference.preferDiscreteGpu = false;
    VulkanDeviceDesc descT1{};
    descT1.maxTier = RenderTier::T1;
    VulkanDeviceDesc allowLegacy{};
    allowLegacy.allowBelowT0 = true;
    VulkanDeviceDesc noOptional{};
    noOptional.enableOptionalFeatures = false;

    std::vector<Scenario> scenarios;
    auto add = [&scenarios](Scenario s) { scenarios.push_back(std::move(s)); };
    // Unmasked Lavapipe: T2 (RT pipeline yes, cooperative matrix no), everything but coop enabled.
    add({.what = "lavapipe T2",
         .mustHave = with({F::ShaderImageInt64Atomics, F::DescriptorBuffer, F::ShaderObject, F::TaskShader,
                           F::MeshShader, F::AccelerationStructure, F::RayQuery, F::RayTracingPipeline}),
         .mustLack = {F::CooperativeMatrix},
         .exercise = true});
    // Tier caps: env and desc. Features above the cap are neither enabled nor reported.
    add({.what = "FUSE_RENDER_TIER_MAX=T1", .tierEnv = "T1", .expectTier = RenderTier::T1,
         .mustHave = with({F::MeshShader, F::TaskShader, F::DescriptorBuffer}),
         .mustLack = {F::AccelerationStructure, F::RayQuery, F::RayTracingPipeline}});
    add({.what = "FUSE_RENDER_TIER_MAX=0", .tierEnv = "0", .expectTier = RenderTier::T0,
         .mustHave = with({F::ShaderObject, F::ShaderImageInt64Atomics}),
         .mustLack = {F::MeshShader, F::TaskShader, F::AccelerationStructure, F::RayQuery, F::RayTracingPipeline},
         .exercise = true});
    add({.what = "desc.maxTier=T1", .desc = descT1, .expectTier = RenderTier::T1,
         .mustHave = with({F::MeshShader}), .mustLack = {F::RayQuery}});
    add({.what = "env T2 over desc T1 (min wins)", .tierEnv = "T2", .desc = descT1, .expectTier = RenderTier::T1});
    add({.what = "optional features off", .desc = noOptional, .expectTier = RenderTier::T0, .mustHave = t0,
         .mustLack = {F::ShaderImageInt64Atomics, F::DescriptorBuffer, F::ShaderObject, F::MeshShader, F::RayQuery}});
    // Masked hardware: the hardware tier itself drops.
    add({.what = "mask VK_EXT_mesh_shader -> T0", .mask = "VK_EXT_mesh_shader", .expectTier = RenderTier::T0,
         .expectHardwareTier = RenderTier::T0, .mustHave = with({F::AccelerationStructure, F::RayQuery}),
         .mustLack = {F::MeshShader, F::TaskShader}, .exercise = true});
    add({.what = "mask VK_KHR_ray_query -> T1", .mask = "VK_KHR_ray_query", .expectTier = RenderTier::T1,
         .expectHardwareTier = RenderTier::T1, .mustHave = with({F::MeshShader, F::AccelerationStructure}),
         .mustLack = {F::RayQuery}});
    add({.what = "mask rayQuery feature bit -> T1", .mask = "rayQuery", .expectTier = RenderTier::T1,
         .expectHardwareTier = RenderTier::T1, .mustLack = {F::RayQuery}});
    add({.what = "mask meshShader feature bit -> T0", .mask = "meshShader", .expectTier = RenderTier::T0,
         .expectHardwareTier = RenderTier::T0, .mustLack = {F::MeshShader, F::TaskShader}});
    add({.what = "mask VK_KHR_acceleration_structure -> T1", .mask = "VK_KHR_acceleration_structure",
         .expectTier = RenderTier::T1, .expectHardwareTier = RenderTier::T1,
         .mustLack = {F::AccelerationStructure, F::RayQuery, F::RayTracingPipeline}});
    add({.what = "mask optional extensions", .mask = "VK_EXT_shader_image_atomic_int64,VK_EXT_descriptor_buffer,VK_EXT_shader_object",
         .mustHave = t0, .mustLack = {F::ShaderImageInt64Atomics, F::DescriptorBuffer, F::ShaderObject}});
    // T0 hard requirements: device rejected (every device masked -> creation fails).
    add({.what = "mask shaderInt64 -> rejected", .mask = "shaderInt64", .expectIndex = -1,
         .messageContains = "rejected, T0 requirements missing: shaderInt64"});
    add({.what = "mask synchronization2 -> rejected", .mask = "synchronization2", .expectIndex = -1,
         .messageContains = "synchronization2"});
    add({.what = "mask drawIndirectCount+shaderDrawParameters -> rejected",
         .mask = "drawIndirectCount,shaderDrawParameters", .expectIndex = -1,
         .messageContains = "drawIndirectCount,shaderDrawParameters"});
    add({.what = "mask shaderBufferInt64Atomics -> rejected", .mask = "shaderBufferInt64Atomics", .expectIndex = -1,
         .messageContains = "shaderBufferInt64Atomics"});
    add({.what = "mask api=1.2 -> rejected", .mask = "api=1.2", .expectIndex = -1, .messageContains = "Vulkan 1.3"});
    // FUSE_VK_ALLOW_1_2 escape: legacy device accepted, reported below T0.
    add({.what = "api=1.2 + allowBelowT0", .mask = "api=1.2", .desc = allowLegacy, .expectTier = RenderTier::T0,
         .expectHardwareTier = RenderTier::T0, .expectMeetsT0 = false,
         .mustHave = {F::Synchronization2, F::DynamicRendering, F::TimelineSemaphore, F::ShaderInt64}});
    add({.what = "shaderInt64 masked + FUSE_VK_ALLOW_1_2", .mask = "shaderInt64", .allow12Env = true,
         .expectTier = RenderTier::T0, .expectHardwareTier = RenderTier::T0, .expectMeetsT0 = false,
         .mustLack = {F::ShaderInt64}});
    // Selection policy with two real devices (#0, #1; both Lavapipe, same type).
    add({.what = "#0 misses T0 -> #1", .mask = "0:shaderInt64", .expectIndex = 1,
         .messageContains = "#0 'llvmpipe"});
    add({.what = "#0 misses T0 (selection text)", .mask = "0:shaderInt64", .expectIndex = 1,
         .messageContains = "rejected, T0 requirements missing: shaderInt64"});
    add({.what = "higher tier wins: #0 T0, #1 T2", .mask = "0:VK_EXT_mesh_shader", .expectIndex = 1});
    add({.what = "higher tier wins: #0 T2, #1 T1", .mask = "1:VK_KHR_ray_query", .expectIndex = 0});
    add({.what = "no preference: first suitable", .mask = "0:VK_EXT_mesh_shader", .desc = noPreference,
         .expectIndex = 0, .expectTier = RenderTier::T0, .expectHardwareTier = RenderTier::T0});
    add({.what = "cap equalises tiers: enumeration order", .mask = "0:VK_EXT_mesh_shader", .tierEnv = "T0",
         .expectIndex = 0, .expectTier = RenderTier::T0, .expectHardwareTier = RenderTier::T0});
    add({.what = "legacy device ranks below T0 device", .mask = "0:api=1.2", .desc = allowLegacy, .expectIndex = 1});

    size_t ran = 0;
    for (const Scenario& s : scenarios) {
        if (!kMaskLayerAvailable && s.mask[0] != '\0') {
            continue; // needs VK_LAYER_FUSE_mask_features (Linux)
        }
        runScenario(s);
        ++ran;
    }
    unsetenv("FUSE_MASK_FEATURES");
    unsetenv("FUSE_RENDER_TIER_MAX");
    unsetenv("FUSE_VK_ALLOW_1_2");
    for (const std::string& line : g_capsSummaries) {
        std::printf("%s\n", line.c_str());
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_rp_device_tiers: OK (%zu of %zu scenarios, %s validation)\n", ran, scenarios.size(),
                validation ? "with" : "without");
    return 0;
}

#endif // FUSE_VULKAN_BACKEND && (__linux__ || _WIN32)

} // namespace

int main(int argc, char** argv) {
    checkTierLogic();
#if defined(FUSE_VULKAN_BACKEND) && (defined(__linux__) || defined(_WIN32))
    if (g_failures != 0) {
        return 1;
    }
    const char* layerDir = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--layer-dir") == 0 && i + 1 < argc) {
            layerDir = argv[++i];
        }
    }
    return run(layerDir);
#else
    (void)argc;
    (void)argv;
    if (g_failures != 0) {
        return 1;
    }
    std::printf("SKIP: tier logic OK; device checks need the Vulkan backend on Linux or Windows\n");
    return kSkip;
#endif
}
