// Test-only Vulkan layer VK_LAYER_FUSE_mask_features (WP-0.1).
//
// Lavapipe reports renderer tier T2. To prove the RHI reports T0 / T1 honestly and rejects a device
// missing a T0 requirement, this layer hides device extensions and feature bits from everything
// above it (the application and VK_LAYER_KHRONOS_validation, which is loaded above this layer, so
// enabling a masked feature is a validation error). Driven by FUSE_MASK_FEATURES, read at
// vkCreateInstance:
//
//   FUSE_MASK_FEATURES = group[;group...]
//   group              = [<device>:]token[,token...]   <device> = index in the enumeration below
//                                                     this layer; no prefix = every device
//   token              = VK_<extension name>          hidden from vkEnumerateDeviceExtensionProperties
//                      | api=<major>.<minor>          reported apiVersion (patch kept)
//                      | <feature>                    reported VK_FALSE (see kFeatures)
//
// The layer also records what vkCreateDevice passes down (enabled extensions and every known
// feature bit set to VK_TRUE in pEnabledFeatures / the pNext chain), readable in-process through
// the exported fuseMaskFeaturesLastDeviceCreate() as "ext=<a>,<b>;feat=<x>,<y>". Masked values
// only ever narrow what the real device supports. An unset / empty spec makes it a pass-through
// (still recording).
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
#define FUSE_LAYER_EXPORT extern "C"
#elif defined(_WIN32)
#define FUSE_LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define FUSE_LAYER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

constexpr const char* kLayerName = "VK_LAYER_FUSE_mask_features";

/// A VkBool32 feature field: structure type + byte offset inside that structure.
struct FeatureField {
    const char* name;
    VkStructureType sType;
    size_t offset;
};

#define FUSE_CORE(field)                                                                                              \
    {#field, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,                                                           \
     offsetof(VkPhysicalDeviceFeatures2, features) + offsetof(VkPhysicalDeviceFeatures, field)}
#define FUSE_FIELD(field, sType, type) {#field, sType, offsetof(type, field)}

const FeatureField kFeatures[] = {
    FUSE_CORE(shaderInt64),
    FUSE_CORE(multiDrawIndirect),
    FUSE_CORE(samplerAnisotropy),
    FUSE_FIELD(shaderDrawParameters, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
               VkPhysicalDeviceVulkan11Features),
    FUSE_FIELD(shaderDrawParameters, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,
               VkPhysicalDeviceShaderDrawParametersFeatures),
    FUSE_FIELD(drawIndirectCount, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
               VkPhysicalDeviceVulkan12Features),
    FUSE_FIELD(shaderBufferInt64Atomics, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
               VkPhysicalDeviceVulkan12Features),
    FUSE_FIELD(shaderBufferInt64Atomics, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES,
               VkPhysicalDeviceShaderAtomicInt64Features),
    FUSE_FIELD(timelineSemaphore, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
               VkPhysicalDeviceVulkan12Features),
    FUSE_FIELD(timelineSemaphore, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
               VkPhysicalDeviceTimelineSemaphoreFeatures),
    FUSE_FIELD(bufferDeviceAddress, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
               VkPhysicalDeviceVulkan12Features),
    FUSE_FIELD(bufferDeviceAddress, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
               VkPhysicalDeviceBufferDeviceAddressFeatures),
    FUSE_FIELD(descriptorIndexing, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
               VkPhysicalDeviceVulkan12Features),
    FUSE_FIELD(synchronization2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
               VkPhysicalDeviceVulkan13Features),
    FUSE_FIELD(synchronization2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
               VkPhysicalDeviceSynchronization2Features),
    FUSE_FIELD(dynamicRendering, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
               VkPhysicalDeviceVulkan13Features),
    FUSE_FIELD(dynamicRendering, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
               VkPhysicalDeviceDynamicRenderingFeatures),
    FUSE_FIELD(maintenance4, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features),
    FUSE_FIELD(maintenance4, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES,
               VkPhysicalDeviceMaintenance4Features),
    FUSE_FIELD(shaderImageInt64Atomics, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT,
               VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT),
    FUSE_FIELD(descriptorBuffer, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
               VkPhysicalDeviceDescriptorBufferFeaturesEXT),
    FUSE_FIELD(shaderObject, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
               VkPhysicalDeviceShaderObjectFeaturesEXT),
    FUSE_FIELD(taskShader, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, VkPhysicalDeviceMeshShaderFeaturesEXT),
    FUSE_FIELD(meshShader, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, VkPhysicalDeviceMeshShaderFeaturesEXT),
    FUSE_FIELD(accelerationStructure, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
               VkPhysicalDeviceAccelerationStructureFeaturesKHR),
    FUSE_FIELD(rayQuery, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, VkPhysicalDeviceRayQueryFeaturesKHR),
    FUSE_FIELD(rayTracingPipeline, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
               VkPhysicalDeviceRayTracingPipelineFeaturesKHR),
    FUSE_FIELD(cooperativeMatrix, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,
               VkPhysicalDeviceCooperativeMatrixFeaturesKHR),
};

