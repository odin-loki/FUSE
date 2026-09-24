#pragma once

// WP-8.3 volumetric clouds on Vulkan (renderer plan P8 "Volumetric clouds (raymarched, temporally amortised)";
// execution doc WP-8.3). Compute passes on render graph v2; the model and the CPU reference of every kernel are
// in cloud_reference.hpp.
//
//   clouds.beginFrame(serial, settings, frame);   // frame.atmosphere / atmosphereAddress: WP-8.2 AtmosphereGpu
//   CloudGraphRefs c = clouds.importInto(graph);
//   // after atmos.addPasses (the cloud kernels sample its LUTs):
//   clouds.addPasses(graph, c, {atmosRefs.luts, backgroundRef, depthRef});
//   // result: clouds.resultAddress() (f32x4 per output pixel: composited radiance, a = cloud transmittance),
//   //         addCopy(.., CloudSection::Result, ..); the reconstruction (cloud resolution) in historyAddress()
//
// Passes (compute, every access declared on the graph by byte range, no manual barriers):
//   clouds.noise.shape / clouds.noise.detail / clouds.weather   the tileable noise volumes and weather map,
//                     baked once (and again only when the noise settings change or after invalidateNoise())
//   clouds.march        one thread per block x block tile: this frame's Bayer pixel raymarched (sun light march
//                       with the atmosphere's sun illuminance, sky ambient, multiple-scattering octaves)
//   clouds.reconstruct  one thread per cloud pixel: fresh sample / reprojected + clamped history / running mean
//                       (ping-pong history sections of the frame buffer)
//   clouds.composite    one thread per output pixel: background (a caller buffer or the sky) x T + the cloud
//                       radiance through the atmosphere's aerial perspective at the cloud depth
// Kernels: Slang primary (-fp-mode precise) with GLSL twins, embedded by cmake/rp_wp83.cmake. Steady-state
// frames make no heap allocation (buffers follow the sizes; pass records live in a fixed array).

#include <fuse/renderer/clouds/cloud_reference.hpp>
#include <fuse/renderer/clouds/cloud_types.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::clouds {

enum class CloudKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct CloudCapabilities {
    bool clouds = false;
    const char* reason = "no device"; ///< "ok" when usable
};

CloudCapabilities queryCloudCapabilities(const VulkanDevice* device);

enum class CloudSection : u8 {
    Shape = 0, ///< noise buffer
    Detail,
    Weather,
    Fresh,    ///< frame buffer
    History0,
    History1,
    Result,
};
inline constexpr u32 kCloudSectionCount = 7u;

/// Byte layout of the two persistent buffers (every section 256-aligned, f32x4 texels).
struct CloudBufferLayout {
    u64 offset[kCloudSectionCount] = {}; ///< within the noise buffer (Shape..Weather) or the frame buffer
    u64 bytes[kCloudSectionCount] = {};
    u64 noiseBytes = 0;
    u64 frameBytes = 0;

    static CloudBufferLayout compute(const CloudParams& p);
};

struct CloudGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr; ///< the pipelines use its layout
    CloudKernelLanguage language = CloudKernelLanguage::Auto;
    u32 framesInFlight = 3; ///< CloudParams ring slots
};

struct CloudGraphRefs {
    rg::BufferRef noise; ///< noise volumes + weather map
    rg::BufferRef frame; ///< fresh samples, history ping-pong, composited result
};

/// Other packages' buffers the kernels read (declared StorageRead so the graph orders them).
struct CloudInputs {
    rg::BufferRef atmosphereLuts; ///< AtmosphereGraphRefs::luts (after AtmosphereGpu::addPasses) when frame.atmosphere
    rg::BufferRef background;     ///< the buffer behind CloudFrame::backgroundAddress
    rg::BufferRef depth;          ///< the buffer behind CloudFrame::depthAddress
};

struct CloudStats {
    u32 noiseBakes = 0;     ///< frames that ran the noise passes
    u32 noiseRebuilds = 0;  ///< noise buffer (re)allocations
    u32 frameRebuilds = 0;  ///< frame buffer (re)allocations
    u32 retired = 0;
    u32 passes = 0;         ///< passes added this frame
    u32 marchedPixels = 0;  ///< this frame
    bool noiseRan = false;  ///< this frame
};

