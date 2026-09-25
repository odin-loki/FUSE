#pragma once

// Screen-space radiance cascades on Vulkan compute, render graph v2 (the WP-6.5 follow-up; the algorithm and the
// depth-buffer thickness model are documented in ssrc_reference.hpp, the CPU twin of every kernel).
//
//   ssrc.beginFrame(serial, settings, camera, ambient, {&rt0, &rt1, &rt2, &rt4, &lighting.outputImage()});
//   SsrcGraphRefs refs = ssrc.importInto(graph);
//   ssrc.addPasses(graph, refs, {rt0Ref, rt1Ref, rt2Ref, rt4Ref, litRef});   // after light.shade
//   // refs.output: RGBA16F, (indirect diffuse rgb, AO) or (settings.compose) the composed lit image
//
// Passes (compute, all declared on the graph, no manual barriers; per-pixel data and the cascades in one persistent
// work buffer reached through BDA):
//   ssrc.prepare   G-buffer + lit image -> geo (linear depth, view normal), lit, diffuse albedo + material AO,
//                  albedo (8 x 8 tiles; the oracle is ssfx_gpu::prepare_pixel, as WP-6.3's ssfx.prepare)
//   ssrc.cascade   one per cascade, top first: one record (probe, direction) per thread (ssrc_cascade_record)
//   ssrc.gather    cascade 0 -> indirect diffuse + AO per pixel, the output image (ssrc_gather_pixel)
// Storage follows the extent and the cascade layout (work buffer + host-written direction table); both are retired,
// never touched in steady state, so steady-state frames make no heap allocation. Kernels: Slang primary (-fp-mode
// precise) with GLSL twins (`precise`), embedded by cmake/rp_ssrc.cmake.

#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/ssrc/ssrc_reference.hpp>
#include <fuse/renderer/ssrc/ssrc_types.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::ssrc {

enum class SsrcKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct SsrcCapabilities {
    bool ssrc = false;
    const char* reason = "no device"; ///< "ok" when usable
};

SsrcCapabilities querySsrcCapabilities(const VulkanDevice* device);

/// Byte layout of the work buffer (every section 256-aligned).
struct SsrcBufferLayout {
    u64 geo = 0;      ///< f32x4
    u64 lit = 0;      ///< f32x4
    u64 diffuse = 0;  ///< f32x4
    u64 albedo = 0;   ///< f32x4
    u64 indirect = 0; ///< f32x4
    u64 records[2] = {0, 0}; ///< u32x2 per record (maxRecords each)
    u64 maxRecords = 0;
    u64 workBytes = 0;

    static SsrcBufferLayout compute(const SsrcLayoutInfo& info, u32 width, u32 height);
};

struct SsrcGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    SsrcKernelLanguage language = SsrcKernelLanguage::Auto;
    u32 framesInFlight = 3; ///< frame-constant ring slots
};

/// This frame's inputs (RT4's extent sets the extent; every image must have it).
struct SsrcFrameImages {
    const Texture* normalAo = nullptr;   ///< RT0 RGBA16F (signed-octahedral world normal, material AO)
    const Texture* albedo = nullptr;     ///< RT1 RGBA8
    const Texture* roughMetal = nullptr; ///< RT2 RGBA8 (roughness, metallic)
    const Texture* depth = nullptr;      ///< RT4 R32F device depth
    const Texture* lit = nullptr;        ///< the WP-2.1 lit image (RGBA16F)
    /// Optional BDA of a gi_gpu::DdgiVolumeView (DdgiGpu::volumeAddress(), the address LightingFrameDesc::ddgi takes);
    /// used when SsrcSettings::ddgi. Declare the buffers it reaches with SsrcGraphInputs::ddgi or, from the owner,
    /// DdgiGpu::addSamplingUse before the ssrc.* passes.
    u64 ddgiVolume = 0;
    /// Optional f32x4-per-pixel buffer (BDA) receiving the output image's value before RGBA16F rounding; declare it
    /// with SsrcGraphInputs::dump.
    u64 dumpAddress = 0;
};

inline constexpr u32 kSsrcMaxDdgiBuffers = 4u;

/// The graph refs of the images of SsrcFrameImages (as imported by their owners, e.g. MaterialResolve::importInto's
/// gbuffer[] and ClusteredLighting::importInto's output) and of the optional buffers.
struct SsrcGraphInputs {
    rg::TextureRef normalAo;
    rg::TextureRef albedo;
    rg::TextureRef roughMetal;
    rg::TextureRef depth;
    rg::TextureRef lit;
    rg::BufferRef dump;
    /// Buffers the DDGI volume view reaches (frame constants, irradiance / distance atlases, probe data); declared
    /// StorageRead on every ssrc.cascade pass.
    rg::BufferRef ddgi[kSsrcMaxDdgiBuffers];
};

struct SsrcGraphRefs {
    rg::BufferRef work;
    rg::TextureRef output; ///< RGBA16F
};

/// Work-buffer section an addCopy reads (gates / debugging).
enum class SsrcCopySource : u8 {
    Geo = 0,  ///< f32x4
    Lit,      ///< f32x4
    Diffuse,  ///< f32x4
    Albedo,   ///< f32x4
    Indirect, ///< f32x4
    Records0, ///< u32x2 per record (cascades 0, 2, ...: after the frame, cascade 0's)
    Records1, ///< u32x2 per record (cascade 1's after the frame)
};

