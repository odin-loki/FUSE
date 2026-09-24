#include <fuse/renderer/vk/instance.hpp>
#include <fuse/renderer/vk/loader.hpp>

#include <fuse/platform/window_wsi.hpp>

#include <cstring>
#include <mutex>
#include <utility>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

std::mutex& validationMutex() {
    static std::mutex mutex;
    return mutex;
}

VulkanValidationCounters& validationCounters() {
    static VulkanValidationCounters counters;
    return counters;
}

#if defined(FUSE_VULKAN_BACKEND)
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT type,
                                             const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                             void* userData) {
    (void)type;
    (void)userData;
    std::lock_guard<std::mutex> lock(validationMutex());
    VulkanValidationCounters& counters = validationCounters();
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        ++counters.errors;
        if (callbackData != nullptr && callbackData->pMessage != nullptr) {
            counters.lastError = callbackData->pMessage;
        }
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        ++counters.warnings;
    }
    return VK_FALSE;
}

bool layerAvailable(const char* name) {
    u32 layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

/// Highest API version the renderer is written against (1.4 when the headers know it, else 1.3),
/// clamped to what the loader implements (vkEnumerateInstanceVersion) so core entry points resolve
/// through the loader's trampolines. Never below 1.2: a 1.2 loader still gets an instance, and
/// device selection then rejects every device unless the FUSE_VK_ALLOW_1_2 escape is set.
u32 requestedInstanceApiVersion() {
#if defined(VK_API_VERSION_1_4)
    u32 wanted = VK_API_VERSION_1_4;
#else
    u32 wanted = VK_API_VERSION_1_3;
#endif
    u32 loader = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loader) != VK_SUCCESS) {
        loader = VK_API_VERSION_1_0;
    }
    // Compare major.minor only (VkApplicationInfo::apiVersion patch is ignored).
    const u32 loaderMinor = VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(loader), VK_API_VERSION_MINOR(loader), 0);
    if (loaderMinor < wanted) {
        wanted = loaderMinor < VK_API_VERSION_1_2 ? VK_API_VERSION_1_2 : loaderMinor;
    }
    return wanted;
}

bool extensionAvailable(const char* name) {
    u32 extensionCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}
#endif

