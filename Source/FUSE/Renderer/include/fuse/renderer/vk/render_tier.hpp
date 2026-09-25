#pragma once

// Renderer hardware tiers (FUSE_RENDERER_PLAN.md §2) and the per-feature capability set the
// logical device was created with (WP-0.1).
//
//   T0 Baseline  Vulkan 1.3 + sync2, dynamic rendering, maintenance4, timeline semaphores,
//                descriptor indexing, buffer device address, drawIndirectCount, multiDrawIndirect,
//                shaderDrawParameters, shaderInt64, shaderBufferInt64Atomics  (hard requirement)
//   T1 Mesh      T0 + VK_EXT_mesh_shader (task + mesh)
//   T2 RT        T1 + VK_KHR_acceleration_structure + VK_KHR_ray_query
//   T3 Full      T2 + VK_KHR_ray_tracing_pipeline + VK_KHR_cooperative_matrix (tensor units)
//
// Optional at any tier (reported, enabled when supported): image int64 atomics, descriptor buffer,
// device-generated commands, shader object. A feature above the effective tier is never enabled.
//
// This header has no Vulkan dependency: it builds (and its helpers work) in the stub backend.

#include <fuse/types.hpp>

#include <string>

namespace fuse::renderer {

enum class RenderTier : u8 {
    T0 = 0,
    T1 = 1,
    T2 = 2,
    T3 = 3,
};

inline constexpr RenderTier kMaxRenderTier = RenderTier::T3;

/// Every feature RendererCaps tracks. Order is stable (bit index into the masks below).
enum class RenderFeature : u8 {
    // T0 hard requirements.
    Synchronization2 = 0,
    DynamicRendering,
    Maintenance4,
    TimelineSemaphore,
    DescriptorIndexing,
    BufferDeviceAddress,
    DrawIndirectCount,
    MultiDrawIndirect,
    ShaderDrawParameters,
    ShaderInt64,
    ShaderBufferInt64Atomics,
    // Optional at T0 (enabled when supported).
    ShaderImageInt64Atomics, ///< VK_EXT_shader_image_atomic_int64
    DescriptorBuffer,        ///< VK_EXT_descriptor_buffer
    DeviceGeneratedCommands, ///< VK_EXT_device_generated_commands
    ShaderObject,            ///< VK_EXT_shader_object
    // T1.
    TaskShader, ///< VK_EXT_mesh_shader taskShader
    MeshShader, ///< VK_EXT_mesh_shader meshShader
    // T2.
    AccelerationStructure,  ///< VK_KHR_acceleration_structure (+ VK_KHR_deferred_host_operations)
    RayQuery,               ///< VK_KHR_ray_query
    // T3.
    RayTracingPipeline, ///< VK_KHR_ray_tracing_pipeline
    CooperativeMatrix,  ///< VK_KHR_cooperative_matrix
    Count,
};

inline constexpr u32 kRenderFeatureCount = static_cast<u32>(RenderFeature::Count);

[[nodiscard]] constexpr u64 renderFeatureBit(RenderFeature feature) {
    return u64{1} << static_cast<u32>(feature);
}

struct RenderFeatureInfo {
    /// Vulkan feature / extension spelling, e.g. "synchronization2", "meshShader".
    const char* name;
    /// Lowest tier that relies on this feature.
    RenderTier tier;
    /// True when the tier cannot be reached without it (T0 set, mesh+task, AS+ray query,
    /// RT pipeline + cooperative matrix). False for "optional at this tier" features.
    bool requiredForTier;
    /// Name of the fallback path used when the feature is absent ("none" for T0 requirements,
    /// which have no fallback: the device is rejected).
    const char* fallback;
};

[[nodiscard]] const RenderFeatureInfo& renderFeatureInfo(RenderFeature feature);
/// "T0".."T3".
[[nodiscard]] const char* renderTierName(RenderTier tier);
/// Accepts "T0".."T3", "t0".."t3" and "0".."3". Returns false (out untouched) otherwise.
bool parseRenderTier(const char* text, RenderTier& out);
/// FUSE_RENDER_TIER_MAX (parsed with parseRenderTier); kMaxRenderTier when unset or malformed.
[[nodiscard]] RenderTier renderTierMaxFromEnv();

/// Mask of the T0 hard requirements.
[[nodiscard]] u64 renderT0RequiredMask();
/// Mask of every feature whose tier is above `tier` (never enabled when capped at `tier`).
[[nodiscard]] u64 renderFeaturesAboveTier(RenderTier tier);
/// Highest tier whose cumulative requirements are all in `mask`. T0 also when the T0 set is
/// incomplete (check `(mask & renderT0RequiredMask()) == renderT0RequiredMask()` for that).
[[nodiscard]] RenderTier renderTierFromMask(u64 mask);
/// Comma-separated names of the T0 requirements missing from `mask` (empty when complete).
[[nodiscard]] std::string renderMissingT0(u64 mask);

/// What the logical device was created with. Filled by VulkanDevice (VulkanDeviceInfo::caps);
/// all false / T0 in the stub backend or when no device was created.
struct RendererCaps {
    bool valid = false;
    /// Effective tier: min(hardwareTier, tierCap). Features above it are not enabled.
    RenderTier tier = RenderTier::T0;
    /// Tier the physical device supports before any cap.
    RenderTier hardwareTier = RenderTier::T0;
    /// min(VulkanDeviceDesc::maxTier, FUSE_RENDER_TIER_MAX).
    RenderTier tierCap = kMaxRenderTier;
    /// False only on a device accepted through the FUSE_VK_ALLOW_1_2 escape (T0 incomplete).
    bool meetsT0 = false;
    /// Effective device API version (VK_MAKE_API_VERSION encoded).
    u32 apiVersion = 0;

