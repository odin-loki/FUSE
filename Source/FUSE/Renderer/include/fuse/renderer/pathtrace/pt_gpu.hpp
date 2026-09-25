#pragma once

// WP-7.3 path-tracing mode on Vulkan (renderer plan Phase 7). One render-graph pass, "pathtrace.trace", in one of
// two backends that run the same integrator (shaders/pathtrace/pt_integrator.{glsl,slang}):
//
//   RayTracingPipeline  tier T3 (VK_KHR_ray_tracing_pipeline, enabled by WP-0.1 only when the tier cap is >= T3):
//                       raygen pt_raygen, miss pt_miss (both ray types), hit group 0 = closest hit pt_closest_hit
//                       (the material dispatch: surface record from the GPU scene) + any hit pt_any_hit (skip the
//                       origin triangle), hit group 1 = any hit only (shadow rays). Shader binding table built from
//                       the device's handle size / alignments (computePtSbtLayout) in a host-visible buffer.
//   RayQuery            tier T2: one compute kernel (pt_trace), SBT-free (the candidate loop is the any-hit shader,
//                       pt_surface the closest-hit shader).
//
// Inputs: the WP-6.0 TLAS, the WP-1.1 GPU scene (instances, transforms, meshes, materials, index buffer), the WP-7.1
// light tree (lt_sample / lt_pmf from the header address) with its RGB table (restir::RestirLight, WP-7.2's) and the
// emitter map (pathtrace.hpp). Outputs (PtBufferLayout): the accumulated mean (f32x4 rgb + samples) and the
// denoiser / ray-reconstruction guides (demodulated signal, linear depth, normal, albedo), in the WP-6.4 layouts.
//
// Accesses: the ray-query pass declares its compute accesses (TLAS AccelerationStructureRead, tables StorageRead,
// accumulation StorageReadWrite, outputs StorageWrite). Render graph v2 has no ray-tracing shader stage yet, so the
// RT-pipeline pass declares the same resources ExternalRead / ExternalWrite (ALL_COMMANDS, MEMORY_READ / WRITE):
// correct and validation-clean, coarser than needed (WP-7.3 open issue for the WP-0.3 owner). No manual barriers.
//
//   pt.init({device, allocator, bindless});            // false below the T2 gate (reason()); Auto picks T3 if enabled
//   pt.setSettings(settings);
//   pt.beginFrame(serial, {camera, w, h, as.tlasAddress(), scene.headerAddress(), treeGpu.headerAddress(),
//                          table.data(), count, version, map.data(), mapWords, mapSlots, mapVersion, frameIndex});
//   PtGraphRefs r = pt.importInto(graph);
//   pt.addPasses(graph, r, {rtRefs.tlas, treeRefs, sceneRefs});
//   // WP-6.4 / NRD / RR: pt.denoiseFrame(motion), pt.denoiseInputs(r, motionRef) (IDenoiser, denoiser.hpp)
//   pt.collectRetired(completedSerial);
//
// Accumulation restarts (sample count 0) on reset(), a camera change, a settings change, an extent change or a
// new light-table / emitter-map version; scene edits are the caller's (PtFrameDesc::reset). Steady-state frames
// make no heap allocation.

#include <fuse/renderer/denoise/svgf_denoiser.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/light_tree/light_tree_gpu.hpp>
#include <fuse/renderer/pathtrace/pathtrace.hpp>
#include <fuse/renderer/pathtrace/pt_types.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/restir/restir_types.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::pathtrace {

enum class PtBackend : u8 {
    Auto = 0,           ///< RayTracingPipeline when the T3 gate passes, else RayQuery
    RayTracingPipeline, ///< T3 only (init fails below it)
    RayQuery,           ///< T2
};

enum class PtKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

/// queryPtCapabilities: evaluatePtCapabilities(device caps) plus a built-kernel check; stub / null: nothing.
PtCapabilities queryPtCapabilities(const VulkanDevice* device);

