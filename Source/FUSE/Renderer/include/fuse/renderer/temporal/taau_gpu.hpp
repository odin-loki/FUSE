#pragma once

// WP-4.1 Vulkan dispatch of the native TAAU kernel (taau_kernel.hpp, kernel "taau"; Batch 9). The kernel body is
// unchanged: shaders/temporal/tm_taau.{comp,slang} (+ tm_common.{glsl,slang}) are line-for-line twins of
// taau_kernel::Kernel and the taa_common helpers it calls (GLSL `precise`, Slang -fp-mode precise), and this
// class drives them with the exact state machine of the CPU driver TaauUpscaler (taau.hpp):
//
//   * display-resolution history ping-pong (f32 RGB exposed + accumulated weight, two device buffers);
//   * the previous frame's render-resolution depth / motion (copies kept by "taau.keep" after the resolve,
//     read as prev_depth / prev_motion only while the history is valid, like the CPU driver's spans);
//   * history invalidated by reset_history, a resolution change or invalidate(); valid after one resolve;
//   * camera-aware disocclusion depth from the unjittered cameras (same host expressions as taau.cpp).
//
//   taau.beginFrame(serial, {resolution, jitter, exposure, reset, camera, previousCamera, color, depth, motion});
//   TaauGraphRefs t = taau.importInto(graph);
//   taau.addResolve(graph, t, {colorRef, depthRef, motionRef});     // taau.resolve + taau.keep
//
// Inputs: colour = any bindless sampled 2D image (render resolution, .rgb, texelFetch: e.g. the WP-2.1 lit
// image); depth = f32 per render pixel, motion = f32 x 2 per render pixel (UV, current - previous, unjittered),
// both at offset 0 of their buffers (TemporalMotion's buffers, or any producer's); optional reactive /
// transparency-composition masks as sampled images (.r). Output: RGBA16F display-resolution storage image
// (+ an optional f32x4 dump for the parity gates). All passes on the render graph; no manual barriers;
// steady-state frames make no heap allocations.

#include <fuse/math/vec.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/taa/taau_kernel.hpp>
#include <fuse/renderer/temporal/temporal_types.hpp>
#include <fuse/renderer/upscale/upscale_inputs.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::temporal {

enum class TemporalKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct TemporalCapabilities {
    bool temporal = false;
    const char* reason = "no device"; ///< "ok" when usable
};

/// BDA + shaderInt64 + RGBA16F storage images (the TAAU output) on `device`.
TemporalCapabilities queryTemporalCapabilities(const VulkanDevice* device);

/// Camera terms of taau_kernel::Params (has_camera, tan_half_x / _y, cur_to_prev_view) with the CPU driver's
/// expressions (taau.cpp): has_camera only for a positive fov / aspect and an invertible current view.
void taau_camera_terms(const UpscaleCamera& camera, const UpscaleCamera& previousCamera, taau_kernel::Params& p);

/// Scalar part of `p` (resolution, jitter, exposure, history / prev flags, camera terms, settings) into the GPU
/// record; addresses and handles are left untouched.
void pack_taau_constants(const taau_kernel::Params& p, TaauFrameConstants& out);

struct TaauGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    UpscaleResolution resolution{}; ///< initial (beginFrame follows the frame's)
    taau_kernel::Settings settings{};
    TemporalKernelLanguage language = TemporalKernelLanguage::Auto;
    u32 framesInFlight = 3; ///< frame-constant ring slots
    const char* name = "taau_gpu";
};

struct TaauGpuFrameDesc {
    UpscaleResolution resolution{};
    math::Vec2 jitter_px{};     ///< render-pixel jitter of this frame (temporal_types.hpp convention)
    f32 exposure = 1.f;
    bool reset_history = false; ///< camera cut / teleport
    UpscaleCamera camera{};          ///< unjittered, this frame (disocclusion depth; fov / aspect / view)
    UpscaleCamera previous_camera{}; ///< unjittered, previous frame
    u32 color = 0;        ///< bindless sampled-image handle (render resolution)
    u32 reactive = 0;     ///< optional bindless sampled-image handle (.r), 0 = none
    u32 transparency = 0; ///< optional bindless sampled-image handle (.r), 0 = none
    u64 depth = 0;        ///< BDA of f32[render pixels] (buffer offset 0)
    u64 motion = 0;       ///< BDA of f32x2[render pixels] (buffer offset 0)
};

