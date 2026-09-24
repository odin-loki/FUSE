// FUSE Relight RL-1.1: plain-Vulkan bootstrap of the FUSE-owned VkInstance / VkDevice that the
// vendored DXVK imports (plan §2.2 AD-2). Stand-in for the FUSE renderer's device creation until
// RL-0.7 builds fuse_renderer for MinGW; the hand-over points (the two dxvk:: functions at the end)
// stay the same when the renderer takes over.
//
// Called from the FUSE-DXVK patches RL-1.1-01 (DxvkInstance constructor) and RL-1.1-02
// (DxvkAdapter::createDevice). Both return false when Relight is off (FUSE_RELIGHT=0) or
// relight.device.import is false, and DXVK then creates its own objects exactly as upstream.
//
// What the device enables: every extension DXVK 3.1.1 knows (DxvkDeviceExtensionInfo) plus the
// ray-tracing set FUSE's renderer tiers use, when the device supports it, and every feature the
// device reports for the core 1.0-1.3 structs and those extensions' feature structs. DXVK filters
// an imported device's features exactly as it filters its own (DxvkDeviceCapabilities), so with a
// superset enabled it selects the same code paths as without import: the passthrough goldens stay
// bit-identical. The instance uses DXVK's engine name and D3D9 client-API flag in VkApplicationInfo,
// which drivers key D3D9-specific behaviour on.
#include <fuse/relight/tap/tap_config.hpp>
#include <fuse/relight/tap/vk_bootstrap.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace fuse::relight::tap::vkboot {