#undef FUSE_CORE
#undef FUSE_FIELD

bool knownFeature(const std::string& name) {
    for (const FeatureField& field : kFeatures) {
        if (name == field.name) {
            return true;
        }
    }
    return false;
}

struct MaskEntry {
    int device = -1; ///< -1 = every device
    std::vector<std::string> extensions;
    std::vector<std::string> features;
    uint32_t apiMajor = 0;
    uint32_t apiMinor = 0;
    bool apiOverride = false;
};

/// What applies to one device: the union of every matching group.
struct DeviceMask {
    std::vector<std::string> extensions;
    std::vector<std::string> features;
    bool apiOverride = false;
    uint32_t apiMajor = 0;
    uint32_t apiMinor = 0;

    bool hidesExtension(const char* name) const {
        return std::find(extensions.begin(), extensions.end(), name) != extensions.end();
    }
    bool hidesFeature(const char* name) const {
        return std::find(features.begin(), features.end(), name) != features.end();
    }
};

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == separator) {
            parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    parts.push_back(current);
    return parts;
}

bool parseSpec(const char* text, std::vector<MaskEntry>& out) {
    out.clear();
    if (text == nullptr || text[0] == '\0') {
        return true;
    }
    for (std::string group : split(text, ';')) {
        if (group.empty()) {
            continue;
        }
        MaskEntry entry{};
        const size_t colon = group.find(':');
        if (colon != std::string::npos) {
            entry.device = std::atoi(group.substr(0, colon).c_str());
            group = group.substr(colon + 1);
        }
        for (const std::string& token : split(group, ',')) {
            if (token.empty()) {
                continue;
            }
            if (token.rfind("VK_", 0) == 0) {
                entry.extensions.push_back(token);
            } else if (token.rfind("api=", 0) == 0) {
                entry.apiOverride = std::sscanf(token.c_str() + 4, "%u.%u", &entry.apiMajor, &entry.apiMinor) == 2;
                if (!entry.apiOverride) {
                    std::fprintf(stderr, "%s: bad api token '%s'\n", kLayerName, token.c_str());
                    return false;
                }
            } else if (knownFeature(token)) {
                entry.features.push_back(token);
            } else {
                std::fprintf(stderr, "%s: unknown token '%s'\n", kLayerName, token.c_str());
                return false;
            }
        }
        out.push_back(std::move(entry));
    }
    return true;
}

using DispatchKey = void*;

DispatchKey dispatchKey(const void* handle) {
    return *static_cast<void* const*>(handle);
}

struct InstanceData {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices = nullptr;
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceProperties properties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 properties2 = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 properties2KHR = nullptr;
    PFN_vkGetPhysicalDeviceFeatures features = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 features2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 features2KHR = nullptr;
    std::vector<MaskEntry> spec;
    /// Per real device (enumeration order below this layer).
    std::unordered_map<VkPhysicalDevice, DeviceMask> masks;
    bool enumerated = false;
};

