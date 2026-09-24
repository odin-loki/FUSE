#pragma once

// WP-7.2 ReSTIR DI and GI on Vulkan (renderer plan Phase 7, tier T2: ray queries). Compute passes on render graph
// v2 over the WP-1.5 G-buffer, the WP-7.1 light tree (lt_sample / lt_pmf from the header address), the WP-6.0 TLAS
// (ray queries, cull mask within rt::kRtMaskAll) and the WP-1.1 GPU scene (GI hit attributes):
//
//   restir.prepare         RT0 / RT1 / RT4 -> this frame's surface records (position + linear depth, normal, albedo)
//   restir.di.initial      RIS over diCandidates light-tree samples + visibility reuse
//   restir.di.temporal     + the previous frame's final reservoir (WP-4.1 UV motion), M-capped
//   restir.di.spatial xN   + up to diNeighbors neighbours per iteration
//   restir.gi.initial      one cosine-weighted ray query per pixel, next-event estimation at the hit
//   restir.gi.temporal     reconnection reuse with the Jacobian
//   restir.gi.spatial xN
//   restir.shade           final reservoirs -> DI / GI signals (demodulated, f32x4) + linear depth; history
// (biased "1 / M" or unbiased Talbot MIS with visibility-tested targets: restir_kernel.hpp). No manual barrier:
// every access is declared (state / work StorageReadWrite, output StorageWrite, TLAS AccelerationStructureRead,
// light-tree slot / light table / scene tables / motion StorageRead, G-buffer SampledRead).
//
//   restir.init({device, allocator, bindless});                 // false below the T2 gate (reason())
//   restir.setSettings(settings);
//   restir.beginFrame(serial, {camera, &resolve, as.tlasAddress(), treeGpu.headerAddress(), scene.headerAddress(),
//                              table.data(), count, tableVersion, motion.motionAddress(), frameIndex});
//   RestirGraphRefs r = restir.importInto(graph);
//   restir.addPasses(graph, r, {rtRefs.tlas, treeRefs, &gbufferRefs, sceneRefs, motionRefs.motion});
//   // WP-6.4: DenoiseFrameDesc{w, h, restir.diSignalAddress(), motion, restir.depthAddress(), restir.normalAddress()}
//   //         DenoiseGraphInputs{r.output, motionRef, r.output, r.state}
//   restir.collectRetired(completedSerial);
//
// Kernels: Slang primary (-fp-mode precise) with GLSL twins (`precise`), embedded by cmake/rp_wp72.cmake. Every
// pass equals restir_kernel.hpp on the same inputs. Steady-state frames make no heap allocation.

#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/light_tree/light_tree_gpu.hpp>
#include <fuse/renderer/material_resolve/material_resolve.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/restir/restir.hpp>
#include <fuse/renderer/restir/restir_types.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::restir {

enum class RestirKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct RestirCapabilities {
    bool gpu = false;
    const char* reason = "no device"; ///< "ok" when usable
};

/// Needs the WP-6.0 T2 gate (ray queries, acceleration structures, buffer device address).
RestirCapabilities queryRestirCapabilities(const VulkanDevice* device);

struct RestirGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    RestirKernelLanguage language = RestirKernelLanguage::Auto;
    u32 framesInFlight = 3;         ///< frame-constant / light-table ring slots
    u32 width = 0;                  ///< initial extent (follows the G-buffer at beginFrame)
    u32 height = 0;
    bool keepIntermediates = false; ///< one work section per stage (parity gates) instead of ping-pong
    u32 initialLights = 1024;       ///< light-table slot capacity (grows by reallocation)
    const char* name = "restir";
};

struct RestirFrameDesc {
    RestirCamera camera{};
    const material_resolve::MaterialResolve* gbuffer = nullptr; ///< RT0 (normal), RT1 (albedo), RT4 (depth)
    u64 tlasAddress = 0;      ///< AccelerationStructures::tlasAddress()
    u64 lightTreeHeader = 0;  ///< LightTreeGpu::headerAddress()
    u64 sceneHeader = 0;      ///< GpuScene::headerAddress()
    const RestirLight* lights = nullptr; ///< one row per light-tree emitter
    u32 lightCount = 0;
    u64 lightsVersion = 0;    ///< the table is copied into a ring slot only when the slot holds another version
    u64 motion = 0;           ///< f32x2 UV motion per pixel (TemporalMotion::motionAddress), 0 = zero motion
    u32 frameIndex = 0;
    u32 seed = 0x5EEDu;
    bool reset = false;       ///< camera cut: no temporal reuse this frame
};

struct RestirGraphRefs {
    rg::BufferRef state;  ///< surfaces + histories (RestirBufferLayout)
    rg::BufferRef work;   ///< stage reservoirs
    rg::BufferRef output; ///< DI / GI signals, linear depth
    rg::BufferRef lights; ///< light-table ring
};

struct RestirGraphInputs {
    rg::BufferRef tlas;                              ///< RtGraphRefs::tlas
    light_tree::LightTreeGraphRefs lightTree{};      ///< LightTreeGpu::importInto
    const material_resolve::ResolveGraphRefs* gbuffer = nullptr;
    gpu_scene::GpuSceneGraphRefs scene{};            ///< GI hits read instances / transforms / meshes / materials / indices
    rg::BufferRef motion;                            ///< when RestirFrameDesc::motion != 0
    rg::BufferRef dump;                              ///< optional RestirGiHitRecord[width x height]
    u64 dumpAddress = 0;
};

