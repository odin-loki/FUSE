// Gate fuse_rp_vk_adopt (RL-4.1 blocker 2): VulkanDevice::adopt wraps an externally created device.
//
//  A  A device created the normal way (VulkanDevice::create) is adopted into a second VulkanDevice
//     (non-owning, VulkanDevice::adoptionDesc). The adoptee reports the same handles, extensions,
//     RendererCaps / tier, descriptor limits and queues; volk stays in device-level dispatch (one
//     distinct device, reference-counted registration). A bindless + render-graph smoke test runs
//     through the adoptee: GpuAllocator, BindlessDescriptors (Auto backend), rg::Executor, and a
//     graph fill -> bindless compute (SSBO handle + buffer-address table) -> copy -> host read.
//     Destroying the adoptee leaves the original usable: same dispatch mode, and the smoke test
//     runs again through the original.
//  B  A raw instance + device created the way the RL-1.1 bootstrap or DXVK does (vkCreateInstance with
//     validation and a create-time messenger, vkCreateDevice declaring only the T0 feature set, no
//     extensions) is adopted with takeOwnership. The adoptee reports the foreign instance, caps of
//     exactly T0 (no descriptor buffer, no mesh shaders), runs the smoke test on the descriptor-set
//     bindless backend while two devices are live (loader trampolines), and destroying it destroys
//     the device: a leak would be reported by the object tracker inside vkDestroyInstance, which the
//     create-time messenger sees.
//
// Validation + synchronization validation on; every message fails the run. Exit 77 = skip (stub
// build, no ICD / validation layer, shader not built).
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/loader.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace {

constexpr int kSkip = 77;
int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

} // namespace

#if !defined(FUSE_VULKAN_BACKEND)

int main() {
    using namespace fuse::renderer;
    // Stub build: adopt() compiles and reports an invalid device.
    VulkanDeviceAdoptDesc desc{};
    auto device = VulkanDevice::adopt(desc);
    expect(device != nullptr && !device->isValid() && device->isAdopted(), "stub: adopt yields an invalid device");
    if (g_failures != 0) {
        return 1;
    }
    std::printf("SKIP: fuse_rp_vk_adopt: Vulkan backend disabled (stub adopt checked)\n");
    return kSkip;
}

#else

namespace {

using namespace fuse::renderer;
using fuse::u32;
using fuse::u64;
using fuse::u8;

#if defined(_WIN32)
int setenv(const char* name, const char* value, int overwrite) {
    if (overwrite == 0 && std::getenv(name) != nullptr) {
        return 0;
    }
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
#endif

constexpr u32 kWords = 1024;

BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}

std::vector<char> readShader() {
#if defined(FUSE_RP_VK_ADOPT_SHADER)
    std::ifstream file(FUSE_RP_VK_ADOPT_SHADER, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
#else
    return {};
#endif
}

// ---- The smoke test: bindless + render graph through one VulkanDevice ---------------------------

struct Smoke {
    VkDevice device = VK_NULL_HANDLE;
    BindlessDescriptors* bindless = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

struct FillPass {
    rg::BufferRef buffer;
    u32 value = 0;
};
void fillFn(const rg::PassContext& c, void* user) {
    const auto* p = static_cast<const FillPass*>(user);
    vkCmdFillBuffer(static_cast<VkCommandBuffer>(c.commandBuffer), static_cast<VkBuffer>(c.buffer(p->buffer)), 0,
                    VK_WHOLE_SIZE, p->value);
}

struct BindlessPass {
    const Smoke* smoke = nullptr;
    u32 push[4] = {};
};
void bindlessFn(const rg::PassContext& c, void* user) {
    const auto* p = static_cast<const BindlessPass*>(user);
    const auto cmd = static_cast<VkCommandBuffer>(c.commandBuffer);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->smoke->pipeline);
    p->smoke->bindless->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->smoke->layout, 0);
    vkCmdPushConstants(cmd, p->smoke->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p->push), p->push);
    vkCmdDispatch(cmd, (kWords + 63u) / 64u, 1, 1);
}