struct DeviceData {
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
};

std::mutex g_mutex;
std::unordered_map<DispatchKey, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<VkPhysicalDevice, InstanceData*> g_physical;
std::unordered_map<DispatchKey, std::unique_ptr<DeviceData>> g_devices;
std::string g_lastDeviceCreate;

InstanceData* instanceData(VkInstance instance) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instances.find(dispatchKey(instance));
    return it != g_instances.end() ? it->second.get() : nullptr;
}

/// Builds the per-device masks from the spec on the first enumeration.
void ensureEnumerated(InstanceData& data) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (data.enumerated) {
            return;
        }
    }
    uint32_t count = 0;
    data.enumeratePhysicalDevices(data.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> real(count);
    data.enumeratePhysicalDevices(data.instance, &count, real.data());
    real.resize(count);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < real.size(); ++i) {
        DeviceMask mask{};
        for (const MaskEntry& entry : data.spec) {
            if (entry.device >= 0 && static_cast<uint32_t>(entry.device) != i) {
                continue;
            }
            mask.extensions.insert(mask.extensions.end(), entry.extensions.begin(), entry.extensions.end());
            mask.features.insert(mask.features.end(), entry.features.begin(), entry.features.end());
            if (entry.apiOverride) {
                mask.apiOverride = true;
                mask.apiMajor = entry.apiMajor;
                mask.apiMinor = entry.apiMinor;
            }
        }
        data.masks[real[i]] = std::move(mask);
        g_physical[real[i]] = &data;
    }
    data.enumerated = true;
}

InstanceData* physicalInstance(VkPhysicalDevice physicalDevice) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_physical.find(physicalDevice);
    if (it != g_physical.end()) {
        return it->second;
    }
    return g_instances.size() == 1 ? g_instances.begin()->second.get() : nullptr;
}

const DeviceMask* maskFor(InstanceData& data, VkPhysicalDevice physicalDevice) {
    ensureEnumerated(data);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = data.masks.find(physicalDevice);
    return it != data.masks.end() ? &it->second : nullptr;
}

VkBool32* fieldIn(void* structure, const FeatureField& field) {
    return reinterpret_cast<VkBool32*>(static_cast<char*>(structure) + field.offset);
}

void maskFeatureChain(const DeviceMask& mask, void* head) {
    for (auto* node = static_cast<VkBaseOutStructure*>(head); node != nullptr; node = node->pNext) {
        for (const FeatureField& field : kFeatures) {
            if (node->sType == field.sType && mask.hidesFeature(field.name)) {
                *fieldIn(node, field) = VK_FALSE;
            }
        }
    }
}

void appendName(std::string& out, const char* name) {
    if (!out.empty() && out.back() != '=') {
        out += ',';
    }
    out += name;
}

/// "ext=<a>,<b>;feat=<x>,<y>" of what reaches the driver.
std::string describeCreateInfo(const VkDeviceCreateInfo& info) {
    std::string ext = "ext=";
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i) {
        appendName(ext, info.ppEnabledExtensionNames[i]);
    }
    std::vector<std::string> features;
    auto add = [&features](const char* name) {
        if (std::find(features.begin(), features.end(), name) == features.end()) {
            features.emplace_back(name);
        }
    };
    if (info.pEnabledFeatures != nullptr) {
        for (const FeatureField& field : kFeatures) {
            if (field.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
                const size_t offset = field.offset - offsetof(VkPhysicalDeviceFeatures2, features);
                const auto* value = reinterpret_cast<const VkBool32*>(
                    reinterpret_cast<const char*>(info.pEnabledFeatures) + offset);
                if (*value == VK_TRUE) {
                    add(field.name);
                }
            }
        }
    }
    for (auto* node = static_cast<const VkBaseInStructure*>(info.pNext); node != nullptr; node = node->pNext) {
        for (const FeatureField& field : kFeatures) {
            if (node->sType == field.sType &&
                *reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(node) + field.offset) == VK_TRUE) {
                add(field.name);
            }
        }
    }
    std::string feat = "feat=";
    for (const std::string& name : features) {
        appendName(feat, name.c_str());
    }
    return ext + ";" + feat;
}

