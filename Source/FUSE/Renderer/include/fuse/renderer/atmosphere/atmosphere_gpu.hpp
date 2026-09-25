#pragma once

// WP-8.2 Hillaire atmosphere on Vulkan (renderer plan P8 "Physically based sky and atmosphere (Hillaire-style
// LUTs)"; execution doc WP-8.2). Four compute passes on render graph v2 build the LUTs of S. Hillaire, "A
// Scalable and Production Ready Sky and Atmosphere Rendering Technique" (EGSR 2020) into one persistent
// device buffer; consumers sample them with the includable helpers of shaders/atmosphere/at_sample.{glsl,slang}.
// The CPU reference of every LUT and helper is atmosphere_luts.hpp (the kernels are its line-for-line twins).
//
//   atmos.beginFrame(serial, settings, view);            // resolves AtParams into this frame's ring slot
//   AtmosphereGraphRefs refs = atmos.importInto(graph);
//   atmos.addPasses(graph, refs);                          // before the passes that sample the LUTs
//   // consumers: push atmos.frameAddress(), include "atmosphere/at_sample.glsl", declare
//   //   .use(refs.luts, rg::Access::StorageRead, {}, rg::kStageCompute | ...)
//
// Passes (compute, all accesses declared on the graph by byte range of the LUT buffer, no manual barriers):
//   atmos.transmittance  transmittance LUT       (only when the medium / sizes changed, or after invalidate())
//   atmos.multiscatter   multi-scattering LUT    (same condition; reads the transmittance LUT)
//   atmos.skyview        sky-view LUT            (every frame: camera altitude and sun change)
//   atmos.aerial         aerial-perspective froxels (every frame)
// Kernels: Slang primary with GLSL twins, embedded by cmake/rp_wp82.cmake. Steady-state frames make no heap
// allocation (the LUT buffer follows the sizes; pass records live in a fixed array).

#include <fuse/renderer/atmosphere/atmosphere_lut_types.hpp>
#include <fuse/renderer/atmosphere/atmosphere_luts.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::atmosphere {

enum class AtmosKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct AtmosCapabilities {
    bool atmosphere = false;
    const char* reason = "no device"; ///< "ok" when usable
};

AtmosCapabilities queryAtmosphereCapabilities(const VulkanDevice* device);

/// Byte layout of the LUT buffer (every section 256-aligned, f32x4 texels).
struct AtmosphereBufferLayout {
    u64 transmittance = 0;
    u64 multiscatter = 0;
    u64 skyView = 0;
    u64 aerialScatter = 0;
    u64 aerialTransmittance = 0;
    u64 bytes[5] = {}; ///< section sizes in the order above
    u64 totalBytes = 0;

    static AtmosphereBufferLayout compute(const AtParams& p);
};

enum class AtmosLut : u8 {
    Transmittance = 0,
    MultiScatter,
    SkyView,
    AerialScatter,
    AerialTransmittance,
};

struct AtmosphereGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr; ///< the pipelines use its layout (shared frame, descriptor-buffer backend)
    AtmosKernelLanguage language = AtmosKernelLanguage::Auto;
    u32 framesInFlight = 3; ///< AtParams ring slots
};

struct AtmosphereGraphRefs {
    rg::BufferRef luts; ///< the LUT buffer (consumers: StorageRead after addPasses)
};

struct AtmosphereStats {
    u32 lutRebuilds = 0;     ///< LUT buffer (re)allocations
    u32 staticBuilds = 0;    ///< frames that ran atmos.transmittance + atmos.multiscatter
    u32 retired = 0;
    u32 passes = 0;          ///< passes added this frame
    bool staticRan = false;  ///< this frame
};

class AtmosphereGpu {
public:
    AtmosphereGpu() = default;
    ~AtmosphereGpu();
    AtmosphereGpu(const AtmosphereGpu&) = delete;
    AtmosphereGpu& operator=(const AtmosphereGpu&) = delete;

    /// Fails (false, nothing created) without a capable device or a built kernel of the requested language,
    /// or in the stub backend.
    bool init(const AtmosphereGpuDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Resolves this frame's AtParams into its ring slot; (re)allocates the LUT buffer when the sizes change
    /// and schedules the static LUTs when the medium changed. False for invalid settings / allocation failure.
    bool beginFrame(u64 frameSerial, const AtmosphereLutSettings& settings, const AtmosphereLutView& view);
    /// Forces atmos.transmittance + atmos.multiscatter on the next addPasses.
    void invalidate() { m_staticDirty = true; }

    AtmosphereGraphRefs importInto(rg::Graph& graph);
    void addPasses(rg::Graph& graph, const AtmosphereGraphRefs& refs);
    /// Copies one LUT section into `dst` at `dstOffset` (after addPasses).
    void addCopy(rg::Graph& graph, const AtmosphereGraphRefs& refs, AtmosLut which, rg::BufferRef dst, u64 dstOffset);
    u64 lutBytes(AtmosLut which) const;
    /// Runs the sampling helpers for `count` probes (inputs / outputs: BDA, see kAtProbeInputs / kAtProbeOutputs;
    /// declare `src` / `dst` as buffers of the graph). After addPasses.
    void addProbe(rg::Graph& graph, const AtmosphereGraphRefs& refs, rg::BufferRef srcBuffer, u64 srcAddress,
                  rg::BufferRef dstBuffer, u64 dstAddress, u32 count);

    /// Destroys buffers retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection / consumers -----------------------------------------------------------------------------
    /// BDA of this frame's AtParams (the at_sample helpers' argument). Valid after beginFrame.
    u64 frameAddress() const { return m_frameAddress; }
    const AtParams& params() const { return m_params; }
    const AtmosphereBufferLayout& layout() const { return m_layout; }
    const Buffer& lutBuffer() const { return m_luts; }
    const AtmosphereStats& stats() const { return m_stats; }
    const char* kernelLanguage() const { return m_language; }

private:
    struct Retired {
        Buffer buffer{};
        u64 serial = 0;
    };
    struct PassRecord {
        AtmosphereGpu* self = nullptr;
        AtPush push{};
        u32 kernel = 0;
        u32 groups[3] = {1u, 1u, 1u};
        u64 copySrc = 0;
        u64 copyDst = 0;
        u64 copyBytes = 0;
        rg::BufferRef copyFrom{};
        rg::BufferRef copyTo{};
    };
    enum Kernel : u32 { kTransmittance = 0, kMultiScatter, kSkyView, kAerial, kProbe, kKernelCount };
    static constexpr u32 kMaxPasses = 32u;

    bool createPipelines();
    bool createLuts(const AtmosphereBufferLayout& layout);
    PassRecord* nextRecord(u32 kernel);
    rg::BufferRange range(AtmosLut which) const;
    static void recordDispatch(const rg::PassContext& context, void* user);
    static void recordCopy(const rg::PassContext& context, void* user);

    AtmosphereGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    AtParams m_params{};
    AtParams m_staticParams{}; ///< the medium the static LUTs were built for
    bool m_staticDirty = true;
    bool m_staticThisFrame = false;
    AtmosphereBufferLayout m_layout{};
    Buffer m_luts{};
    u8 m_lutQueue = rg::kNoQueue;
    std::vector<Retired> m_retired;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;
    void* m_layoutHandle = nullptr;
    void* m_pipelines[kKernelCount] = {};
    AtmosphereStats m_stats{};
};

} // namespace fuse::renderer::atmosphere