struct CopyPass {
    rg::BufferRef src, dst;
};
void copyFn(const rg::PassContext& c, void* user) {
    const auto* p = static_cast<const CopyPass*>(user);
    const VkBufferCopy region{0, 0, kWords * sizeof(u32)};
    vkCmdCopyBuffer(static_cast<VkCommandBuffer>(c.commandBuffer), static_cast<VkBuffer>(c.buffer(p->src)),
                    static_cast<VkBuffer>(c.buffer(p->dst)), 1, &region);
}

/// Runs the smoke test through `device`; `expected` is the bindless backend it must pick (Auto).
void runSmoke(VulkanDevice& device, BindlessBackend expected, const std::string& what) {
    const std::vector<char> code = readShader();
    if (code.empty() || code.size() % 4u != 0u) {
        expect(false, what + ": smoke shader not readable");
        return;
    }
    Smoke smoke{};
    smoke.device = static_cast<VkDevice>(device.nativeHandle());

    auto allocator = GpuAllocator::create(device);
    expect(allocator != nullptr && allocator->isValid(), what + ": GpuAllocator on the device");
    if (allocator == nullptr || !allocator->isValid()) {
        return;
    }
    BindlessDescriptors bindless;
    smoke.bindless = &bindless;
    BindlessDesc bindlessDesc{};
    bindlessDesc.maxTextures = 64;
    bindlessDesc.maxBuffers = 64;
    bindlessDesc.maxSamplers = 16;
    const bool bindlessOk = bindless.init(device, bindlessDesc);
    expect(bindlessOk, what + ": bindless GPU backend created");
    expect(bindless.backend() == expected, what + ": bindless backend " + bindlessBackendName(bindless.backend()) +
                                               ", expected " + bindlessBackendName(expected));

    Buffer src{}, dst{}, readback{};
    BufferDesc desc{};
    desc.size = kWords * sizeof(u32);
    desc.usage = BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::ShaderDeviceAddress;
    desc.memoryUsage = MemoryUsage::GpuOnly;
    desc.name = "rp_vk_adopt.src";
    bool ok = allocator->createBuffer(desc, src);
    desc.usage = BufferUsage::Storage | BufferUsage::TransferSrc;
    desc.name = "rp_vk_adopt.dst";
    ok = allocator->createBuffer(desc, dst) && ok;
    desc.usage = BufferUsage::TransferDst;
    desc.memoryUsage = MemoryUsage::GpuToCpu;
    desc.name = "rp_vk_adopt.readback";
    ok = allocator->createBuffer(desc, readback) && readback.mapped != nullptr && ok;
    expect(ok && src.deviceAddress != 0u, what + ": buffers (src has a device address)");

    VkDescriptorSetLayout setLayout = static_cast<VkDescriptorSetLayout>(bindless.layoutHandle());
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 * sizeof(u32)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    ok = ok && bindlessOk && vkCreatePipelineLayout(smoke.device, &layoutInfo, nullptr, &smoke.layout) == VK_SUCCESS;
    VkShaderModule module = VK_NULL_HANDLE;
    if (ok) {
        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = code.size();
        moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        ok = vkCreateShaderModule(smoke.device, &moduleInfo, nullptr, &module) == VK_SUCCESS;
    }
    if (ok) {
        VkComputePipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        info.flags = static_cast<VkPipelineCreateFlags>(bindless.pipelineCreateFlags());
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = module;
        info.stage.pName = "main";
        info.layout = smoke.layout;
        ok = vkCreateComputePipelines(smoke.device, VK_NULL_HANDLE, 1, &info, nullptr, &smoke.pipeline) == VK_SUCCESS;
        vkDestroyShaderModule(smoke.device, module, nullptr);
    }
    expect(ok, what + ": pipeline layout + compute pipeline");

    const BindlessSlotHandle srcSlot = ok ? bindless.registerBufferSlot(src) : BindlessSlotHandle::invalid();
    const BindlessSlotHandle dstSlot = ok ? bindless.registerBufferSlot(dst) : BindlessSlotHandle::invalid();
    expect(srcSlot.isValid() && dstSlot.isValid(), what + ": bindless buffer slots");

    auto executor = ok ? rg::Executor::create(device, allocator.get()) : nullptr;
    expect(executor != nullptr && executor->isValid(), what + ": render-graph executor");
    if (ok && srcSlot.isValid() && dstSlot.isValid() && executor != nullptr && executor->isValid()) {
        rg::Graph graph;
        for (u32 frame = 0; frame < 3; ++frame) {
            const u32 value = 0x1000u + frame * 0x111u;
            const u32 add = 7u + frame;
            std::memset(readback.mapped, 0, kWords * sizeof(u32));
            graph.reset();
            const rg::BufferRef srcRef = graph.importBuffer({src.handle, kWords * 4u, rg::kNoQueue, nullptr, "src"});
            const rg::BufferRef dstRef = graph.importBuffer({dst.handle, kWords * 4u, rg::kNoQueue, nullptr, "dst"});
            const rg::BufferRef outRef =
                graph.importBuffer({readback.handle, kWords * 4u, rg::kNoQueue, nullptr, "readback"});
            FillPass fill{srcRef, value};
            BindlessPass compute{};
            compute.smoke = &smoke;
            compute.push[0] = bindless.shaderHandle(srcSlot);
            compute.push[1] = bindless.shaderHandle(dstSlot);
            compute.push[2] = kWords;
            compute.push[3] = add;
            CopyPass copy{dstRef, outRef};
            graph.addPass("adopt.fill", fillFn, &fill).use(srcRef, rg::Access::TransferDst);
            graph.addPass("adopt.bindless", bindlessFn, &compute)
                .use(srcRef, rg::Access::StorageRead, {}, rg::kStageCompute)
                .use(dstRef, rg::Access::StorageWrite, {}, rg::kStageCompute);
            graph.addPass("adopt.copy", copyFn, &copy)
                .use(dstRef, rg::Access::TransferSrc)
                .use(outRef, rg::Access::TransferDst);
            graph.addPass("adopt.host_read", nullptr, nullptr).use(outRef, rg::Access::HostRead);
            const rg::ExecuteResult result = executor->execute(graph);
            expect(result.ok && result.executedPasses == 4u, what + ": graph executed (4 passes)");
            expect(executor->waitIdle(), what + ": graph retired");
            std::vector<u32> words(kWords, 0u);
            allocator->readMapped(readback, words.data(), kWords * sizeof(u32), 0);
            u32 bad = 0;
            for (u32 w : words) {
                bad += w != value * 2u + add ? 1u : 0u;
            }
            expect(bad == 0u, what + ": frame " + std::to_string(frame) + " readback (" + std::to_string(bad) +
                                  " wrong words, first " + std::to_string(words[0]) + ")");
        }
        std::printf("%s: bindless %s, 3 frames x 4 passes OK\n", what.c_str(), bindlessBackendName(bindless.backend()));
    }
    executor.reset();
    vkDeviceWaitIdle(smoke.device);
    if (srcSlot.isValid()) {
        bindless.unregisterSlot(srcSlot);
    }
    if (dstSlot.isValid()) {
        bindless.unregisterSlot(dstSlot);
    }
    if (smoke.pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(smoke.device, smoke.pipeline, nullptr);
    }
    if (smoke.layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(smoke.device, smoke.layout, nullptr);
    }
    bindless.destroy(device);
    for (Buffer* b : {&src, &dst, &readback}) {
        if (b->handle != nullptr) {
            allocator->destroyBuffer(*b);
        }
    }
}

