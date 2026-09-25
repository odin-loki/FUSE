// FUSE Relight RL-5.6: volumetrics and the particle composite on the GPU - the stages of shaders/rl_vol_core.h as
// render-graph (WP-0.3 v2) compute passes, recorded after the frame's radiance exists:
//
//   relight.vol.inject     current, reservoirs[cur] <- RIS over the RL-4.4 light set (+ reservoirs[prev]), shadow rays
//                          on the TLAS (ray query)
//   relight.vol.temporal   history[cur] <- current + history[prev] (reprojected)
//   relight.vol.integrate  integrated <- history[cur], per column front to back
//   relight.vol.apply      output <- colour x T + S with the particle layers (RL-3.6 billboard quads)
//
// Every pass declares its accesses (the ring slot, the light ring / tree slot, the TLAS, the external colour / depth,
// this class's sections); no manual barriers. Sections are persistent host-visible buffers (the tests read them
// back); the per-frame ring slot holds the block (addresses, params, systems) and the frame's particle vertices.
// Steady-state frames (same grid / extent, no more particle vertices than before) make no heap allocation.
//
//   vg.beginFrame(serial, desc, lightCount, vertices);
//   VolExternal ext = volExternalFromPathTracer(pt, ptRefs);   // or any colour / depth / lights / TLAS
//   vg.addPasses(graph, ext);
//   ... execute; vg.collectRetired(completedSerial); output: vg.outputAddress() / graphRefs().sections[kVolSecOutput]
//
// Path-tracer hook (RL-5.1, no change to its files): the passes run after "relight.pt.trace" of the same graph and
// read its kPtOutRadiance / kPtOutDepth sections, its RL-4.4 light ring and its TLAS; the composite is this class's
// output section (the frame's final radiance before denoise / post when volumetrics are on).
#pragma once

#include <fuse/relight/render/volumetrics/volumetrics.hpp>

#include <fuse/relight/render/lights/light_set_gpu.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <span>
#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::relight::render::pathtrace {
class PathTracerGpu;
struct PtGraphRefs;
} // namespace fuse::relight::render::pathtrace

namespace fuse::relight::render::volumetrics {

enum class VolKernelLanguage : u8 { Auto = 0, Slang, Glsl };

struct VolumetricsGpuDesc {
    renderer::VulkanDevice* device = nullptr;
    renderer::GpuAllocator* allocator = nullptr;
    renderer::BindlessDescriptors* bindless = nullptr; ///< required (pipeline layout / create flags)
    VolKernelLanguage language = VolKernelLanguage::Auto;
    u32 framesInFlight = 3;
};

/// Persistent sections.
enum VolSection : u32 {
    kVolSecCurrent = 0,
    kVolSecHistory0 = 1,
    kVolSecHistory1 = 2,
    kVolSecReservoir0 = 3,
    kVolSecReservoir1 = 4,
    kVolSecIntegrated = 5,
    kVolSecOutput = 6,
    kVolSections = 7,
};

/// What the passes read that this class does not own.
struct VolExternal {
    renderer::rg::BufferRef color;       ///< f32x4 per pixel (StorageRead)
    renderer::rg::BufferRange colorRange{};
    u64 colorAddress = 0;
    renderer::rg::BufferRef depth;       ///< f32 view depth per pixel, 0 = sky (optional: address 0 = all sky)
    renderer::rg::BufferRange depthRange{};
    u64 depthAddress = 0;
    lights::RelightLightsGraphRefs lights{}; ///< lightCount 0: ambient only
    renderer::rg::BufferRef tlas;        ///< optional (kVolShadows): AccelerationStructureRead
    u64 tlasAddress = 0;
};

/// The path tracer's frame as the passes' input (radiance, depth, lights, TLAS of the same graph).
VolExternal volExternalFromPathTracer(const pathtrace::PathTracerGpu& pt, const pathtrace::PtGraphRefs& refs);

struct VolGraphRefs {
    renderer::rg::BufferRef sections[kVolSections];
    renderer::rg::BufferRef ring;
    renderer::rg::BufferRange ringRange{};
    bool valid = false;
};

struct VolumetricsGpuStats {
    u32 frames = 0;
    u32 passes = 0; ///< this frame
    u32 reallocations = 0;
};

class VolumetricsGpu {
public:
    VolumetricsGpu();
    ~VolumetricsGpu();
    VolumetricsGpu(const VolumetricsGpu&) = delete;
    VolumetricsGpu& operator=(const VolumetricsGpu&) = delete;