class VolumetricClouds {
public:
    VolumetricClouds() = default;
    ~VolumetricClouds();
    VolumetricClouds(const VolumetricClouds&) = delete;
    VolumetricClouds& operator=(const VolumetricClouds&) = delete;

    /// Fails (false, nothing created) without a capable device or a built kernel of the requested language, or
    /// in the stub backend.
    bool init(const CloudGpuDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Resolves this frame's CloudParams into its ring slot (with the temporal state of the previous frame);
    /// (re)allocates buffers when sizes change. Every beginFrame must be followed by addPasses and the graph's
    /// execution. False for invalid settings / allocation failure.
    bool beginFrame(u64 frameSerial, const CloudSettings& settings, const CloudFrame& frame);
    /// Re-bakes the noise volumes on the next addPasses.
    void invalidateNoise() { m_noiseDirty = true; }
    /// Drops the history (camera cut): the next frame starts from its fresh samples.
    void resetHistory() { m_history.historyValid = false; }

    CloudGraphRefs importInto(rg::Graph& graph);
    void addPasses(rg::Graph& graph, const CloudGraphRefs& refs, const CloudInputs& inputs = {});
    /// Copies one section into `dst` at `dstOffset` (after addPasses).
    void addCopy(rg::Graph& graph, const CloudGraphRefs& refs, CloudSection which, rg::BufferRef dst, u64 dstOffset);
    u64 sectionBytes(CloudSection which) const { return m_layout.bytes[static_cast<u32>(which)]; }
    /// Runs the single-ray integration + density for `count` probes (kClProbeInputs / kClProbeOutputs f32x4
    /// each, BDA; declare `src` / `dst` as buffers of the graph). After addPasses.
    void addProbe(rg::Graph& graph, const CloudGraphRefs& refs, const CloudInputs& inputs, rg::BufferRef srcBuffer,
                  u64 srcAddress, rg::BufferRef dstBuffer, u64 dstAddress, u32 count);

    /// Destroys buffers retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection / consumers -----------------------------------------------------------------------------
    u64 frameAddress() const { return m_frameAddress; } ///< this frame's CloudParams
    const CloudParams& params() const { return m_params; }
    const CloudBufferLayout& layout() const { return m_layout; }
    /// The section this frame's reconstruction writes (History0 / History1).
    CloudSection historySection() const { return m_historyOut; }
    u64 resultAddress() const { return m_params.result; }
    u64 historyAddress() const { return m_params.historyOut; }
    const CloudStats& stats() const { return m_stats; }
    const char* kernelLanguage() const { return m_language; }
    const CloudHistoryState& historyState() const { return m_history; }

private:
    struct Retired {
        Buffer buffer{};
        u64 serial = 0;
    };
    struct PassRecord {
        VolumetricClouds* self = nullptr;
        CloudPush push{};
        u32 kernel = 0;
        u32 groups[3] = {1u, 1u, 1u};
        u64 copySrc = 0;
        u64 copyDst = 0;
        u64 copyBytes = 0;
        rg::BufferRef copyFrom{};
        rg::BufferRef copyTo{};
    };
    enum Kernel : u32 { kShape = 0, kDetail, kWeather, kMarch, kReconstruct, kComposite, kProbe, kKernelCount };
    static constexpr u32 kMaxPasses = 32u;

    bool createPipelines();
    bool createBuffer(u64 bytes, const char* name, Buffer& target, u8& queue);
    void retire(Buffer& buffer);
    PassRecord* nextRecord(u32 kernel);
    rg::BufferRange range(CloudSection which) const;
    static void recordDispatch(const rg::PassContext& context, void* user);
    static void recordCopy(const rg::PassContext& context, void* user);

    CloudGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    CloudParams m_params{};
    CloudParams m_noiseParams{}; ///< the noise settings the volumes were baked with
    bool m_noiseDirty = true;
    CloudHistoryState m_history{};
    CloudHistoryState m_nextHistory{};
    CloudSection m_historyOut = CloudSection::History0;
    CloudBufferLayout m_layout{};
    Buffer m_noise{};
    Buffer m_frame{};
    u8 m_noiseQueue = rg::kNoQueue;
    u8 m_frameQueue = rg::kNoQueue;
    std::vector<Retired> m_retired;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;
    void* m_layoutHandle = nullptr;
    void* m_pipelines[kKernelCount] = {};
    CloudStats m_stats{};
};

} // namespace fuse::renderer::clouds
