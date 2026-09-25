#pragma once
// WP-9.3 device-generated commands (renderer plan P9: "device-generated commands for full GPU-side
// pipeline selection"): per-draw pipeline / material-bucket selection on the GPU for the WP-1.3
// culling output, executed with VK_EXT_device_generated_commands, falling back to the existing
// indirect-count path split by bucket.
//
// Per frame (everything is a render-graph v2 pass with declared accesses, no manual barriers):
//
//   selector.beginFrame(serial);                                   // uploads the material -> bucket table
//   DgcGraphRefs dgc = selector.importInto(graph);
//   selector.addGenerate(graph, dgc, dgc_cull_input(culler, cull, CullPhase::Phase1),
//                        sceneRefs, gpuScene.headerAddress());     // "dgc.reset" + "dgc.generate"
//   graph.addPass("draw", ...) + selector.useDraws(pass, dgc)
//       // callback: begin rendering, bind descriptors / push constants / index + vertex buffers, then
//       selector.recordDraws(cmd);
//
// recordDraws, DGC path: vkCmdBindPipeline(bucket 0) + vkCmdExecuteGeneratedCommandsEXT over the
// sequences the kernel wrote (one per culled draw: EXECUTION_SET = bucket pipeline, DRAW_INDEXED =
// the culled args), sequence count read from the GPU. 2 commands whatever the bucket count.
// Fallback: for every bucket, vkCmdBindPipeline + vkCmdDrawIndexedIndirectCount over the bucket's
// compacted args: 2 x bucketCount commands.
//
// Capability gate (dgc_evaluate_capability, reason() / reasonText()): the Vulkan headers of the
// local SDK (1.3.275) predate the extension, so VulkanDevice never enables it and cannot report the
// feature bit. The selector reads it from the creator's feature chain (DgcSelectorDesc::
// enabledFeatureChain, e.g. a device created by the caller and wrapped with VulkanDevice::adopt), or
// from RendererCaps when the headers define the extension; the structs are mirrored locally
// (src/vk/dgc/dgc_vk_ext.hpp). Pipelines handed to setPipelines() must be created with
// pipelineCreateFlags2() (VK_PIPELINE_CREATE_2_INDIRECT_BINDABLE_BIT_EXT) in a
// VkPipelineCreateFlags2CreateInfoKHR when dgcCapable().
//
// Stub backend / no device: init() fails with reason() == NoVulkanBackend; the CPU reference and
// the layout emulation (dgc_reference.hpp) remain available.
#include <fuse/renderer/culling/instance_culler.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/dgc/dgc_reference.hpp>
#include <fuse/renderer/vk/dgc/dgc_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
class BindlessDescriptors;
} // namespace fuse::renderer

namespace fuse::renderer::dgc {

enum class DgcMode : u8 {
    Auto = 0,      ///< DGC when the capability gate passes, else the indirect-count fallback
    ForceFallback, ///< always the indirect-count fallback (reason() == ForcedFallback)
};

enum class DgcKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct DgcSelectorDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr; ///< its set layout is bound with the kernel (descriptor-buffer safe)
    u32 maxDraws = 1024;    ///< draws (sequences) per frame; draws above it are dropped (>= the culler's capacity)
    u32 bucketCount = 1;    ///< pipelines (1..kDgcMaxBuckets)
    u32 defaultBucket = 0;  ///< materials without a bucket (clamped to bucketCount - 1)
    u32 framesInFlight = 2; ///< material-table ring slots
    DgcMode mode = DgcMode::Auto;
    DgcKernelLanguage language = DgcKernelLanguage::Auto;
    /// The VkDeviceCreateInfo::pNext chain the device was created with (read during init only).
    /// Needed on headers without VK_EXT_device_generated_commands to see the enabled feature bit.
    const void* enabledFeatureChain = nullptr;
    /// Tests / debugging: the kernel writes both the DGC sequences and the fallback's bucket args
    /// whatever the path (normally only what recordDraws consumes).
    bool writeBothStreams = false;
    const char* name = "dgc";
};

/// Where the culled draws are (WP-1.3 layout: VkDrawIndexedIndirectCommand, firstInstance = slot).
struct DgcCullInput {
    rg::BufferRef args;
    rg::BufferRef counts;
    void* argsBuffer = nullptr;   ///< VkBuffer (ShaderDeviceAddress usage)
    u64 argsOffset = 0;
    u64 argsBytes = 0;
    void* countsBuffer = nullptr; ///< VkBuffer
    u64 countOffset = 0;          ///< byte offset of the u32 draw count
};

/// The culler's draws of one phase.
DgcCullInput dgc_cull_input(const culling::InstanceCuller& culler, const culling::CullGraphRefs& refs,
                            culling::CullPhase phase);

struct DgcGraphRefs {
    rg::BufferRef sequences;
    rg::BufferRef bucketArgs;
    rg::BufferRef counts;
    rg::BufferRef materials;
};

