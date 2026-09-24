#pragma once

// WP-4.5 GPU post stack (renderer plan Phase 4 "post stack: exposure, bloom, tonemap (AgX / ACES),
// grading LUT, DoF, motion blur"; execution doc WP-4.5). Compute passes on render graph v2 that turn
// the lit HDR image (WP-2.1 ClusteredLighting::outputImage / WP-2.3 ForwardTransparency::colorImage,
// RGBA16F) into the display image, with the B5 CPU post code (renderer/postprocess/*) as the oracle.
//
//   post.beginFrame(serial, settings, {&hdrImage, &depthImage, &velocityImage});
//        // settings: settings_from_post_stack(stack) or settings_from_look(graph, look, ...)
//   PostGraphRefs refs = post.importInto(graph);
//   post.addPasses(graph, refs, {hdrRef, depthRef, velocityRef});
//
// Passes (all compute, declared on the graph, no manual barriers; f32x4 intermediates in one
// persistent work buffer reached through BDA):
//   post.load                     input image -> working HDR buffer
//   post.bloom.*                  prefilter, 2 x (L - 1) separable tent downsamples, 2 x (L - 1)
//                                 separable bilinear upsamples with the scatter combine, composite
//   post.dof.coc / .gather        CoC radius + disc weight per pixel, gather in the oracle's scatter order
//   post.mb.tile / .neighbor / .gather   tile max, 3 x 3 neighbour max, reconstruction filter
//   post.exposure.histogram/.adapt       64 x 64 tile histograms, sum + percentile metering + adaptation
//                                 (state stays on the GPU, reset with resetExposure())
//   post.display                  exposure, curve, tone map (ACES / Filmic / Reinhard / Neutral / AgX),
//                                 grade (PostStack stages or the Look 3D LUT), vignette, grain, sRGB ->
//                                 RGBA16F output (+ optional f32 dump)
// The spatial passes run in the settings' order (PostStack: bloom, DoF, motion blur; a Look: graph
// order), so a Look drives the stack node by node (LookEffectGraph), and a no-op Look resolves to the
// same constants as the PostStack it feeds (apply_look_to_post_stack) - bit-identical output.
//
// Kernels: Slang primary (-fp-mode precise) with GLSL twins (`precise`), embedded by cmake/rp_wp45.cmake.
// Steady-state frames make no heap allocation (buffers and the output follow the input extent; the
// LUT buffer follows the LUT size).

#include <fuse/renderer/look/effect_graph.hpp>
#include <fuse/renderer/look/look_params.hpp>
#include <fuse/renderer/look/lut3d.hpp>
#include <fuse/renderer/postprocess/gpu/post_gpu_reference.hpp>
#include <fuse/renderer/postprocess/gpu/post_gpu_types.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::post_gpu {

enum class PostKernelLanguage : u8 {
    Auto = 0, ///< Slang if built, else GLSL
    Slang,
    Glsl,
};

struct PostCapabilities {
    bool post = false;
    const char* reason = "no device"; ///< "ok" when usable
};

PostCapabilities queryPostCapabilities(const VulkanDevice* device);

/// Byte layout of the work buffer for an extent (every section 256-aligned).
struct PostBufferLayout {
    u64 hdr[2] = {0, 0};                  ///< f32x4 ping-pong working images
    u64 down[kPostMaxBloomLevels] = {};   ///< bloom pyramid (level 0 = prefiltered full resolution)
    u32 levelW[kPostMaxBloomLevels] = {};
    u32 levelH[kPostMaxBloomLevels] = {};
    u32 levels = 0;                       ///< pyramid levels reserved (down to 1 x 1)
    u64 tmp = 0;                          ///< separable-pass intermediate
    u64 up[2] = {0, 0};                   ///< upsample chain (level >= 1)
    u64 bloom = 0;                        ///< full-resolution bloom
    u64 cocs = 0;                         ///< f32x2 per pixel
    u64 tileMax = 0;                      ///< f32x2 per motion-blur tile (tile size >= 1)
    u64 neighborMax = 0;
    u64 histPartials = 0;                 ///< u32 [groups][kPostHistMaxBins]
    u32 histGroupsX = 0;
    u32 histGroups = 0;
    u64 workBytes = 0;