/// This frame's input resources on the graph (declared by addResolve).
struct TaauGpuInputs {
    rg::TextureRef color;
    rg::TextureRef reactive;     ///< invalid when the frame has none
    rg::TextureRef transparency; ///< invalid when the frame has none
    rg::BufferRef depth;
    rg::BufferRef motion;
};

struct TaauGraphRefs {
    rg::BufferRef history[2];
    rg::BufferRef prevDepth;
    rg::BufferRef prevMotion;
    rg::TextureRef output; ///< RGBA16F display image
};

struct TaauGpuStats {
    u32 rebuilds = 0;       ///< resource (re)creations (1 after init)
    u32 resolvePasses = 0;  ///< this frame
    u32 retired = 0;
    bool historyUsed = false; ///< this frame read a valid history
};

class TaauGpu {
public:
    TaauGpu() = default;
    ~TaauGpu();
    TaauGpu(const TaauGpu&) = delete;
    TaauGpu& operator=(const TaauGpu&) = delete;

    /// Fails (false, nothing created) without a capable device, without a built kernel of the requested
    /// language, with an invalid resolution, or in the stub backend.
    bool init(const TaauGpuDesc& desc);
    /// The caller must have retired every frame that used the resources.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Writes this frame's constants (ring slot of `frameSerial`); follows the frame's resolution (a change
    /// rebuilds the history / prev / output resources and drops the history). False on invalid input.
    bool beginFrame(u64 frameSerial, const TaauGpuFrameDesc& frame);
    TaauGraphRefs importInto(rg::Graph& graph);
    /// "taau.resolve" (display pixels, 8 x 8 tiles) + "taau.keep" (depth / motion -> prev copies). `dump`, when
    /// valid: also writes f32x4 (rgb, 0) per display pixel at `dumpAddress` (= the dump buffer's address +
    /// `dumpOffset`). Advances the history ping-pong: the graph must execute before the next beginFrame.
    void addResolve(rg::Graph& graph, const TaauGraphRefs& refs, const TaauGpuInputs& inputs, rg::BufferRef dump = {},
                    u64 dumpAddress = 0, u64 dumpOffset = 0);
    void invalidate() { m_valid = false; }

    /// Destroys resources / bindless slots retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection ------------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    taau_kernel::Settings& settings() { return m_settings; }
    const taau_kernel::Settings& settings() const { return m_settings; }
    const UpscaleResolution& resolution() const { return m_resolution; }
    bool hasHistory() const { return m_valid; }
    const Texture& outputImage() const { return m_res.output; }
    u32 outputStorageHandle() const;
    /// History buffer the NEXT resolve reads (after addResolve: the one it wrote).
    const Buffer& historyBuffer() const { return m_res.history[m_current]; }
    /// Index (into TaauGraphRefs::history) of historyBuffer().
    u32 historyIndex() const { return m_current; }
    const TaauGpuStats& stats() const { return m_stats; }

private:
    struct Resources {
        Buffer history[2]{};
        Buffer prevDepth{};
        Buffer prevMotion{};
        Texture output{};
        BindlessSlotHandle outputSlot{};
        u8 queues[4] = {rg::kNoQueue, rg::kNoQueue, rg::kNoQueue, rg::kNoQueue};
        u32 outputLayout = 0;
        u8 outputQueue = rg::kNoQueue;
    };
    struct Retired {
        Resources res{};
        u64 serial = 0;
    };
    struct PassRecord {
        TaauGpu* self = nullptr;
        TemporalPush push{};
        u32 groups[2] = {1u, 1u};
        rg::BufferRef depth, motion, prevDepth, prevMotion;
        u64 depthBytes = 0;
        u64 motionBytes = 0;
    };

    bool createPipeline();
    bool createResources(Resources& r, const UpscaleResolution& resolution);
    void destroyResources(Resources& r);
    static void recordResolve(const rg::PassContext& context, void* user);
    static void recordKeep(const rg::PassContext& context, void* user);

    TaauGpuDesc m_desc{};
    taau_kernel::Settings m_settings{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    UpscaleResolution m_resolution{};
    Resources m_res{};
    std::vector<Retired> m_retired;
    u32 m_current = 0; ///< history buffer read by the next resolve
    bool m_valid = false;
    bool m_frameHistory = false; ///< history_valid of the begun frame
    bool m_begun = false;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    PassRecord m_record{};
    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 16-byte push, compute)
    void* m_pipeline = nullptr;
    TaauGpuStats m_stats{};
};

} // namespace fuse::renderer::temporal
