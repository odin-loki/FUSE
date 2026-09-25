// Test-only Vulkan layer VK_LAYER_FUSE_spoof_multi_gpu.
//
// Physical device selection ("prefer discrete over integrated", tie-breaks, rejection of devices
// missing a required feature) cannot be exercised on a CI machine with one Lavapipe device. This
// layer rewrites what the application (and any layer above it, e.g. VK_LAYER_KHRONOS_validation)
// sees of the physical devices below it, driven by the environment variable FUSE_MULTI_GPU_SPEC,
// read at vkCreateInstance:
//
//   FUSE_MULTI_GPU_SPEC = entry[;entry...]      one entry per presented device, in presented order
//   entry               = type[,option...]
//   type                = discrete | integrated | virtual | cpu | other
//   option              = src=<n>        real device (enumeration order below this layer), default = entry index
//                       | name=<text>    deviceName (default "FUSE spoof <i> <type>")
//                       | vram=<MiB>     size of every DEVICE_LOCAL heap (and its budget)
//                       | dim2d=<n>      limits.maxImageDimension2D
//                       | api=<maj.min>  apiVersion (patch kept)
//                       | notimeline     timelineSemaphore reported VK_FALSE
//                       | nographics     VK_QUEUE_GRAPHICS_BIT stripped from every queue family
//
// vkEnumeratePhysicalDevices / vkEnumeratePhysicalDeviceGroups return exactly the spec's devices
// in spec order (a real device not named by any entry is hidden). Handles are the real ones from
// below (no wrapping), so every entry point the layer does not intercept stays valid; the several
// real devices come from registering the Lavapipe ICD more than once (see the test). Spoofed
// values must only ever narrow what the real device supports (the device is really created on the
// ICD). An unset / empty spec makes the layer a pass-through.
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_MSC_VER)
// vulkan.h already declared these entry points without dllexport (redeclaring with it is
// ill-formed for cl / clang-cl); vk_followups.cmake exports them with /EXPORT instead.
#define FUSE_LAYER_EXPORT extern "C"
#elif defined(_WIN32)
#define FUSE_LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define FUSE_LAYER_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

struct SpoofEntry {
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    uint32_t src = 0;
    std::string name;
    uint64_t vramMiB = 0; ///< 0 = unchanged
    uint32_t dim2d = 0;   ///< 0 = unchanged
    uint32_t apiMajor = 0;
    uint32_t apiMinor = 0;
    bool apiOverride = false;
    bool noTimeline = false;
    bool noGraphics = false;
};

bool parseType(const std::string& text, VkPhysicalDeviceType& out) {
    if (text == "discrete") {
        out = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    } else if (text == "integrated") {
        out = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    } else if (text == "virtual") {
        out = VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU;
    } else if (text == "cpu") {
        out = VK_PHYSICAL_DEVICE_TYPE_CPU;
    } else if (text == "other") {
        out = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    } else {
        return false;
    }
    return true;
}

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