    static PostBufferLayout compute(u32 width, u32 height);
};

struct PostStackGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    BindlessDescriptors* bindless = nullptr;
    PostKernelLanguage language = PostKernelLanguage::Auto;
    u32 framesInFlight = 3; ///< frame-constant / LUT ring slots
    const char* name = "post_stack_gpu";
};

/// This frame's images (the input extent sets the post extent).
struct PostFrameImages {
    const Texture* hdr = nullptr;      ///< required: RGBA16F (or any float colour) sampled image
    const Texture* depth = nullptr;    ///< optional: R32F linear view depth in metres (DoF, motion blur)
    const Texture* velocity = nullptr; ///< optional: RG16F / RG32F velocity in pixels per frame (motion blur)
    /// Optional f32x4-per-pixel buffer (BDA) receiving the display result before RGBA16F rounding;
    /// declare it with PostGraphInputs::dump.
    u64 dumpAddress = 0;
};

/// The graph refs of the images of PostFrameImages (as imported by their owners).
struct PostGraphInputs {
    rg::TextureRef hdr;
    rg::TextureRef depth;
    rg::TextureRef velocity;
    rg::BufferRef dump;
};

struct PostGraphRefs {
    rg::BufferRef work;
    rg::BufferRef state;   ///< PostExposureState (persistent)
    rg::TextureRef output; ///< RGBA16F display image
};

/// Intermediate a debug / gate copy reads (addCopy).
enum class PostCopySource : u8 {
    DisplayInput = 0,  ///< f32x4 HDR after the spatial passes (the display transform's input)
    Bloom = 1,         ///< f32x4 full-resolution bloom (valid when bloom ran)
    ExposureState = 2, ///< PostExposureState
};

struct PostStats {
    u32 workRebuilds = 0;
    u32 outputRebuilds = 0;
    u32 lutRebuilds = 0;
    u32 lutUploads = 0;    ///< ring slots rewritten after setGradeLut
    u32 inputBinds = 0;
    u32 retired = 0;
    u32 passes = 0;        ///< passes added this frame
    u32 bloomLevels = 0;   ///< this frame
    u32 lookUnsupported = 0; ///< beginLookFrame: look_unsupported_nodes mask
    bool dofRan = false;
    bool motionBlurRan = false;
    bool bloomRan = false;
    bool exposureRan = false;
    bool lutApplied = false;
};

class PostStackGpu {
public:
    PostStackGpu() = default;
    ~PostStackGpu();
    PostStackGpu(const PostStackGpu&) = delete;
    PostStackGpu& operator=(const PostStackGpu&) = delete;

    /// Fails (false, nothing created) without a capable device, without a built kernel of the
    /// requested language, or in the stub backend.
    bool init(const PostStackGpuDesc& desc);
    /// The caller must have retired every frame that used the buffers.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Look grade LUT (look::LookSystem::gradeLut()): copied now (allocates only when the size
    /// changes), uploaded to each frame-ring slot on the following beginFrames. Null clears it.
    void setGradeLut(const look::Lut3D* lut);
    /// Resets the GPU adaptation state to `ev` on the next adaptation pass (scene cut / load).
    void resetExposure(f32 ev = 0.f);

    /// Writes this frame's constants; follows the input extent (work buffer + output) and images
    /// (bindless slots). False when the input is missing or a (re)allocation failed.
    bool beginFrame(u64 frameSerial, const PostGpuSettings& settings, const PostFrameImages& images);
    /// settings_from_look + beginFrame: the Look graph drives the passes node by node.
    bool beginLookFrame(u64 frameSerial, const look::LookEffectGraph& graph, const look::LookResolved& look,
                        u64 frameSeed, f32 deltaSeconds, const PostFrameImages& images);

