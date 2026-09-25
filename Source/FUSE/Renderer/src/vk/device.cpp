#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/loader.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

#if defined(FUSE_VULKAN_BACKEND)
bool extensionSupported(VkPhysicalDevice device, const char* name) {
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

u32 findQueueFamily(VkPhysicalDevice device, VkQueueFlagBits flags) {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (u32 i = 0; i < count; ++i) {
        if ((families[i].queueFlags & flags) != 0) {
            return i;
        }
    }
    return 0;
}

u32 findComputeFamily(VkPhysicalDevice device) {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (u32 i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) != 0 && (flags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            return i;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            return i;
        }
    }
    return 0;
}

u32 findTransferFamily(VkPhysicalDevice device) {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (u32 i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_TRANSFER_BIT) != 0 && (flags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
            (flags & VK_QUEUE_COMPUTE_BIT) == 0) {
            return i;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_TRANSFER_BIT) != 0 && (flags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            return i;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0) {
            return i;
        }
    }
    return 0;
}

bool queueFamilyPresentsToSurface(VkPhysicalDevice device, u32 family, VkSurfaceKHR surface) {
    VkBool32 supported = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(device, family, surface, &supported) != VK_SUCCESS) {
        return false;
    }
    return supported == VK_TRUE;
}

u32 findPresentableGraphicsFamily(VkPhysicalDevice device, VkSurfaceKHR surface, bool& found) {
    found = false;
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());
    for (u32 i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        if (queueFamilyPresentsToSurface(device, i, surface)) {
            found = true;
            return i;
        }
    }
    return 0;
}

const char* const kPreferredExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
};

const char* const kPlatformExternalExtensions[] = {
#if defined(_WIN32)
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
#else
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
#endif
};

const char* deviceTypeName(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "cpu";
    default:
        return "other";
    }
}

/// Preference among suitable devices when `preferDiscreteGpu` is set: discrete > integrated >
/// virtual > CPU > other.
u32 deviceTypeRank(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 1;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Feature probing (WP-0.1). One plain-value set of feature structs; `linkFeatureChain` rebuilds the
// pNext chain from an inclusion plan, so the set can be copied and reused for the enable chain.

/// Extensions the tier / optional features depend on, as advertised by the physical device.
struct DeviceExtensionPresence {
    bool synchronization2 = false;
    bool dynamicRendering = false;
    bool maintenance4 = false;
    bool imageAtomicInt64 = false;
    bool descriptorBuffer = false;
    bool deviceGeneratedCommands = false; ///< advertised (enabling also needs header support)
    bool maintenance5 = false;
    bool shaderObject = false;
    bool meshShader = false;
    bool accelerationStructure = false;
    bool deferredHostOperations = false;
    bool rayQuery = false;
    bool rayTracingPipeline = false;
    bool cooperativeMatrix = false;
};

struct DeviceFeatureSet {
    VkPhysicalDeviceFeatures2 core{};
    VkPhysicalDeviceVulkan11Features v11{};
    VkPhysicalDeviceVulkan12Features v12{};
    VkPhysicalDeviceVulkan13Features v13{};
    VkPhysicalDeviceSynchronization2Features sync2{};    ///< API < 1.3 only
    VkPhysicalDeviceDynamicRenderingFeatures dynamic{};  ///< API < 1.3 only
    VkPhysicalDeviceMaintenance4Features maintenance4{}; ///< API < 1.3 only
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT imageAtomicInt64{};
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer{};
    VkPhysicalDeviceShaderObjectFeaturesEXT shaderObject{};
    VkPhysicalDeviceMeshShaderFeaturesEXT meshShader{};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure{};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipeline{};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cooperativeMatrix{};
#if defined(VK_EXT_device_generated_commands)
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT deviceGeneratedCommands{};
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5{};
#endif
};

/// Which structs of a DeviceFeatureSet go into a chain.
struct FeatureChainPlan {
    bool v13 = false;
    bool sync2 = false;
    bool dynamic = false;
    bool maintenance4 = false;
    bool imageAtomicInt64 = false;
    bool descriptorBuffer = false;
    bool shaderObject = false;
    bool meshShader = false;
    bool accelerationStructure = false;
    bool rayQuery = false;
    bool rayTracingPipeline = false;
    bool cooperativeMatrix = false;
    bool deviceGeneratedCommands = false;
    bool maintenance5 = false;
};

/// Sets every sType and links the planned structs behind `set.core`. Returns &set.core.
VkPhysicalDeviceFeatures2* linkFeatureChain(DeviceFeatureSet& set, const FeatureChainPlan& plan) {
    set.core.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    set.v11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    set.v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    set.v13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    set.sync2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    set.dynamic.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    set.maintenance4.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES;
    set.imageAtomicInt64.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT;
    set.descriptorBuffer.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT;
    set.shaderObject.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT;
    set.meshShader.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
    set.accelerationStructure.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    set.rayQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    set.rayTracingPipeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    set.cooperativeMatrix.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
#if defined(VK_EXT_device_generated_commands)
    set.deviceGeneratedCommands.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT;
    set.maintenance5.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR;
#endif
    void** tail = &set.core.pNext;
    auto append = [&tail](bool include, auto& node) {
        if (include) {
            *tail = &node;
            tail = &node.pNext;
        }
    };
    append(true, set.v11);
    append(true, set.v12);
    append(plan.v13, set.v13);
    append(plan.sync2, set.sync2);
    append(plan.dynamic, set.dynamic);
    append(plan.maintenance4, set.maintenance4);
    append(plan.imageAtomicInt64, set.imageAtomicInt64);
    append(plan.descriptorBuffer, set.descriptorBuffer);
    append(plan.shaderObject, set.shaderObject);
    append(plan.meshShader, set.meshShader);
    append(plan.accelerationStructure, set.accelerationStructure);
    append(plan.rayQuery, set.rayQuery);
    append(plan.rayTracingPipeline, set.rayTracingPipeline);
    append(plan.cooperativeMatrix, set.cooperativeMatrix);
#if defined(VK_EXT_device_generated_commands)
    append(plan.deviceGeneratedCommands, set.deviceGeneratedCommands);
    append(plan.maintenance5, set.maintenance5);
#endif
    *tail = nullptr;
    return &set.core;
}

u32 apiMajorMinor(u32 version) {
    return VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), 0);
}

bool envFlag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

/// Everything selection and creation need to know about one physical device's features.
struct DeviceProbe {
    /// min(instance apiVersion, device apiVersion), major.minor only.
    u32 apiVersion = 0;
    DeviceExtensionPresence ext{};
    /// Supported values (sTypes set, pNext stale: relink before use).
    DeviceFeatureSet supported{};
    u64 supportedMask = 0;
    RenderTier hardwareTier = RenderTier::T0;
    /// Vulkan >= 1.3 and every T0 feature supported.
    bool meetsT0 = false;
    /// Comma-separated T0 gaps ("Vulkan 1.3", feature names); empty when meetsT0.
    std::string missingT0;
};

