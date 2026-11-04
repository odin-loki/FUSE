// FUSE UI GPU backend (fuse_ui_vk, plan §4.9 / UI4): the "ui.draw_list" compute kernel over raw Vulkan handles, so it
// runs inside the engine's render graph (fuse::hybrid::UiLayerGpu, next to "hybrid.sprite_layer"), in an app's own
// swapchain loop, or headless (tests). Vulkan 1.1 + storage images and buffers only (UI-T0, decision in §4.9).
//
// Per frame: packDrawList (gpu_draw_data.hpp) -> upload(slot, data) copies into the slot's persistently mapped
// host-coherent buffers -> record(cmd, targetView, slot) binds and dispatches: one tile dispatch per draw-list segment,
// with the backdrop blur passes (box passes over a device-local scratch, then a composite) and compute -> compute
// barriers between segments. The target is an RGBA8 storage image in
// VK_IMAGE_LAYOUT_GENERAL; the kernel blends over its contents (clear it first for a fresh layer). Capacities are fixed
// at init; upload() fails (returns false) rather than reallocating, so steady state never allocates.
//
// Custom effects (fuse/ui/effects.hpp): setEffects() builds one compute pipeline per registered effect (its SPIR-V from
// fuse_ui_add_effect, or the built-in one of the same name); an Effect command ends a segment like a backdrop and runs
// as copy-to-scratch + effect dispatch.
//
// Backends (UiVkDesc::backend): Compute is the tile kernel. SdfQuads draws each segment as one instanced draw of SDF
// quads (ui_sdf_quad.vert / .frag: the same coverage arithmetic, fixed-function premultiplied-over blending) inside a
// render pass on the same GENERAL-layout image, which then also needs VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; backdrops
// and effects still run as compute passes between the draws. Both leave the target ready for compute-stage access.
#pragma once

#include "fuse/ui/render/gpu_draw_data.hpp"

#include <span>
#include <string_view>

namespace fuse::ui {
class EffectRegistry;
} // namespace fuse::ui

namespace fuse::ui::render {

enum class UiVkBackend : u8 {
    Compute,  ///< "ui.draw_list" tile kernel (default)
    SdfQuads, ///< instanced SDF quads, graphics pipeline + fixed-function blending
};

struct UiVkDesc {
    void* physicalDevice = nullptr; ///< VkPhysicalDevice
    void* device = nullptr;         ///< VkDevice
    u32 framesInFlight = 2;
    u32 maxCmds = 16384;
    u32 maxTileWords = 1u << 18;
    u32 maxImageWords = 1u << 20;
    u32 maxAtlasSize = 1024;
    /// Backdrop blur scratch (device-local, 2 x 16 bytes per pixel): the largest blur source region (the backdrop
    /// rect plus 3 x its box radius, clamped to the target). A larger backdrop is drawn unblurred (tint only).
    u32 maxBackdropPixels = 1u << 20;
    UiVkBackend backend = UiVkBackend::Compute;
    /// SdfQuads shaders (set by useSdfQuads; a separate translation unit, so binaries that never select the backend do
    /// not carry its SPIR-V).
    std::span<const u32> sdfVertex, sdfFragment;
    /// Persistent VkPipelineCache file for the kernel (null: no cache). Loaded only when its header matches this
    /// device (vendor, device id, pipelineCacheUUID) and this kernel (SPIR-V hash); rewritten after a miss.
    const char* pipelineCachePath = nullptr;
};

/// What init() did with the pipeline cache.
enum class PipelineCacheUse : u8 { None, Hit, Miss, Rejected };

/// Selects the SDF-quad backend for `desc` (backend + its shaders). False when the shaders were not compiled in.
bool useSdfQuads(UiVkDesc& desc);

class UiVkRenderer {
public:
    static constexpr u32 kMaxSlots = 4;
    /// External texture slots (fuse::ui::Image::externalSlot 0..3).
    static constexpr u32 kMaxExternal = 4;
    /// Draw-list segments per frame (backdrops + effects + 1); upload() fails beyond it.
    static constexpr u32 kMaxSegments = 32;
    /// Effect pipelines (EffectRegistry::kMaxEffects).
    static constexpr u32 kMaxEffects = 32;
    /// SdfQuads: framebuffers cached per target view (oldest replaced; see forgetTarget).
    static constexpr u32 kMaxFramebuffers = 16;

    UiVkRenderer() = default;
    ~UiVkRenderer() { destroy(); }
    UiVkRenderer(const UiVkRenderer&) = delete;
    UiVkRenderer& operator=(const UiVkRenderer&) = delete;

    /// False without the embedded kernel or on any Vulkan failure.
    bool init(const UiVkDesc& desc);
    void destroy();
    /// Device memory the renderer allocated: ring-slot buffers (host-visible) + backdrop / effect scratch.
    u64 deviceMemoryBytes() const { return valid() ? u64(m_desc.framesInFlight) * m_offsets[4] + u64(m_scratchPixels) * 32u : 0u; }
    bool valid() const { return m_pipeline != nullptr; }