// ---- Checks --------------------------------------------------------------------------------------

std::set<std::string> names(const std::vector<const char*>& list) {
    std::set<std::string> out;
    for (const char* n : list) {
        out.insert(n);
    }
    return out;
}

void expectSameDevice(const VulkanDevice& original, const VulkanDevice& adoptee) {
    const VulkanDeviceInfo& a = original.info();
    const VulkanDeviceInfo& b = adoptee.info();
    expect(adoptee.nativeHandle() == original.nativeHandle() &&
               adoptee.nativePhysicalDevice() == original.nativePhysicalDevice() &&
               adoptee.instanceHandle() == original.instanceHandle(),
           "A: adoptee wraps the same instance / physical device / device");
    expect(b.deviceName == a.deviceName && b.deviceType == a.deviceType && b.apiVersion == a.apiVersion,
           "A: name, type, apiVersion");
    expect(b.physicalDeviceIndex == a.physicalDeviceIndex, "A: physical device index");
    expect(names(b.enabledExtensions) == names(a.enabledExtensions), "A: enabled extensions");
    const RendererCaps& ca = a.caps;
    const RendererCaps& cb = b.caps;
    expect(cb.valid && cb.tier == ca.tier && cb.hardwareTier == ca.hardwareTier && cb.tierCap == ca.tierCap &&
               cb.meetsT0 == ca.meetsT0 && cb.apiVersion == ca.apiVersion,
           "A: tier / hardware tier / cap / meetsT0 (" + cb.summary() + " vs " + ca.summary() + ")");
    expect(cb.enabledMask == ca.enabledMask && cb.supportedMask == ca.supportedMask, "A: enabled / supported masks");
    expect(b.descriptorIndexing == a.descriptorIndexing && b.sampledImageUpdateAfterBind == a.sampledImageUpdateAfterBind &&
               b.storageImageUpdateAfterBind == a.storageImageUpdateAfterBind &&
               b.storageBufferUpdateAfterBind == a.storageBufferUpdateAfterBind &&
               b.uniformBufferUpdateAfterBind == a.uniformBufferUpdateAfterBind &&
               b.sampledImageNonUniformIndexing == a.sampledImageNonUniformIndexing,
           "A: descriptor indexing flags");
    expect(std::memcmp(&b.descriptorLimits, &a.descriptorLimits, sizeof(VulkanDescriptorLimits)) == 0,
           "A: descriptor limits");
    expect(b.swapchainExtension == a.swapchainExtension && b.bufferDeviceAddress == a.bufferDeviceAddress &&
               b.timelineSemaphore == a.timelineSemaphore && b.dynamicRendering == a.dynamicRendering &&
               b.pipelineCreationCacheControl == a.pipelineCreationCacheControl &&
               b.samplerAnisotropy == a.samplerAnisotropy && b.maxSamplerAnisotropy == a.maxSamplerAnisotropy &&
               b.geometryShader == a.geometryShader && b.fragmentStoresAndAtomics == a.fragmentStoresAndAtomics,
           "A: feature flags");
    expect(b.queues.graphics == a.queues.graphics && b.queues.compute == a.queues.compute &&
               b.queues.transfer == a.queues.transfer && b.queues.graphicsFamily == a.queues.graphicsFamily &&
               b.queues.computeFamily == a.queues.computeFamily && b.queues.transferFamily == a.queues.transferFamily &&
               b.queues.dedicatedCompute == a.queues.dedicatedCompute &&
               b.queues.dedicatedTransfer == a.queues.dedicatedTransfer,
           "A: queues and families");
}