    PostGraphRefs importInto(rg::Graph& graph);
    /// post.load, the spatial passes, the exposure passes, post.display.
    void addPasses(rg::Graph& graph, const PostGraphRefs& refs, const PostGraphInputs& inputs);
    /// Copies an intermediate into `dst` at `dstOffset` (gates / debugging; after addPasses).
    void addCopy(rg::Graph& graph, const PostGraphRefs& refs, PostCopySource what, rg::BufferRef dst, u64 dstOffset);
    /// Bytes addCopy(what) copies for the current extent.
    u64 copyBytes(PostCopySource what) const;

    /// Destroys buffers / images / bindless slots retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection ------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    const Texture& outputImage() const { return m_output.image; }
    u32 width() const { return m_width; }
    u32 height() const { return m_height; }
    const PostBufferLayout& layout() const { return m_layout; }
    const PostStats& stats() const { return m_stats; }
    const PostFrameConstants& constants() const { return m_constants; }
    const Buffer& workBuffer() const { return m_work; }
    const Buffer& stateBuffer() const { return m_state; }

private:
    struct Output {
        Texture image{};
        BindlessSlotHandle slot{};
        u32 layout = 0;
        u8 queue = rg::kNoQueue;
    };
    static constexpr u32 kInputCount = 3u; ///< hdr, depth, velocity
    struct Retired {
        Buffer buffers[2]{};
        Texture image{};
        BindlessSlotHandle slots[1u + kInputCount]{};
        u64 serial = 0;
    };
    struct PassRecord {
        PostStackGpu* self = nullptr;
        PostPush push{};
        u32 kernel = 0;
        u32 groups[3] = {1u, 1u, 1u};
        // copy passes
        u64 copySrc = 0;
        u64 copyDst = 0;
        u64 copyBytes = 0;
        rg::BufferRef copyFrom{};
        rg::BufferRef copyTo{};
    };
    enum Kernel : u32 { kLoad = 0, kBloom, kDof, kMotionBlur, kExposure, kDisplay, kKernelCount };
    static constexpr u32 kMaxPasses = 128u;

    bool createPipelines();
    bool createWork(u32 width, u32 height);
    bool createOutput(u32 width, u32 height);
    bool bindInput(u32 index, const Texture* image);
    bool writeLut(u32 slot);
    void retire(Retired r);
    void destroyRetired(Retired& r);
    PassRecord* nextRecord(u32 kernel);
    rg::PassBuilder addKernelPass(rg::Graph& graph, const char* name, PassRecord* r, const PostGraphRefs& refs);
    static void recordDispatch(const rg::PassContext& context, void* user);
    static void recordCopy(const rg::PassContext& context, void* user);

    PostStackGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    u64 m_frameSerial = 0;
    u32 m_width = 0;
    u32 m_height = 0;
    PostBufferLayout m_layout{};
    Buffer m_work{};
    u8 m_workQueue = rg::kNoQueue;
    Buffer m_state{};
    u8 m_stateQueue = rg::kNoQueue;
    bool m_stateInitialized = false;
    bool m_resetPending = true;
    f32 m_resetEv = 0.f;
    Output m_output{};
    std::vector<Retired> m_retired;
    Buffer m_frameRing{};
    u64 m_frameAddress = 0;
    PostFrameConstants m_constants{};
    PostGpuSettings m_settings{};
    bool m_hasDepth = false;
    bool m_hasVelocity = false;
    u64 m_displayInput = 0; ///< work-buffer offset of the display transform's input (after addPasses)

    // LUT: host copy (f32x4) + a host-visible ring, one slot per frame in flight.
    std::vector<f32> m_lutHost;
    u32 m_lutSize = 0;
    u64 m_lutRevision = 0;
    Buffer m_lutRing{};
    u32 m_lutRingSize = 0;
    u64 m_lutSlotStride = 0;
    std::vector<u64> m_lutSlotRevision;

    void* m_inputImages[kInputCount] = {};
    BindlessSlotHandle m_inputSlots[kInputCount]{};
    u32 m_inputHandles[kInputCount] = {};

    PassRecord m_records[kMaxPasses] = {};
    u32 m_recordCount = 0;

    void* m_layoutHandle = nullptr; ///< VkPipelineLayout (bindless set + 64-byte push, compute)
    void* m_pipelines[kKernelCount] = {};

    PostStats m_stats{};
};

} // namespace fuse::renderer::post_gpu
