#pragma once

// WP-4.2 FSR 3.1 temporal upscaler on Vulkan, as render-graph v2 passes (AMD FidelityFX SDK v1.1.4, FSR 3.1.4
// upscaler, MIT; conventions and the constant setup: fsr3_types.hpp).
//
//   Fsr3Gpu fsr;  fsr.init({device, allocator, maxResolution});
//   fsr.beginFrame(serial, {resolution, jitter, exposure, reset, camera ...});   // constants into a host ring
//   Fsr3GraphRefs r = fsr.importInto(graph);
//   fsr.addPasses(graph, r, {colorRef, depthRef, motionRef, reactiveRef, transparencyRef});
//   ... r.output (RGBA16F display) is this frame's upscaled linear colour.
//
// Passes, in the SDK's order (fsr3upscalerDispatch): "fsr3.clear" (first frame / reset / per-frame clears the
// SDK schedules as clear jobs) -> "fsr3.convert" (FUSE: buffers -> textures) -> "fsr3.prepare_inputs" ->
// "fsr3.luma_pyramid" -> "fsr3.shading_change_pyramid" -> "fsr3.shading_change" -> "fsr3.prepare_reactivity" ->
// "fsr3.luma_instability" -> "fsr3.accumulate" | "fsr3.accumulate_sharpen" + "fsr3.rcas". Every pass declares its
// accesses (derived from the SPIR-V reflection of the vendored pass); the graph places every barrier and layout
// transition. The eight SDK passes are the vendored Vulkan GLSL compiled verbatim with glslangValidator -Os
// (the SDK's own flags; permutation: HDR input, render-resolution unjittered motion vectors, inverted depth,
// reference Lanczos reprojection, FP32); descriptors are classic per-pass sets bound by variable name like the
// SDK backend (pre-allocated per frame-in-flight slot, rewritten in the pass callback: no steady-state heap
// allocations). No bindless dependency: the passes work next to either bindless backend.
//
// Inputs: colour = any sampled 2D image (render resolution, linear HDR, .rgb), depth = f32 linear view depth
// per render pixel (<= 0 = sky) and motion = f32 x 2 UV motion (current - previous, unjittered) at offset 0 of
// their buffers (the WP-4.1 TemporalMotion buffers, or any producer's), optional reactive / transparency-and-
// composition masks as sampled images (.r). Output: RGBA16F display-resolution storage + sampled image.
// Dynamic resolution: any render / display size up to the init maximum (larger rebuilds the resources).

#include <fuse/math/vec.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/upscale/upscale_inputs.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_reflect.hpp>
#include <fuse/renderer/upscale_backends/fsr3/fsr3_types.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::renderer::fsr3 {

enum class Fsr3KernelLanguage : u8 {
    Auto = 0, ///< the FUSE convert pass in Slang if built, else GLSL (the SDK passes are always the vendored GLSL)
    Slang,
    Glsl,
};

/// Embedded SPIR-V of one pass (`language` only matters for Convert); words == null when not built.
struct Fsr3KernelCode {
    const u32* words = nullptr;
    usize bytes = 0;
    const char* language = "none";
};
Fsr3KernelCode fsr3_kernel_code(Fsr3Pass pass, Fsr3KernelLanguage language = Fsr3KernelLanguage::Auto);

struct Fsr3Capabilities {
    bool supported = false;
    const char* reason = "no device"; ///< "ok" when usable
};
/// Compute subgroup quad operations (SPD), the storage formats of the internal resources and every pass built.
Fsr3Capabilities queryFsr3Capabilities(const VulkanDevice* device);

struct Fsr3GpuDesc {
    VulkanDevice* device = nullptr;
    GpuAllocator* allocator = nullptr;
    UpscaleResolution maxResolution{}; ///< resources are sized for this (SDK maxRenderSize / maxUpscaleSize)
    Fsr3Settings settings{};
    Fsr3KernelLanguage language = Fsr3KernelLanguage::Auto;
    u32 framesInFlight = 3; ///< constant-ring / descriptor-set slots
    const char* name = "fsr3";
};

