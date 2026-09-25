#pragma once

// WP-6.1 DDGI on Vulkan (renderer plan Phase 6a; docs/unification/RENDERER-EXECUTION.md WP-6.1).
//
// The GPU form of the B5 DDGI probe update. The CPU kernels (include/fuse/renderer/gi/ddgi_probe_kernel.hpp,
// ddgi_cpu.hpp: DdgiCpuVolume) are the oracle: same probe grid, same bordered octahedral irradiance
// (E / pi) and distance-moment tiles, same ray set, same blend rules, same Chebyshev-weighted sample.
// Every pass is RG v2 compute (all accesses declared, no hand barriers), pure BDA + one push constant:
//
//   ddgi.reset    first frame / after reset(): the oracle's initial atlases (DdgiCpuVolume::init)
//   ddgi.raygen   the update's ray set: rotation(frameIndex) x spherical Fibonacci (ddgi_cpu.cpp)
//   ddgi.trace    one thread per (ray, scheduled probe): T2 ray query against the WP-6.0 TLAS, or T0
//                 sphere tracing of the global SDF (the Compute agent's analytic compute::SdfObject scene);
//                 radiance at hits = emissive + sun (shadow ray) + multi-bounce from the PREVIOUS volume;
//                 miss = sky; backface (probe inside geometry) = 0 and a short distance
//   ddgi.blend    one workgroup per scheduled probe: the oracle's BlendKernel phases (incoming irradiance,
//                 probe / texel change detection, hysteresis blends, border rings, update counts);
//                 probes classified inactive are skipped
//   ddgi.state    when DdgiCpuConfig::probe_relocation / probe_classification is on: one thread per
//                 scheduled probe, the oracle's ProbeStateKernel (RTXGI relocation + classification on the
//                 update's ray results) -> the per-probe (offset, state) the next trace / blend / every
//                 sample read
//   ddgi.probe    optional: irradiance at explicit points (the sampling code the lighting shade uses)
//
// Frame protocol (one owner thread):
//
//   ddgi.init({device, allocator, volume, config, tracer});            // T2 when rt caps allow (Auto)
//   ddgi.setSdfScene(objects, n, surfaces, m);                          // T0 tracer (any time; copied)
//   ddgi.beginFrame(serial, {frameIndex, sun, sky, rt.tlasAddress(), scene.headerAddress()});
//   DdgiGraphRefs d = ddgi.importInto(graph);
//   ddgi.addUpdate(graph, d, &rtRefs, &sceneRefs);                      // after rt.importInto (T2)
//   ddgi.addSamplingUse(graph, d, rg::kStageCompute);                   // before light.shade
//   lightingFrame.ddgi = ddgi.volumeAddress();                          // WP-2.1 shade: indirect diffuse
//
// The atlases and the ray buffers live in one persistent device buffer (DdgiWorkLayout); the per-frame
// constants, the probe schedule and the T0 SDF scene in a host-visible ring (framesInFlight slots).
// Steady-state frames make no heap allocation (fixed pass records, preallocated schedule / scratch).
//
// Probe relocation and inside / backface-heavy / inactive classification follow the oracle
// (ddgi_kernel::update_probe_state, RTXGI DDGI: Majercik et al. 2021): ray results carry signed distances
// (backface hits negative), the probe data lives in the work buffer, and sampling uses the relocated
// positions, skips inactive probes and applies the RTXGI surface bias (normal + view, DdgiCpuConfig::
// view_bias) and distance clamp (DdgiCpuConfig::distance_clamp) exactly as the oracle does.