bool containsExtensionName(const std::vector<const char*>& list, const char* name) {
    if (name == nullptr) {
        return false;
    }
    for (const char* existing : list) {
        if (existing != nullptr && std::strcmp(existing, name) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

VulkanValidationCounters vulkanValidationCounters() {
    std::lock_guard<std::mutex> lock(validationMutex());
    return validationCounters();
}

void resetVulkanValidationCounters() {
    std::lock_guard<std::mutex> lock(validationMutex());
    validationCounters() = VulkanValidationCounters{};
}

bool VulkanInstanceInfo::instanceHasExtension(const char* name) const {
    return containsExtensionName(enabledExtensions, name);
}

std::unique_ptr<VulkanInstance> VulkanInstance::create(const VulkanInstanceDesc& desc) {
    auto instance = std::unique_ptr<VulkanInstance>(new VulkanInstance());
    if (!instance->initialize(desc)) {
        instance->m_info.valid = false;
    }
    return instance;
}

VulkanInstance::~VulkanInstance() {
    shutdown();
}

void* VulkanInstance::nativeHandle() const {
    return m_handle;
}

bool VulkanInstance::initialize(const VulkanInstanceDesc& desc) {
#if defined(FUSE_VULKAN_BACKEND)
    // fuse_rhi does not link the loader; volk opens it at run time (vk/loader.hpp).
    if (!vkloader::initialize()) {
        m_info.mode = VulkanBackendMode::Stub;
        m_info.message = "Vulkan loader library not found (volk) — falling back to stub semantics";
        return false;
    }
    m_info.mode = VulkanBackendMode::Headless;

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = desc.appName != nullptr ? desc.appName : "FUSE";
    appInfo.applicationVersion = desc.appVersion;
    appInfo.pEngineName = "FUSE";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = requestedInstanceApiVersion();

    std::vector<const char*> extensions;
    if (extensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    std::vector<const char*> platformExtensions;
    fuse::platform::requiredVulkanInstanceExtensions(platformExtensions);
    for (const char* name : platformExtensions) {
        if (name == nullptr || containsExtensionName(extensions, name)) {
            continue;
        }
        if (desc.extraExtensionCount == 0 && extensionAvailable(name)) {
            extensions.push_back(name);
        }
    }

    if (desc.extraExtensions != nullptr && desc.extraExtensionCount > 0) {
        for (u32 i = 0; i < desc.extraExtensionCount; ++i) {
            const char* extensionName = desc.extraExtensions[i];
            if (extensionName != nullptr && !containsExtensionName(extensions, extensionName)) {
                extensions.push_back(extensionName);
            }
        }
        for (const char* name : platformExtensions) {
            if (name == nullptr || containsExtensionName(extensions, name)) {
                continue;
            }
            if (extensionAvailable(name)) {
                extensions.push_back(name);
            }
        }
    }

    std::vector<const char*> layers;
    const bool validationRequested = desc.enableValidation;
    const bool validationAvailable = layerAvailable("VK_LAYER_KHRONOS_validation");
    if (validationRequested && validationAvailable) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
    createInfo.enabledLayerCount = static_cast<u32>(layers.size());
    createInfo.ppEnabledLayerNames = layers.empty() ? nullptr : layers.data();

    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        m_info.message = "vkCreateInstance failed — falling back to stub semantics";
        m_info.mode = VulkanBackendMode::Stub;
        return false;
    }

    // Load the instance-level table (and loader trampolines for device entry points) before any
    // other call on this instance.
    vkloader::registerInstance(instance);
    m_handle = instance;
    m_info.valid = true;
    m_info.apiVersion = appInfo.apiVersion;
    // Moved, not copied: the locals are dead after vkCreateInstance (and copying the 0/1-element
    // layer vector trips GCC 13's -Warray-bounds false positive in vector::operator=).
    const bool validationEnabled = !layers.empty();
    m_info.enabledExtensions = std::move(extensions);
    m_info.enabledLayers = std::move(layers);
    if (validationRequested && !validationAvailable) {
        m_info.message = "Instance ready (validation layers unavailable — CI-safe stub path)";
    } else if (validationEnabled) {
        m_info.message = "Instance ready with validation layers";
    } else {
        m_info.message = "Instance ready (validation disabled)";
    }

    if (extensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) && validationEnabled) {
        auto vkCreateDebugUtilsMessengerEXT =
            reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (vkCreateDebugUtilsMessengerEXT != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
            messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            messengerInfo.pfnUserCallback = debugCallback;

            VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
            if (vkCreateDebugUtilsMessengerEXT(instance, &messengerInfo, nullptr, &messenger) ==
                VK_SUCCESS) {
                m_debugMessenger = messenger;
            }
        }
    }

    return true;
#else
    (void)desc;
    m_info.mode = VulkanBackendMode::Stub;
    m_info.valid = false;
    m_info.message = "Vulkan loader unavailable — FUSE_VULKAN_BACKEND not enabled";
    return false;
#endif
}

void VulkanInstance::shutdown() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_handle == nullptr) {
        return;
    }

    auto instance = static_cast<VkInstance>(m_handle);
    if (m_debugMessenger != nullptr) {
        auto vkDestroyDebugUtilsMessengerEXT =
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (vkDestroyDebugUtilsMessengerEXT != nullptr) {
            vkDestroyDebugUtilsMessengerEXT(instance,
                                            static_cast<VkDebugUtilsMessengerEXT>(m_debugMessenger),
                                            nullptr);
        }
        m_debugMessenger = nullptr;
    }

    vkDestroyInstance(instance, nullptr);
    vkloader::unregisterInstance(instance);
    m_handle = nullptr;
#endif
}

} // namespace fuse::renderer
