#pragma once

// WP-6.0 T2 capability gate. Acceleration structures and ray queries are tier T2 features
// (renderer plan §2, WP-0.1): VulkanDevice enables VK_KHR_acceleration_structure (+ deferred host
// operations) and VK_KHR_ray_query only when the hardware supports them AND the effective tier
// (min(hardware tier, VulkanDeviceDesc::maxTier, FUSE_RENDER_TIER_MAX)) is at least T2. The RT
// path therefore keys off the ENABLED caps, never the physical device's support: a T2 GPU capped to
// T1 takes the T0 fallback (global SDF / screen-space traces, WP-6.1 / WP-6.3) exactly like a T0 GPU.
//
// evaluateRtCapabilities() is the pure rule (unit-tested in the stub build with synthetic caps);
// queryRtCapabilities() adds the device limits the builder needs.

#include <fuse/renderer/vk/render_tier.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class VulkanDevice;
}

namespace fuse::renderer::rt {

struct RtCapabilities {
    /// Acceleration structures can be built and traced with ray queries on this device.
    bool usable = false;
    /// Why not (static string), or "ok".
    const char* reason = "no device";
    /// Name of the path callers take instead ("ray-query" when usable).
    const char* fallback = "global-sdf";
    RenderTier tier = RenderTier::T0;
    bool accelerationStructure = false;
    bool rayQuery = false;
    bool rayTracingPipeline = false; ///< T3 (informational; WP-6.0 only uses ray queries)
    /// VkPhysicalDeviceAccelerationStructurePropertiesKHR (0 when unknown / unusable).
    u32 minScratchAlignment = 0;
    u64 maxInstanceCount = 0;
    u64 maxPrimitiveCount = 0;
    u64 maxGeometryCount = 0;
    /// The driver mishandles inactive TLAS instances (reference 0 or mask 0): Mesa llvmpipe drops the
    /// last N active instances when N are inactive. The builder then keeps dead slots active with
    /// kRtMaskDead (rt_types.hpp packRtInstance). VkPhysicalDeviceDriverProperties::driverID based.
    bool inactiveInstanceQuirk = false;
    u32 driverId = 0; ///< VkDriverId
};

/// The T2 rule over the caps a device was created with.
RtCapabilities evaluateRtCapabilities(const RendererCaps& caps);

/// evaluateRtCapabilities(device->info().caps) plus the device's acceleration-structure limits.
/// Stub backend / null device: usable = false.
RtCapabilities queryRtCapabilities(const VulkanDevice* device);

} // namespace fuse::renderer::rt