struct Fsr3GpuFrameDesc {
    UpscaleResolution resolution{};
    math::Vec2 jitter_px{};         ///< FUSE sample jitter (upscale_inputs.hpp convention)
    f32 exposure = 1.f;
    f32 pre_exposure = 1.f;
    f32 frame_time_s = 1.f / 60.f;
    bool reset_history = false;     ///< camera cut / teleport
    UpscaleCamera camera{};         ///< near / far / vertical fov of this frame (unjittered)
    bool sharpen = false;           ///< RCAS after the accumulation
    f32 sharpness = 0.f;            ///< [0, 1]
    /// Test hook for the jitter-sign gate: feed FSR +jitter_px instead of the correct -jitter_px.
    bool debug_flip_jitter_sign = false;
};

/// This frame's inputs on the graph.
struct Fsr3GpuInputs {
    rg::TextureRef color;
    rg::BufferRef depth;
    rg::BufferRef motion;
    rg::TextureRef reactive;     ///< invalid = none (the SDK's 1x1 default reactivity)
    rg::TextureRef transparency; ///< invalid = none
};

/// Upper bound of the backend's physical images (internal resources + output).
inline constexpr u32 kFsr3MaxImages = 32u;

struct Fsr3GraphRefs {
    rg::TextureRef output;                   ///< RGBA16F display image
    rg::TextureRef images[kFsr3MaxImages] = {}; ///< every imported internal image (by physical index)
};

struct Fsr3GpuStats {
    u32 rebuilds = 0;    ///< resource (re)creations (1 after init)
    u32 passes = 0;      ///< compute passes added this frame
    u32 clears = 0;      ///< images cleared this frame
    bool reset = false;  ///< this frame reset the accumulation
    u32 retired = 0;
    u32 frames = 0;      ///< dispatches since init
};

class Fsr3Gpu {
public:
    Fsr3Gpu() = default;
    ~Fsr3Gpu();
    Fsr3Gpu(const Fsr3Gpu&) = delete;
    Fsr3Gpu& operator=(const Fsr3Gpu&) = delete;

    /// False (nothing created) without a capable device, without the built passes, with an invalid maximum
    /// resolution, or in the stub backend.
    bool init(const Fsr3GpuDesc& desc);
    /// The caller must have retired every frame that used the resources.
    void destroy();
    bool valid() const { return m_initialized; }

    /// Host constants of this frame into ring slot `frameSerial % framesInFlight`. A resolution above the current
    /// maximum rebuilds the resources (and resets the history). False on invalid input.
    bool beginFrame(u64 frameSerial, const Fsr3GpuFrameDesc& frame);
    Fsr3GraphRefs importInto(rg::Graph& graph);
    /// Adds the frame's passes. The graph must execute before the next beginFrame.
    void addPasses(rg::Graph& graph, const Fsr3GraphRefs& refs, const Fsr3GpuInputs& inputs);
    /// Drop the history: the next frame starts a new accumulation (like reset_history).
    void invalidate() { m_forceReset = true; }

    /// Destroys resources retired at serials <= completedSerial.
    u32 collectRetired(u64 completedSerial);

