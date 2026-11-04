#pragma once

// Test support (header-only, never used by the engine): the environment the Vulkan gates set up
// before vkCreateInstance — the validation layer's synchronization validation, and loader list
// variables (VK_ADD_LAYER_PATH, VK_INSTANCE_LAYERS, VK_ICD_FILENAMES, ...) joined with the
// platform's separator.
//
// Sync validation: layers from 1.3.268 on read layer settings (VK_KHRONOS_VALIDATION_VALIDATE_SYNC).
// Newer ones still parse the legacy VK_LAYER_ENABLES list but report it from vkCreateInstance as a
// VALIDATION-SETTINGS "deprecated" warning (SDK 1.4.357), which a create-time messenger counts as a
// validation message. The legacy variable is therefore only set for layers that predate settings.
//
// List separator: the loader splits list variables on ':' on POSIX and ';' on Windows.

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace fuse::renderer::test {

#if defined(_WIN32)
inline constexpr char kLoaderListSeparator = ';';
#else
inline constexpr char kLoaderListSeparator = ':';
#endif

inline void setProcessEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

/// `head` followed by the current value of `name` (if any), joined with the loader separator.
inline std::string prependEnvList(const char* name, const std::string& head) {
    std::string out = head;
    const char* existing = std::getenv(name);
    if (existing != nullptr && existing[0] != '\0') {
        out += kLoaderListSeparator;
        out += existing;
    }
    return out;
}

/// Appends `item` to a loader list held in `list` (no separator before the first item).
inline void appendLoaderList(std::string& list, const std::string& item) {
    if (!list.empty()) {
        list += kLoaderListSeparator;
    }
    list += item;
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

/// Synchronization validation on for instances created after this call. With `onlyIfUnset`, an
/// explicit choice already in the environment wins (harnesses run under outer gate scripts).
inline void enableSyncValidationEnv(bool onlyIfUnset = false) {
    auto set = [onlyIfUnset](const char* name, const char* value) {
        if (!onlyIfUnset || std::getenv(name) == nullptr) {
            setProcessEnv(name, value);
        }
    };
    set("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true");
    const uint32_t version = validationLayerSpecVersion();
    if (version != 0 && version < VK_MAKE_API_VERSION(0, 1, 3, 268)) {
        set("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
    }
}

} // namespace fuse::renderer::test