struct SsrcGpuStats {
    u32 workRebuilds = 0; ///< work buffer + direction table rebuilds (extent / layout changes)
    u32 outputRebuilds = 0;
    u32 inputBinds = 0;
    u32 retired = 0;
    u32 passes = 0; ///< passes added this frame
    u32 cascades = 0;
};

class SsrcGpu {
public:
    SsrcGpu() = default;
    ~SsrcGpu();
    SsrcGpu(const SsrcGpu&) = delete;
    SsrcGpu& operator=(const SsrcGpu&) = delete;

    /// False (nothing created) without a capable device, without a built kernel of the requested language, or in
    /// the stub backend.
    bool init(const SsrcGpuDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Resolves and writes this frame's constants (ring slot of `frameSerial`); follows the extent / cascade layout
    /// (work buffer, direction table, output) and the input images (bindless slots). False when an input is missing,
    /// the extents differ, the camera or the settings are invalid, settings.ddgi without a volume, or a
    /// (re)allocation failed.
    bool beginFrame(u64 frameSerial, const SsrcSettings& settings, const ssfx_gpu::SsfxCameraDesc& camera,
                    const f32 (&ambient)[3], const SsrcFrameImages& images);

    SsrcGraphRefs importInto(rg::Graph& graph);
    /// ssrc.prepare, ssrc.cascade x cascades (top first), ssrc.gather.
    void addPasses(rg::Graph& graph, const SsrcGraphRefs& refs, const SsrcGraphInputs& inputs);
    /// Copies a work-buffer section into `dst` at `dstOffset` (after addPasses).
    void addCopy(rg::Graph& graph, const SsrcGraphRefs& refs, SsrcCopySource what, rg::BufferRef dst, u64 dstOffset);
    /// Bytes addCopy(what) copies for the current layout.
    u64 copyBytes(SsrcCopySource what) const;

    /// Destroys buffers / images / bindless slots retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection ------------------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    const Texture& outputImage() const { return m_output.image; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    const SsrcBufferLayout& layout() const { return m_layout; }
    const SsrcLayoutInfo& layoutInfo() const { return m_info; }
    const SsrcGpuStats& stats() const { return m_stats; }
    const SsrcFrameConstants& constants() const { return m_constants; }
    /// The direction table the kernels read (host copy, buildDirectionTable).
    const std::vector<f32>& directions() const { return m_dirScratch; }
    /// BDA of the indirect section (f32x4 per pixel: indirect rgb, AO; valid after ssrc.gather of this frame) for
    /// consumers that compose it themselves (declare a StorageRead of SsrcGraphRefs::work).
    u64 indirectAddress() const { return m_work.deviceAddress != 0u ? m_work.deviceAddress + m_layout.indirect : 0u; }

private:
    struct Output {
        Texture image{};
        BindlessSlotHandle slot{};
        u32 layout = 0;
        u8 queue = rg::kNoQueue;
    };
    static constexpr u32 kInputCount = 5u; ///< RT4, RT0, RT1, RT2, lit
    struct Retired {
        Buffer buffers[2]{};
        Texture image{};
        BindlessSlotHandle slots[1u + kInputCount]{};
        u64 serial = 0;
    };
    struct PassRecord {
        SsrcGpu* self = nullptr;
        SsrcPush push{};
        u32 kernel = 0;
        u32 groups[3] = {1u, 1u, 1u};
        u64 copySrc = 0;
        u64 copyDst = 0;
        u64 copyBytes = 0;
        rg::BufferRef copyFrom{};
        rg::BufferRef copyTo{};
    };
    enum Kernel : u32 { kPrepare = 0, kCascade, kGather, kKernelCount };
    static constexpr u32 kMaxPasses = kSsrcMaxCascades + 12u;

    bool createPipelines();
    bool rebuildWork(const SsrcFrameConstants& c, const SsrcLayoutInfo& info, u32 width, u32 height);
    bool createOutput(u32 width, u32 height);
    bool bindInput(u32 index, const Texture* image);
    void retire(Retired r);
    void destroyRetired(Retired& r);
    PassRecord* nextRecord(u32 kernel);
    static void recordDispatch(const rg::PassContext& context, void* user);
    static void recordCopy(const rg::PassContext& context, void* user);

    SsrcGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    SsrcBufferLayout m_layout{};
    SsrcLayoutInfo m_info{};
    SsrcFrameConstants m_layoutKey{}; ///< the constants the work buffer / direction table were built for
    Buffer m_work{};
    u8 m_workQueue = rg::kNoQueue;
    Buffer m_dirs{};
    std::vector<f32> m_dirScratch;
    Output m_output{};
    std::vector<Retired> m_retired;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    SsrcFrameConstants m_constants{};
    bool m_hasDump = false;

    void* m_inputImages[kInputCount] = {};
    BindlessSlotHandle m_inputSlots[kInputCount]{};
    u32 m_inputHandles[kInputCount] = {};

    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;

    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 40-byte push, compute)
    void* m_pipelines[kKernelCount] = {};

    SsrcGpuStats m_stats{};
};

} // namespace fuse::renderer::ssrc