namespace {

/// A zero-initialised Vulkan struct with its sType set.
template <typename T>
T vkStruct(VkStructureType sType) {
    T t{};
    t.sType = sType;
    return t;
}

struct FeatureStruct {
    const char* extension; ///< nullptr for core structs
    VkStructureType sType;
    std::size_t size;
};

#define FUSE_FEAT(ext, stype, type) FeatureStruct{ext, stype, sizeof(type)}

// Feature structs chained for enabled extensions (DXVK 3.1.1's EXTENSIONS_WITH_FEATURES + FUSE RT).
const FeatureStruct kExtensionFeatures[] = {
    FUSE_FEAT(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT,
              VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT),
    FUSE_FEAT(VK_EXT_BORDER_COLOR_SWIZZLE_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT,
              VkPhysicalDeviceBorderColorSwizzleFeaturesEXT),
    FUSE_FEAT(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
              VkPhysicalDeviceCustomBorderColorFeaturesEXT),
    FUSE_FEAT(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
              VkPhysicalDeviceDepthClipEnableFeaturesEXT),
    FUSE_FEAT(VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT,
              VkPhysicalDeviceDepthBiasControlFeaturesEXT),
    FUSE_FEAT(VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,
              VkPhysicalDeviceDescriptorBufferFeaturesEXT),
    FUSE_FEAT(VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,
              VkPhysicalDeviceDescriptorHeapFeaturesEXT),
    FUSE_FEAT(VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT,
              VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT),
    FUSE_FEAT(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,
              VkPhysicalDeviceExtendedDynamicState3FeaturesEXT),
    FUSE_FEAT(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
              VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT),
    FUSE_FEAT(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
              VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT),
    FUSE_FEAT(VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_EXT,
              VkPhysicalDeviceLineRasterizationFeaturesEXT),
    FUSE_FEAT(VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT,
              VkPhysicalDeviceMemoryPriorityFeaturesEXT),
    FUSE_FEAT(VK_EXT_MULTI_DRAW_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT,
              VkPhysicalDeviceMultiDrawFeaturesEXT),
    FUSE_FEAT(VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT,
              VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT),
    FUSE_FEAT(VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT,
              VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT),
    FUSE_FEAT(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
              VkPhysicalDeviceRobustness2FeaturesEXT),
    FUSE_FEAT(VK_EXT_SHADER_MODULE_IDENTIFIER_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT,
              VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT),
    FUSE_FEAT(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
              VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT),
    FUSE_FEAT(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
              VkPhysicalDeviceTransformFeedbackFeaturesEXT),
    FUSE_FEAT(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
              VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT),
    FUSE_FEAT(VK_KHR_DEVICE_FAULT_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_KHR,
              VkPhysicalDeviceFaultFeaturesKHR),
    FUSE_FEAT(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR,
              VkPhysicalDeviceDynamicRenderingLocalReadFeatures),
    FUSE_FEAT(VK_KHR_MAINTENANCE_5_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR,
              VkPhysicalDeviceMaintenance5FeaturesKHR),
    FUSE_FEAT(VK_KHR_MAINTENANCE_6_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR,
              VkPhysicalDeviceMaintenance6FeaturesKHR),
    FUSE_FEAT(VK_KHR_MAINTENANCE_7_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR,
              VkPhysicalDeviceMaintenance7FeaturesKHR),
    FUSE_FEAT(VK_KHR_MAINTENANCE_8_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR,
              VkPhysicalDeviceMaintenance8FeaturesKHR),
    FUSE_FEAT(VK_KHR_MAINTENANCE_9_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_FEATURES_KHR,
              VkPhysicalDeviceMaintenance9FeaturesKHR),
    FUSE_FEAT(VK_KHR_MAINTENANCE_10_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_10_FEATURES_KHR,
              VkPhysicalDeviceMaintenance10FeaturesKHR),
    FUSE_FEAT(VK_KHR_MAINTENANCE_11_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_11_FEATURES_KHR,
              VkPhysicalDeviceMaintenance11FeaturesKHR),
    FUSE_FEAT(VK_KHR_PRESENT_ID_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
              VkPhysicalDevicePresentIdFeaturesKHR),
    FUSE_FEAT(VK_KHR_PRESENT_ID_2_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR,
              VkPhysicalDevicePresentId2FeaturesKHR),
    FUSE_FEAT(VK_KHR_PRESENT_WAIT_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
              VkPhysicalDevicePresentWaitFeaturesKHR),
    FUSE_FEAT(VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR,
              VkPhysicalDevicePresentWait2FeaturesKHR),
    FUSE_FEAT(VK_KHR_SHADER_FLOAT_CONTROLS_2_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES_KHR,
              VkPhysicalDeviceShaderFloatControls2FeaturesKHR),
    FUSE_FEAT(VK_KHR_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR,
              VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR),
    FUSE_FEAT(VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR,
              VkPhysicalDeviceShaderUntypedPointersFeaturesKHR),
    FUSE_FEAT(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,
              VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR),
    FUSE_FEAT(VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR,
              VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR),
    FUSE_FEAT(VK_NV_RAW_ACCESS_CHAINS_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAW_ACCESS_CHAINS_FEATURES_NV,
              VkPhysicalDeviceRawAccessChainsFeaturesNV),
    // FUSE renderer (T2/T3 ray tracing).
    FUSE_FEAT(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
              VkPhysicalDeviceAccelerationStructureFeaturesKHR),
    FUSE_FEAT(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
              VkPhysicalDeviceRayTracingPipelineFeaturesKHR),
    FUSE_FEAT(VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
              VkPhysicalDeviceRayQueryFeaturesKHR),
    FUSE_FEAT(VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME,
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR,
              VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR),
};

#undef FUSE_FEAT

// Extensions without a feature struct (DXVK 3.1.1's list), plus memory budget, which DXVK uses
// passively on its own devices and only sees on an imported device when it is enabled.
const char* const kPlainExtensions[] = {
    VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME,
    VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME,
    VK_EXT_HDR_METADATA_EXTENSION_NAME,
    VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
    VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME,
    VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
    VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME,
    VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME,
    VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME,
    VK_NV_LOW_LATENCY_2_EXTENSION_NAME,
    VK_NVX_BINARY_IMPORT_EXTENSION_NAME,
    VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, // dependency of acceleration structures
};

// Extensions that depend on another extension of the list (enabled only together).
struct ExtensionDependency {
    const char* extension;
    const char* requires_;
};
const ExtensionDependency kDependencies[] = {
    {VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME},
    {VK_KHR_PRESENT_WAIT_EXTENSION_NAME, VK_KHR_PRESENT_ID_EXTENSION_NAME},
    {VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME, VK_KHR_PRESENT_ID_2_EXTENSION_NAME},
    {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME},
    {VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME},
    {VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME},
    {VK_KHR_RAY_TRACING_POSITION_FETCH_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME},
};

struct Device {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    std::vector<std::string> extensionStorage;
    std::vector<const char*> extensionNames;
    std::vector<std::unique_ptr<std::uint8_t[]>> featureStorage;
    VkPhysicalDeviceFeatures2 features = vkStruct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    VkPhysicalDeviceVulkan11Features vk11 = vkStruct<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    VkPhysicalDeviceVulkan12Features vk12 = vkStruct<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    VkPhysicalDeviceVulkan13Features vk13 = vkStruct<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
};

struct State {
    std::mutex mutex;
    bool instanceTried = false;
    HMODULE library = nullptr;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    std::vector<std::string> instanceExtensionStorage;
    std::vector<const char*> instanceExtensionNames;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    bool validationLayer = false;
    std::vector<std::unique_ptr<Device>> devices;
    std::recursive_mutex queueMutex;
    std::atomic<std::uint32_t> errors{0};
    std::atomic<std::uint32_t> warnings{0};
};

State& state() {
    static State* s = new State(); // never destroyed: DXVK may call in during process teardown
    return *s;
}

template <typename T>
T instanceProc(State& s, VkInstance instance, const char* name) {
    return reinterpret_cast<T>(s.getInstanceProcAddr(instance, name));
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT types,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    State& s = state();
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    const bool warning = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0;
    if (!(types & (VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT))) {
        return VK_FALSE;
    }
    if (error) {
        s.errors.fetch_add(1);
    } else if (warning) {
        s.warnings.fetch_add(1);
    } else {
        return VK_FALSE;
    }
    std::fprintf(stderr, "fuse-relight vk %s: %s\n", error ? "error" : "warning",
                 data && data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

bool loadLoader(State& s) {
    // Same order as DXVK's vk::LibraryFn (winevulkan first under Wine, then the Khronos loader).
    for (const char* name : {"winevulkan.dll", "vulkan-1.dll"}) {
        HMODULE lib = LoadLibraryA(name);
        if (!lib) {
            continue;
        }
        auto proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
        if (proc) {
            s.library = lib;
            s.getInstanceProcAddr = proc;
            return true;
        }
        FreeLibrary(lib);
    }
    return false;
}

bool createInstance(State& s, std::uint32_t clientFlags, bool validation) {
    if (!loadLoader(s)) {
        std::fprintf(stderr, "fuse-relight: no Vulkan loader; DXVK creates its own instance\n");
        return false;
    }
    auto enumExts = instanceProc<PFN_vkEnumerateInstanceExtensionProperties>(s, VK_NULL_HANDLE,
                                                                             "vkEnumerateInstanceExtensionProperties");
    auto enumLayers =
        instanceProc<PFN_vkEnumerateInstanceLayerProperties>(s, VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    auto create = instanceProc<PFN_vkCreateInstance>(s, VK_NULL_HANDLE, "vkCreateInstance");
    if (!enumExts || !create) {
        return false;
    }
    std::uint32_t count = 0;
    enumExts(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    enumExts(nullptr, &count, available.data());
    auto has = [&](const char* name) {
        return std::any_of(available.begin(), available.end(),
                           [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    };
    std::vector<const char*> wanted = {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_win32_surface",
                                       VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
    // DXVK enables one of the two surface-maintenance extensions (KHR preferred).
    wanted.push_back(has(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME) ? VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME
                                                                      : VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    if (validation) {
        wanted.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    for (const char* name : wanted) {
        if (has(name)) {
            s.instanceExtensionStorage.emplace_back(name);
        }
    }
    for (const std::string& name : s.instanceExtensionStorage) {
        s.instanceExtensionNames.push_back(name.c_str());
    }

    std::vector<const char*> layers;
    if (validation && enumLayers) {
        std::uint32_t layerCount = 0;
        enumLayers(&layerCount, nullptr);
        std::vector<VkLayerProperties> layerProps(layerCount);
        enumLayers(&layerCount, layerProps.data());
        for (const VkLayerProperties& l : layerProps) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                s.validationLayer = true;
            }
        }
        if (!s.validationLayer) {
            // winevulkan exposes no layers to the PE side; the host loader can still inject the
            // layer (VK_INSTANCE_LAYERS in the Unix environment), and its messages arrive through
            // the debug-utils messenger below.
            std::fprintf(stderr, "fuse-relight: VK_LAYER_KHRONOS_validation not offered by the loader\n");
        }
    }

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    const char* exeName = std::strrchr(exePath, '\\');
    exeName = exeName ? exeName + 1 : exePath;

    VkApplicationInfo app = vkStruct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = exeName;
    app.applicationVersion = clientFlags; // DXVK: DxvkInstanceFlags (ClientApiIsD3D9)
    app.pEngineName = "DXVK";             // drivers key D3D9 behaviour on DXVK's engine name
    app.engineVersion = VK_MAKE_API_VERSION(0, 3, 1, 1);
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo info = vkStruct<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    info.pApplicationInfo = &app;
    info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<std::uint32_t>(s.instanceExtensionNames.size());
    info.ppEnabledExtensionNames = s.instanceExtensionNames.data();
    const VkResult vr = create(&info, nullptr, &s.instance);
    if (vr != VK_SUCCESS) {
        std::fprintf(stderr, "fuse-relight: vkCreateInstance failed (%d); DXVK creates its own instance\n", int(vr));
        s.instance = VK_NULL_HANDLE;
        return false;
    }

    if (validation && has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        auto createMessenger =
            instanceProc<PFN_vkCreateDebugUtilsMessengerEXT>(s, s.instance, "vkCreateDebugUtilsMessengerEXT");
        if (createMessenger) {
            VkDebugUtilsMessengerCreateInfoEXT m = vkStruct<VkDebugUtilsMessengerCreateInfoEXT>(VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
            m.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            m.pfnUserCallback = &debugCallback;
            createMessenger(s.instance, &m, nullptr, &s.messenger);
        }
    }
    return true;
}

Device* createDevice(State& s, VkPhysicalDevice pd) {
    auto enumDevExts =
        instanceProc<PFN_vkEnumerateDeviceExtensionProperties>(s, s.instance, "vkEnumerateDeviceExtensionProperties");
    auto getQueueProps = instanceProc<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        s, s.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto getFeatures2 = instanceProc<PFN_vkGetPhysicalDeviceFeatures2>(s, s.instance, "vkGetPhysicalDeviceFeatures2");
    auto getProps = instanceProc<PFN_vkGetPhysicalDeviceProperties>(s, s.instance, "vkGetPhysicalDeviceProperties");
    auto create = instanceProc<PFN_vkCreateDevice>(s, s.instance, "vkCreateDevice");
    auto getQueue = instanceProc<PFN_vkGetDeviceQueue>(s, s.instance, "vkGetDeviceQueue");
    if (!enumDevExts || !getQueueProps || !getFeatures2 || !getProps || !create || !getQueue) {
        return nullptr;
    }
    VkPhysicalDeviceProperties props{};
    getProps(pd, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) {
        return nullptr; // DXVK rejects the adapter anyway
    }

    auto dev = std::make_unique<Device>();
    dev->physicalDevice = pd;

    // Queue: the first family with graphics + compute (DXVK's graphics queue).
    std::uint32_t familyCount = 0;
    getQueueProps(pd, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    getQueueProps(pd, &familyCount, families.data());
    bool found = false;
    for (std::uint32_t f = 0; f < familyCount; ++f) {
        if ((families[f].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            dev->queueFamily = f;
            found = true;
            break;
        }
    }
    if (!found) {
        return nullptr;
    }

    // Extensions.
    std::uint32_t extCount = 0;
    enumDevExts(pd, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> available(extCount);
    enumDevExts(pd, nullptr, &extCount, available.data());
    std::set<std::string> supported;
    for (const VkExtensionProperties& e : available) {
        supported.insert(e.extensionName);
    }
    std::set<std::string> enable;
    for (const FeatureStruct& f : kExtensionFeatures) {
        if (supported.count(f.extension)) {
            enable.insert(f.extension);
        }
    }
    for (const char* e : kPlainExtensions) {
        if (supported.count(e)) {
            enable.insert(e);
        }
    }
    // LOAD_OP_NONE: KHR, or its EXT alias (FUSE-DXVK RL-0.2-01 accepts either).
    if (supported.count(VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME)) {
        enable.insert(VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME);
    } else if (supported.count(VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME)) {
        enable.insert(VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME);
    }
    // Drop extensions whose dependency is missing (repeat: dependencies chain).
    for (bool changed = true; changed;) {
        changed = false;
        for (const ExtensionDependency& d : kDependencies) {
            if (enable.count(d.extension) && !enable.count(d.requires_)) {
                enable.erase(d.extension);
                changed = true;
            }
        }
    }
    dev->extensionStorage.assign(enable.begin(), enable.end());
    for (const std::string& e : dev->extensionStorage) {
        dev->extensionNames.push_back(e.c_str());
    }

    // Features: core 1.1-1.3 plus one struct per enabled extension (an sType aliased by two
    // extensions, e.g. swapchain maintenance EXT/KHR, is chained once).
    dev->features.pNext = &dev->vk11;
    dev->vk11.pNext = &dev->vk12;
    dev->vk12.pNext = &dev->vk13;
    void** tail = &dev->vk13.pNext;
    std::set<VkStructureType> chained;
    for (const FeatureStruct& f : kExtensionFeatures) {
        if (!enable.count(f.extension) || chained.count(f.sType)) {
            continue;
        }
        chained.insert(f.sType);
        auto storage = std::make_unique<std::uint8_t[]>(f.size);
        std::memset(storage.get(), 0, f.size);
        auto* base = reinterpret_cast<VkBaseOutStructure*>(storage.get());
        base->sType = f.sType;
        *tail = base;
        tail = reinterpret_cast<void**>(&base->pNext);
        dev->featureStorage.push_back(std::move(storage));
    }
    getFeatures2(pd, &dev->features); // everything supported is enabled

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = vkStruct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    queue.queueFamilyIndex = dev->queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info = vkStruct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    info.pNext = &dev->features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<std::uint32_t>(dev->extensionNames.size());
    info.ppEnabledExtensionNames = dev->extensionNames.data();
    const VkResult vr = create(pd, &info, nullptr, &dev->device);
    if (vr != VK_SUCCESS) {
        std::fprintf(stderr, "fuse-relight: vkCreateDevice failed (%d); DXVK creates its own device\n", int(vr));
        return nullptr;
    }
    getQueue(dev->device, dev->queueFamily, 0, &dev->queue);
    s.devices.push_back(std::move(dev));
    return s.devices.back().get();
}

} // namespace

bool isImportedDevice(std::uint64_t device) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (const auto& d : s.devices) {
        if (reinterpret_cast<std::uint64_t>(d->device) == device) {
            return true;
        }
    }
    return false;
}

Stats stats() {
    State& s = state();
    Stats out;
    out.validationErrors = s.errors.load();
    out.validationWarnings = s.warnings.load();
    out.validationLayer = s.validationLayer;
    out.debugMessenger = s.messenger != VK_NULL_HANDLE;
    return out;
}

} // namespace fuse::relight::tap::vkboot

// ---- hand-over points called from the FUSE-DXVK patches (declared there at block scope) -----------
namespace dxvk { // fuse-lint-allow(namespace): hand-over points the FUSE-DXVK patches declare in dxvk::

bool fuseRelightImportInstance(PFN_vkGetInstanceProcAddr* loader, VkInstance* instance, std::uint32_t* extensionCount,
                               const char*** extensionNames, std::uint32_t clientFlags) {
    using namespace fuse::relight::tap;
    const RuntimeConfig& config = runtimeConfig();
    if (!config.relightEnabled || !config.importDevice) {
        return false;
    }
    vkboot::State& s = vkboot::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.instanceTried) {
        s.instanceTried = true;
        vkboot::createInstance(s, clientFlags, config.vkValidation);
    }
    if (s.instance == VK_NULL_HANDLE) {
        return false;
    }
    *loader = s.getInstanceProcAddr;
    *instance = s.instance;
    *extensionCount = static_cast<std::uint32_t>(s.instanceExtensionNames.size());
    *extensionNames = s.instanceExtensionNames.data();
    return true;
}

bool fuseRelightImportDevice(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice* device, VkQueue* queue,
                             std::uint32_t* queueFamily, std::uint32_t* extensionCount, const char*** extensionNames,
                             const VkPhysicalDeviceFeatures2** features) {
    using namespace fuse::relight::tap;
    vkboot::State& s = vkboot::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.instance == VK_NULL_HANDLE || instance != s.instance) {
        return false; // DXVK runs on its own instance: nothing to import
    }
    vkboot::Device* dev = nullptr;
    for (const auto& d : s.devices) {
        if (d->physicalDevice == physicalDevice) {
            dev = d.get(); // one VkDevice per adapter, shared by every D3D9 device on it
        }
    }
    if (!dev) {
        dev = vkboot::createDevice(s, physicalDevice);
    }
    if (!dev) {
        return false;
    }
    *device = dev->device;
    *queue = dev->queue;
    *queueFamily = dev->queueFamily;
    *extensionCount = static_cast<std::uint32_t>(dev->extensionNames.size());
    *extensionNames = dev->extensionNames.data();
    *features = &dev->features;
    return true;
}

void fuseRelightQueueLock(bool lock) {
    // FUSE's submission lock (plan §2.2): DXVK takes it around every use of the shared queue.
    std::recursive_mutex& m = fuse::relight::tap::vkboot::state().queueMutex;
    if (lock) {
        m.lock();
    } else {
        m.unlock();
    }
}

} // namespace dxvk