struct DgcStats {
    u32 recordedCommands = 0; ///< draw-side commands recorded by the last recordDraws()
    u32 executes = 0;         ///< vkCmdExecuteGeneratedCommandsEXT calls (total)
    u32 indirectCountDraws = 0; ///< vkCmdDrawIndexedIndirectCount calls (total)
    u64 preprocessBytes = 0;
};

class DgcPipelineSelector {
public:
    DgcPipelineSelector() = default;
    ~DgcPipelineSelector();
    DgcPipelineSelector(const DgcPipelineSelector&) = delete;
    DgcPipelineSelector& operator=(const DgcPipelineSelector&) = delete;

    /// Creates the kernel and buffers and evaluates the capability gate. Fails only when not even the
    /// fallback can run (no device / BDA / drawIndirectCount / kernel).
    bool init(const DgcSelectorDesc& desc);
    void destroy();
    bool valid() const { return m_initialized; }

    /// The gate passed: pipelines for setPipelines() must carry pipelineCreateFlags2().
    bool dgcCapable() const { return m_reason == DgcReason::Supported; }
    /// DGC is in use (gate passed and setPipelines() created the execution set + layout).
    bool dgcActive() const { return m_dgcActive; }
    DgcReason reason() const { return m_reason; }
    const char* reasonText() const { return dgc_reason_text(m_reason); }
    /// VkPipelineCreateFlags2KHR bits the bucket pipelines need (0 when !dgcCapable()).
    u64 pipelineCreateFlags2() const;

    /// The bucket pipelines (VkPipeline[bucketCount], one layout, caller-owned, alive until destroy()
    /// or the next call). DGC: creates the execution set, the indirect-commands layout and the
    /// preprocess buffer; a failure falls back (reason() == CreationFailed).
    bool setPipelines(void* const* pipelines, u32 count, void* pipelineLayout);
    /// Material row -> bucket table (GpuInstance::material indexes it); copied, uploaded by beginFrame.
    bool setMaterialBuckets(const u32* buckets, u32 materialCount);

    bool beginFrame(u64 frameSerial);
    DgcGraphRefs importInto(rg::Graph& graph);
    /// "dgc.reset" (counts <- 0) and "dgc.generate" (one thread per maxDraws slot).
    void addGenerate(rg::Graph& graph, const DgcGraphRefs& refs, const DgcCullInput& input,
                     const gpu_scene::GpuSceneGraphRefs& scene, u64 sceneHeaderAddress);
    /// Declares the IndirectRead of what recordDraws consumes on the caller's raster pass.
    void useDraws(rg::PassBuilder& pass, const DgcGraphRefs& refs) const;
    /// Inside the caller's rendering scope; descriptors, push constants, viewport / scissor and the
    /// index + vertex buffers must be bound (DGC inherits them).
    void recordDraws(void* commandBuffer) const;

    // --- inspection --------------------------------------------------------------------------------
    const DgcStats& stats() const { return m_stats; }
    const char* kernelLanguage() const { return m_language; }
    u32 maxDraws() const { return m_desc.maxDraws; }
    u32 bucketCount() const { return m_desc.bucketCount; }
    u32 countWords() const { return dgc_count_words(m_desc.bucketCount); }
    void* sequencesBuffer() const { return m_sequences.handle; }
    void* bucketArgsBuffer() const { return m_bucketArgs.handle; }
    void* countsBuffer() const { return m_counts.handle; }
    u64 sequencesBytes() const { return m_sequences.desc.size; }
    u64 bucketArgsBytes() const { return m_bucketArgs.desc.size; }

private:
    struct Ext; // Vulkan-side DGC state (dispatch table, execution set, layout, preprocess memory)

    bool createPipeline();
    bool createBuffer(Buffer& out, u64 bytes, bool indirect, bool hostVisible, const char* name);
    void evaluateCapability();
    void destroyDgcObjects();
    static void recordReset(const rg::PassContext& context, void* user);
    static void recordGenerate(const rg::PassContext& context, void* user);

    DgcSelectorDesc m_desc{};
    bool m_initialized = false;
    bool m_dgcActive = false;
    DgcReason m_reason = DgcReason::NoVulkanBackend;
    const char* m_language = "none";
    Ext* m_ext = nullptr;

    Buffer m_sequences{};
    Buffer m_bucketArgs{};
    Buffer m_counts{};
    Buffer m_materials{}; ///< host-visible ring: framesInFlight x m_materialCapacity words
    u32 m_materialCapacity = 0;
    std::vector<u32> m_materialBuckets;
    u32 m_ringSlot = 0;
    u64 m_frameSerial = 0;

    std::vector<void*> m_pipelines;
    void* m_pipelineLayout = nullptr;

    struct GenerateRecord {
        DgcPipelineSelector* self = nullptr;
        DgcPush push{};
        void* counts = nullptr;
        u32 groups = 0;
    };
    GenerateRecord m_generate{};

    void* m_layout = nullptr;   ///< VkPipelineLayout (bindless set + 80-byte push)
    void* m_pipeline = nullptr; ///< dgc_generate
    mutable DgcStats m_stats{};
};

} // namespace fuse::renderer::dgc