    /// False (nothing created, reason()) without a device / kernel, or in the stub backend.
    bool init(const VolumetricsGpuDesc& desc);
    void destroy();
    bool valid() const { return m_initialized; }
    const char* reason() const { return m_reason; }
    const char* kernelLanguage() const { return m_language; }

    /// This frame's sections, ring slot (block + particle vertices). `lightCount` = the light set of the frame.
    bool beginFrame(u64 frameSerial, const VolFrameDesc& desc, u32 lightCount,
                    std::span<const particles::GpuParticleVertex> vertices);
    bool addPasses(renderer::rg::Graph& graph, const VolExternal& ext);
    const VolGraphRefs& graphRefs() const { return m_refs; }
    u32 collectRetired(u64 completedSerial);
    void resetHistory() { m_historyValid = false; }

    /// Read-back (after the frame completed).
    const void* mappedSection(u32 section) const;
    /// The sections that were this frame's history / reservoirs (current) and the previous frame's.
    u32 historySection(bool current) const;
    u32 reservoirSection(bool current) const;
    u64 outputAddress() const;
    u32 frameFlags() const { return m_flags; }
    const VolumetricsGpuStats& stats() const { return m_stats; }

private:
    struct Push {
        u64 block = 0;
        u64 tlas = 0;
        u64 lightTable = 0;
        u64 lightTree = 0;
        u32 stage = 0;
        u32 count = 0;
        u32 reserved0 = 0;
        u32 reserved1 = 0;
    };
    static_assert(sizeof(Push) == 48u, "VolPush (rl_vol.comp / .slang)");
    struct PassRecord {
        VolumetricsGpu* self = nullptr;
        Push push{};
        u32 groups = 1;
    };
    struct Retired {
        renderer::Buffer buffer{};
        u64 serial = 0;
    };
    static constexpr u32 kMaxSlots = 8u;

    bool createPipeline();
    bool ensureBuffer(renderer::Buffer& buffer, u64 bytes, u32 memory, const char* name, u8* queue);
    static void record(const renderer::rg::PassContext& context, void* user);

    VolumetricsGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_reason = "not initialised";
    const char* m_language = "none";
    renderer::Buffer m_sections[kVolSections]{};
    u8 m_sectionQueues[kVolSections] = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu};
    u64 m_sectionBytes[kVolSections] = {};
    renderer::Buffer m_ring{};
    u8 m_ringQueue = 0xFFu;
    u64 m_slotStride = 0;
    u64 m_slotOffset = 0;
    u64 m_slotBytes = 0;
    u64 m_verticesOffset = 0;
    std::vector<Retired> m_retired;
    PassRecord m_records[4]{};
    VolGraphRefs m_refs{};
    VolFrameDesc m_frame{};
    u64 m_frameSerial = 0;
    u32 m_cur = 0;
    u32 m_flags = 0;
    u32 m_lightCount = 0;
    u32 m_froxels = 0;
    u32 m_pixels = 0;
    u32 m_columns = 0;
    bool m_historyValid = false;
    bool m_frameReady = false;
    void* m_layoutHandle = nullptr;
    void* m_pipeline = nullptr;
    VolumetricsGpuStats m_stats{};
};

} // namespace fuse::relight::render::volumetrics
