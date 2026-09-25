#pragma once

// RL-5.5 radiance denoiser on render graph v2 (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.5; algorithm: rdn_core.h, host
// vocabulary: radiance_denoise.hpp). One compute kernel (Slang primary, -fp-mode precise; GLSL twin), one pipeline,
// every pass a dispatch with its RdnPush; every access declared on the graph, no manual barriers.
//
//   RadianceDenoiser dn;
//   dn.init({device, allocator, bindless});
//   dn.setSettings(settings);                                   // RdnSettings (gradients = true: A-SVGF)
//   dn.beginFrame(serial, {w, h, diffuseAddr, specularAddr, normalAddr, depthAddr, motionAddr, instanceAddr,
//                          gradientAddr, camera, prevCamera});
//   RdnGraphRefs r = dn.importInto(graph);
//   dn.addPasses(graph, r, inputs);                            // inputs: the buffers holding those addresses
//   // consumers: r.output at outputAddressD() / outputAddressS(): f32x4 (rgb, variance) per pixel, demodulated
//
// Passes: rdn.prepare, rdn.preblur, rdn.gradient.prepare / .atrous (A-SVGF), rdn.temporal, rdn.mip (x 3),
// rdn.historyfix, rdn.variance, rdn.atrous (x N). The state (guide, aux, colour histories, moments) ping-pongs by
// frame parity in one persistent buffer; resize or reset() starts every pixel over. Steady-state frames make no heap
// allocation. hostVisible: every buffer is host-visible and mapped (the Relight post reads the output on the host;
// the parity gates read every section).

#include <fuse/renderer/denoise/radiance_denoise.hpp>
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

enum class RdnKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct RadianceDenoiserDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr; ///< the pipeline layout carries its set (descriptor-buffer backends)
    RdnKernelLanguage language = RdnKernelLanguage::Auto;
    u32 framesInFlight = 3;
    bool keepIntermediates = false; ///< RdnBufferLayout::compute
    bool hostVisible = false; ///< state / work / output host-visible and mapped
    bool hostVisibleOutput = false; ///< only the output host-visible and mapped
    const char* name = "radiance_denoiser";
};

/// This frame's inputs: device addresses of the caller's buffers (layouts: RdnReferenceInputs) and the cameras.
struct RdnFrameDesc {
    u32 width = 0;
    u32 height = 0;
    u64 diffuse = 0;
    u64 specular = 0;
    u64 normal = 0;
    u64 depth = 0;
    u64 motion = 0;
    u64 instance = 0; ///< settings.instanceTest
    u64 gradient = 0; ///< settings.gradients
    RdnCamera camera{};
    RdnCamera prevCamera{};
    bool reset = false;
};

/// The buffers the input addresses live in (each read range declared on the
/// passes that read inputs). The same buffer may hold several inputs (the
/// Relight path tracer's sections): list it once.
struct RdnGraphInputs {
    static constexpr u32 kMax = 8u;
    rg::BufferRef buffers[kMax]{};
    rg::BufferRange ranges[kMax]{};
    u32 count = 0;
    void add(rg::BufferRef b, rg::BufferRange r = {}) {
        if (count < kMax) {
            buffers[count] = b;
            ranges[count] = r;
            ++count;
        }
    }
};

struct RdnGraphRefs {
    rg::BufferRef state;
    rg::BufferRef work;
    rg::BufferRef output;
    bool valid = false;
};

struct RdnStats {
    u32 rebuilds = 0; ///< (re)allocations of the arenas
    u32 retired = 0;
    u32 passes = 0; ///< added this frame
    bool history = false;
};

class RadianceDenoiser {
public:
    RadianceDenoiser() = default;
    ~RadianceDenoiser();
    RadianceDenoiser(const RadianceDenoiser&) = delete;
    RadianceDenoiser& operator=(const RadianceDenoiser&) = delete;

    /// False (nothing created) without BDA + int64, without a built kernel, or in the stub backend.
    bool init(const RadianceDenoiserDesc& desc);
    void destroy();
    bool valid() const { return m_initialized; }
    const char* kernelLanguage() const { return m_language; }

    void setSettings(const RdnSettings& settings) { m_settings = settings; }
    const RdnSettings& settings() const { return m_settings; }
    void reset() { m_historyValid = false; }

    /// Writes this frame's constants; follows the extent. False when an input is missing or an allocation failed.
    bool beginFrame(u64 frameSerial, const RdnFrameDesc& frame);
    RdnGraphRefs importInto(rg::Graph& graph);
    void addPasses(rg::Graph& graph, const RdnGraphRefs& refs, const RdnGraphInputs& inputs);
    u32 collectRetired(u64 completedSerial);

    // --- inspection --------------------------------------------------------------------------------------------------
    u32 width() const { return m_layout.width; }
    u32 height() const { return m_layout.height; }
    const RdnBufferLayout& layout() const { return m_layout; }
    /// This frame's constants (device addresses).
    const RdnFrame& constants() const { return m_constants; }
    u32 parity() const { return m_parity; }
    u32 passCount() const { return m_passCount; }
    const RdnPassDesc& pass(u32 k) const { return m_passes[k]; }
    const Buffer& stateBuffer() const { return m_state; }
    const Buffer& workBuffer() const { return m_work; }
    const Buffer& outputBuffer() const { return m_output; }
    u64 outputAddressD() const { return m_output.deviceAddress + m_layout.outD; }
    u64 outputAddressS() const { return m_output.deviceAddress + m_layout.outS; }
    /// Host views of the output (hostVisible / hostVisibleOutput; valid after the frame completed).
    const f32* mappedOutputD() const;
    const f32* mappedOutputS() const;
    const RdnStats& stats() const { return m_stats; }

private:
    struct Retired {
        Buffer buffers[3]{};
        u64 serial = 0;
    };
    struct PassRecord {
        RadianceDenoiser* self = nullptr;
        RdnPush push{};
        u32 groups[2] = {1u, 1u};
    };

    bool createPipeline();
    bool createResources(u32 width, u32 height);
    void destroyRetired(Retired& r);
    static void recordDispatch(const rg::PassContext& context, void* user);

    RadianceDenoiserDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    RdnSettings m_settings{};
    bool m_historyValid = false;
    u32 m_parity = 1u;
    RdnBufferLayout m_layout{};
    Buffer m_state{};
    Buffer m_work{};
    Buffer m_output{};
    u8 m_stateQueue = rg::kNoQueue;
    u8 m_workQueue = rg::kNoQueue;
    u8 m_outputQueue = rg::kNoQueue;
    std::vector<Retired> m_retired;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    RdnFrame m_constants{};
    RdnPassDesc m_passes[kRdnMaxPasses]{};
    u32 m_passCount = 0;
    PassRecord m_records[kRdnMaxPasses]{};
    void* m_layoutHandle = nullptr; ///< VkPipelineLayout
    void* m_pipeline = nullptr; ///< VkPipeline
    RdnStats m_stats{};
};

/// True when the device can run the denoiser (BDA + shaderInt64); `reason` explains a false.
bool rdn_supported(const VulkanDevice* device, const char** reason = nullptr);

} // namespace fuse::renderer::denoise