void expectDispatch(vkloader::DispatchMode mode, const void* device, const std::string& what) {
    expect(vkloader::dispatchMode() == mode, what + ": dispatch mode " +
                                                 std::to_string(static_cast<int>(vkloader::dispatchMode())) +
                                                 ", expected " + std::to_string(static_cast<int>(mode)));
    expect(vkloader::dispatchDevice() == device, what + ": loaded device");
}

/// A bootstrap-style instance: validation + sync validation, and a messenger chained into
/// VkInstanceCreateInfo so messages emitted inside vkDestroyInstance (leaked objects) are seen.
struct RawInstance {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE; ///< everything between create and destroy
    u32 apiVersion = 0;
    std::vector<const char*> extensions;
    u32 messages = 0;
    std::string lastMessage;
};

VKAPI_ATTR VkBool32 VKAPI_CALL onRawMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                            void* user) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) != 0) {
        auto* raw = static_cast<RawInstance*>(user);
        ++raw->messages;
        raw->lastMessage = data != nullptr && data->pMessage != nullptr ? data->pMessage : "";
        std::fprintf(stderr, "VALIDATION (raw instance): %s\n", raw->lastMessage.c_str());
    }
    return VK_FALSE;
}

bool createRawInstance(RawInstance& raw) {
    raw.apiVersion = VK_API_VERSION_1_3;
    raw.extensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const char* layer = "VK_LAYER_KHRONOS_validation";
    VkDebugUtilsMessengerCreateInfoEXT messenger{};
    messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    messenger.pfnUserCallback = onRawMessage;
    messenger.pUserData = &raw;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "fuse_rp_vk_adopt.bootstrap";
    app.apiVersion = raw.apiVersion;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pNext = &messenger;
    info.pApplicationInfo = &app;
    info.enabledLayerCount = 1;
    info.ppEnabledLayerNames = &layer;
    info.enabledExtensionCount = static_cast<u32>(raw.extensions.size());
    info.ppEnabledExtensionNames = raw.extensions.data();
    if (vkCreateInstance(&info, nullptr, &raw.instance) != VK_SUCCESS) {
        return false;
    }
    // The chained messenger covers only vkCreateInstance / vkDestroyInstance; this one the rest.
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(raw.instance, "vkCreateDebugUtilsMessengerEXT"));
    return create != nullptr && create(raw.instance, &messenger, nullptr, &raw.messenger) == VK_SUCCESS;
}