/// Parses FUSE_MULTI_GPU_SPEC; returns false (and logs) on a malformed spec.
bool parseSpec(const char* text, std::vector<SpoofEntry>& out) {
    out.clear();
    if (text == nullptr || text[0] == '\0') {
        return true;
    }
    uint32_t index = 0;
    for (const std::string& entryText : split(text, ';')) {
        if (entryText.empty()) {
            continue;
        }
        const std::vector<std::string> fields = split(entryText, ',');
        SpoofEntry entry{};
        entry.src = index;
        if (!parseType(fields[0], entry.type)) {
            std::fprintf(stderr, "VK_LAYER_FUSE_spoof_multi_gpu: bad device type '%s'\n", fields[0].c_str());
            return false;
        }
        for (size_t i = 1; i < fields.size(); ++i) {
            const std::string& field = fields[i];
            const size_t eq = field.find('=');
            const std::string key = field.substr(0, eq);
            const std::string value = eq == std::string::npos ? std::string() : field.substr(eq + 1);
            if (key == "src") {
                entry.src = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
            } else if (key == "name") {
                entry.name = value;
            } else if (key == "vram") {
                entry.vramMiB = std::strtoull(value.c_str(), nullptr, 10);
            } else if (key == "dim2d") {
                entry.dim2d = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
            } else if (key == "api") {
                entry.apiOverride = std::sscanf(value.c_str(), "%u.%u", &entry.apiMajor, &entry.apiMinor) == 2;
                if (!entry.apiOverride) {
                    std::fprintf(stderr, "VK_LAYER_FUSE_spoof_multi_gpu: bad api '%s'\n", value.c_str());
                    return false;
                }
            } else if (key == "notimeline") {
                entry.noTimeline = true;
            } else if (key == "nographics") {
                entry.noGraphics = true;
            } else {
                std::fprintf(stderr, "VK_LAYER_FUSE_spoof_multi_gpu: unknown option '%s'\n", field.c_str());
                return false;
            }
        }
        if (entry.name.empty()) {
            static const char* const kTypeNames[] = {"other", "integrated", "discrete", "virtual", "cpu"};
            entry.name = "FUSE spoof " + std::to_string(index) + " " + kTypeNames[entry.type];
        }
        out.push_back(entry);
        ++index;
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
    PFN_vkEnumeratePhysicalDeviceGroups enumeratePhysicalDeviceGroups = nullptr;
    PFN_vkEnumeratePhysicalDeviceGroups enumeratePhysicalDeviceGroupsKHR = nullptr;
    PFN_vkGetPhysicalDeviceProperties properties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 properties2 = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 properties2KHR = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 features2 = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 features2KHR = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties memoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties2 memoryProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties2 memoryProperties2KHR = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queueFamilyProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 queueFamilyProperties2 = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 queueFamilyProperties2KHR = nullptr;
    std::vector<SpoofEntry> spec;
    /// Real devices in presented order (filled on the first enumeration).
    std::vector<VkPhysicalDevice> presented;
    bool enumerated = false;
    bool specValid = true;
};

struct PhysicalData {
    InstanceData* instance = nullptr;
    const SpoofEntry* entry = nullptr; ///< null: pass-through
};

struct DeviceData {
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
};

std::mutex g_mutex;
std::unordered_map<DispatchKey, std::unique_ptr<InstanceData>> g_instances;
std::unordered_map<VkPhysicalDevice, PhysicalData> g_physical;
std::unordered_map<DispatchKey, std::unique_ptr<DeviceData>> g_devices;

InstanceData* instanceData(VkInstance instance) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_instances.find(dispatchKey(instance));
    return it != g_instances.end() ? it->second.get() : nullptr;
}

PhysicalData physicalData(VkPhysicalDevice physicalDevice) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_physical.find(physicalDevice);
    if (it != g_physical.end()) {
        return it->second;
    }
    // Not seen through an enumeration of ours: pass-through against the only instance.
    PhysicalData data{};
    data.instance = g_instances.size() == 1 ? g_instances.begin()->second.get() : nullptr;
    return data;
}

DeviceData* deviceData(const void* handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(dispatchKey(handle));
    return it != g_devices.end() ? it->second.get() : nullptr;
}

/// Enumerates the real devices once and builds the presented list from the spec.
VkResult ensurePresented(InstanceData& data) {
    if (data.enumerated) {
        return VK_SUCCESS;
    }
    uint32_t count = 0;
    VkResult result = data.enumeratePhysicalDevices(data.instance, &count, nullptr);
    if (result != VK_SUCCESS) {
        return result;
    }
    std::vector<VkPhysicalDevice> real(count);
    result = data.enumeratePhysicalDevices(data.instance, &count, real.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        return result;
    }
    real.resize(count);
    std::lock_guard<std::mutex> lock(g_mutex);
    data.presented.clear();
    if (data.spec.empty() || !data.specValid) {
        data.presented = real;
        for (VkPhysicalDevice device : real) {
            g_physical[device] = PhysicalData{&data, nullptr};
        }
    } else {
        for (const SpoofEntry& entry : data.spec) {
            if (entry.src >= real.size()) {
                std::fprintf(stderr, "VK_LAYER_FUSE_spoof_multi_gpu: src=%u but only %u real device(s)\n", entry.src,
                             static_cast<unsigned>(real.size()));
                continue;
            }
            const VkPhysicalDevice device = real[entry.src];
            if (std::find(data.presented.begin(), data.presented.end(), device) != data.presented.end()) {
                std::fprintf(stderr, "VK_LAYER_FUSE_spoof_multi_gpu: real device %u named twice\n", entry.src);
                continue;
            }
            data.presented.push_back(device);
            g_physical[device] = PhysicalData{&data, &entry};
        }
    }
    data.enumerated = true;
    return VK_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// Spoofing

void spoofProperties(const SpoofEntry& entry, VkPhysicalDeviceProperties& props) {
    props.deviceType = entry.type;
    std::snprintf(props.deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE, "%s", entry.name.c_str());
    if (entry.dim2d != 0) {
        props.limits.maxImageDimension2D = std::min(props.limits.maxImageDimension2D, entry.dim2d);
    }
    if (entry.apiOverride) {
        props.apiVersion = VK_MAKE_API_VERSION(0, entry.apiMajor, entry.apiMinor, VK_API_VERSION_PATCH(props.apiVersion));
    }
}

void spoofFeatureChain(const SpoofEntry& entry, void* pNext) {
    for (auto* node = static_cast<VkBaseOutStructure*>(pNext); node != nullptr; node = node->pNext) {
        if (!entry.noTimeline) {
            continue;
        }
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(node)->timelineSemaphore = VK_FALSE;
        } else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(node)->timelineSemaphore = VK_FALSE;
        }
    }
}