struct PathTracerGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    /// The frame's bindless heap: the pipelines bind no descriptor but follow its backend
    /// (VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT on the descriptor-buffer backend, VUID-...-08117). Optional.
    BindlessDescriptors* bindless = nullptr;
    PtBackend backend = PtBackend::Auto;
    PtKernelLanguage language = PtKernelLanguage::Auto;
    u32 framesInFlight = 3;   ///< frame-constant / light-table ring slots (1..8)
    u32 width = 0;            ///< initial extent (follows PtFrameDesc)
    u32 height = 0;
    u32 initialLights = 256;  ///< light-table slot capacity in lights (grows by reallocation)
    u32 initialMapWords = 1024;
    const char* name = "pathtrace";
};

struct PtFrameDesc {
    PtCamera camera{};
    u32 width = 0;
    u32 height = 0;
    u64 tlasAddress = 0;      ///< AccelerationStructures::tlasAddress()
    u64 sceneHeader = 0;      ///< GpuScene::headerAddress()
    u64 lightTreeHeader = 0;  ///< LightTreeGpu::headerAddress() (0: no light sampling, no emitter hits)
    const restir::RestirLight* lights = nullptr; ///< one row per light-tree emitter
    u32 lightCount = 0;
    u64 lightsVersion = 0;    ///< copied into a ring slot only when the slot holds another version
    const u32* emitterMap = nullptr; ///< buildPtEmitterMap words (null: none)
    u32 emitterMapWords = 0;
    u32 emitterMapSlots = 0;  ///< instance slots the map covers
    u64 emitterMapVersion = 0;
    u32 frameIndex = 0;
    u32 seed = 0x5EEDu;
    bool reset = false;       ///< restart the accumulation (scene edit, camera cut)
};

struct PtGraphRefs {
    rg::BufferRef state;  ///< accumulation (PtBufferLayout::accum / accumSq)
    rg::BufferRef output; ///< mean + guides
    rg::BufferRef lights; ///< light-table / emitter-map ring
};

struct PtGraphInputs {
    rg::BufferRef tlas;                         ///< RtGraphRefs::tlas
    light_tree::LightTreeGraphRefs lightTree{}; ///< LightTreeGpu::importInto (when lightTreeHeader != 0)
    gpu_scene::GpuSceneGraphRefs scene{};
};

struct PtGpuStats {
    u32 rebuilds = 0;       ///< state / output (re)allocations
    u32 tableUploads = 0;   ///< light-table / emitter-map slot copies (cumulative)
    u32 passes = 0;         ///< this frame
    u32 restarts = 0;       ///< accumulation restarts (cumulative)
    u32 samples = 0;        ///< samples per pixel accumulated after this frame
    bool traced = false;    ///< this frame added samples
};

class PathTracerGpu {
public:
    PathTracerGpu() = default;
    ~PathTracerGpu();
    PathTracerGpu(const PathTracerGpu&) = delete;
    PathTracerGpu& operator=(const PathTracerGpu&) = delete;

    /// False (nothing created, reason()) below the requested backend's gate, without a built kernel or in the
    /// stub backend.
    bool init(const PathTracerGpuDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }
    const char* reason() const { return m_reason; }
    const char* kernelLanguage() const { return m_language; }
    /// "rt_pipeline" (T3) or "ray_query" (T2).
    const char* backendName() const { return m_rtPipeline ? "rt_pipeline" : "ray_query"; }
    bool usesRayTracingPipeline() const { return m_rtPipeline; }
    const PtCapabilities& capabilities() const { return m_caps; }

    /// Takes effect at the next beginFrame (restarts the accumulation).
    void setSettings(const PtSettings& settings);
    const PtSettings& settings() const { return m_settings; }
    /// The next frame restarts the accumulation.
    void reset() { m_restart = true; }

    bool beginFrame(u64 frameSerial, const PtFrameDesc& desc);
    PtGraphRefs importInto(rg::Graph& graph);
    bool addPasses(rg::Graph& graph, const PtGraphRefs& refs, const PtGraphInputs& inputs);
    u32 collectRetired(u64 completedSerial);