#include <fuse/compute/ray_march.hpp>
#include <fuse/renderer/gi/ddgi.hpp>
#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/gi/gpu/ddgi_gpu_types.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/rt/acceleration_structures.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class BindlessDescriptors;
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::gi_gpu {

enum class DdgiTracer : u8 {
    Auto = 0, ///< RayQuery when rt::queryRtCapabilities(device).usable, else Sdf
    RayQuery, ///< T2: WP-6.0 TLAS (fails init below T2)
    Sdf,      ///< T0: sphere tracing of the global SDF (setSdfScene)
};

enum class DdgiKernelLanguage : u8 {
    Auto = 0, ///< Slang when built, else GLSL
    Slang,
    Glsl,
};

struct DdgiGpuCapabilities {
    bool compute = false;  ///< the T0 path (and the blend / sampling) can run
    bool rayQuery = false; ///< the T2 tracer can run (rt caps usable)
    const char* reason = "no device"; ///< "ok" when compute
};

DdgiGpuCapabilities queryDdgiGpuCapabilities(const VulkanDevice* device);

/// Tunables of the GPU path beyond DdgiCpuConfig (tracing only; the blend uses DdgiCpuConfig).
struct DdgiGpuTuning {
    f32 rayEpsilon = 1e-4f;      ///< T2 shadow-ray origin offset (ddgi_kernel::kRayEpsilon)
    f32 sdfMinDistance = 1e-4f;  ///< T0 hit threshold
    u32 sdfMaxSteps = 256;       ///< T0 steps per ray (exhausted before maxRayDistance = hit)
    f32 sdfShadowBias = 1e-3f;   ///< T0 shadow-ray origin offset along the normal
    u32 traceMask = 0x7Fu;       ///< T2 probe-ray cull mask (rt::kRtMaskAll; kRtMaskDead never traced)
    u32 shadowMask = 0x2u;       ///< T2 shadow-ray cull mask (rt::kRtMaskShadow)
    bool frontFaceCounterClockwise = true; ///< T2: winding seen from outside (engine / glTF convention)
    f32 intensity = 1.f;         ///< DdgiVolumeView::intensity (scale of the shade's indirect diffuse)
};

struct DdgiGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    /// The frame's bindless heap, when the graph also runs bindless passes: the DDGI pipelines bind no
    /// descriptors but follow its backend (VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT on the descriptor-
    /// buffer backend, so they may be dispatched while its descriptor buffers are bound). Null = none.
    BindlessDescriptors* bindless = nullptr;
    /// Probe grid, ray count, tile resolutions, hysteresis, max ray distance (irradiance_res <=
    /// kMaxIrradianceRes, depth_res <= kMaxDepthRes). probes_per_frame = rolling budget per frame.
    DDGIDesc volume{};
    /// The oracle's blend / sample tunables and the ray-rotation seed.
    DdgiCpuConfig config{};
    DdgiGpuTuning tuning{};
    DdgiTracer tracer = DdgiTracer::Auto;
    DdgiKernelLanguage language = DdgiKernelLanguage::Auto;
    u32 framesInFlight = 3;
    /// Most probes one frame updates (explicit schedules are clamped to it); 0 = probes_per_frame.
    u32 probeCapacity = 0;
    u32 sdfCapacity = 256;     ///< T0 primitives
    u32 surfaceCapacity = 256; ///< T0 material rows
    const char* name = "ddgi";
};

struct DdgiFrameDesc {
    u32 frameIndex = 0; ///< ray-set rotation (ddgi_cpu::updateRotation) and the rolling schedule
    math::Vec3 sunDirection{0.f, 1.f, 0.f}; ///< surface -> sun (normalised by beginFrame)
    math::Vec3 sunIrradiance{};             ///< irradiance on a surface facing the sun (0 = no sun)
    math::Vec3 skyRadiance{};               ///< radiance of rays that escape
    /// WP-8.2 sky for misses: AtmosphereGpu::frameAddress() (0 = the constant skyRadiance). The caller sets
    /// DdgiGraphRefs::atmosphere to the LUT buffer (AtmosphereGraphRefs::luts) so ddgi.trace declares the read.
    u64 atmosphereAddress = 0;
    u64 tlasAddress = 0;  ///< T2: AccelerationStructures::tlasAddress()
    u64 sceneAddress = 0; ///< T2: GpuScene::headerAddress()
    /// Explicit probe list (duplicates dropped, clamped to the capacity); null = the oracle's rolling
    /// schedule ddgi_util::scheduleProbeUpdates(frameIndex, probeCount, budget) with the oracle's budget
    /// kernel::scaled_count(probes_per_frame, kernel::load_scale().probes) (DdgiCpuVolume::update), clamped
    /// to the capacity (a LoadScale above 1 cannot grow the preallocated schedule).
    const u32* probes = nullptr;
    u32 probeCount = 0;
    bool update = true; ///< false: no trace / blend this frame (sampling only)
};

