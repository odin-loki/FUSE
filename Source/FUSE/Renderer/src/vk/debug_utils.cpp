#include <fuse/renderer/vk/debug_utils.hpp>

#include <cstdint>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer {

bool setDebugObjectName(void* vkDevice, u32 vkObjectType, u64 handle, const char* name) {
    if (vkDevice == nullptr || name == nullptr || handle == 0) {
        return false;
    }

#if defined(FUSE_VULKAN_BACKEND)
    const VkDevice device = static_cast<VkDevice>(vkDevice);
    const auto pfn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetDeviceProcAddr(device, "vkSetDebugUtilsObjectNameEXT"));
    if (pfn == nullptr) {
        return false;
    }

    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = static_cast<VkObjectType>(vkObjectType);
    info.objectHandle = handle;
    info.pObjectName = name;
    return pfn(device, &info) == VK_SUCCESS;
#else
    (void)vkObjectType;
    return false;
#endif
}

bool nameVkObject(void* vkDevice, u32 vkObjectType, const void* handle, const char* name) {
    return setDebugObjectName(vkDevice, vkObjectType,
                              static_cast<u64>(reinterpret_cast<uintptr_t>(handle)), name);
}

} // namespace fuse::renderer
