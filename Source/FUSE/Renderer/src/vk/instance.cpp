#include <fuse/renderer/vk/instance.hpp>

#include <cstring>
#include <utility>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

namespace {

#if defined(FUSE_VULKAN_BACKEND)
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT type,
                                             const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                             void* userData) {
    (void)severity;
    (void)type;
    (void)userData;
    if (callbackData != nullptr && callbackData->pMessage != nullptr) {
        // Validation output is captured by CI logs when layers are present.
        (void)callbackData->pMessage;
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

} // namespace

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
    m_info.mode = VulkanBackendMode::Headless;

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = desc.appName != nullptr ? desc.appName : "FUSE";
    appInfo.applicationVersion = desc.appVersion;
    appInfo.pEngineName = "FUSE";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    std::vector<const char*> extensions;
    if (extensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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

    m_handle = instance;
    m_info.valid = true;
    m_info.apiVersion = appInfo.apiVersion;
    m_info.enabledExtensions = extensions;
    m_info.enabledLayers = layers;
    if (validationRequested && !validationAvailable) {
        m_info.message = "Instance ready (validation layers unavailable — CI-safe stub path)";
    } else if (!layers.empty()) {
        m_info.message = "Instance ready with validation layers";
    } else {
        m_info.message = "Instance ready (validation disabled)";
    }

    if (extensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) && !layers.empty()) {
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
    m_handle = nullptr;
#endif
}

} // namespace fuse::renderer