/// RenderFeature bits set in a feature struct set. `plan` says which structs carry values (were
/// queried / declared, and their extension is present); on Vulkan 1.3 sync2 / dynamic rendering /
/// maintenance4 come from the 1.3 struct or the promoted extension struct.
u64 featureMask(const DeviceFeatureSet& f, const FeatureChainPlan& plan, bool api13, const DeviceExtensionPresence& ext) {
    (void)ext;
    const bool sync2 = (api13 && plan.v13 && f.v13.synchronization2 == VK_TRUE) ||
                       (plan.sync2 && f.sync2.synchronization2 == VK_TRUE);
    const bool dynamic = (api13 && plan.v13 && f.v13.dynamicRendering == VK_TRUE) ||
                         (plan.dynamic && f.dynamic.dynamicRendering == VK_TRUE);
    const bool maintenance4 = (api13 && plan.v13 && f.v13.maintenance4 == VK_TRUE) ||
                              (plan.maintenance4 && f.maintenance4.maintenance4 == VK_TRUE);
    const bool accelerationStructure =
        plan.accelerationStructure && f.accelerationStructure.accelerationStructure == VK_TRUE;
    const bool bda = f.v12.bufferDeviceAddress == VK_TRUE;

    u64 mask = 0;
    auto set = [&mask](RenderFeature feature, bool value) {
        if (value) {
            mask |= renderFeatureBit(feature);
        }
    };
    set(RenderFeature::Synchronization2, sync2);
    set(RenderFeature::DynamicRendering, dynamic);
    set(RenderFeature::Maintenance4, maintenance4);
    set(RenderFeature::TimelineSemaphore, f.v12.timelineSemaphore == VK_TRUE);
    set(RenderFeature::DescriptorIndexing, f.v12.descriptorIndexing == VK_TRUE);
    set(RenderFeature::BufferDeviceAddress, bda);
    set(RenderFeature::DrawIndirectCount, f.v12.drawIndirectCount == VK_TRUE);
    set(RenderFeature::MultiDrawIndirect, f.core.features.multiDrawIndirect == VK_TRUE);
    set(RenderFeature::ShaderDrawParameters, f.v11.shaderDrawParameters == VK_TRUE);
    set(RenderFeature::ShaderInt64, f.core.features.shaderInt64 == VK_TRUE);
    set(RenderFeature::ShaderBufferInt64Atomics, f.v12.shaderBufferInt64Atomics == VK_TRUE);
    set(RenderFeature::ShaderImageInt64Atomics,
        plan.imageAtomicInt64 && f.imageAtomicInt64.shaderImageInt64Atomics == VK_TRUE);
    set(RenderFeature::DescriptorBuffer, plan.descriptorBuffer && f.descriptorBuffer.descriptorBuffer == VK_TRUE && bda);
#if defined(VK_EXT_device_generated_commands)
    set(RenderFeature::DeviceGeneratedCommands,
        plan.deviceGeneratedCommands && f.deviceGeneratedCommands.deviceGeneratedCommands == VK_TRUE &&
            f.maintenance5.maintenance5 == VK_TRUE && bda);
#else
    // Headers predate the extension: reported as supported when advertised, never enabled.
    set(RenderFeature::DeviceGeneratedCommands, ext.deviceGeneratedCommands && ext.maintenance5 && bda);
#endif
    set(RenderFeature::ShaderObject, plan.shaderObject && f.shaderObject.shaderObject == VK_TRUE && dynamic);
    // Task shaders are only useful in front of mesh shaders: no meshShader, no taskShader.
    const bool meshShader = plan.meshShader && f.meshShader.meshShader == VK_TRUE;
    set(RenderFeature::TaskShader, meshShader && f.meshShader.taskShader == VK_TRUE);
    set(RenderFeature::MeshShader, meshShader);
    set(RenderFeature::AccelerationStructure, accelerationStructure && bda);
    set(RenderFeature::RayQuery, accelerationStructure && bda && plan.rayQuery && f.rayQuery.rayQuery == VK_TRUE);
    set(RenderFeature::RayTracingPipeline, accelerationStructure && bda && plan.rayTracingPipeline &&
                                               f.rayTracingPipeline.rayTracingPipeline == VK_TRUE);
    set(RenderFeature::CooperativeMatrix,
        plan.cooperativeMatrix && f.cooperativeMatrix.cooperativeMatrix == VK_TRUE);
    return mask;
}

