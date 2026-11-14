// VK_KHR_android_surface for the RHI (the Android counterpart of fuse::platform::createVulkanSurface).
#include <fuse/renderer/vk/surface_android.hpp>

#if defined(__ANDROID__) && defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>

namespace fuse::renderer {

namespace {
// VkAndroidSurfaceCreateInfoKHR (vulkan_android.h): spelled out so this file does not depend on the volk shim's
// platform defines
struct AndroidSurfaceCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    void* window;
};
using CreateAndroidSurfaceFn = VkResult(VKAPI_PTR*)(VkInstance, const AndroidSurfaceCreateInfo*,
                                                    const VkAllocationCallbacks*, VkSurfaceKHR*);
constexpr VkStructureType kAndroidSurfaceCreateInfo = static_cast<VkStructureType>(1000008000);
} // namespace

bool createAndroidSurface(void* vkInstance, void* nativeWindow, void** outSurface, std::string* err) {
    if (outSurface == nullptr) return false;
    *outSurface = nullptr;
    auto instance = static_cast<VkInstance>(vkInstance);
    if (instance == VK_NULL_HANDLE || nativeWindow == nullptr) {
        if (err) *err = "createAndroidSurface: null instance or window";
        return false;
    }
    const auto create = reinterpret_cast<CreateAndroidSurfaceFn>(vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR"));
    if (create == nullptr) {
        if (err) *err = "vkCreateAndroidSurfaceKHR missing (instance without VK_KHR_android_surface)";
        return false;
    }
    AndroidSurfaceCreateInfo info{kAndroidSurfaceCreateInfo, nullptr, 0, nativeWindow};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult r = create(instance, &info, nullptr, &surface);
    if (r != VK_SUCCESS) {
        if (err) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "vkCreateAndroidSurfaceKHR failed (VkResult %d)", static_cast<int>(r));
            *err = buf;
        }
        return false;
    }
    *outSurface = reinterpret_cast<void*>(surface);
    return true;
}

void destroyAndroidSurface(void* vkInstance, void* surface) {
    if (vkInstance == nullptr || surface == nullptr) return;
    vkDestroySurfaceKHR(static_cast<VkInstance>(vkInstance), reinterpret_cast<VkSurfaceKHR>(surface), nullptr);
}

} // namespace fuse::renderer

#else

namespace fuse::renderer {
bool createAndroidSurface(void*, void*, void** outSurface, std::string* err) {
    if (outSurface) *outSurface = nullptr;
    if (err) *err = "createAndroidSurface: not an Android Vulkan build";
    return false;
}
void destroyAndroidSurface(void*, void*) {}
} // namespace fuse::renderer

#endif