    /// Features the physical device reports (feature bit + extension present).
    u64 supportedMask = 0;
    /// Features enabled on the logical device (subset of supportedMask; tier-capped).
    u64 enabledMask = 0;

    // Per-feature view of enabledMask (true = enabled on the logical device and usable).
    bool synchronization2 = false;
    bool dynamicRendering = false;
    bool maintenance4 = false;
    bool timelineSemaphore = false;
    bool descriptorIndexing = false;
    bool bufferDeviceAddress = false;
    bool drawIndirectCount = false;
    bool multiDrawIndirect = false;
    bool shaderDrawParameters = false;
    bool shaderInt64 = false;
    bool shaderBufferInt64Atomics = false;
    bool shaderImageInt64Atomics = false;
    bool descriptorBuffer = false;
    bool deviceGeneratedCommands = false;
    bool shaderObject = false;
    bool taskShader = false;
    bool meshShader = false;
    bool accelerationStructure = false; ///< implies VK_KHR_deferred_host_operations enabled
    bool rayQuery = false;
    bool rayTracingPipeline = false;
    bool cooperativeMatrix = false;

    [[nodiscard]] bool has(RenderFeature feature) const { return (enabledMask & renderFeatureBit(feature)) != 0; }
    [[nodiscard]] bool supports(RenderFeature feature) const {
        return (supportedMask & renderFeatureBit(feature)) != 0;
    }
    [[nodiscard]] bool atLeast(RenderTier t) const { return static_cast<u8>(tier) >= static_cast<u8>(t); }
    /// nullptr when `feature` is enabled, else the fallback path name from renderFeatureInfo.
    [[nodiscard]] const char* fallbackFor(RenderFeature feature) const;
    /// Comma-separated T0 gaps: "Vulkan 1.3" when apiVersion < 1.3, then the names of T0
    /// features that are not enabled (empty when meetsT0).
    [[nodiscard]] std::string missingT0() const;
    /// Sets enabledMask and syncs the per-feature bools from it.
    void setEnabledMask(u64 mask);
    /// One line: "T2 (hw T2, cap T3) enabled: synchronization2,... | fallback: cooperativeMatrix->...".
    [[nodiscard]] std::string summary() const;
};

} // namespace fuse::renderer