DeviceProbe probeDevice(VkPhysicalDevice device, u32 instanceApiVersion) {
    DeviceProbe probe{};
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device, &props);
    probe.apiVersion = apiMajorMinor(std::min(apiMajorMinor(instanceApiVersion), apiMajorMinor(props.apiVersion)));
    const bool api13 = probe.apiVersion >= VK_API_VERSION_1_3;

    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());
    auto has = [&extensions](const char* name) {
        for (const VkExtensionProperties& extension : extensions) {
            if (std::strcmp(extension.extensionName, name) == 0) {
                return true;
            }
        }
        return false;
    };
    DeviceExtensionPresence& ext = probe.ext;
    ext.synchronization2 = has(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    ext.dynamicRendering = has(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    ext.maintenance4 = has(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    ext.imageAtomicInt64 = has(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
    ext.descriptorBuffer = has(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    ext.deviceGeneratedCommands = has("VK_EXT_device_generated_commands");
    ext.maintenance5 = has(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    ext.shaderObject = has(VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
    ext.meshShader = has(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    ext.deferredHostOperations = has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    ext.accelerationStructure = has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && ext.deferredHostOperations;
    ext.rayQuery = has(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    ext.rayTracingPipeline = has(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    ext.cooperativeMatrix = has(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);

    FeatureChainPlan plan{};
    plan.v13 = api13;
    plan.sync2 = !api13 && ext.synchronization2;
    plan.dynamic = !api13 && ext.dynamicRendering;
    plan.maintenance4 = !api13 && ext.maintenance4;
    plan.imageAtomicInt64 = ext.imageAtomicInt64;
    plan.descriptorBuffer = ext.descriptorBuffer;
    plan.shaderObject = ext.shaderObject;
    plan.meshShader = ext.meshShader;
    plan.accelerationStructure = ext.accelerationStructure;
    plan.rayQuery = ext.rayQuery;
    plan.rayTracingPipeline = ext.rayTracingPipeline;
    plan.cooperativeMatrix = ext.cooperativeMatrix;
#if defined(VK_EXT_device_generated_commands)
    plan.deviceGeneratedCommands = ext.deviceGeneratedCommands && ext.maintenance5;
    plan.maintenance5 = ext.deviceGeneratedCommands && ext.maintenance5;
#endif
    DeviceFeatureSet& f = probe.supported;
    vkGetPhysicalDeviceFeatures2(device, linkFeatureChain(f, plan));
    const u64 mask = featureMask(f, plan, api13, ext);
    probe.supportedMask = mask;
    probe.hardwareTier = renderTierFromMask(api13 ? mask : 0);

    std::string missing = api13 ? std::string() : std::string("Vulkan 1.3");
    const std::string features = renderMissingT0(mask);
    if (!features.empty()) {
        missing += missing.empty() ? features : "," + features;
    }
    probe.meetsT0 = missing.empty();
    probe.missingT0 = std::move(missing);
    return probe;
}

RenderTier minTier(RenderTier a, RenderTier b) {
    return static_cast<u8>(a) < static_cast<u8>(b) ? a : b;
}

/// Effective tier cap for a device description: min(desc.maxTier, FUSE_RENDER_TIER_MAX), and T0
/// when optional features are switched off.
RenderTier tierCapFor(const VulkanDeviceDesc& desc) {
    RenderTier cap = minTier(desc.maxTier, renderTierMaxFromEnv());
    if (!desc.enableOptionalFeatures) {
        cap = RenderTier::T0;
    }
    return cap;
}

bool allowBelowT0(const VulkanDeviceDesc& desc) {
    return desc.allowBelowT0 || envFlag("FUSE_VK_ALLOW_1_2");
}

struct DeviceCandidate {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    u32 index = 0;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    std::string name;
    VkDeviceSize deviceLocalBytes = 0;
    u32 maxImageDimension2D = 0;
    /// Feature probe (only filled once the device passes the API-version check).
    DeviceProbe probe{};
    /// min(probe.hardwareTier, tier cap): what this device would run at.
    RenderTier tier = RenderTier::T0;
    /// Empty when the device meets every hard requirement.
    std::string rejectReason;
};

/// Hard requirements of the RHI: the renderer T0 set (Vulkan 1.3 and the features listed in
/// render_tier.hpp; with the FUSE_VK_ALLOW_1_2 escape only Vulkan 1.2 + timeline semaphores), a
/// graphics queue family and, when presentation is required, VK_KHR_swapchain plus a graphics
/// family that presents to the given surface.
DeviceCandidate evaluateCandidate(VkPhysicalDevice device, u32 index, const VulkanDeviceDesc& desc,
                                  bool instanceHasSurface, u32 instanceApiVersion) {
    DeviceCandidate candidate{};
    candidate.handle = device;
    candidate.index = index;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device, &props);
    candidate.type = props.deviceType;
    candidate.name = props.deviceName;
    candidate.maxImageDimension2D = props.limits.maxImageDimension2D;
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(device, &memory);
    for (u32 i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            candidate.deviceLocalBytes += memory.memoryHeaps[i].size;
        }
    }

    if (apiMajorMinor(std::min(apiMajorMinor(instanceApiVersion), apiMajorMinor(props.apiVersion))) <
        VK_API_VERSION_1_2) {
        candidate.rejectReason = "Vulkan 1.2 unsupported";
        return candidate;
    }
    u32 familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
    bool hasGraphics = false;
    for (const VkQueueFamilyProperties& family : families) {
        hasGraphics = hasGraphics || ((family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && family.queueCount > 0);
    }
    if (!hasGraphics) {
        candidate.rejectReason = "no graphics queue family";
        return candidate;
    }
    candidate.probe = probeDevice(device, instanceApiVersion);
    candidate.tier = minTier(candidate.probe.hardwareTier, tierCapFor(desc));
    if (!candidate.probe.meetsT0) {
        const bool legacyOk = allowBelowT0(desc) &&
                              (candidate.probe.supportedMask & renderFeatureBit(RenderFeature::TimelineSemaphore)) != 0;
        if (!legacyOk) {
            candidate.rejectReason = "T0 requirements missing: " + candidate.probe.missingT0;
            return candidate;
        }
    }
    if (desc.requirePresentation) {
        if (!instanceHasSurface || !extensionSupported(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            candidate.rejectReason = "VK_KHR_swapchain unavailable";
            return candidate;
        }
        if (desc.presentSurface != nullptr) {
            bool presentable = false;
            findPresentableGraphicsFamily(device, reinterpret_cast<VkSurfaceKHR>(desc.presentSurface), presentable);
            if (!presentable) {
                candidate.rejectReason = "no graphics queue family presents to the surface";
                return candidate;
            }
        }
    }
    return candidate;
}

/// True when `a` should be picked over `b` (both suitable). With `preferDiscrete`: device type rank,
/// then renderer tier, then more device-local memory, then larger maxImageDimension2D, then enumeration order.
/// Without it: enumeration order only.
bool betterCandidate(const DeviceCandidate& a, const DeviceCandidate& b, bool preferDiscrete) {
    if (preferDiscrete) {
        const u32 rankA = deviceTypeRank(a.type);
        const u32 rankB = deviceTypeRank(b.type);
        if (rankA != rankB) {
            return rankA > rankB;
        }
        // Renderer tier (capped), a device through the FUSE_VK_ALLOW_1_2 escape below any T0 one.
        const u32 tierA = a.probe.meetsT0 ? 1u + static_cast<u32>(a.tier) : 0u;
        const u32 tierB = b.probe.meetsT0 ? 1u + static_cast<u32>(b.tier) : 0u;
        if (tierA != tierB) {
            return tierA > tierB;
        }
        if (a.deviceLocalBytes != b.deviceLocalBytes) {
            return a.deviceLocalBytes > b.deviceLocalBytes;
        }
        if (a.maxImageDimension2D != b.maxImageDimension2D) {
            return a.maxImageDimension2D > b.maxImageDimension2D;
        }
    }
    return a.index < b.index;
}
/// Descriptor limits for the bindless layout (VulkanDescriptorLimits): update-after-bind limits
/// when descriptor indexing is enabled, else the core limits.
void fillDescriptorLimits(VkPhysicalDevice physical, const VkPhysicalDeviceProperties& props, bool descriptorIndexing,
                          VulkanDescriptorLimits& limits) {
    VkPhysicalDeviceVulkan12Properties props12{};
    props12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &props12;
    vkGetPhysicalDeviceProperties2(physical, &props2);
    const VkPhysicalDeviceLimits& core = props.limits;
    if (descriptorIndexing && props12.maxPerStageUpdateAfterBindResources > 0u) {
        limits.sampledImages = std::min(props12.maxDescriptorSetUpdateAfterBindSampledImages,
                                        props12.maxPerStageDescriptorUpdateAfterBindSampledImages);
        limits.storageImages = std::min(props12.maxDescriptorSetUpdateAfterBindStorageImages,
                                        props12.maxPerStageDescriptorUpdateAfterBindStorageImages);
        limits.storageBuffers = std::min(props12.maxDescriptorSetUpdateAfterBindStorageBuffers,
                                         props12.maxPerStageDescriptorUpdateAfterBindStorageBuffers);
        limits.uniformBuffers = std::min(props12.maxDescriptorSetUpdateAfterBindUniformBuffers,
                                         props12.maxPerStageDescriptorUpdateAfterBindUniformBuffers);
        limits.samplers = std::min(props12.maxDescriptorSetUpdateAfterBindSamplers,
                                   props12.maxPerStageDescriptorUpdateAfterBindSamplers);
        limits.perStageResources = props12.maxPerStageUpdateAfterBindResources;
        limits.allPools = props12.maxUpdateAfterBindDescriptorsInAllPools;
    } else {
        limits.sampledImages = std::min(core.maxDescriptorSetSampledImages, core.maxPerStageDescriptorSampledImages);
        limits.storageImages = std::min(core.maxDescriptorSetStorageImages, core.maxPerStageDescriptorStorageImages);
        limits.storageBuffers = std::min(core.maxDescriptorSetStorageBuffers, core.maxPerStageDescriptorStorageBuffers);
        limits.uniformBuffers = std::min(core.maxDescriptorSetUniformBuffers, core.maxPerStageDescriptorUniformBuffers);
        limits.samplers = std::min(core.maxDescriptorSetSamplers, core.maxPerStageDescriptorSamplers);
        limits.perStageResources = core.maxPerStageResources;
        limits.allPools = UINT32_MAX;
    }
}

bool containsName(const std::vector<const char*>& names, const char* name) {
    for (const char* n : names) {
        if (n != nullptr && std::strcmp(n, name) == 0) {
            return true;
        }
    }
    return false;
}

/// Copies a feature struct out of a caller's pNext chain (keeps our sType; pNext is relinked later).
template <typename T>
void copyFeatureStruct(T& dst, const VkBaseInStructure* node) {
    const VkStructureType sType = dst.sType;
    std::memcpy(static_cast<void*>(&dst), node, sizeof(T));
    dst.sType = sType;
    dst.pNext = nullptr;
}
#endif

} // namespace

/// The enabled feature structs of a device and the plan that links them (adoptionDesc() hands out
/// &set.core as the chain).
struct VulkanDevice::EnabledFeatures {
#if defined(FUSE_VULKAN_BACKEND)
    DeviceFeatureSet set{};
    FeatureChainPlan plan{};
#endif
};

void VulkanDevice::EnabledFeaturesDeleter::operator()(EnabledFeatures* features) const {
    delete features;
}

std::unique_ptr<VulkanDevice> VulkanDevice::create(VulkanInstance& instance,
                                                   const VulkanDeviceDesc& desc) {
    auto device = std::unique_ptr<VulkanDevice>(new VulkanDevice());
    if (!device->initialize(instance, desc)) {
        device->m_info.valid = false;
    }
    return device;
}

std::unique_ptr<VulkanDevice> VulkanDevice::adopt(const VulkanDeviceAdoptDesc& desc) {
    auto device = std::unique_ptr<VulkanDevice>(new VulkanDevice());
    if (!device->initializeAdopted(desc)) {
        device->m_info.valid = false;
    }
    return device;
}

VulkanDeviceAdoptDesc VulkanDevice::adoptionDesc() const {
    VulkanDeviceAdoptDesc d{};
    d.instance = instanceHandle();
    d.physicalDevice = m_physicalDevice;
    d.device = m_handle;
    d.instanceApiVersion = m_instanceApiVersion;
    d.enabledExtensions = m_info.enabledExtensions.empty() ? nullptr : m_info.enabledExtensions.data();
    d.enabledExtensionCount = static_cast<u32>(m_info.enabledExtensions.size());
    const std::vector<const char*>& instanceExtensions =
        m_instance != nullptr ? m_instance->info().enabledExtensions : m_instanceExtensions;
    d.instanceExtensions = instanceExtensions.empty() ? nullptr : instanceExtensions.data();
    d.instanceExtensionCount = static_cast<u32>(instanceExtensions.size());
#if defined(FUSE_VULKAN_BACKEND)
    d.enabledFeatureChain = m_features != nullptr ? static_cast<const void*>(&m_features->set.core) : nullptr;
#endif
    d.graphicsFamily = m_info.queues.graphicsFamily;
    d.computeFamily = m_info.queues.computeFamily;
    d.transferFamily = m_info.queues.transferFamily;
    d.graphicsQueue = m_info.queues.graphics;
    d.computeQueue = m_info.queues.compute;
    d.transferQueue = m_info.queues.transfer;
    d.takeOwnership = false;
    d.maxTier = m_info.caps.valid ? m_info.caps.tierCap : kMaxRenderTier;
    return d;
}

bool VulkanDevice::initializeAdopted(const VulkanDeviceAdoptDesc& desc) {
    m_adopted = true;
    m_ownsDevice = false;
#if defined(FUSE_VULKAN_BACKEND)
    if (desc.instance == nullptr || desc.physicalDevice == nullptr || desc.device == nullptr) {
        m_info.message = "Adopt failed: instance, physical device and device handles are required";
        return false;
    }
    // volk: the host's vkGetInstanceProcAddr when given (no loader library opened by us), else the
    // loader library. Then the instance and device tables, before any other call on them.
    const bool loaded = desc.getInstanceProcAddr != nullptr ? vkloader::initializeWithProcAddr(desc.getInstanceProcAddr)
                                                            : vkloader::initialize();
    if (!loaded) {
        m_info.message = "Adopt failed: no Vulkan loader (volk)";
        return false;
    }
    const auto vkInstance = static_cast<VkInstance>(desc.instance);
    const auto physical = static_cast<VkPhysicalDevice>(desc.physicalDevice);
    const auto logicalDevice = static_cast<VkDevice>(desc.device);
    vkloader::registerInstance(vkInstance);
    vkloader::registerDevice(logicalDevice, vkInstance);
    m_registered = true;
    m_instanceHandle = desc.instance;
    m_physicalDevice = desc.physicalDevice;
    m_handle = desc.device;
    m_ownsDevice = desc.takeOwnership;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    m_info.deviceName = props.deviceName;
    m_info.deviceType = static_cast<u32>(props.deviceType);
    m_instanceApiVersion = desc.instanceApiVersion != 0u ? desc.instanceApiVersion : props.apiVersion;
    m_info.apiVersion = std::min(m_instanceApiVersion, props.apiVersion);
    const u32 apiVersion = apiMajorMinor(m_info.apiVersion);
    const bool api13 = apiVersion >= VK_API_VERSION_1_3;

    {
        u32 count = 0;
        vkEnumeratePhysicalDevices(vkInstance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(vkInstance, &count, devices.data());
        m_info.physicalDeviceCount = count;
        for (u32 i = 0; i < count; ++i) {
            if (devices[i] == physical) {
                m_info.physicalDeviceIndex = i;
            }
        }
        m_info.selection = "#" + (m_info.physicalDeviceIndex == UINT32_MAX ? std::string("?")
                                                                            : std::to_string(m_info.physicalDeviceIndex)) +
                           " '" + m_info.deviceName + "' (" + deviceTypeName(props.deviceType) + "): adopted";
    }

    // Extensions: our own copies (the caller's strings may not outlive adopt()).
    for (u32 i = 0; i < desc.enabledExtensionCount; ++i) {
        const char* name = desc.enabledExtensions != nullptr ? desc.enabledExtensions[i] : nullptr;
        if (name != nullptr) {
            m_extensionNames.emplace_back(name);
        }
    }
    for (const std::string& name : m_extensionNames) {
        m_info.enabledExtensions.push_back(name.c_str());
    }
    for (u32 i = 0; i < desc.instanceExtensionCount; ++i) {
        const char* name = desc.instanceExtensions != nullptr ? desc.instanceExtensions[i] : nullptr;
        if (name != nullptr) {
            m_instanceExtensionNames.emplace_back(name);
        }
    }
    for (const std::string& name : m_instanceExtensionNames) {
        m_instanceExtensions.push_back(name.c_str());
    }
    const std::vector<const char*>& extensions = m_info.enabledExtensions;
    auto enabledExt = [&extensions](const char* name) { return containsName(extensions, name); };
    DeviceExtensionPresence ext{};
    ext.synchronization2 = enabledExt(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    ext.dynamicRendering = enabledExt(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    ext.maintenance4 = enabledExt(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    ext.imageAtomicInt64 = enabledExt(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
    ext.descriptorBuffer = enabledExt(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
    ext.deviceGeneratedCommands = enabledExt("VK_EXT_device_generated_commands");
    ext.maintenance5 = enabledExt(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    ext.shaderObject = enabledExt(VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
    ext.meshShader = enabledExt(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    ext.deferredHostOperations = enabledExt(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    ext.accelerationStructure = enabledExt(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && ext.deferredHostOperations;
    ext.rayQuery = enabledExt(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    ext.rayTracingPipeline = enabledExt(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    ext.cooperativeMatrix = enabledExt(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);

    // Enabled features: the declared chain, struct by struct (promoted 1.1/1.2 structs folded into
    // the Vulkan11/12 structs), then pEnabledFeatures when no VkPhysicalDeviceFeatures2 came.
    m_features.reset(new EnabledFeatures{});
    DeviceFeatureSet& f = m_features->set;
    FeatureChainPlan present{};
    linkFeatureChain(f, present); // sTypes
    bool haveFeatures2 = false;
    for (auto node = static_cast<const VkBaseInStructure*>(desc.enabledFeatureChain); node != nullptr;
         node = node->pNext) {
        switch (node->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            copyFeatureStruct(f.core, node);
            haveFeatures2 = true;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
            copyFeatureStruct(f.v11, node);
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
            copyFeatureStruct(f.v12, node);
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
            copyFeatureStruct(f.v13, node);
            present.v13 = true;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: {
            const auto* s = reinterpret_cast<const VkPhysicalDeviceShaderDrawParametersFeatures*>(node);
            f.v11.shaderDrawParameters |= s->shaderDrawParameters;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES: {
            const auto* s = reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(node);
            f.v12.timelineSemaphore |= s->timelineSemaphore;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES: {
            const auto* s = reinterpret_cast<const VkPhysicalDeviceBufferDeviceAddressFeatures*>(node);
            f.v12.bufferDeviceAddress |= s->bufferDeviceAddress;
            f.v12.bufferDeviceAddressCaptureReplay |= s->bufferDeviceAddressCaptureReplay;
            f.v12.bufferDeviceAddressMultiDevice |= s->bufferDeviceAddressMultiDevice;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES: {
            const auto* s = reinterpret_cast<const VkPhysicalDeviceShaderAtomicInt64Features*>(node);
            f.v12.shaderBufferInt64Atomics |= s->shaderBufferInt64Atomics;
            f.v12.shaderSharedInt64Atomics |= s->shaderSharedInt64Atomics;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES: {
            const auto* s = reinterpret_cast<const VkPhysicalDeviceDescriptorIndexingFeatures*>(node);
            VkPhysicalDeviceVulkan12Features& v12 = f.v12;
            // No umbrella bit in the extension struct: enabling it is enabling the extension.
            v12.descriptorIndexing |= (apiVersion >= VK_API_VERSION_1_2 ||
                                       enabledExt(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
                                          ? VK_TRUE
                                          : VK_FALSE;
            v12.shaderSampledImageArrayNonUniformIndexing |= s->shaderSampledImageArrayNonUniformIndexing;
            v12.shaderStorageBufferArrayNonUniformIndexing |= s->shaderStorageBufferArrayNonUniformIndexing;
            v12.shaderStorageImageArrayNonUniformIndexing |= s->shaderStorageImageArrayNonUniformIndexing;
            v12.descriptorBindingUniformBufferUpdateAfterBind |= s->descriptorBindingUniformBufferUpdateAfterBind;
            v12.descriptorBindingSampledImageUpdateAfterBind |= s->descriptorBindingSampledImageUpdateAfterBind;
            v12.descriptorBindingStorageImageUpdateAfterBind |= s->descriptorBindingStorageImageUpdateAfterBind;
            v12.descriptorBindingStorageBufferUpdateAfterBind |= s->descriptorBindingStorageBufferUpdateAfterBind;
            v12.descriptorBindingPartiallyBound |= s->descriptorBindingPartiallyBound;
            v12.runtimeDescriptorArray |= s->runtimeDescriptorArray;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES:
            copyFeatureStruct(f.sync2, node);
            present.sync2 = api13 || ext.synchronization2;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
            copyFeatureStruct(f.dynamic, node);
            present.dynamic = api13 || ext.dynamicRendering;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES:
            copyFeatureStruct(f.maintenance4, node);
            present.maintenance4 = api13 || ext.maintenance4;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT:
            copyFeatureStruct(f.imageAtomicInt64, node);
            present.imageAtomicInt64 = ext.imageAtomicInt64;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT:
            copyFeatureStruct(f.descriptorBuffer, node);
            present.descriptorBuffer = ext.descriptorBuffer;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT:
            copyFeatureStruct(f.shaderObject, node);
            present.shaderObject = ext.shaderObject;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT:
            copyFeatureStruct(f.meshShader, node);
            present.meshShader = ext.meshShader;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR:
            copyFeatureStruct(f.accelerationStructure, node);
            present.accelerationStructure = ext.accelerationStructure;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR:
            copyFeatureStruct(f.rayQuery, node);
            present.rayQuery = ext.rayQuery;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR:
            copyFeatureStruct(f.rayTracingPipeline, node);
            present.rayTracingPipeline = ext.rayTracingPipeline;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR:
            copyFeatureStruct(f.cooperativeMatrix, node);
            present.cooperativeMatrix = ext.cooperativeMatrix;
            break;
#if defined(VK_EXT_device_generated_commands)
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT:
            copyFeatureStruct(f.deviceGeneratedCommands, node);
            present.deviceGeneratedCommands = ext.deviceGeneratedCommands && ext.maintenance5;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR:
            copyFeatureStruct(f.maintenance5, node);
            present.maintenance5 = ext.maintenance5;
            break;
#endif
        default:
            break; // not a feature the renderer tracks
        }
    }
    if (!haveFeatures2 && desc.enabledCoreFeatures != nullptr) {
        std::memcpy(&f.core.features, desc.enabledCoreFeatures, sizeof(VkPhysicalDeviceFeatures));
    }
    present.v13 = present.v13 && api13;
    m_features->plan = present;
    linkFeatureChain(f, present);

    // Caps / tier from what is enabled, never more than the device supports, capped like create().
    const DeviceProbe probe = probeDevice(physical, m_instanceApiVersion);
    RenderTier tierCap = minTier(desc.maxTier, renderTierMaxFromEnv());
    u64 enabledMask = featureMask(f, present, api13, ext) & probe.supportedMask & ~renderFeaturesAboveTier(tierCap);
#if !defined(VK_EXT_device_generated_commands)
    enabledMask &= ~renderFeatureBit(RenderFeature::DeviceGeneratedCommands);
#endif
    if ((enabledMask & renderFeatureBit(RenderFeature::AccelerationStructure)) == 0) {
        enabledMask &= ~(renderFeatureBit(RenderFeature::RayQuery) | renderFeatureBit(RenderFeature::RayTracingPipeline));
    }
    {
        RendererCaps& caps = m_info.caps;
        caps.valid = true;
        caps.apiVersion = m_info.apiVersion;
        caps.supportedMask = probe.supportedMask;
        caps.setEnabledMask(enabledMask);
        caps.meetsT0 = api13 && (enabledMask & renderT0RequiredMask()) == renderT0RequiredMask();
        caps.hardwareTier = probe.hardwareTier;
        caps.tierCap = tierCap;
        caps.tier = caps.meetsT0 ? minTier(renderTierFromMask(enabledMask), tierCap) : RenderTier::T0;
    }

    const VkPhysicalDeviceVulkan12Features& v12 = f.v12;
    m_info.descriptorIndexing = v12.descriptorIndexing == VK_TRUE;
    m_info.sampledImageUpdateAfterBind = v12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE;
    m_info.storageImageUpdateAfterBind = v12.descriptorBindingStorageImageUpdateAfterBind == VK_TRUE;
    m_info.storageBufferUpdateAfterBind = v12.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE;
    m_info.uniformBufferUpdateAfterBind = v12.descriptorBindingUniformBufferUpdateAfterBind == VK_TRUE;
    m_info.sampledImageNonUniformIndexing = v12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    m_info.swapchainExtension = enabledExt(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    m_info.bufferDeviceAddress = v12.bufferDeviceAddress == VK_TRUE;
    m_info.timelineSemaphore = v12.timelineSemaphore == VK_TRUE;
    m_info.dynamicRendering = (enabledMask & renderFeatureBit(RenderFeature::DynamicRendering)) != 0;
    m_info.pipelineCreationCacheControl = present.v13 && f.v13.pipelineCreationCacheControl == VK_TRUE;
    m_info.samplerAnisotropy = f.core.features.samplerAnisotropy == VK_TRUE;
    m_info.geometryShader = f.core.features.geometryShader == VK_TRUE;
    m_info.fragmentStoresAndAtomics = f.core.features.fragmentStoresAndAtomics == VK_TRUE;
    m_info.maxSamplerAnisotropy = props.limits.maxSamplerAnisotropy;
    fillDescriptorLimits(physical, props, m_info.descriptorIndexing, m_info.descriptorLimits);

    const u32 graphicsFamily = desc.graphicsFamily;
    const u32 computeFamily = desc.computeFamily == kAdoptSameAsGraphics ? graphicsFamily : desc.computeFamily;
    const u32 transferFamily = desc.transferFamily == kAdoptSameAsGraphics ? graphicsFamily : desc.transferFamily;
    auto queueOf = [logicalDevice](void* given, u32 family) {
        if (given != nullptr) {
            return static_cast<VkQueue>(given);
        }
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(logicalDevice, family, 0, &queue);
        return queue;
    };
    VulkanQueues& queues = m_info.queues;
    queues.graphicsFamily = graphicsFamily;
    queues.computeFamily = computeFamily;
    queues.transferFamily = transferFamily;
    queues.dedicatedCompute = computeFamily != graphicsFamily;
    queues.dedicatedTransfer = transferFamily != graphicsFamily;
    queues.graphics = queueOf(desc.graphicsQueue, graphicsFamily);
    queues.compute = computeFamily == graphicsFamily && desc.computeQueue == nullptr ? queues.graphics
                                                                                     : queueOf(desc.computeQueue, computeFamily);
    queues.transfer = transferFamily == graphicsFamily && desc.transferQueue == nullptr
                          ? queues.graphics
                          : queueOf(desc.transferQueue, transferFamily);

    m_info.vmaAllocator = nullptr;
    m_info.valid = true;
    m_info.message = desc.takeOwnership ? "Adopted device (owning: destroyed with this object)"
                                        : "Adopted device (non-owning: the creator destroys it)";
    return true;
#else
    (void)desc;
    m_info.message = "Adopt skipped — Vulkan backend disabled at build time";
    return false;
#endif
}

VulkanDevice::~VulkanDevice() {
    shutdown();
}

void* VulkanDevice::nativeHandle() const {
    return m_handle;
}

void* VulkanDevice::nativePhysicalDevice() const {
    return m_physicalDevice;
}

void* VulkanDevice::instanceHandle() const {
    if (m_instance == nullptr) {
        return m_instanceHandle;
    }
    return m_instance->nativeHandle();
}

void VulkanDevice::setVmaAllocator(void* allocator) {
    m_info.vmaAllocator = allocator;
}

void VulkanDevice::waitIdle() const {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_handle != nullptr) {
        vkDeviceWaitIdle(static_cast<VkDevice>(m_handle));
    }
#endif
}

bool VulkanDevice::initialize(VulkanInstance& instance, const VulkanDeviceDesc& desc) {
    m_instance = &instance;

#if defined(FUSE_VULKAN_BACKEND)
    if (!instance.isValid()) {
        m_info.message = "Device skipped — instance not ready";
        return false;
    }

    auto vkInstance = static_cast<VkInstance>(instance.nativeHandle());

    u32 deviceCount = 0;
    vkEnumeratePhysicalDevices(vkInstance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        m_info.message = "No Vulkan physical devices — CI headless stub path";
        return false;
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(vkInstance, &deviceCount, devices.data());

    // VK_KHR_swapchain and surface queries both depend on VK_KHR_surface at instance level.
    const bool instanceHasSurface = instance.info().instanceHasExtension(VK_KHR_SURFACE_EXTENSION_NAME);

    // Physical device selection: drop devices missing a hard requirement, then rank the rest
    // (see betterCandidate). The summary lands in m_info.selection either way.
    m_info.physicalDeviceCount = deviceCount;
    const DeviceCandidate* best = nullptr;
    std::vector<DeviceCandidate> candidates;
    candidates.reserve(deviceCount);
    for (u32 i = 0; i < deviceCount; ++i) {
        candidates.push_back(evaluateCandidate(devices[i], i, desc, instanceHasSurface, instance.info().apiVersion));
    }
    for (const DeviceCandidate& candidate : candidates) {
        if (candidate.rejectReason.empty() &&
            (best == nullptr || betterCandidate(candidate, *best, desc.preferDiscreteGpu))) {
            best = &candidate;
        }
    }
    for (const DeviceCandidate& candidate : candidates) {
        if (!m_info.selection.empty()) {
            m_info.selection += "; ";
        }
        m_info.selection += "#" + std::to_string(candidate.index) + " '" + candidate.name + "' (" +
                            deviceTypeName(candidate.type) + ", " +
                            std::to_string(candidate.deviceLocalBytes >> 20) + " MiB device-local, 2D " +
                            std::to_string(candidate.maxImageDimension2D) +
                            (candidate.rejectReason.empty()
                                 ? std::string(", ") + renderTierName(candidate.tier) +
                                       (candidate.probe.meetsT0 ? "" : " below-T0")
                                 : std::string()) +
                            "): " +
                            (&candidate == best                  ? std::string("selected")
                             : candidate.rejectReason.empty() ? std::string("suitable")
                                                               : "rejected, " + candidate.rejectReason);
    }
    if (best == nullptr) {
        m_info.message = "No suitable Vulkan physical device — " + m_info.selection;
        return false;
    }
    VkPhysicalDevice selected = best->handle;
    m_info.physicalDeviceIndex = best->index;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(selected, &props);
    m_info.deviceName = props.deviceName;

    u32 graphicsFamily = findQueueFamily(selected, VK_QUEUE_GRAPHICS_BIT);
    const u32 computeFamily = findComputeFamily(selected);
    const u32 transferFamily = findTransferFamily(selected);

    std::string presentNote;
    if (desc.presentSurface != nullptr && !instanceHasSurface) {
        presentNote = "present surface ignored: instance lacks VK_KHR_surface";
        m_info.message = presentNote;
        if (desc.requirePresentation) {
            return false;
        }
    } else if (desc.presentSurface != nullptr) {
        const auto surface = reinterpret_cast<VkSurfaceKHR>(desc.presentSurface);
        bool foundPresentableGraphics = false;
        const u32 presentableFamily =
            findPresentableGraphicsFamily(selected, surface, foundPresentableGraphics);
        if (foundPresentableGraphics) {
            graphicsFamily = presentableFamily;
        } else {
            presentNote = "no graphics queue family presents to the given surface";
            m_info.message = presentNote;
            if (desc.requirePresentation) {
                return false;
            }
        }
    }

    std::vector<const char*> enabledExtensions;
    for (const char* extension : kPreferredExtensions) {
        if (!instanceHasSurface && std::strcmp(extension, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            continue;
        }
        if (extensionSupported(selected, extension)) {
            enabledExtensions.push_back(extension);
        }
    }
    for (const char* extension : kPlatformExternalExtensions) {
        if (extensionSupported(selected, extension)) {
            enabledExtensions.push_back(extension);
        }
    }

    if (desc.requirePresentation &&
        (!instanceHasSurface || !extensionSupported(selected, VK_KHR_SWAPCHAIN_EXTENSION_NAME))) {
        m_info.message = "Presentation requested but VK_KHR_swapchain unavailable";
        return false;
    }

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    const float queuePriority = 1.f;
    auto addQueue = [&](u32 family) {
        for (const VkDeviceQueueCreateInfo& existing : queueCreateInfos) {
            if (existing.queueFamilyIndex == family) {
                return;
            }
        }
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueInfo);
    };

    addQueue(graphicsFamily);
    if (computeFamily != graphicsFamily) {
        addQueue(computeFamily);
    }
    if (transferFamily != graphicsFamily && transferFamily != computeFamily) {
        addQueue(transferFamily);
    }

    // Feature enable chain (WP-0.1): the T0 set always (selection guaranteed it unless the
    // FUSE_VK_ALLOW_1_2 escape let a legacy device through), optional and tier features when
    // supported and not above the tier cap. Only structs with something enabled are chained, so
    // no struct of a non-enabled extension reaches vkCreateDevice.
    const DeviceProbe& probe = best->probe;
    const bool api13 = probe.apiVersion >= VK_API_VERSION_1_3;
    const RenderTier tierCap = tierCapFor(desc);
    u64 enabledMask = probe.supportedMask & ~renderFeaturesAboveTier(tierCap);
    if (!desc.enableOptionalFeatures) {
        enabledMask &= renderT0RequiredMask();
    }
#if !defined(VK_EXT_device_generated_commands)
    enabledMask &= ~renderFeatureBit(RenderFeature::DeviceGeneratedCommands);
#endif
    auto enabledFeature = [&enabledMask](RenderFeature feature) {
        return (enabledMask & renderFeatureBit(feature)) != 0;
    };
    const DeviceFeatureSet& supportedSet = probe.supported;
    const VkPhysicalDeviceVulkan12Features& supported12 = supportedSet.v12;

    DeviceFeatureSet enabledSet{};
    VkPhysicalDeviceVulkan12Features& enabled12 = enabledSet.v12;
    enabled12.descriptorIndexing = supported12.descriptorIndexing;
    enabled12.descriptorBindingPartiallyBound = supported12.descriptorBindingPartiallyBound;
    enabled12.runtimeDescriptorArray = supported12.runtimeDescriptorArray;
    enabled12.descriptorBindingSampledImageUpdateAfterBind =
        supported12.descriptorBindingSampledImageUpdateAfterBind;
    enabled12.descriptorBindingStorageBufferUpdateAfterBind =
        supported12.descriptorBindingStorageBufferUpdateAfterBind;
    enabled12.descriptorBindingStorageImageUpdateAfterBind =
        supported12.descriptorBindingStorageImageUpdateAfterBind;
    enabled12.descriptorBindingUniformBufferUpdateAfterBind =
        supported12.descriptorBindingUniformBufferUpdateAfterBind;
    // Composite/bindless shaders index texture arrays with nonuniformEXT.
    enabled12.shaderSampledImageArrayNonUniformIndexing =
        supported12.shaderSampledImageArrayNonUniformIndexing;
    enabled12.shaderStorageImageArrayNonUniformIndexing =
        supported12.shaderStorageImageArrayNonUniformIndexing;
    enabled12.shaderStorageBufferArrayNonUniformIndexing =
        supported12.shaderStorageBufferArrayNonUniformIndexing;
    enabled12.bufferDeviceAddress = supported12.bufferDeviceAddress;
    enabled12.timelineSemaphore = supported12.timelineSemaphore;
    enabled12.drawIndirectCount = supported12.drawIndirectCount;
    enabled12.shaderBufferInt64Atomics = supported12.shaderBufferInt64Atomics;
    enabledSet.v11.shaderDrawParameters = supportedSet.v11.shaderDrawParameters;

    VkPhysicalDeviceFeatures& deviceFeatures = enabledSet.core.features;
    deviceFeatures.samplerAnisotropy = supportedSet.core.features.samplerAnisotropy;
    deviceFeatures.multiDrawIndirect = supportedSet.core.features.multiDrawIndirect;
    deviceFeatures.shaderInt64 = supportedSet.core.features.shaderInt64;
    // WP-1.4 visibility buffer: gl_PrimitiveID / SV_PrimitiveID in a fragment shader needs the SPIR-V
    // Geometry capability (geometryShader feature; no geometry stage is used), and the 64-bit atomic
    // path writes from the fragment stage (fragmentStoresAndAtomics). Enabled when supported.
    deviceFeatures.geometryShader = supportedSet.core.features.geometryShader;
    deviceFeatures.fragmentStoresAndAtomics = supportedSet.core.features.fragmentStoresAndAtomics;

    auto hasEnabledExtension = [&enabledExtensions](const char* name) {
        for (const char* extension : enabledExtensions) {
            if (std::strcmp(extension, name) == 0) {
                return true;
            }
        }
        return false;
    };
    auto enableExtension = [&](const char* name) {
        if (!hasEnabledExtension(name)) {
            enabledExtensions.push_back(name);
        }
    };

    FeatureChainPlan enablePlan{};
    const bool wantSync2 = enabledFeature(RenderFeature::Synchronization2);
    const bool wantDynamic = enabledFeature(RenderFeature::DynamicRendering);
    const bool wantMaintenance4 = enabledFeature(RenderFeature::Maintenance4);
    if (api13) {
        enablePlan.v13 = true;
        enabledSet.v13.synchronization2 = wantSync2 ? VK_TRUE : VK_FALSE;
        enabledSet.v13.dynamicRendering = wantDynamic ? VK_TRUE : VK_FALSE;
        enabledSet.v13.maintenance4 = wantMaintenance4 ? VK_TRUE : VK_FALSE;
        // Optional (WP-0.5 pipeline cache: FAIL_ON_PIPELINE_COMPILE_REQUIRED probes); core 1.3 only.
        enabledSet.v13.pipelineCreationCacheControl =
            desc.enableOptionalFeatures ? supportedSet.v13.pipelineCreationCacheControl : VK_FALSE;
    } else {
        // Legacy (FUSE_VK_ALLOW_1_2) path: the promoted extensions' own feature structs.
        if (wantSync2 && hasEnabledExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)) {
            enablePlan.sync2 = true;
            enabledSet.sync2.synchronization2 = VK_TRUE;
        } else {
            enabledMask &= ~renderFeatureBit(RenderFeature::Synchronization2);
        }
        if (wantDynamic && hasEnabledExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
            enablePlan.dynamic = true;
            enabledSet.dynamic.dynamicRendering = VK_TRUE;
        } else {
            enabledMask &= ~renderFeatureBit(RenderFeature::DynamicRendering);
        }
        if (wantMaintenance4) {
            enableExtension(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
            enablePlan.maintenance4 = true;
            enabledSet.maintenance4.maintenance4 = VK_TRUE;
        }
    }
    if (enabledFeature(RenderFeature::ShaderImageInt64Atomics)) {
        enableExtension(VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME);
        enablePlan.imageAtomicInt64 = true;
        enabledSet.imageAtomicInt64.shaderImageInt64Atomics = VK_TRUE;
    }
    if (enabledFeature(RenderFeature::DescriptorBuffer)) {
        enableExtension(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);
        enablePlan.descriptorBuffer = true;
        enabledSet.descriptorBuffer.descriptorBuffer = VK_TRUE;
    }
#if defined(VK_EXT_device_generated_commands)
    if (enabledFeature(RenderFeature::DeviceGeneratedCommands)) {
        enableExtension(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        enableExtension(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
        enablePlan.maintenance5 = true;
        enabledSet.maintenance5.maintenance5 = VK_TRUE;
        enablePlan.deviceGeneratedCommands = true;
        enabledSet.deviceGeneratedCommands.deviceGeneratedCommands = VK_TRUE;
    }
#endif
    if (enabledFeature(RenderFeature::ShaderObject)) {
        enableExtension(VK_EXT_SHADER_OBJECT_EXTENSION_NAME);
        enablePlan.shaderObject = true;
        enabledSet.shaderObject.shaderObject = VK_TRUE;
    }
    if (enabledFeature(RenderFeature::TaskShader) || enabledFeature(RenderFeature::MeshShader)) {
        enableExtension(VK_EXT_MESH_SHADER_EXTENSION_NAME);
        enablePlan.meshShader = true;
        enabledSet.meshShader.taskShader = enabledFeature(RenderFeature::TaskShader) ? VK_TRUE : VK_FALSE;
        enabledSet.meshShader.meshShader = enabledFeature(RenderFeature::MeshShader) ? VK_TRUE : VK_FALSE;
    }
    if (!enabledFeature(RenderFeature::AccelerationStructure)) {
        // Ray query and the RT pipeline both need acceleration structures.
        enabledMask &= ~(renderFeatureBit(RenderFeature::RayQuery) | renderFeatureBit(RenderFeature::RayTracingPipeline));
    } else {
        enableExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        enableExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        enablePlan.accelerationStructure = true;
        enabledSet.accelerationStructure.accelerationStructure = VK_TRUE;
        enabledSet.accelerationStructure.descriptorBindingAccelerationStructureUpdateAfterBind =
            supportedSet.accelerationStructure.descriptorBindingAccelerationStructureUpdateAfterBind;
        if (enabledFeature(RenderFeature::RayQuery)) {
            enableExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            enablePlan.rayQuery = true;
            enabledSet.rayQuery.rayQuery = VK_TRUE;
        }
        if (enabledFeature(RenderFeature::RayTracingPipeline)) {
            enableExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
            enablePlan.rayTracingPipeline = true;
            enabledSet.rayTracingPipeline.rayTracingPipeline = VK_TRUE;
        }
    }
    if (enabledFeature(RenderFeature::CooperativeMatrix)) {
        enableExtension(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
        enablePlan.cooperativeMatrix = true;
        enabledSet.cooperativeMatrix.cooperativeMatrix = VK_TRUE;
    }

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = linkFeatureChain(enabledSet, enablePlan);
    createInfo.queueCreateInfoCount = static_cast<u32>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.pEnabledFeatures = nullptr; // VkPhysicalDeviceFeatures2 in the chain
    createInfo.enabledExtensionCount = static_cast<u32>(enabledExtensions.size());
    createInfo.ppEnabledExtensionNames =
        enabledExtensions.empty() ? nullptr : enabledExtensions.data();

    VkDevice logicalDevice = VK_NULL_HANDLE;
    if (vkCreateDevice(selected, &createInfo, nullptr, &logicalDevice) != VK_SUCCESS) {
        m_info.message = "vkCreateDevice failed";
        return false;
    }
    // Device-level dispatch (volkLoadDevice) while this is the only live device; loader
    // trampolines while several are (vk/loader.hpp). Before any other call on the device.
    vkloader::registerDevice(logicalDevice, vkInstance);
    m_registered = true;
    m_features.reset(new EnabledFeatures{});
    m_features->set = enabledSet;
    m_features->plan = enablePlan;
    linkFeatureChain(m_features->set, m_features->plan);

    m_physicalDevice = selected;
    m_handle = logicalDevice;
    m_info.valid = true;
    m_info.enabledExtensions = enabledExtensions;
    m_info.descriptorIndexing = enabled12.descriptorIndexing == VK_TRUE;
    m_info.sampledImageUpdateAfterBind = enabled12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE;
    m_info.storageImageUpdateAfterBind = enabled12.descriptorBindingStorageImageUpdateAfterBind == VK_TRUE;
    m_info.storageBufferUpdateAfterBind = enabled12.descriptorBindingStorageBufferUpdateAfterBind == VK_TRUE;
    m_info.uniformBufferUpdateAfterBind = enabled12.descriptorBindingUniformBufferUpdateAfterBind == VK_TRUE;
    m_info.sampledImageNonUniformIndexing = enabled12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    for (const char* extension : enabledExtensions) {
        if (std::strcmp(extension, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
            m_info.swapchainExtension = true;
        }
    }
    m_info.bufferDeviceAddress = enabled12.bufferDeviceAddress == VK_TRUE;
    m_info.timelineSemaphore = enabled12.timelineSemaphore == VK_TRUE;
    m_info.dynamicRendering = enabledFeature(RenderFeature::DynamicRendering);
    m_info.pipelineCreationCacheControl = api13 && enabledSet.v13.pipelineCreationCacheControl == VK_TRUE;
    m_info.samplerAnisotropy = deviceFeatures.samplerAnisotropy == VK_TRUE;
    m_info.geometryShader = deviceFeatures.geometryShader == VK_TRUE;
    m_info.fragmentStoresAndAtomics = deviceFeatures.fragmentStoresAndAtomics == VK_TRUE;
    m_info.maxSamplerAnisotropy = props.limits.maxSamplerAnisotropy;
    fillDescriptorLimits(selected, props, m_info.descriptorIndexing, m_info.descriptorLimits);
    m_info.deviceType = static_cast<u32>(props.deviceType);
    m_instanceApiVersion = instance.info().apiVersion;
    m_info.apiVersion = std::min(instance.info().apiVersion, props.apiVersion);
    {
        RendererCaps& caps = m_info.caps;
        caps.valid = true;
        caps.apiVersion = m_info.apiVersion;
        caps.supportedMask = probe.supportedMask;
        caps.setEnabledMask(enabledMask);
        caps.meetsT0 = probe.meetsT0 && (enabledMask & renderT0RequiredMask()) == renderT0RequiredMask();
        caps.hardwareTier = probe.hardwareTier;
        caps.tierCap = tierCap;
        caps.tier = caps.meetsT0 ? minTier(renderTierFromMask(enabledMask), tierCap) : RenderTier::T0;
    }
    m_info.queues.graphicsFamily = graphicsFamily;
    m_info.queues.computeFamily = computeFamily;
    m_info.queues.transferFamily = transferFamily;
    m_info.queues.dedicatedTransfer = transferFamily != graphicsFamily;
    m_info.queues.dedicatedCompute = computeFamily != graphicsFamily;

    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue computeQueue = VK_NULL_HANDLE;
    VkQueue transferQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(logicalDevice, graphicsFamily, 0, &graphicsQueue);
    vkGetDeviceQueue(logicalDevice, computeFamily, 0, &computeQueue);
    vkGetDeviceQueue(logicalDevice, transferFamily, 0, &transferQueue);
    m_info.queues.graphics = graphicsQueue;
    m_info.queues.compute = computeQueue;
    m_info.queues.transfer = transferQueue;
    m_info.vmaAllocator = nullptr;
    m_info.message = "Logical device ready (VMA via GpuAllocator)";
    if (!presentNote.empty()) {
        m_info.message += " — ";
        m_info.message += presentNote;
    }
    return true;
#else
    (void)desc;
    m_info.message = "Device skipped — Vulkan backend disabled at build time";
    return false;
#endif
}

void VulkanDevice::shutdown() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_handle != nullptr) {
        if (m_ownsDevice) {
            vkDestroyDevice(static_cast<VkDevice>(m_handle), nullptr);
        }
        if (m_registered) {
            // Drops this object's reference only: a device adopted from a live VulkanDevice (or the
            // creator of an adopted one) keeps its registration and its dispatch tables.
            vkloader::unregisterDevice(m_handle);
            if (m_adopted) {
                vkloader::unregisterInstance(m_instanceHandle);
            }
        }
        m_handle = nullptr;
    }
    m_registered = false;
    m_physicalDevice = nullptr;
    m_instanceHandle = nullptr;
#endif
}

} // namespace fuse::renderer
