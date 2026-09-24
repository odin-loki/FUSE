// FUSE Relight RL-4.1: the Vulkan entry points FUSE's frame work uses on the host's device.
//
// Resolved through the host's own vkGetInstanceProcAddr (IFrameHost::getInstanceProcAddr): inside d3d9.dll
// that is DXVK's loader (vulkan-1.dll, or winevulkan under Wine), in the native harness the process loader.
// The table is independent of fuse_rhi's volk globals, which are loaded only for devices fuse_rhi created.
#pragma once

#include <vulkan/vulkan.h>

// PE builds: <vulkan/vulkan.h> includes <windows.h> (VK_USE_PLATFORM_WIN32_KHR), whose winuser.h maps DrawState to
// DrawStateA and would rename tap::DrawState in whatever is included next.
#if defined(_WIN32) && defined(DrawState)
#undef DrawState
#endif

#include <cstdint>
#include <type_traits>

namespace fuse::relight::render::frame {

/// Raw handle values (tap structs carry Vulkan handles as std::uint64_t) <-> typed handles. Non-dispatchable
/// handles are pointers on 64-bit targets and uint64_t on 32-bit ones.
template <typename T>
T vkHandle(std::uint64_t value) {
    if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(static_cast<std::uintptr_t>(value));
    } else {
        return static_cast<T>(value);
    }
}
template <typename T>
std::uint64_t vkValue(T handle) {
    if constexpr (std::is_pointer_v<T>) {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}

struct VkDispatch {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;

    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;

    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2 = nullptr;
    PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;
    PFN_vkCmdCopyImage CmdCopyImage = nullptr;
    PFN_vkQueueSubmit2 QueueSubmit2 = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkGetFenceStatus GetFenceStatus = nullptr;
    PFN_vkGetSemaphoreCounterValue GetSemaphoreCounterValue = nullptr;
    PFN_vkWaitSemaphores WaitSemaphores = nullptr;

    /// Loads every entry point above. False (with the missing name in `missing`) when one is absent.
    bool load(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
              const char** missing = nullptr);
    bool loaded() const { return QueueSubmit2 != nullptr; }
};

} // namespace fuse::relight::render::frame