    // --- results -----------------------------------------------------------------------------------------
    u64 meanAddress() const { return address(m_output, m_layout.output); }
    u64 accumAddress() const { return address(m_state, m_layout.accum); }
    u64 accumSqAddress() const { return address(m_state, m_layout.accumSq); }
    u64 signalAddress() const { return address(m_output, m_layout.signal); }
    u64 depthAddress() const { return address(m_output, m_layout.depth); }
    u64 normalAddress() const { return address(m_output, m_layout.normal); }
    u64 albedoAddress() const { return address(m_output, m_layout.albedo); }
    /// WP-6.4 / IDenoiser input of this frame (GI-style RGB signal, demodulated by the primary albedo).
    denoise::DenoiseFrameDesc denoiseFrame(u64 motionAddress, bool reset = false) const;
    denoise::DenoiseGraphInputs denoiseInputs(const PtGraphRefs& refs, rg::BufferRef motion) const;

    const PtFrameConstants& frameConstants() const { return m_constants; }
    const PtBufferLayout& layout() const { return m_layout; }
    const PtSbtLayout& sbtLayout() const { return m_sbt; }
    const Buffer& stateBuffer() const { return m_state; }
    const Buffer& outputBuffer() const { return m_output; }
    u32 width() const { return m_layout.width; }
    u32 height() const { return m_layout.height; }
    u32 sampleCount() const { return m_samples; }
    const PtGpuStats& stats() const { return m_stats; }

private:
    static constexpr u32 kMaxSlots = 8u;
    static constexpr u32 kMaxRetired = 8u;
    struct Retired {
        Buffer buffers[3]{};
        u64 serial = 0;
        bool used = false;
    };
    struct PassRecord {
        PathTracerGpu* self = nullptr;
        PtPush push{};
    };

    static u64 address(const Buffer& b, u64 offset) { return b.deviceAddress != 0u ? b.deviceAddress + offset : 0u; }
    bool createPipelines();
    bool createRtPipeline(const u32* const* words, const usize* bytes);
    bool createBuffers(u32 width, u32 height);
    bool ensureTableCapacity(u64 bytes);
    bool retire(const Retired& r);
    static void recordTrace(const rg::PassContext& context, void* user);

    PathTracerGpuDesc m_desc{};
    PtCapabilities m_caps{};
    PtSettings m_settings{};
    bool m_settingsDirty = true;
    bool m_initialized = false;
    bool m_rtPipeline = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    PtBufferLayout m_layout{};
    Buffer m_state{};
    Buffer m_output{};
    u8 m_stateQueue = rg::kNoQueue;
    u8 m_outputQueue = rg::kNoQueue;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    Buffer m_tableRing{};
    u8 m_tableQueue = rg::kNoQueue;
    u64 m_tableSlotBytes = 0;
    u64 m_slotLightsVersion[kMaxSlots] = {};
    u64 m_slotMapVersion[kMaxSlots] = {};
    u32 m_slotLightCount[kMaxSlots] = {};
    u32 m_slotMapWords[kMaxSlots] = {};
    bool m_slotValid[kMaxSlots] = {};
    u64 m_tableOffset = 0;
    u64 m_tableBytes = 0;
    u64 m_lightsVersion = 0;
    u64 m_mapVersion = 0;
    bool m_haveTables = false;
    bool m_lightTree = false;
    PtFrameConstants m_constants{};
    PtCamera m_lastCamera{};
    bool m_restart = true;
    u32 m_samples = 0;
    bool m_traceThisFrame = false;
    Retired m_retired[kMaxRetired]{};
    PassRecord m_record{};
    PtSbtLayout m_sbt{};
    void* m_layoutHandle = nullptr;   ///< VkPipelineLayout (push constants only)
    void* m_pipeline = nullptr;       ///< VkPipeline (compute or ray tracing)
    void* m_sbtBuffer = nullptr;      ///< VkBuffer
    void* m_sbtMemory = nullptr;      ///< VkDeviceMemory
    u64 m_sbtAddress = 0;             ///< base-aligned
    void* m_traceRays = nullptr;      ///< PFN_vkCmdTraceRaysKHR
    PtGpuStats m_stats{};
};

} // namespace fuse::renderer::pathtrace