void spoofMemory(const SpoofEntry& entry, VkPhysicalDeviceMemoryProperties& memory, void* pNext) {
    if (entry.vramMiB == 0) {
        return;
    }
    const VkDeviceSize size = static_cast<VkDeviceSize>(entry.vramMiB) << 20;
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            memory.memoryHeaps[i].size = std::min(memory.memoryHeaps[i].size, size);
        }
    }
    for (auto* node = static_cast<VkBaseOutStructure*>(pNext); node != nullptr; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) {
            auto* budget = reinterpret_cast<VkPhysicalDeviceMemoryBudgetPropertiesEXT*>(node);
            for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
                budget->heapBudget[i] = std::min(budget->heapBudget[i], memory.memoryHeaps[i].size);
            }
        }
    }
}

void spoofQueueFamily(const SpoofEntry& entry, VkQueueFamilyProperties& family) {
    if (entry.noGraphics) {
        family.queueFlags &= ~static_cast<VkQueueFlags>(VK_QUEUE_GRAPHICS_BIT);
    }
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
    const VkResult result = createInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        return result;
    }
    auto data = std::make_unique<InstanceData>();
    data->instance = *pInstance;
    data->gipa = gipa;
    data->specValid = parseSpec(std::getenv("FUSE_MULTI_GPU_SPEC"), data->spec);
