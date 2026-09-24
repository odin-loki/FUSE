#pragma once

// WP-4.4 FSR 3.1 frame generation on Vulkan, as render-graph v2 compute passes (AMD FidelityFX SDK v1.1.4: optical
// flow 1.1.2 + frame interpolation 1.1.3, MIT; conventions, constants and the dispatch plan: fg_types.hpp).
//
//   FrameGenGpu fg;  fg.init({device, allocator, display size, max render size, source format});
//   per rendered frame N (after upscaling and post, before the UI):
//     fg.beginFrame(serial, {render size, jitter, camera, frame time, reset, frame id});
//     FgGraphRefs r = fg.importInto(graph);
//     fg.addPasses(graph, r, {hudless colour N, UI texture N, depth N, motion N});
//     -> r.presentInterpolated: the frame halfway between N - 1 and N with the UI of frame N on top (present first),
//        r.presentReal: frame N with its UI (present half a frame interval later; pacing: present_timing.hpp),
//        r.interpolated: the interpolated HUD-less frame (FSR 3.1 output, RGBA16F, alpha = inpainting weight).
//
// Passes (plan order, fg_setup_frame): "fg.clear" (first use: everything; per frame: the SDK's clear jobs) ->
// "fg.convert" (FUSE, Slang + GLSL twin) -> "fg.fi.reconstruct_and_dilate" (ffxFrameInterpolationPrepare) -> optical
// flow: "fg.of.prepare_luma", "fg.of.luminance_pyramid", "fg.of.scd_histogram", "fg.of.scd_divergence", then per level
// 6..0 "fg.of.search" (or "fg.of.search_portable"), "fg.of.filter", "fg.of.scale" -> frame interpolation: "fg.fi.setup",
// (not on reset:) "fg.fi.reconstruct_previous_depth", "fg.fi.game_motion_vector_field",
// "fg.fi.game_vector_field_inpainting_pyramid", "fg.fi.optical_flow_vector_field", "fg.fi.disocclusion_mask", then
// "fg.fi.interpolation", "fg.fi.inpainting_pyramid", "fg.fi.inpainting" -> "fg.ui_composite" x 2 (FUSE) ->
// "fg.copy_source" (current -> previous interpolation source). Every vendored pass is the SDK's Vulkan GLSL compiled
// verbatim (glslangValidator -Os, the SDK's flags; permutation: low-res unjittered motion vectors, inverted depth,
// FP32); every access is declared from a mini SPIR-V reflection (fsr3::reflect_spirv), descriptors are classic sets
// bound by variable name like the SDK backend (one per pass occurrence and frame-in-flight slot, rewritten in the pass
// callback: no steady-state heap allocations), no bindless dependency.
//
// The SDK's search pass reduces with wave intrinsics that are only correct for 32 / 64-lane subgroups; on other
// devices (Lavapipe: 8 lanes) the FUSE-derived fg_of_search_portable.comp (same algorithm, shared-memory reductions)
// runs instead (FgSearchVariant::Auto picks; queryFgCapabilities reports which).
//
// Inputs: source = the frame's HUD-less display-referred colour (display size, the init format, Sampled + TransferSrc
// usage), ui = premultiplied RGBA UI (display size, optional), depth = f32 linear view depth per render pixel and
// motion = f32 x 2 UV motion (current - previous) at offset 0 of their buffers (WP-4.1 TemporalMotion or any producer).

#include <fuse/math/vec.hpp>
#include <fuse/renderer/framegen/fg_types.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_reflect.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::framegen {

enum class FgKernelLanguage : u8 {
    Auto = 0, ///< the FUSE passes in Slang if built, else GLSL (the SDK passes are always the vendored GLSL)
    Slang,
    Glsl,
};

enum class FgSearchVariant : u8 {
    Auto = 0, ///< vendored when the device's compute subgroup size is fixed at 32 or 64, else portable
    Vendored,
    Portable,
};

struct FgKernelCode {
    const u32* words = nullptr;
    usize bytes = 0;
    const char* language = "none";
};
/// Embedded SPIR-V of one pass (`language` only matters for the FUSE passes); words == null when not built.
FgKernelCode fg_kernel_code(FgPass pass, FgKernelLanguage language = FgKernelLanguage::Auto);

struct FgCapabilities {
    bool supported = false;
    const char* reason = "no device";
    bool vendoredSearch = false; ///< the vendored search pass is usable (fixed 32 / 64-lane subgroups)
    u32 subgroupMin = 0, subgroupMax = 0;
};
/// Every pass built, Vulkan 1.3, the storage formats of the internal resources, storage read / write without format
/// on RGBA16F (the SDK's rw_output), sampled + transfer on the source format, compute quad operations (SPD).
FgCapabilities queryFgCapabilities(const VulkanDevice* device, u32 sourceFormat);

struct FgGpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    u32 displayWidth = 0, displayHeight = 0;     ///< interpolation resolution (>= 64 x 64)
    u32 maxRenderWidth = 0, maxRenderHeight = 0; ///< depth / motion resolution upper bound
    u32 sourceFormat = 97u;                      ///< VkFormat of the HUD-less source (RGBA16F default)
    FgKernelLanguage language = FgKernelLanguage::Auto;
    FgSearchVariant search = FgSearchVariant::Auto;
    u32 framesInFlight = 3;
    const char* name = "fg";
};

struct FgGpuFrameDesc {
    u32 renderWidth = 0, renderHeight = 0;
    math::Vec2 jitter_px{};
    f32 near_plane = 0.1f, far_plane = 1000.f, vertical_fov_rad = 1.f;
    f32 view_space_to_meters = 1.f;
    f32 frame_time_ms = 16.667f;
    bool reset = false; ///< camera cut: the interpolated frame is the current frame
    u64 frame_id = 0;   ///< +1 per frame (a jump resets, like the SDK)
};

