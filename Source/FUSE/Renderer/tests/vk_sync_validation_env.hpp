#pragma once

// Test helper: turn on synchronization validation in VK_LAYER_KHRONOS_validation through the
// environment (read at vkCreateInstance), with the settings mechanism the installed layer expects.
//
// Layers from 1.3.268 on read layer settings (VK_KHRONOS_VALIDATION_VALIDATE_SYNC). Newer ones
// also still parse the legacy VK_LAYER_ENABLES list, but report it from vkCreateInstance as a
// VALIDATION-SETTINGS warning ("deprecated ... enables"), which any create-time messenger (a
// VkDebugUtilsMessengerCreateInfoEXT chained into VkInstanceCreateInfo) counts as a validation
// message. So the legacy variable is only set for layers that predate the settings mechanism.

#include <vulkan/vulkan.h>

#include <cstdlib>
#include <cstring>
#include <vector>

namespace fuse::renderer::test {

inline void setProcessEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/// specVersion of the installed Khronos validation layer, 0 when it is not installed.
inline uint32_t validationLayerSpecVersion() {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS || count == 0) {
        return 0;
    }
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) {
        return 0;
    }
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
            return layer.specVersion;
        }
    }
    return 0;
}

/// Synchronization validation on for instances created after this call.
inline void enableSyncValidationEnv() {
    setProcessEnv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true");
    const uint32_t version = validationLayerSpecVersion();
    if (version != 0 && version < VK_MAKE_API_VERSION(0, 1, 3, 268)) {
        setProcessEnv("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
    }
}

} // namespace fuse::renderer::test