    /// Copies `data` into ring slot `slot` (host writes before the submit are visible to it). False when over capacity.
    bool upload(u32 slot, const GpuDrawData& data);
    /// Records descriptor update + bind + dispatch for the last upload into `slot`. `targetView`: VkImageView of an
    /// RGBA8 storage image in GENERAL layout, at least data.push.extent in size.
    void record(void* commandBuffer, void* targetView, u32 slot);

    /// Binds a sampled RGBA image (VkImageView, any colour format the device can sample) to external slot `ext`; it
    /// must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL when the recorded dispatch executes, and outlive it. Null
    /// unbinds (a 1x1 dummy). Takes effect at the next record(); kImage commands whose Image has externalSlot == ext
    /// sample it (nearest, scaled over the command rect) instead of the packed CPU pixels: the zero-copy path for
    /// engine frames (the editor's 3D viewport).
    void setExternalImage(u32 ext, void* view);
    void* externalImage(u32 ext) const { return ext < kMaxExternal ? m_external[ext] : nullptr; }

    /// True when the kernel SPIR-V was compiled into this build.
    static bool kernelAvailable();
    /// The kernel's SPIR-V (empty when not compiled in). UI-T0 (plan §4.9): it needs Vulkan 1.1 with no device
    /// features or extensions and classic descriptor sets (fuse_ui_vk_tier0 checks its capabilities).
    static std::span<const u32> kernelSpirv();
    /// True when the SDF-quad shaders were compiled into this build.
    static bool sdfQuadsAvailable();
    UiVkBackend backend() const { return m_desc.backend; }

    /// Builds the effect pipelines of `registry` (ids = registry ids): each effect's SPIR-V, or the built-in SPIR-V of
    /// that name. Effects without either are skipped on the GPU. Not for in-flight frames (pipelines are replaced).
    /// (ui_vk_effects.cpp: the built-in SPIR-V is linked only into binaries that call this.)
    bool setEffects(const EffectRegistry& registry);
    /// Sets (empty: removes) the pipeline of effect `id` from SPIR-V made by fuse_ui_add_effect.
    bool setEffect(u32 id, std::span<const u32> spirv);
    bool hasEffect(u32 id) const { return id < kMaxEffects && m_effects[id] != nullptr; }
    /// SPIR-V of a built-in effect (desaturate, ripple, grain); empty when unknown or not compiled in.
    static std::span<const u32> builtinEffectSpirv(std::string_view name);
    /// SdfQuads: drops the cached framebuffer of `view` (call before destroying a target view that may be reused).
    void forgetTarget(void* view);

    /// Pipeline cache outcome of the last init() and the vkCreateComputePipelines time (ms).
    PipelineCacheUse pipelineCacheUse() const { return m_cacheUse; }
    double pipelineMs() const { return m_pipelineMs; }

private:
    PipelineCacheUse m_cacheUse = PipelineCacheUse::None;
    double m_pipelineMs = 0.0;
    struct Slot {
        void* buffer = nullptr; ///< VkBuffer
        void* memory = nullptr; ///< VkDeviceMemory
        u8* mapped = nullptr;
        void* set = nullptr; ///< VkDescriptorSet
        GpuPush push{};
        GpuSegment segments[kMaxSegments]{};
        u32 segmentCount = 0;
    };
    UiVkDesc m_desc{};
    u64 m_offsets[5] = {};  ///< cmds, tiles, atlas, images, end
    void* m_setLayout = nullptr;
    void* m_layout = nullptr;
    void* m_pipeline = nullptr;
    void* m_pool = nullptr;
    Slot m_slots[kMaxSlots]{};
    void* m_external[kMaxExternal]{}; ///< VkImageView per external slot (null: dummy)
    void* m_sampler = nullptr;        ///< nearest, clamp
    void* m_dummy = nullptr;          ///< 1x1 VkImage bound to unused external slots
    void* m_dummyMem = nullptr;
    void* m_dummyView = nullptr;
    bool m_dummyReady = false;        ///< layout transitioned (first record)
    void* m_scratch = nullptr;        ///< VkBuffer, backdrop ping-pong halves (shared by all slots: one queue)
    void* m_scratchMem = nullptr;
    u32 m_scratchPixels = 0;          ///< per half
    u32 m_stages = 0;                 ///< VkShaderStageFlags of the set layout / push constants
    void* m_effects[kMaxEffects]{};   ///< VkPipeline per effect id
    void* m_renderPass = nullptr;     ///< SdfQuads
    void* m_gfxPipeline = nullptr;
    struct Framebuffer {
        void* view = nullptr;
        void* fb = nullptr;
        u32 w = 0, h = 0;
    };
    Framebuffer m_fbs[kMaxFramebuffers]{};
    u32 m_fbNext = 0;
    void* framebufferFor(void* view, u32 w, u32 h);
    void* makeComputePipeline(std::span<const u32> spirv);
};

} // namespace fuse::ui::render
