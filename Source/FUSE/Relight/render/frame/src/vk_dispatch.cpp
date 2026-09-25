// FUSE Relight RL-4.1: Vulkan entry points of FUSE's frame work (see vk_dispatch.hpp).
#include <fuse/relight/render/frame/vk_dispatch.hpp>

#include <type_traits>

namespace fuse::relight::render::frame {

bool VkDispatch::load(PFN_vkGetInstanceProcAddr gipa, VkInstance inst, VkPhysicalDevice pd, VkDevice dev,
                      const char** missing) {
    *this = VkDispatch{};
    if (!gipa || inst == VK_NULL_HANDLE || dev == VK_NULL_HANDLE) {
        if (missing) {
            *missing = "vkGetInstanceProcAddr / instance / device";
        }
        return false;
    }
    instance = inst;
    physicalDevice = pd;
    device = dev;
    GetInstanceProcAddr = gipa;
    const char* absent = nullptr;
    auto inst_ = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(gipa(inst, name));
        if (!fn && !absent) {
            absent = name;
        }
    };
    inst_(GetDeviceProcAddr, "vkGetDeviceProcAddr");
    inst_(GetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    inst_(GetPhysicalDeviceFormatProperties, "vkGetPhysicalDeviceFormatProperties");
    if (absent) {
        if (missing) {
            *missing = absent;
        }
        return false;
    }
    auto dev_ = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetDeviceProcAddr(dev, name));
        if (!fn && !absent) {
            absent = name;
        }
    };
    dev_(CreateImage, "vkCreateImage");
    dev_(DestroyImage, "vkDestroyImage");
    dev_(GetImageMemoryRequirements, "vkGetImageMemoryRequirements");
    dev_(AllocateMemory, "vkAllocateMemory");
    dev_(FreeMemory, "vkFreeMemory");
    dev_(BindImageMemory, "vkBindImageMemory");
    dev_(CreateImageView, "vkCreateImageView");
    dev_(DestroyImageView, "vkDestroyImageView");
    dev_(CreateCommandPool, "vkCreateCommandPool");
    dev_(DestroyCommandPool, "vkDestroyCommandPool");
    dev_(AllocateCommandBuffers, "vkAllocateCommandBuffers");
    dev_(ResetCommandBuffer, "vkResetCommandBuffer");
    dev_(BeginCommandBuffer, "vkBeginCommandBuffer");
    dev_(EndCommandBuffer, "vkEndCommandBuffer");
    dev_(CmdPipelineBarrier2, "vkCmdPipelineBarrier2");
    dev_(CmdClearColorImage, "vkCmdClearColorImage");
    dev_(CmdCopyImage, "vkCmdCopyImage");
    dev_(CmdCopyImageToBuffer, "vkCmdCopyImageToBuffer");
    dev_(CreateBuffer, "vkCreateBuffer");
    dev_(DestroyBuffer, "vkDestroyBuffer");
    dev_(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    dev_(BindBufferMemory, "vkBindBufferMemory");
    dev_(MapMemory, "vkMapMemory");
    dev_(UnmapMemory, "vkUnmapMemory");
    dev_(QueueSubmit2, "vkQueueSubmit2");
    dev_(CreateFence, "vkCreateFence");
    dev_(DestroyFence, "vkDestroyFence");
    dev_(WaitForFences, "vkWaitForFences");
    dev_(ResetFences, "vkResetFences");
    dev_(GetFenceStatus, "vkGetFenceStatus");
    dev_(GetSemaphoreCounterValue, "vkGetSemaphoreCounterValue");
    dev_(WaitSemaphores, "vkWaitSemaphores");
    if (absent) {
        if (missing) {
            *missing = absent;
        }
        QueueSubmit2 = nullptr;
        return false;
    }
    return true;
}

} // namespace fuse::relight::render::frame