struct RestirGpuStats {
    u32 rebuilds = 0;      ///< state / work / output (re)allocations
    u32 gbufferBinds = 0;
    u32 lightUploads = 0;  ///< light-table slot copies (cumulative)
    u32 passes = 0;        ///< this frame
    bool history = false;  ///< this frame reuses the previous one
};

class RestirGpu {
public:
    static constexpr u32 kMaxPasses = 16u;

    RestirGpu() = default;
    ~RestirGpu();
    RestirGpu(const RestirGpu&) = delete;
    RestirGpu& operator=(const RestirGpu&) = delete;

    /// False (nothing created, reason()) below the T2 gate, without a built kernel or in the stub backend.
    bool init(const RestirGpuDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }
    const char* reason() const { return m_reason; }
    const char* kernelLanguage() const { return m_language; }

    /// Takes effect at the next beginFrame (a change of the chain structure drops the history).
    void setSettings(const RestirSettings& settings);
    const RestirSettings& settings() const { return m_settings; }

    /// Writes this frame's constants, follows the G-buffer's extent / images, uploads the light table.
    bool beginFrame(u64 frameSerial, const RestirFrameDesc& desc);
    RestirGraphRefs importInto(rg::Graph& graph);
    /// Every enabled pass of the frame (see the header comment).
    bool addPasses(rg::Graph& graph, const RestirGraphRefs& refs, const RestirGraphInputs& inputs);
    /// Destroys buffers / bindless slots retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- results / inspection -----------------------------------------------------------------------------
    u64 diSignalAddress() const;
    u64 giSignalAddress() const;
    u64 depthAddress() const;
    /// This frame's surface normals (f32x4, xyz unit): the WP-6.4 normal-buffer input.
    u64 normalAddress() const;
    const RestirFrameConstants& frameConstants() const { return m_constants; }
    const RestirBufferLayout& layout() const { return m_layout; }
    /// State slot written this frame (RestirBufferLayout::surfPos[slot()] ...).
    u32 slot() const { return m_slot; }
    /// Work stage holding the final DI / GI reservoirs of this frame (kRestirInvalid when the chain is off).
    u32 finalDiStage() const { return m_finalDi; }
    u32 finalGiStage() const { return m_finalGi; }
    const Buffer& stateBuffer() const { return m_state; }
    const Buffer& workBuffer() const { return m_work; }
    const Buffer& outputBuffer() const { return m_output; }
    u32 width() const { return m_layout.width; }
    u32 height() const { return m_layout.height; }
    const RestirGpuStats& stats() const { return m_stats; }

private:
    static constexpr u32 kGBufferInputs = 3u; ///< RT0, RT1, RT4
    static constexpr u32 kMaxRetired = 8u;
    static constexpr u32 kMaxSlots = 8u;
    struct Retired {
        Buffer buffers[4]{};
        BindlessSlotHandle slots[kGBufferInputs]{};
        u64 serial = 0;
        bool used = false;
    };
    enum Kernel : u32 { kPrepare = 0, kDiInitial, kDiReuse, kGiInitial, kGiReuse, kShade, kKernelCount };
    struct PassRecord {
        RestirGpu* self = nullptr;
        RestirPush push{};
        u32 kernel = 0;
    };

    bool createPipelines(RestirKernelLanguage language);
    bool createBuffers(u32 width, u32 height);
    bool ensureLightCapacity(u32 lights);
    bool bindGBuffer(const material_resolve::MaterialResolve& gbuffer);
    bool retire(const Retired& r);
    static void recordDispatch(const rg::PassContext& context, void* user);

    RestirGpuDesc m_desc{};
    RestirSettings m_settings{};
    RestirSettings m_active{};
    bool m_settingsDirty = true;
    bool m_initialized = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    RestirBufferLayout m_layout{};
    Buffer m_state{};
    Buffer m_work{};
    Buffer m_output{};
    u8 m_stateQueue = rg::kNoQueue;
    u8 m_workQueue = rg::kNoQueue;
    u8 m_outputQueue = rg::kNoQueue;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    Buffer m_lightRing{};
    u8 m_lightQueue = rg::kNoQueue;
    u64 m_lightSlotBytes = 0;
    u64 m_lightSlotVersion[kMaxSlots] = {};
    bool m_lightSlotValid[kMaxSlots] = {};
    u64 m_lightOffset = 0;
    u32 m_lightCount = 0;
    RestirFrameConstants m_constants{};
    u32 m_slot = 0;
    bool m_history = false;
    bool m_motion = false;
    u32 m_finalDi = kRestirInvalid;
    u32 m_finalGi = kRestirInvalid;
    const material_resolve::MaterialResolve* m_gbuffer = nullptr;
    void* m_gbufferImages[kGBufferInputs] = {};
    BindlessSlotHandle m_gbufferSlots[kGBufferInputs]{};
    u32 m_gbufferHandles[kGBufferInputs] = {};
    Retired m_retired[kMaxRetired]{};
    PassRecord m_records[kMaxPasses]{};
    u32 m_recordCount = 0;
    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 48-byte push)
    void* m_pipelines[kKernelCount] = {};
    RestirGpuStats m_stats{};
};

} // namespace fuse::renderer::restir
