#include <fuse/renderer/rt/rt_caps.hpp>

#include <fuse/renderer/vk/device.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer::rt {

RtCapabilities evaluateRtCapabilities(const RendererCaps& caps) {
    RtCapabilities out{};
    out.tier = caps.tier;
    out.accelerationStructure = caps.accelerationStructure;
    out.rayQuery = caps.rayQuery;
    out.rayTracingPipeline = caps.rayTracingPipeline;
    if (!caps.valid) {
        out.reason = "no device caps (stub backend or no device)";
        return out;
    }
    if (!caps.meetsT0) {
        out.reason = "device below T0 (FUSE_VK_ALLOW_1_2 escape)";
        return out;
    }
    if (!caps.atLeast(RenderTier::T2)) {
        out.reason = caps.hardwareTier >= RenderTier::T2 ? "tier capped below T2 (VulkanDeviceDesc::maxTier / FUSE_RENDER_TIER_MAX)"
                                                          : "device tier below T2";
        return out;
    }
    if (!caps.bufferDeviceAddress) {
        out.reason = "bufferDeviceAddress not enabled";
        return out;
    }
    if (!caps.accelerationStructure) {
        out.reason = "VK_KHR_acceleration_structure not enabled";
        return out;
    }
    if (!caps.rayQuery) {
        out.reason = "VK_KHR_ray_query not enabled";
        return out;
    }
    out.usable = true;
    out.reason = "ok";
    out.fallback = "ray-query";
    return out;
}

RtCapabilities queryRtCapabilities(const VulkanDevice* device) {
    if (device == nullptr || !device->isValid()) {
        return RtCapabilities{};
    }
    RtCapabilities out = evaluateRtCapabilities(device->info().caps);
#if defined(FUSE_VULKAN_BACKEND) && defined(VK_KHR_acceleration_structure)
    if (out.usable) {
        VkPhysicalDeviceDriverProperties driver{};
        driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceAccelerationStructurePropertiesKHR as{};
        as.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        as.pNext = &driver;
        VkPhysicalDeviceProperties2 props{};
        props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props.pNext = &as;
        vkGetPhysicalDeviceProperties2(static_cast<VkPhysicalDevice>(device->nativePhysicalDevice()), &props);
        out.minScratchAlignment = as.minAccelerationStructureScratchOffsetAlignment;
        out.maxInstanceCount = as.maxInstanceCount;
        out.maxPrimitiveCount = as.maxPrimitiveCount;
        out.maxGeometryCount = as.maxGeometryCount;
        out.driverId = static_cast<u32>(driver.driverID);
        out.inactiveInstanceQuirk = driver.driverID == VK_DRIVER_ID_MESA_LLVMPIPE;
        if (out.minScratchAlignment == 0u) {
            out.minScratchAlignment = 256u;
        }
    }
#else
    if (out.usable) {
        out.usable = false;
        out.reason = "built without VK_KHR_acceleration_structure headers";
        out.fallback = "global-sdf";
    }
#endif
    return out;
}

} // namespace fuse::renderer::rt