#define FUSE_LOAD(member, type, name) data->member = reinterpret_cast<type>(gipa(*pInstance, name))
    FUSE_LOAD(destroyInstance, PFN_vkDestroyInstance, "vkDestroyInstance");
    FUSE_LOAD(enumeratePhysicalDevices, PFN_vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    FUSE_LOAD(enumeratePhysicalDeviceGroups, PFN_vkEnumeratePhysicalDeviceGroups, "vkEnumeratePhysicalDeviceGroups");
    FUSE_LOAD(enumeratePhysicalDeviceGroupsKHR, PFN_vkEnumeratePhysicalDeviceGroups, "vkEnumeratePhysicalDeviceGroupsKHR");
    FUSE_LOAD(properties, PFN_vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    FUSE_LOAD(properties2, PFN_vkGetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2");
    FUSE_LOAD(properties2KHR, PFN_vkGetPhysicalDeviceProperties2, "vkGetPhysicalDeviceProperties2KHR");
    FUSE_LOAD(features2, PFN_vkGetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2");
    FUSE_LOAD(features2KHR, PFN_vkGetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2KHR");
    FUSE_LOAD(memoryProperties, PFN_vkGetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    FUSE_LOAD(memoryProperties2, PFN_vkGetPhysicalDeviceMemoryProperties2, "vkGetPhysicalDeviceMemoryProperties2");
    FUSE_LOAD(memoryProperties2KHR, PFN_vkGetPhysicalDeviceMemoryProperties2, "vkGetPhysicalDeviceMemoryProperties2KHR");
    FUSE_LOAD(queueFamilyProperties, PFN_vkGetPhysicalDeviceQueueFamilyProperties,
              "vkGetPhysicalDeviceQueueFamilyProperties");
    FUSE_LOAD(queueFamilyProperties2, PFN_vkGetPhysicalDeviceQueueFamilyProperties2,
              "vkGetPhysicalDeviceQueueFamilyProperties2");
    FUSE_LOAD(queueFamilyProperties2KHR, PFN_vkGetPhysicalDeviceQueueFamilyProperties2,
              "vkGetPhysicalDeviceQueueFamilyProperties2KHR");
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
            physical = physical->second.instance == data ? g_physical.erase(physical) : std::next(physical);
        }
        g_instances.erase(it);
    }
    destroy(instance, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance, uint32_t* pCount,
                                                        VkPhysicalDevice* pDevices) {
    InstanceData* data = instanceData(instance);
    if (data == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = ensurePresented(*data);
    if (result != VK_SUCCESS) {
        return result;
    }
    const uint32_t available = static_cast<uint32_t>(data->presented.size());
    if (pDevices == nullptr) {
        *pCount = available;
        return VK_SUCCESS;
    }
    const uint32_t written = std::min(*pCount, available);
    for (uint32_t i = 0; i < written; ++i) {
        pDevices[i] = data->presented[i];
    }
    *pCount = written;
    return written < available ? VK_INCOMPLETE : VK_SUCCESS;
}

/// One single-device group per presented device (the spoofed devices are never linked).
VkResult enumerateGroups(VkInstance instance, uint32_t* pCount, VkPhysicalDeviceGroupProperties* pGroups) {
    InstanceData* data = instanceData(instance);
    if (data == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const VkResult result = ensurePresented(*data);
    if (result != VK_SUCCESS) {
        return result;
    }
    const uint32_t available = static_cast<uint32_t>(data->presented.size());
    if (pGroups == nullptr) {
        *pCount = available;
        return VK_SUCCESS;
    }
    const uint32_t written = std::min(*pCount, available);
    for (uint32_t i = 0; i < written; ++i) {
        pGroups[i].physicalDeviceCount = 1;
        std::memset(pGroups[i].physicalDevices, 0, sizeof(pGroups[i].physicalDevices));
        pGroups[i].physicalDevices[0] = data->presented[i];
        pGroups[i].subsetAllocation = VK_FALSE;
    }
    *pCount = written;
    return written < available ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDeviceGroups(VkInstance instance, uint32_t* pCount,
                                                             VkPhysicalDeviceGroupProperties* pGroups) {
    return enumerateGroups(instance, pCount, pGroups);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                       VkPhysicalDeviceProperties* pProperties) {
    const PhysicalData data = physicalData(physicalDevice);
    data.instance->properties(physicalDevice, pProperties);
    if (data.entry != nullptr) {
        spoofProperties(*data.entry, *pProperties);
    }
}

void properties2(PFN_vkGetPhysicalDeviceProperties2 next, VkPhysicalDevice physicalDevice,
                 VkPhysicalDeviceProperties2* pProperties) {
    const PhysicalData data = physicalData(physicalDevice);
    next(physicalDevice, pProperties);
    if (data.entry != nullptr) {
        spoofProperties(*data.entry, pProperties->properties);
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                                        VkPhysicalDeviceProperties2* pProperties) {
    properties2(physicalData(physicalDevice).instance->properties2, physicalDevice, pProperties);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice,
                                                           VkPhysicalDeviceProperties2* pProperties) {
    InstanceData* instance = physicalData(physicalDevice).instance;
    properties2(instance->properties2KHR != nullptr ? instance->properties2KHR : instance->properties2, physicalDevice,
                pProperties);
}

void features2(PFN_vkGetPhysicalDeviceFeatures2 next, VkPhysicalDevice physicalDevice,
               VkPhysicalDeviceFeatures2* pFeatures) {
    const PhysicalData data = physicalData(physicalDevice);
    next(physicalDevice, pFeatures);
    if (data.entry != nullptr) {
        spoofFeatureChain(*data.entry, pFeatures->pNext);
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                                                      VkPhysicalDeviceFeatures2* pFeatures) {
    features2(physicalData(physicalDevice).instance->features2, physicalDevice, pFeatures);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice,
                                                         VkPhysicalDeviceFeatures2* pFeatures) {
    InstanceData* instance = physicalData(physicalDevice).instance;
    features2(instance->features2KHR != nullptr ? instance->features2KHR : instance->features2, physicalDevice,
              pFeatures);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                                             VkPhysicalDeviceMemoryProperties* pMemory) {
    const PhysicalData data = physicalData(physicalDevice);
    data.instance->memoryProperties(physicalDevice, pMemory);
    if (data.entry != nullptr) {
        spoofMemory(*data.entry, *pMemory, nullptr);
    }
}

void memoryProperties2(PFN_vkGetPhysicalDeviceMemoryProperties2 next, VkPhysicalDevice physicalDevice,
                       VkPhysicalDeviceMemoryProperties2* pMemory) {
    const PhysicalData data = physicalData(physicalDevice);
    next(physicalDevice, pMemory);
    if (data.entry != nullptr) {
        spoofMemory(*data.entry, pMemory->memoryProperties, pMemory->pNext);
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                                              VkPhysicalDeviceMemoryProperties2* pMemory) {
    memoryProperties2(physicalData(physicalDevice).instance->memoryProperties2, physicalDevice, pMemory);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice physicalDevice,
                                                                 VkPhysicalDeviceMemoryProperties2* pMemory) {
    InstanceData* instance = physicalData(physicalDevice).instance;
    memoryProperties2(instance->memoryProperties2KHR != nullptr ? instance->memoryProperties2KHR
                                                                : instance->memoryProperties2,
                      physicalDevice, pMemory);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t* pCount,
                                                                  VkQueueFamilyProperties* pProperties) {
    const PhysicalData data = physicalData(physicalDevice);
    data.instance->queueFamilyProperties(physicalDevice, pCount, pProperties);
    if (data.entry != nullptr && pProperties != nullptr) {
        for (uint32_t i = 0; i < *pCount; ++i) {
            spoofQueueFamily(*data.entry, pProperties[i]);
        }
    }
}

void queueFamilyProperties2(PFN_vkGetPhysicalDeviceQueueFamilyProperties2 next, VkPhysicalDevice physicalDevice,
                            uint32_t* pCount, VkQueueFamilyProperties2* pProperties) {
    const PhysicalData data = physicalData(physicalDevice);
    next(physicalDevice, pCount, pProperties);
    if (data.entry != nullptr && pProperties != nullptr) {
        for (uint32_t i = 0; i < *pCount; ++i) {
            spoofQueueFamily(*data.entry, pProperties[i].queueFamilyProperties);
        }
    }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t* pCount,
                                                                   VkQueueFamilyProperties2* pProperties) {
    queueFamilyProperties2(physicalData(physicalDevice).instance->queueFamilyProperties2, physicalDevice, pCount,
                           pProperties);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceQueueFamilyProperties2KHR(VkPhysicalDevice physicalDevice, uint32_t* pCount,
                                                                      VkQueueFamilyProperties2* pProperties) {
    InstanceData* instance = physicalData(physicalDevice).instance;
    queueFamilyProperties2(instance->queueFamilyProperties2KHR != nullptr ? instance->queueFamilyProperties2KHR
                                                                          : instance->queueFamilyProperties2,
                           physicalDevice, pCount, pProperties);
}

// ---------------------------------------------------------------------------------------------
// Device level: the layer only has to sit in the device chain (advance the link); no device
// entry point is intercepted besides destroy.

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
                                            const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    InstanceData* instance = physicalData(physicalDevice).instance;
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
    DeviceData* data = deviceData(device);
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
        FUSE_ENTRY("vkEnumeratePhysicalDevices", EnumeratePhysicalDevices),
        FUSE_ENTRY("vkEnumeratePhysicalDeviceGroups", EnumeratePhysicalDeviceGroups),
        FUSE_ENTRY("vkEnumeratePhysicalDeviceGroupsKHR", EnumeratePhysicalDeviceGroups),
        FUSE_ENTRY("vkGetPhysicalDeviceProperties", GetPhysicalDeviceProperties),
        FUSE_ENTRY("vkGetPhysicalDeviceProperties2", GetPhysicalDeviceProperties2),
        FUSE_ENTRY("vkGetPhysicalDeviceProperties2KHR", GetPhysicalDeviceProperties2KHR),
        FUSE_ENTRY("vkGetPhysicalDeviceFeatures2", GetPhysicalDeviceFeatures2),
        FUSE_ENTRY("vkGetPhysicalDeviceFeatures2KHR", GetPhysicalDeviceFeatures2KHR),
        FUSE_ENTRY("vkGetPhysicalDeviceMemoryProperties", GetPhysicalDeviceMemoryProperties),
        FUSE_ENTRY("vkGetPhysicalDeviceMemoryProperties2", GetPhysicalDeviceMemoryProperties2),
        FUSE_ENTRY("vkGetPhysicalDeviceMemoryProperties2KHR", GetPhysicalDeviceMemoryProperties2KHR),
        FUSE_ENTRY("vkGetPhysicalDeviceQueueFamilyProperties", GetPhysicalDeviceQueueFamilyProperties),
        FUSE_ENTRY("vkGetPhysicalDeviceQueueFamilyProperties2", GetPhysicalDeviceQueueFamilyProperties2),
        FUSE_ENTRY("vkGetPhysicalDeviceQueueFamilyProperties2KHR", GetPhysicalDeviceQueueFamilyProperties2KHR),
    };
#undef FUSE_ENTRY
    bool known = false;
    PFN_vkVoidFunction own = nullptr;
    for (const Entry& entry : kEntries) {
        if (std::strcmp(entry.name, pName) == 0) {
            own = entry.fn;
            known = true;
            break;
        }
    }
    if (instance == VK_NULL_HANDLE) {
        return known ? own : nullptr;
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