// ---------------------------------------------------------------------------------------------
// Instance level

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                              const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    auto* chain = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(pCreateInfo->pNext));
    while (chain != nullptr &&
           !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO)) {
        chain = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(chain->pNext));
    }
    if (chain == nullptr || chain->u.pLayerInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (createInstance == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    auto data = std::make_unique<InstanceData>();
    if (!parseSpec(std::getenv("FUSE_MASK_FEATURES"), data->spec)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = createInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        return result;
    }
    data->instance = *pInstance;
    data->gipa = gipa;
#define FUSE_LOAD(member, type, name) data->member = reinterpret_cast<type>(gipa(*pInstance, name))
    FUSE_LOAD(destroyInstance, PFN_vkDestroyInstance, "vkDestroyInstance");
    FUSE_LOAD(enumeratePhysicalDevices, PFN_vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    FUSE_LOAD(enumerateDeviceExtensionProperties, PFN_vkEnumerateDeviceExtensionProperties,
              "vkEnumerateDeviceExtensionProperties");
    FUSE_LOAD(properties, PFN_vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    FUSE_LOAD(properties2, PFN_vkGetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2");
    FUSE_LOAD(properties2KHR, PFN_vkGetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2KHR");
    FUSE_LOAD(features, PFN_vkGetPhysicalDeviceFeatures, "vkGetPhysicalDeviceFeatures");
    FUSE_LOAD(features2, PFN_vkGetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2");
    FUSE_LOAD(features2KHR, PFN_vkGetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2KHR");
#undef FUSE_LOAD
    std::lock_guard<std::mutex> lock(g_mutex);
    g_instances[dispatchKey(*pInstance)] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    if (instance == VK_NULL_HANDLE) {
        return;
    }
    PFN_vkDestroyInstance destroy = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_instances.find(dispatchKey(instance));
        if (it == g_instances.end()) {
            return;
        }
        InstanceData* data = it->second.get();
        destroy = data->destroyInstance;
        for (auto physical = g_physical.begin(); physical != g_physical.end();) {
            physical = physical->second == data ? g_physical.erase(physical) : std::next(physical);
        }
        g_instances.erase(it);
    }
    destroy(instance, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                  const char* pLayerName, uint32_t* pCount,
                                                                  VkExtensionProperties* pProperties) {
    if (pLayerName != nullptr && std::strcmp(pLayerName, kLayerName) == 0) {
        *pCount = 0;
        return VK_SUCCESS;
    }
    InstanceData* data = physicalInstance(physicalDevice);
    if (data == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pLayerName != nullptr) {
        return data->enumerateDeviceExtensionProperties(physicalDevice, pLayerName, pCount, pProperties);
    }
    uint32_t count = 0;
    VkResult result = data->enumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
    if (result != VK_SUCCESS) {
        return result;
    }
    std::vector<VkExtensionProperties> all(count);
    result = data->enumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, all.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return result;
    }
    all.resize(count);
    const DeviceMask* mask = maskFor(*data, physicalDevice);
    std::vector<VkExtensionProperties> visible;
    for (const VkExtensionProperties& extension : all) {
        if (mask == nullptr || !mask->hidesExtension(extension.extensionName)) {
            visible.push_back(extension);
        }
    }
    const uint32_t available = static_cast<uint32_t>(visible.size());
    if (pProperties == nullptr) {
        *pCount = available;
        return VK_SUCCESS;
    }
    const uint32_t written = std::min(*pCount, available);
    std::copy(visible.begin(), visible.begin() + written, pProperties);
    *pCount = written;
    return written < available ? VK_INCOMPLETE : VK_SUCCESS;
}

void maskProperties(const DeviceMask* mask, VkPhysicalDeviceProperties& props) {
    if (mask != nullptr && mask->apiOverride) {
        props.apiVersion =
            VK_MAKE_API_VERSION(0, mask->apiMajor, mask->apiMinor, VK_API_VERSION_PATCH(props.apiVersion));
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                       VkPhysicalDeviceProperties* pProperties) {
    InstanceData* data = physicalInstance(physicalDevice);
    data->properties(physicalDevice, pProperties);
    maskProperties(maskFor(*data, physicalDevice), *pProperties);
}

void properties2(PFN_vkGetPhysicalDeviceProperties2 next, VkPhysicalDevice physicalDevice,
                 VkPhysicalDeviceProperties2* pProperties) {
    InstanceData* data = physicalInstance(physicalDevice);
    next(physicalDevice, pProperties);
    maskProperties(maskFor(*data, physicalDevice), pProperties->properties);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                                        VkPhysicalDeviceProperties2* pProperties) {
    properties2(physicalInstance(physicalDevice)->properties2, physicalDevice, pProperties);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice,
                                                           VkPhysicalDeviceProperties2* pProperties) {
    InstanceData* data = physicalInstance(physicalDevice);
    properties2(data->properties2KHR != nullptr ? data->properties2KHR : data->properties2, physicalDevice,
                pProperties);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                                     VkPhysicalDeviceFeatures* pFeatures) {
    InstanceData* data = physicalInstance(physicalDevice);
    data->features(physicalDevice, pFeatures);
    const DeviceMask* mask = maskFor(*data, physicalDevice);
    if (mask == nullptr) {
        return;
    }
    for (const FeatureField& field : kFeatures) {
        if (field.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 && mask->hidesFeature(field.name)) {
            *reinterpret_cast<VkBool32*>(reinterpret_cast<char*>(pFeatures) + field.offset -
                                         offsetof(VkPhysicalDeviceFeatures2, features)) = VK_FALSE;
        }
    }
}

void features2(PFN_vkGetPhysicalDeviceFeatures2 next, VkPhysicalDevice physicalDevice,
               VkPhysicalDeviceFeatures2* pFeatures) {
    InstanceData* data = physicalInstance(physicalDevice);
    next(physicalDevice, pFeatures);
    if (const DeviceMask* mask = maskFor(*data, physicalDevice)) {
        maskFeatureChain(*mask, pFeatures);
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                                      VkPhysicalDeviceFeatures2* pFeatures) {
    features2(physicalInstance(physicalDevice)->features2, physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice,
                                                         VkPhysicalDeviceFeatures2* pFeatures) {
    InstanceData* data = physicalInstance(physicalDevice);
    features2(data->features2KHR != nullptr ? data->features2KHR : data->features2, physicalDevice, pFeatures);
}

// ---------------------------------------------------------------------------------------------
// Device level: advance the link, record the create info; nothing else is intercepted.

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
                                            const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    InstanceData* instance = physicalInstance(physicalDevice);
    auto* chain = const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(pCreateInfo->pNext));
    while (chain != nullptr &&
           !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO)) {
        chain = const_cast<VkLayerDeviceCreateInfo*>(static_cast<const VkLayerDeviceCreateInfo*>(chain->pNext));
    }
    if (instance == nullptr || chain == nullptr || chain->u.pLayerInfo == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const PFN_vkGetInstanceProcAddr gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    const PFN_vkGetDeviceProcAddr gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(instance->instance, "vkCreateDevice"));
    if (createDevice == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    {
        const std::string described = describeCreateInfo(*pCreateInfo);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_lastDeviceCreate = described;
    }
    const VkResult result = createDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }
    auto data = std::make_unique<DeviceData>();
    data->gdpa = gdpa;
    data->destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(*pDevice, "vkDestroyDevice"));
    std::lock_guard<std::mutex> lock(g_mutex);
    g_devices[dispatchKey(*pDevice)] = std::move(data);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    if (device == VK_NULL_HANDLE) {
        return;
    }
    std::unique_ptr<DeviceData> data;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_devices.find(dispatchKey(device));
        if (it == g_devices.end()) {
            return;
        }
        data = std::move(it->second);
        g_devices.erase(it);
    }
    data->destroyDevice(device, pAllocator);
}

// ---------------------------------------------------------------------------------------------
// Proc address lookup

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* pName) {
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceProcAddr);
    }
    if (std::strcmp(pName, "vkDestroyDevice") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&DestroyDevice);
    }
    DeviceData* data = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_devices.find(dispatchKey(device));
        data = it != g_devices.end() ? it->second.get() : nullptr;
    }
    return data != nullptr ? data->gdpa(device, pName) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* pName) {
    struct Entry {
        const char* name;
        PFN_vkVoidFunction fn;
    };
#define FUSE_ENTRY(name, fn) {name, reinterpret_cast<PFN_vkVoidFunction>(&fn)}
    static const Entry kEntries[] = {
        FUSE_ENTRY("vkGetInstanceProcAddr", GetInstanceProcAddr),
        FUSE_ENTRY("vkCreateInstance", CreateInstance),
        FUSE_ENTRY("vkDestroyInstance", DestroyInstance),
        FUSE_ENTRY("vkCreateDevice", CreateDevice),
        FUSE_ENTRY("vkGetDeviceProcAddr", GetDeviceProcAddr),
        FUSE_ENTRY("vkDestroyDevice", DestroyDevice),
        FUSE_ENTRY("vkEnumerateDeviceExtensionProperties", EnumerateDeviceExtensionProperties),
        FUSE_ENTRY("vkGetPhysicalDeviceProperties", GetPhysicalDeviceProperties),
        FUSE_ENTRY("vkGetPhysicalDeviceProperties2", GetPhysicalDeviceProperties2),
        FUSE_ENTRY("vkGetPhysicalDeviceProperties2KHR", GetPhysicalDeviceProperties2KHR),
        FUSE_ENTRY("vkGetPhysicalDeviceFeatures", GetPhysicalDeviceFeatures),
        FUSE_ENTRY("vkGetPhysicalDeviceFeatures2", GetPhysicalDeviceFeatures2),
        FUSE_ENTRY("vkGetPhysicalDeviceFeatures2KHR", GetPhysicalDeviceFeatures2KHR),
    };
#undef FUSE_ENTRY
    PFN_vkVoidFunction own = nullptr;
    for (const Entry& entry : kEntries) {
        if (std::strcmp(entry.name, pName) == 0) {
            own = entry.fn;
            break;
        }
    }
    if (instance == VK_NULL_HANDLE) {
        return own;
    }
    InstanceData* data = instanceData(instance);
    if (data == nullptr) {
        return own;
    }
    const PFN_vkVoidFunction next = data->gipa(instance, pName);
    if (next == nullptr && std::strcmp(pName, "vkGetInstanceProcAddr") != 0) {
        return nullptr; // not supported below (e.g. a KHR alias): do not advertise an intercept
    }
    return own != nullptr ? own : next;
}

} // namespace

FUSE_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return GetInstanceProcAddr(instance, pName);
}

FUSE_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return GetDeviceProcAddr(device, pName);
}

FUSE_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (pVersionStruct == nullptr || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion >= 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
        pVersionStruct->pfnGetInstanceProcAddr = &GetInstanceProcAddr;
        pVersionStruct->pfnGetDeviceProcAddr = &GetDeviceProcAddr;
        pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    }
    return VK_SUCCESS;
}

/// Test hook: "ext=<a>,<b>;feat=<x>,<y>" of the most recent vkCreateDevice through this layer
/// (empty before the first). The pointer stays valid until the next vkCreateDevice.
FUSE_LAYER_EXPORT const char* fuseMaskFeaturesLastDeviceCreate() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_lastDeviceCreate.c_str();
}