    // --- inspection ------------------------------------------------------------------------------------
    const char* kernelLanguage() const { return m_language; }
    Fsr3Settings& settings() { return m_desc.settings; }
    const Fsr3HostState& hostState() const { return m_host; }
    const Fsr3FrameSetup& frameSetup() const { return m_setup; }
    const Texture& outputImage() const;
    const UpscaleResolution& maxResolution() const { return m_max; }
    const UpscaleResolution& resolution() const { return m_resolution; }
    const Fsr3GpuStats& stats() const { return m_stats; }
    const SpirvReflection& reflection(Fsr3Pass pass) const { return m_passes[static_cast<u32>(pass)].reflection; }

private:
    static constexpr u32 kResourceCount = static_cast<u32>(Fsr3Resource::Count);
    static constexpr u32 kMaxBindings = 16;
    static constexpr u32 kSpdViews = 6;

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
    /// Physical images; ping-pong pairs are two entries.
    enum Phys : u8 {
        kPhysDepth = 0,
        kPhysMotion,
        kPhysExposure,
        kPhysDefaultReactive,
        kPhysFrameInfo,
        kPhysRecon,
        kPhysDilatedMotion,
        kPhysDilatedDepth,
        kPhysAccum0,
        kPhysAccum1,
        kPhysUpscaled0,
        kPhysUpscaled1,
        kPhysLumaHistory0,
        kPhysLumaHistory1,
        kPhysLuma0,
        kPhysLuma1,
        kPhysIntermediate,
        kPhysFarthestMip1,
        kPhysShadingChange,
        kPhysSpdMips,
        kPhysSpdAtomic,
        kPhysNewLocks,
        kPhysDilatedReactive,
        kPhysOutput,
        kPhysCount,
    };
    struct Resources {
        Image images[kPhysCount]{};
        void* spdViews[kSpdViews] = {}; ///< VkImageView per SPD mip (storage)
        UpscaleResolution max{};
        bool cleared = false; ///< first-use clear recorded
    };
    struct Retired {
        Resources res{};
        u64 serial = 0;
    };
    struct PassBinding {
        u32 binding = 0;
        u32 descriptorType = 0; ///< VkDescriptorType
        Fsr3BindingTarget target{};
    };
    struct PassPipeline {
        SpirvReflection reflection{};
        PassBinding bindings[kMaxBindings]{};
        u32 bindingCount = 0;
        u32 pushBytes = 0;
        void* setLayout = nullptr;
        void* layout = nullptr;
        void* pipeline = nullptr;
        void* sets[8] = {}; ///< one per frame-in-flight slot
    };
    struct PassRecord {
        Fsr3Gpu* self = nullptr;
        Fsr3Pass pass = Fsr3Pass::Convert;
        u32 groups[2] = {1u, 1u};
    };
    struct ClearRecord {
        Fsr3Gpu* self = nullptr;
        u8 phys[kPhysCount] = {};
        rg::TextureRef refs[kPhysCount] = {};
        u32 count = 0;
    };

    bool createPipelines();
    void destroyPipelines();
    bool createResources(Resources& r, const UpscaleResolution& max);
    void destroyResources(Resources& r);
    /// Physical image behind a logical resource for this frame (ping-pong by parity); kPhysCount if not an image.
    u32 physFor(Fsr3Resource resource, bool storage) const;
    rg::TextureRef refFor(const Fsr3GraphRefs& refs, const Fsr3GpuInputs& inputs, Fsr3Resource resource, bool storage) const;
    void addPass(rg::Graph& graph, const Fsr3GraphRefs& refs, const Fsr3GpuInputs& inputs, Fsr3Pass pass);
    static void recordPass(const rg::PassContext& context, void* user);
    static void recordClear(const rg::PassContext& context, void* user);

    Fsr3GpuDesc m_desc{};
    bool m_initialized = false;
    const char* m_language = "none";
    UpscaleResolution m_max{};
    UpscaleResolution m_resolution{};
    Resources m_res{};
    std::vector<Retired> m_retired;
    PassPipeline m_passes[kFsr3PassCount]{};
    void* m_samplers[2] = {}; ///< point clamp, linear clamp
    void* m_pool = nullptr;   ///< VkDescriptorPool
    Buffer m_ring{};          ///< host-visible constants, framesInFlight slots
    u64 m_slotOffset = 0;
    u32 m_slot = 0;
    Fsr3HostState m_host{};
    Fsr3FrameSetup m_setup{};
    Fsr3GpuFrameDesc m_frame{};
    bool m_begun = false;
    bool m_forceReset = false;
    bool m_hasReactive = false;
    bool m_hasTransparency = false;
    PassRecord m_records[kFsr3PassCount]{};
    ClearRecord m_clear{};
    Fsr3GpuInputs m_inputs{};   ///< this frame's input refs (read by the callbacks)
    Fsr3GraphRefs m_refs{};
    Fsr3GpuStats m_stats{};
};

} // namespace fuse::renderer::fsr3
