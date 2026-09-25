// FUSE Relight RL-5.6: VK_EXT_opacity_micromap capability query (see omm.hpp).
#include <fuse/relight/render/pathtrace/omm/omm.hpp>

#include <fuse/renderer/vk/device.hpp>

#include <cstring>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::relight::render::pathtrace::omm {

bool ommQueryDevice(const renderer::VulkanDevice& device, OmmDeviceCaps& caps) {
    caps = OmmDeviceCaps{};
#if defined(FUSE_VULKAN_BACKEND) && defined(VK_EXT_opacity_micromap)
    const VkPhysicalDevice pd = static_cast<VkPhysicalDevice>(device.nativePhysicalDevice());
    if (pd == VK_NULL_HANDLE) {
        return false;
    }
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, exts.data());
    for (const VkExtensionProperties& e : exts) {
        if (std::strcmp(e.extensionName, VK_EXT_OPACITY_MICROMAP_EXTENSION_NAME) == 0) {
            caps.extension = true;
        }
    }
    if (!caps.extension) {
        return true;
    }
    VkPhysicalDeviceOpacityMicromapFeaturesEXT features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &features;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    caps.micromap = features.micromap == VK_TRUE;
    VkPhysicalDeviceOpacityMicromapPropertiesEXT props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &props;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    caps.max2StateLevel = props.maxOpacity2StateSubdivisionLevel;
    caps.max4StateLevel = props.maxOpacity4StateSubdivisionLevel;
    return true;
#else
    (void)device;
    return false;
#endif
}

} // namespace fuse::relight::render::pathtrace::omm
