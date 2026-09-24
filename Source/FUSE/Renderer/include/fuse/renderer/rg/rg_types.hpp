#pragma once

// Render graph v2 (WP-0.3) — Vulkan-free vocabulary shared by the compiler (rg/graph.hpp) and the
// Vulkan executor (rg/executor.hpp). Flags, layouts and formats are carried as the numeric Vulkan
// values (VkPipelineStageFlags2 / VkAccessFlags2 / VkImageLayout / VkFormat) so the compiler and
// its barrier planner build and unit-test in the stub backend; src/rg/sync_model.cpp static_asserts
// the constants against vulkan.h in Vulkan builds.
//
// The API is NOT frozen: per plan §9 / RENDERER-EXECUTION §6 it freezes only after WP-1.3, WP-1.4
// and WP-2.1 have used it. Until then every RG-facing change goes through the WP-0.3 owner.

#include <fuse/types.hpp>

namespace fuse::renderer::rg {

/// Queue a pass runs on. AsyncCompute / Transfer resolve to Graphics when the device has no
/// distinct queue for them (Lavapipe without the split-family layer).
enum class QueueClass : u8 {
    Graphics = 0,
    AsyncCompute = 1,
    Transfer = 2,
};
constexpr u32 kQueueClassCount = 3;
constexpr u8 kNoQueue = 0xFFu;

/// What a pass does to a resource. Each value maps to one (stages, access, layout, usage) tuple in
/// sync_model.cpp; shader accesses take their stages from `ShaderStage` bits on the use.
enum class Access : u8 {
    None = 0,
    ColorAttachmentWrite, ///< COLOR_ATTACHMENT_OUTPUT, read|write (load/blend), COLOR_ATTACHMENT_OPTIMAL
    DepthAttachmentWrite, ///< EARLY|LATE_FRAGMENT_TESTS, read|write, DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    DepthAttachmentRead,  ///< EARLY|LATE_FRAGMENT_TESTS, read, DEPTH_STENCIL_READ_ONLY_OPTIMAL
    SampledRead,          ///< shader stages, SHADER_READ, SHADER_READ_ONLY_OPTIMAL
    StorageRead,          ///< shader stages, SHADER_READ, GENERAL
    StorageWrite,         ///< shader stages, SHADER_WRITE, GENERAL
    StorageReadWrite,     ///< shader stages, SHADER_READ|SHADER_WRITE, GENERAL
    UniformRead,          ///< shader stages, UNIFORM_READ (buffers)
    VertexRead,           ///< VERTEX_INPUT, VERTEX_ATTRIBUTE_READ (buffers)
    IndexRead,            ///< VERTEX_INPUT, INDEX_READ (buffers)
    IndirectRead,         ///< DRAW_INDIRECT, INDIRECT_COMMAND_READ (buffers)
    TransferSrc,          ///< TRANSFER, TRANSFER_READ, TRANSFER_SRC_OPTIMAL
    TransferDst,          ///< TRANSFER, TRANSFER_WRITE, TRANSFER_DST_OPTIMAL
    HostRead,             ///< HOST, HOST_READ — declares a host readback after the frame's fence
    Present,              ///< no GPU access; PRESENT_SRC_KHR
    ExternalRead,         ///< CUDA / foreign API: ALL_COMMANDS, MEMORY_READ, GENERAL
    ExternalWrite,        ///< CUDA / foreign API: ALL_COMMANDS, MEMORY_READ|WRITE, GENERAL
    // Acceleration structures (WP-6.0; buffers only, numeric values of VK_KHR_acceleration_structure).
    /// Geometry / instance input of vkCmdBuildAccelerationStructuresKHR (vertex, index, instance
    /// buffers): ACCELERATION_STRUCTURE_BUILD, SHADER_READ, BUILD_INPUT_READ_ONLY usage.
    AccelerationStructureBuildInput,
    /// An acceleration structure read by a build or copy command (BLAS referenced by a TLAS build,
    /// compaction source, vkCmdWriteAccelerationStructuresPropertiesKHR): ACCELERATION_STRUCTURE_BUILD,
    /// ACCELERATION_STRUCTURE_READ.
    AccelerationStructureBuildRead,
    /// Build / update / copy destination and build scratch: ACCELERATION_STRUCTURE_BUILD,
    /// ACCELERATION_STRUCTURE_READ|WRITE (an in-place update reads its source too).
    AccelerationStructureBuildWrite,
    /// Ray query / ray tracing reads of an acceleration structure: shader stages,
    /// ACCELERATION_STRUCTURE_READ.
    AccelerationStructureRead,
};

/// Shader stages of a shader access (SampledRead / Storage* / UniformRead). 0 = derived from the
/// pass queue: Compute on AsyncCompute, Vertex|Fragment|Compute on Graphics.
enum ShaderStage : u8 {
    kStageVertex = 1u << 0,
    kStageFragment = 1u << 1,
    kStageCompute = 1u << 2,
    kStageTask = 1u << 3, ///< VK_EXT_mesh_shader task stage (WP-5.1; only on devices with taskShader)
    kStageMesh = 1u << 4, ///< VK_EXT_mesh_shader mesh stage (WP-5.1; only on devices with meshShader)
};

/// Image subresource range. Counts of 0 mean "all remaining" (VK_REMAINING_*).
struct ImageRange {
    u32 baseMip = 0;
    u32 mipCount = 0;
    u32 baseLayer = 0;
    u32 layerCount = 0;
};

/// Buffer byte range. size 0 = to the end of the buffer (VK_WHOLE_SIZE).
struct BufferRange {
    u64 offset = 0;
    u64 size = 0;
};

struct TextureRef {
    u32 id = 0; ///< 0 = invalid
    bool valid() const { return id != 0; }
};

struct BufferRef {
    u32 id = 0; ///< 0 = invalid
    bool valid() const { return id != 0; }
};

/// Graph-owned image, placed in aliased transient memory by the executor. Usage is the union of
/// every declared access plus `extraUsage` (VkImageUsageFlags).
struct ImageDesc {
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mipLevels = 1;
    u32 arrayLayers = 1;
    u32 format = 37; ///< VkFormat (37 = R8G8B8A8_UNORM)
    u32 extraUsage = 0;
    const char* name = nullptr;
};

/// Graph-owned buffer, placed in aliased transient memory. Usage = declared accesses + extraUsage.
struct BufferDesc {
    u64 size = 0;
    u32 extraUsage = 0; ///< VkBufferUsageFlags
    const char* name = nullptr;
};

/// Externally owned image (swapchain image, persistent target, CUDA interop image).
struct ImportedImage {
    void* image = nullptr; ///< VkImage
    void* view = nullptr;  ///< optional VkImageView returned by PassContext::imageView
    u32 format = 0;        ///< VkFormat (selects the aspect: depth formats -> DEPTH[|STENCIL])
    u32 width = 1;
    u32 height = 1;
    u32 depth = 1;
    u32 mipLevels = 1;
    u32 arrayLayers = 1;
    /// VkImageLayout of every subresource at graph start (0 = UNDEFINED, contents discarded).
    u32 initialLayout = 0;
    /// Queue class that owns the image at graph start (kNoQueue = not owned by any family yet,
    /// e.g. fresh image or VK_SHARING_MODE_CONCURRENT). A first use on another queue schedules the
    /// ownership release in a prologue batch on the owning queue.
    u8 initialQueue = kNoQueue;
    /// VkImageLayout to leave the image in (UINT32_MAX = whatever the last pass left).
    u32 finalLayout = UINT32_MAX;
    /// Written after compile with the final layout of subresource 0 and the owning queue class, so
    /// the next frame can import with the right state.
    u32* layoutTracker = nullptr;
    u8* queueTracker = nullptr;
    const char* name = nullptr;
};

struct ImportedBuffer {
    void* buffer = nullptr; ///< VkBuffer
    u64 size = 0;
    u8 initialQueue = kNoQueue;
    u8* queueTracker = nullptr;
    const char* name = nullptr;
};

} // namespace fuse::renderer::rg