struct FgGpuInputs {
    rg::TextureRef source; ///< HUD-less colour of this frame
    rg::TextureRef ui;     ///< invalid = no UI
    rg::BufferRef depth;
    rg::BufferRef motion;
};

struct FgGraphRefs {
    rg::TextureRef interpolated;        ///< FSR 3.1 output (HUD-less)
    rg::TextureRef presentInterpolated; ///< interpolated + UI
    rg::TextureRef presentReal;         ///< current + UI
    rg::TextureRef images[kFgImageCount] = {};
    rg::BufferRef counters;
};

struct FgGpuStats {
    u32 passes = 0; ///< compute passes this frame
    u32 clears = 0; ///< images cleared this frame
    bool fiReset = false;
    bool ofReset = false;
    u32 frames = 0;
};

class FrameGenGpu {
public:
    FrameGenGpu() = default;
    ~FrameGenGpu();
    FrameGenGpu(const FrameGenGpu&) = delete;
    FrameGenGpu& operator=(const FrameGenGpu&) = delete;

    /// False (nothing created) without a capable device / built passes, with invalid sizes, or in the stub backend.
    bool init(const FgGpuDesc& desc);
    /// The caller must have retired every frame that used the resources.
    void destroy();
    bool valid() const { return m_initialized; }

    bool beginFrame(u64 frameSerial, const FgGpuFrameDesc& frame);
    FgGraphRefs importInto(rg::Graph& graph);
    /// Adds the frame's passes. The graph must execute before the next beginFrame.
    void addPasses(rg::Graph& graph, const FgGraphRefs& refs, const FgGpuInputs& inputs);
    /// The next frame starts a new interpolation history (like FgGpuFrameDesc::reset).
    void invalidate() { m_forceReset = true; }

    // --- inspection ----------------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    bool portableSearch() const { return m_host.portableSearch; }
    const FgHostState& hostState() const { return m_host; }
    const FgFrameSetup& frameSetup() const { return m_setup; }
    const FgGpuStats& stats() const { return m_stats; }
    const Texture& image(u32 slot) const { return m_images[slot < kFgImageCount ? slot : 0u].tex; }
    const fsr3::SpirvReflection& reflection(FgPass pass) const { return m_passes[static_cast<u32>(pass)].reflection; }

private:
    static constexpr u32 kMaxBindings = 20;
    static constexpr u32 kMaxOccurrences = 8;
    static constexpr u32 kMaxSlots = 8;
    static constexpr u32 kPyramidViews = 13;

    struct Image {
        Texture tex{};
        u32 format = 0;
        u32 width = 0;
        u32 height = 0;
        u32 mips = 1;
        u32 layout = 0;
        u8 queue = rg::kNoQueue;
        const char* name = nullptr;
    };
    struct PassBinding {
        u32 binding = 0;
        u32 descriptorType = 0; ///< VkDescriptorType
        FgBindingTarget target{};
    };
    struct PassPipeline {
        fsr3::SpirvReflection reflection{};
        PassBinding bindings[kMaxBindings]{};
        u32 bindingCount = 0;
        u32 pushBytes = 0;
        u32 occurrences = 1;
        void* setLayout = nullptr;
        void* layout = nullptr;
        void* pipeline = nullptr;
        void* sets[kMaxOccurrences][kMaxSlots] = {};
    };
    struct DispatchRecord {
        FrameGenGpu* self = nullptr;
        u32 index = 0;      ///< into m_setup.dispatches
        u32 occurrence = 0; ///< of its pass this frame (descriptor set)
    };
    struct ClearRecord {
        FrameGenGpu* self = nullptr;
        rg::TextureRef refs[kFgImageCount] = {};
        u32 count = 0;
        bool counters = false;
    };
    struct CopyRecord {
        FrameGenGpu* self = nullptr;
    };

    bool createPipelines();
    void destroyPipelines();
    bool createResources();
    void destroyResources();
    u16 slotFor(const FgDispatch& d, const FgBindingTarget& t) const;
    rg::TextureRef textureFor(u16 slot) const;
    rg::BufferRef bufferFor(u16 slot) const;
    bool boundAsStorage(const PassPipeline& pp, const FgDispatch& d, u16 slot) const;
    void addDispatch(rg::Graph& graph, u32 index, u32 occurrence);
    static void recordDispatch(const rg::PassContext& context, void* user);
    static void recordClear(const rg::PassContext& context, void* user);
    static void recordCopy(const rg::PassContext& context, void* user);

    FgGpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    Image m_images[kFgImageCount]{};
    void* m_pyramidViews[kPyramidViews] = {}; ///< VkImageView per inpainting-pyramid mip (storage)
    Buffer m_counters{};
    u8 m_countersQueue = rg::kNoQueue;
    bool m_cleared = false;
    PassPipeline m_passes[kFgPassCount]{};
    void* m_samplers[2] = {}; ///< point clamp, linear clamp
    void* m_pool = nullptr;
    Buffer m_ring{};
    u64 m_slotOffset = 0;
    u32 m_slot = 0;
    FgHostState m_host{};
    FgFrameSetup m_setup{};
    FgGpuFrameDesc m_frame{};
    bool m_begun = false;
    bool m_forceReset = false;
    DispatchRecord m_records[kFgMaxDispatches]{};
    ClearRecord m_clear{};
    CopyRecord m_copy{};
    FgGpuInputs m_inputs{};
    FgGraphRefs m_refs{};
    FgGpuStats m_stats{};
};

} // namespace fuse::renderer::framegen
