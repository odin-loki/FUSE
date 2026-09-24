#pragma once

// WP-6.4 in-tree denoiser: SVGF with the A-SVGF temporal gradients (papers and the algorithm:
// denoise_types.hpp / svgf_kernel.hpp), compute passes on render graph v2, for shadows (scalar),
// reflections and GI (RGB). The CPU reference of every pass is svgf_kernel.hpp (SvgfReference runs the chain).
//
//   SvgfDenoiser gi;
//   gi.init({device, allocator, bindless});
//   gi.setSettings(svgf_preset(DenoiseSignal::Gi));        // or Shadow / Reflection; gradients = true: A-SVGF
//   gi.beginFrame(serial, {w, h, signalAddr, motion.motionAddress(), motion.depthAddress(), normalAddr,
//                          &resolve.normalImage() /* or nullptr */, gradientAddr});
//   DenoiseGraphRefs d = gi.importInto(graph);
//   gi.addPasses(graph, d, {signalRef, motionRefs.motion, motionRefs.depth, normalRef, normalImageRef, gradientRef});
//   // consumers: d.output (f32x4 (rgb, variance) per pixel, outputAddress()) or d.outputImage (RGBA16F,
//   // outputImageHandle()); the signal is demodulated, so the WP-2.1 lighting remodulates it by the albedo.
//
// Inputs (conventions: denoise_types.hpp): the noisy signal (f32 or f32x4 per pixel), the WP-4.1
// temporal.motion buffers (UV motion, linear depth), the normal as an f32x4 buffer or the WP-1.5 G-buffer RT0
// image (signed octahedral, RGBA16F), and for A-SVGF the producer's gradient samples (f32x4 per 3 x 3 stratum).
//
// Passes (compute, 8 x 8, every access declared on the graph, no manual barriers):
//   denoise.guide                     guide (n, z) + depth gradient into the state / work buffers
//   denoise.gradient.prepare/.atrous  A-SVGF only: stratum records + gradientIterations a-trous iterations
//   denoise.temporal                  reprojection, disocclusion, colour + moments accumulation
//   denoise.variance                  temporal / spatial variance
//   denoise.atrous (x N)              edge-aware wavelet iterations; iteration historyTap also writes the colour
//                                     history, the last one writes the output buffer and the output image
// State (guide, colour history, moments) ping-pongs by frame parity in one persistent buffer; resize or
// reset() starts every pixel over. Kernels: Slang primary (-fp-mode precise) with GLSL twins (`precise`),
// embedded by cmake/rp_wp64.cmake. Steady-state frames make no heap allocation.

#include <fuse/renderer/denoise/denoise_types.hpp>
#include <fuse/renderer/denoise/svgf_reference.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::denoise {

enum class DenoiseKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct DenoiseCapabilities {
    bool denoise = false;
    const char* reason = "no device"; ///< "ok" when usable
};

/// Needs buffer device address + shaderInt64 and RGBA16F storage images.
DenoiseCapabilities queryDenoiseCapabilities(const VulkanDevice* device);

/// Byte offsets of the state / work buffers for an extent (sections 256-aligned).
struct SvgfBufferLayout {
    // state buffer (persistent)
    u64 guide[2] = {0, 0};
    u64 hist[2] = {0, 0};
    u64 mom[2] = {0, 0};
    u64 stateBytes = 0;
    // work buffer (per frame)
    u64 gradZ = 0;
    u64 accum = 0;
    u64 variance = 0;
    u64 atrous[kDenoiseMaxAtrous] = {}; ///< iteration k's result (the last iteration writes the output buffer)
    u64 gradient[kDenoiseMaxGradientIterations + 1u] = {}; ///< 0 = prepare, k = after iteration k
    u64 workBytes = 0;
    u64 outputBytes = 0;
    u32 width = 0;
    u32 height = 0;
    u32 strataW = 0;
    u32 strataH = 0;
    bool keepIntermediates = false;

    /// keepIntermediates: every a-trous / gradient iteration gets its own section (parity gates);
    /// otherwise they ping-pong through two.
    static SvgfBufferLayout compute(u32 width, u32 height, bool keepIntermediates);
};

struct SvgfDenoiserDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    DenoiseKernelLanguage language = DenoiseKernelLanguage::Auto;
    u32 framesInFlight = 3;         ///< frame-constant ring slots
    bool keepIntermediates = false; ///< see SvgfBufferLayout::compute
    const char* name = "svgf_denoiser";
};

/// This frame's inputs (addresses of the caller's buffers; the extent sets the denoiser extent).
struct DenoiseFrameDesc {
    u32 width = 0;
    u32 height = 0;
    u64 signal = 0;   ///< required: signal_stride(settings.signal) floats per pixel
    u64 motion = 0;   ///< required: f32x2 UV motion per pixel (TemporalMotion::motionAddress)
    u64 depth = 0;    ///< required: f32 linear depth per pixel (TemporalMotion::depthAddress)
    u64 normal = 0;   ///< f32x4 unit normal per pixel, or ...
    const Texture* normalImage = nullptr; ///< ... the G-buffer RT0 image (signed oct normal in xy); wins over `normal`
    u64 gradient = 0; ///< A-SVGF: f32x4 per stratum (required when settings.gradients)
    bool reset = false; ///< camera cut: start every pixel over this frame
};