void destroyRawInstance(RawInstance& raw) {
    if (raw.instance == VK_NULL_HANDLE) {
        return;
    }
    auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(raw.instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroyMessenger != nullptr && raw.messenger != VK_NULL_HANDLE) {
        destroyMessenger(raw.instance, raw.messenger, nullptr);
    }
    // Through the instance's own chain (the creator's dispatch), not whatever volk has loaded.
    auto destroy =
        reinterpret_cast<PFN_vkDestroyInstance>(vkGetInstanceProcAddr(raw.instance, "vkDestroyInstance"));
    if (destroy != nullptr) {
        destroy(raw.instance, nullptr);
    }
    raw.instance = VK_NULL_HANDLE;
}

// ---- B: a raw device, T0 only (what a bootstrap would create) -----------------------------------

VkDevice createRawT0Device(VkPhysicalDevice physical, u32 family, VkPhysicalDeviceFeatures2& f2,
                           VkPhysicalDeviceVulkan11Features& f11, VkPhysicalDeviceVulkan12Features& f12,
                           VkPhysicalDeviceVulkan13Features& f13) {
    VkPhysicalDeviceVulkan11Features s11{};
    s11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features s12{};
    s12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    s12.pNext = &s11;
    VkPhysicalDeviceFeatures2 s2{};
    s2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    s2.pNext = &s12;
    vkGetPhysicalDeviceFeatures2(physical, &s2);

    f11 = {};
    f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    f11.shaderDrawParameters = VK_TRUE;
    f12 = {};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f11;
    f12.timelineSemaphore = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.drawIndirectCount = VK_TRUE;
    f12.shaderBufferInt64Atomics = VK_TRUE;
    // What the bindless descriptor-set backend binds with, when supported.
    f12.runtimeDescriptorArray = s12.runtimeDescriptorArray;
    f12.descriptorBindingPartiallyBound = s12.descriptorBindingPartiallyBound;
    f12.descriptorBindingSampledImageUpdateAfterBind = s12.descriptorBindingSampledImageUpdateAfterBind;
    f12.descriptorBindingStorageImageUpdateAfterBind = s12.descriptorBindingStorageImageUpdateAfterBind;
    f12.descriptorBindingStorageBufferUpdateAfterBind = s12.descriptorBindingStorageBufferUpdateAfterBind;
    f12.descriptorBindingUniformBufferUpdateAfterBind = s12.descriptorBindingUniformBufferUpdateAfterBind;
    f12.shaderSampledImageArrayNonUniformIndexing = s12.shaderSampledImageArrayNonUniformIndexing;
    f12.shaderStorageBufferArrayNonUniformIndexing = s12.shaderStorageBufferArrayNonUniformIndexing;
    f12.shaderStorageImageArrayNonUniformIndexing = s12.shaderStorageImageArrayNonUniformIndexing;
    f13 = {};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.pNext = &f12;
    f13.synchronization2 = VK_TRUE;
    f13.dynamicRendering = VK_TRUE;
    f13.maintenance4 = VK_TRUE;
    f2 = {};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f13;
    f2.features.multiDrawIndirect = VK_TRUE;
    f2.features.shaderInt64 = VK_TRUE;

    const float priority = 1.f;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &f2;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physical, &info, nullptr, &device) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return device;
}