/// Byte layout of the persistent device buffer (256-aligned sections).
struct DdgiWorkLayout {
    u64 irradiance = 0;   ///< f32 x 3 per bordered texel (DdgiCpuVolume::irradianceAtlas layout)
    u64 distance = 0;     ///< f32 x 2 per bordered texel
    u64 updateCounts = 0; ///< u32 per probe
    u64 rayDirs = 0;      ///< f32 x 4 per ray
    u64 rays = 0;         ///< f32 x 4 per (slot, ray)
    u64 slotStats = 0;    ///< u32 per slot
    u64 probeData = 0;    ///< f32 x 4 per probe (relocation offset, state)
    u64 irradianceBytes = 0;
    u64 distanceBytes = 0;
    u64 bytes = 0;

    static DdgiWorkLayout compute(const DDGIDesc& volume, u32 probeCapacity);
};

struct DdgiGraphRefs {
    rg::BufferRef work; ///< the persistent atlases / rays buffer
    rg::BufferRef atmosphere; ///< optional (caller-set): WP-8.2 LUT buffer read by ddgi.trace (DdgiFrameDesc::atmosphereAddress)
};

struct DdgiFrameStats {
    u32 scheduled = 0;     ///< probes updated this frame
    u32 passes = 0;        ///< ddgi.* passes added this frame
    bool reset = false;    ///< ddgi.reset ran this frame
    u32 duplicatesDropped = 0;
    bool probeStates = false; ///< ddgi.state ran this frame (relocation / classification)
};

class DdgiGpu {
public:
    static constexpr u32 kMaxPasses = 12;

    DdgiGpu() = default;
    ~DdgiGpu();
    DdgiGpu(const DdgiGpu&) = delete;
    DdgiGpu& operator=(const DdgiGpu&) = delete;