/// Graph refs of the DenoiseFrameDesc resources (as imported by their owners).
struct DenoiseGraphInputs {
    rg::BufferRef signal;
    rg::BufferRef motion;
    rg::BufferRef depth;
    rg::BufferRef normal;       ///< when the normal is a buffer
    rg::TextureRef normalImage; ///< when the normal is the RT0 image
    rg::BufferRef gradient;     ///< A-SVGF
};

struct DenoiseGraphRefs {
    rg::BufferRef state;
    rg::BufferRef work;
    rg::BufferRef output;       ///< f32x4 (rgb, variance) per pixel
    rg::TextureRef outputImage; ///< RGBA16F (rgb, variance)
};

struct DenoiseStats {
    u32 stateRebuilds = 0; ///< (re)allocations of the state / work / output resources
    u32 inputBinds = 0;
    u32 retired = 0;
    u32 passes = 0;        ///< passes added this frame
    bool history = false;  ///< this frame reprojects the previous one
    bool gradients = false;
};

class SvgfDenoiser {
public:
    SvgfDenoiser() = default;
    ~SvgfDenoiser();
    SvgfDenoiser(const SvgfDenoiser&) = delete;
    SvgfDenoiser& operator=(const SvgfDenoiser&) = delete;

    /// Fails (false, nothing created) without a capable device, without a built kernel of the requested
    /// language, or in the stub backend.
    bool init(const SvgfDenoiserDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Takes effect on the next beginFrame (a change of the signal variant restarts the history).
    void setSettings(const SvgfSettings& settings);
    const SvgfSettings& settings() const { return m_settings; }
    /// The next frame starts without history (same as DenoiseFrameDesc::reset).
    void reset() { m_historyValid = false; }

    /// Writes this frame's constants; follows the extent (state / work / output) and the normal image.
    /// False when a required input is missing or an allocation failed.
    bool beginFrame(u64 frameSerial, const DenoiseFrameDesc& frame);
    DenoiseGraphRefs importInto(rg::Graph& graph);
    void addPasses(rg::Graph& graph, const DenoiseGraphRefs& refs, const DenoiseGraphInputs& inputs);

    /// Destroys resources retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection ------------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    u32 width() const { return m_layout.width; }
    u32 height() const { return m_layout.height; }
    const SvgfBufferLayout& layout() const { return m_layout; }
    /// This frame's constants (addresses included); SvgfReference / svgf_kernel take them verbatim.
    const DenoiseFrameConstants& constants() const { return m_constants; }
    u32 parity() const { return m_parity; } ///< state index this frame writes
    const Buffer& stateBuffer() const { return m_state; }
    const Buffer& workBuffer() const { return m_work; }
    const Buffer& outputBuffer() const { return m_output; }
    u64 outputAddress() const { return m_output.deviceAddress; }
    const Texture& outputImage() const { return m_image.image; }
    u32 outputImageHandle() const;
    const DenoiseStats& stats() const { return m_stats; }

private:
    struct Image {
        Texture image{};
        BindlessSlotHandle slot{};
        u32 layout = 0;
        u8 queue = rg::kNoQueue;
    };
    struct Retired {
        Buffer buffers[3]{};
        Texture image{};
        BindlessSlotHandle slots[2]{};
        u64 serial = 0;
    };
    struct PassRecord {
        SvgfDenoiser* self = nullptr;
        DenoisePush push{};
        u32 kernel = 0;
        u32 groups[2] = {1u, 1u};
    };
    enum Kernel : u32 { kGuide = 0, kGradient, kTemporal, kVariance, kAtrous, kKernelCount };
    static constexpr u32 kMaxPasses = 4u + kDenoiseMaxGradientIterations + kDenoiseMaxAtrous;

    bool createPipelines();
    bool createResources(u32 width, u32 height);
    bool bindNormalImage(const Texture* image);
    void retire(Retired r);
    void destroyRetired(Retired& r);
    PassRecord* nextRecord(u32 kernel);
    static void recordDispatch(const rg::PassContext& context, void* user);

    SvgfDenoiserDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    SvgfSettings m_settings{};
    DenoiseSignal m_lastSignal = DenoiseSignal::Gi;
    bool m_historyValid = false;
    u32 m_parity = 1u;
    SvgfBufferLayout m_layout{};
    Buffer m_state{};
    Buffer m_work{};
    Buffer m_output{};
    u8 m_stateQueue = rg::kNoQueue;
    u8 m_workQueue = rg::kNoQueue;
    u8 m_outputQueue = rg::kNoQueue;
    Image m_image{};
    std::vector<Retired> m_retired;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    DenoiseFrameConstants m_constants{};
    bool m_normalFromImage = false;

    void* m_normalImage = nullptr;
    BindlessSlotHandle m_normalSlot{};
    u32 m_normalHandle = 0;

    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;

    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 64-byte push, compute)
    void* m_pipelines[kKernelCount] = {};

    DenoiseStats m_stats{};
};

} // namespace fuse::renderer::denoise