int run() {
    setenv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT", 1);
    setenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true", 1);
    if (readShader().empty()) {
        std::printf("SKIP: fuse_rp_vk_adopt: rp_vk_adopt.comp not built (glslangValidator missing)\n");
        return kSkip;
    }
    resetVulkanValidationCounters();
    VulkanInstanceDesc instanceDesc{};
    instanceDesc.appName = "fuse_rp_vk_adopt";
    instanceDesc.enableValidation = true;
    auto instance = VulkanInstance::create(instanceDesc);
    if (instance == nullptr || !instance->isValid()) {
        std::printf("SKIP: fuse_rp_vk_adopt: no Vulkan instance\n");
        return kSkip;
    }
    if (instance->info().enabledLayers.empty()) {
        std::printf("SKIP: fuse_rp_vk_adopt: VK_LAYER_KHRONOS_validation unavailable\n");
        return kSkip;
    }
    auto original = VulkanDevice::create(*instance);
    if (original == nullptr || !original->isValid()) {
        std::printf("SKIP: fuse_rp_vk_adopt: no Vulkan device (%s)\n",
                    original != nullptr ? original->info().message.c_str() : "null");
        return kSkip;
    }
    std::printf("original: %s | %s\n", original->info().deviceName.c_str(), original->info().caps.summary().c_str());
    expect(!original->isAdopted() && original->ownsDevice(), "original owns its device");
    expectDispatch(vkloader::DispatchMode::Device, original->nativeHandle(), "original");
    const BindlessBackend autoBackend =
        original->info().caps.descriptorBuffer ? BindlessBackend::DescriptorBuffer : BindlessBackend::DescriptorSet;

    // ---- A: adopt the original (non-owning) ----
    {
        auto adoptee = VulkanDevice::adopt(original->adoptionDesc());
        expect(adoptee != nullptr && adoptee->isValid(),
               "A: adopt succeeded (" + (adoptee != nullptr ? adoptee->info().message : std::string("null")) + ")");
        if (adoptee != nullptr && adoptee->isValid()) {
            std::printf("A adoptee: %s | %s\n", adoptee->info().message.c_str(), adoptee->info().caps.summary().c_str());
            expect(adoptee->isAdopted() && !adoptee->ownsDevice(), "A: adoptee is adopted and non-owning");
            expectSameDevice(*original, *adoptee);
            expectDispatch(vkloader::DispatchMode::Device, original->nativeHandle(), "A: same device adopted");
            runSmoke(*adoptee, autoBackend, "A adoptee");
            // A chain of adoption: the adoptee describes itself the same way.
            auto second = VulkanDevice::adopt(adoptee->adoptionDesc());
            expect(second != nullptr && second->isValid() && second->info().caps.enabledMask ==
                                                                 original->info().caps.enabledMask,
                   "A: adoptee's adoptionDesc adopts again with the same caps");
        }
    }
    expectDispatch(vkloader::DispatchMode::Device, original->nativeHandle(), "A: adoptee destroyed");
    runSmoke(*original, autoBackend, "A original after the adoptee");

    // ---- B: a bootstrap-style raw instance + T0 device, adopted with ownership ----
    {
        RawInstance raw;
        expect(createRawInstance(raw), "B: raw vkCreateInstance with validation + create-time messenger");
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        u32 family = UINT32_MAX;
        if (raw.instance != VK_NULL_HANDLE) {
            // The same GPU as the original (Lavapipe exposes one device).
            u32 count = 0;
            vkEnumeratePhysicalDevices(raw.instance, &count, nullptr);
            std::vector<VkPhysicalDevice> devices(count);
            vkEnumeratePhysicalDevices(raw.instance, &count, devices.data());
            for (VkPhysicalDevice candidate : devices) {
                VkPhysicalDeviceProperties props{};
                vkGetPhysicalDeviceProperties(candidate, &props);
                if (physical == VK_NULL_HANDLE && original->info().deviceName == props.deviceName) {
                    physical = candidate;
                }
            }
            if (physical != VK_NULL_HANDLE) {
                u32 familyCount = 0;
                vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
                std::vector<VkQueueFamilyProperties> families(familyCount);
                vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
                for (u32 i = 0; i < familyCount && family == UINT32_MAX; ++i) {
                    family = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 ? i : UINT32_MAX;
                }
            }
        }
        expect(physical != VK_NULL_HANDLE && family != UINT32_MAX, "B: physical device + graphics family");
        VkPhysicalDeviceFeatures2 f2{};
        VkPhysicalDeviceVulkan11Features f11{};
        VkPhysicalDeviceVulkan12Features f12{};
        VkPhysicalDeviceVulkan13Features f13{};
        const VkDevice rawDevice =
            physical != VK_NULL_HANDLE && family != UINT32_MAX ? createRawT0Device(physical, family, f2, f11, f12, f13)
                                                               : VK_NULL_HANDLE;
        expect(rawDevice != VK_NULL_HANDLE, "B: raw vkCreateDevice (T0 features, no extensions)");
        if (rawDevice != VK_NULL_HANDLE) {
            VulkanDeviceAdoptDesc desc{};
            desc.instance = raw.instance;
            desc.physicalDevice = physical;
            desc.device = rawDevice;
            desc.instanceApiVersion = raw.apiVersion;
            desc.instanceExtensions = raw.extensions.data();
            desc.instanceExtensionCount = static_cast<u32>(raw.extensions.size());
            desc.enabledFeatureChain = &f2;
            desc.graphicsFamily = family;
            desc.takeOwnership = true;
            desc.getInstanceProcAddr = reinterpret_cast<void*>(vkGetInstanceProcAddr); // already loaded: kept
            auto adoptee = VulkanDevice::adopt(desc);
            expect(adoptee != nullptr && adoptee->isValid(), "B: adopt succeeded");
            if (adoptee != nullptr && adoptee->isValid()) {
                const RendererCaps& caps = adoptee->info().caps;
                std::printf("B adoptee: %s | %s\n", adoptee->info().message.c_str(), caps.summary().c_str());
                expect(adoptee->isAdopted() && adoptee->ownsDevice(), "B: adoptee owns the device");
                expect(adoptee->instanceHandle() == raw.instance, "B: instanceHandle is the foreign instance");
                expect(caps.meetsT0 && caps.tier == RenderTier::T0 && caps.enabledMask == renderT0RequiredMask(),
                       "B: caps are exactly the declared T0 set (" + caps.summary() + ")");
                expect(caps.hardwareTier == original->info().caps.hardwareTier &&
                           caps.supportedMask == original->info().caps.supportedMask,
                       "B: hardware tier / supported mask from the physical device");
                expect(!caps.descriptorBuffer && !caps.meshShader && !adoptee->info().pipelineCreationCacheControl &&
                           adoptee->info().enabledExtensions.empty() && !adoptee->info().swapchainExtension,
                       "B: nothing undeclared is reported");
                expect(adoptee->queues().graphicsFamily == family && adoptee->queues().computeFamily == family &&
                           adoptee->queues().transferFamily == family && adoptee->queues().graphics != nullptr,
                       "B: queues from the graphics family");
                expectDispatch(vkloader::DispatchMode::Instance, nullptr, "B: two live devices");
                runSmoke(*adoptee, BindlessBackend::DescriptorSet, "B adoptee");
                runSmoke(*original, autoBackend, "B original while two devices live");
                adoptee.reset(); // destroys rawDevice (owning)
            } else {
                vkDestroyDevice(rawDevice, nullptr);
            }
        }
        // A device the adoptee failed to destroy would be reported here (object tracker, caught by
        // the create-time messenger).
        destroyRawInstance(raw);
        expect(raw.messages == 0u, "B: zero validation messages on the raw instance (" +
                                       std::to_string(raw.messages) + ": " + raw.lastMessage + ")");
    }
    expectDispatch(vkloader::DispatchMode::Device, original->nativeHandle(), "B: adoptee (and its device) destroyed");
    runSmoke(*original, autoBackend, "B original after the adoptee");

    original->waitIdle();
    original.reset();
    instance.reset();
    const VulkanValidationCounters counters = vulkanValidationCounters();
    expect(counters.errors == 0 && counters.warnings == 0,
           "zero validation messages (errors " + std::to_string(counters.errors) + ", warnings " +
               std::to_string(counters.warnings) + ": " + counters.lastError + ")");
    return 0;
}

} // namespace

int main() {
    const int rc = run();
    if (rc == kSkip) {
        return kSkip;
    }
    if (g_failures != 0) {
        std::printf("fuse_rp_vk_adopt: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("fuse_rp_vk_adopt: OK\n");
    return 0;
}

#endif