    /// False (reason()) without a capable device, with an unsupported volume, without a built kernel of
    /// the requested language, for RayQuery below T2, or in the stub backend.
    bool init(const DdgiGpuDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool ready() const { return m_ready; }
    const char* reason() const { return m_reason; }
    DdgiTracer tracer() const { return m_tracer; }
    const char* tracerName() const { return m_tracer == DdgiTracer::RayQuery ? "ray-query" : "global-sdf"; }
    const char* kernelLanguage() const { return m_language; }
    const DdgiGpuDesc& desc() const { return m_desc; }

    /// T0 scene: the global SDF (Compute agent's analytic primitives; material_id = surface row) and the
    /// Lambertian surface rows. Copied; applied from the next beginFrame. False above the capacities.
    bool setSdfScene(const compute::SdfObject* objects, u32 objectCount, const DdgiSurface* surfaces, u32 surfaceCount);
    /// The next frame re-initialises the atlases (ddgi.reset) before updating.
    void reset() { m_needsReset = true; }

    // --- frame ----------------------------------------------------------------------------------
    bool beginFrame(u64 frameSerial, const DdgiFrameDesc& frame);
    DdgiGraphRefs importInto(rg::Graph& graph);
    /// ddgi.reset (when pending), ddgi.raygen, ddgi.trace, ddgi.blend [, ddgi.state]. T2 needs the frame's rt / scene
    /// refs (the trace declares AccelerationStructureRead on the TLAS and StorageRead on the scene).
    bool addUpdate(rg::Graph& graph, const DdgiGraphRefs& refs, const rt::RtGraphRefs* rtRefs = nullptr,
                   const gpu_scene::GpuSceneGraphRefs* sceneRefs = nullptr);
    /// ddgi.probe: E at `count` DdgiProbePoint at `pointsAddress` -> f32 x 4 at `outAddress`.
    bool addProbe(rg::Graph& graph, const DdgiGraphRefs& refs, rg::BufferRef points, u64 pointsAddress, rg::BufferRef out,
                  u64 outAddress, u32 count);
    /// Declares the atlases StorageRead at `stages` for passes that sample the volume through
    /// volumeAddress() (the WP-2.1 light.shade) without knowing this package: a command-less neverCull pass.
    void addSamplingUse(rg::Graph& graph, const DdgiGraphRefs& refs, u8 stages = rg::kStageCompute);

    // --- results / inspection -------------------------------------------------------------------
    /// This frame's DdgiVolumeView (valid after beginFrame, until the frame retires): the
    /// LightingFrameDesc::ddgi hook and ddgi_sample.{glsl,slang}.
    u64 volumeAddress() const { return m_frameAddress; }
    const DdgiFrameConstants& constants() const { return m_constants; }
    const DdgiWorkLayout& layout() const { return m_layout; }
    const Buffer& workBuffer() const { return m_work; }
    /// This frame's schedule (constants().scheduled entries).
    const u32* schedule() const { return m_schedule.data(); }
    u32 scheduled() const { return m_constants.scheduled; }
    u32 probeCapacity() const { return m_probeCapacity; }
    /// Relocation or classification is on (the probe data takes part in trace / blend / sampling).
    bool probeStatesEnabled() const { return m_desc.config.probe_relocation || m_desc.config.probe_classification; }
    u32 probeCount() const { return m_probeCount; }
    const DdgiFrameStats& stats() const { return m_stats; }
    /// The interior texel directions the blend uses (ddgi_cpu::texelDirection, == DdgiCpuVolume's).
    const std::vector<math::Vec3>& irradianceTexelDirs() const { return m_irrTexelDirs; }
    const std::vector<math::Vec3>& distanceTexelDirs() const { return m_distTexelDirs; }

private:
    enum Kernel : u32 { kReset = 0, kRaygen, kTrace, kBlend, kProbe, kState, kKernelCount };
    struct PassRecord {
        DdgiGpu* self = nullptr;
        DdgiPush push{};
        u32 kernel = 0;
        u32 groups[3] = {1u, 1u, 1u};
    };

    bool createPipelines();
    PassRecord* nextRecord();
    static void recordDispatch(const rg::PassContext& context, void* user);

    DdgiGpuDesc m_desc{};
    bool m_ready = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    DdgiTracer m_tracer = DdgiTracer::Sdf;
    u32 m_probeCount = 0;
    u32 m_probeCapacity = 0;
    bool m_needsReset = true;
    u64 m_frameSerial = 0;

    DdgiWorkLayout m_layout{};
    Buffer m_work{};        ///< GpuOnly: atlases, counts, rays
    Buffer m_texelDirs{};   ///< CpuToGpu, written once: irradiance then distance texel directions (f32 x 4)
    Buffer m_ring{};        ///< CpuToGpu: framesInFlight x (constants, schedule, SDF objects, surfaces)
    u64 m_ringSlotBytes = 0;
    u64 m_ringSchedule = 0; ///< offsets inside a ring slot
    u64 m_ringObjects = 0;
    u64 m_ringSurfaces = 0;
    u64 m_frameAddress = 0;
    DdgiFrameConstants m_constants{};
    DdgiFrameStats m_stats{};
    bool m_frameBegun = false;

    std::vector<u32> m_schedule;
    std::vector<u8> m_seen;
    std::vector<DdgiSdfObject> m_sdfObjects;
    std::vector<DdgiSurface> m_surfaces;
    u32 m_sdfCount = 0;
    u32 m_surfaceCount = 0;
    std::vector<math::Vec3> m_irrTexelDirs;
    std::vector<math::Vec3> m_distTexelDirs;

    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;
    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (32-byte push constant, compute)
    void* m_pipelines[kKernelCount] = {};
};

} // namespace fuse::renderer::gi_gpu
