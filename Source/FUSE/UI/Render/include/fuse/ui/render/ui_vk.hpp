// FUSE UI GPU backend (fuse_ui_vk, plan §4.9 / UI4): the "ui.draw_list" compute kernel over raw Vulkan handles, so it
// runs inside the engine's render graph (fuse::hybrid::UiLayerGpu, next to "hybrid.sprite_layer"), in an app's own
// swapchain loop, or headless (tests). Vulkan 1.1 + storage images and buffers only (UI-T0, decision in §4.9).
//
// Per frame: packDrawList (gpu_draw_data.hpp) -> upload(slot, data) copies into the slot's persistently mapped
// host-coherent buffers -> record(cmd, targetView, slot) binds and dispatches. The target is an RGBA8 storage image in
// VK_IMAGE_LAYOUT_GENERAL; the kernel blends over its contents (clear it first for a fresh layer). Capacities are fixed
// at init; upload() fails (returns false) rather than reallocating, so steady state never allocates.
#pragma once

#include "fuse/ui/render/gpu_draw_data.hpp"

namespace fuse::ui::render {

struct UiVkDesc {
    void* physicalDevice = nullptr; ///< VkPhysicalDevice
    void* device = nullptr;         ///< VkDevice
    u32 framesInFlight = 2;
    u32 maxCmds = 16384;
    u32 maxTileWords = 1u << 19; ///< headers, tile slots (with headroom) and glyph run records
    u32 maxImageWords = 1u << 20;
    u32 maxAtlasSize = 1024;
    /// Backdrop blur scratch (device-local floats, 32 bytes per pixel of a blur's source box). A blur whose source
    /// box exceeds it is skipped (the panel is drawn unblurred).
    u32 maxBlurPixels = 1u << 21;
};

class UiVkRenderer {
public:
    static constexpr u32 kMaxSlots = 4;
    /// External texture slots (fuse::ui::Image::externalSlot 0..3).
    static constexpr u32 kMaxExternal = 4;

    UiVkRenderer() = default;
    ~UiVkRenderer() { destroy(); }
    UiVkRenderer(const UiVkRenderer&) = delete;
    UiVkRenderer& operator=(const UiVkRenderer&) = delete;

    /// False without the embedded kernel or on any Vulkan failure.
    bool init(const UiVkDesc& desc);
    void destroy();
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
    /// True when DrawOp::Backdrop runs on the GPU (ui.backdrop kernel built in and created at init).
    bool backdropOnGpu() const { return m_blurPipeline != nullptr; }
    /// HDR target: record() then expects an RGBA16F storage image holding extended-range sRGB-encoded values (1.0 =
    /// SDR white; brighter content above it, see color::hdr), blended in float without 8-bit quantisation (the
    /// ui_draw_list_hdr / ui_backdrop_hdr variants). False (and unchanged) when the HDR kernel is not built in.
    bool setTargetHdr(bool hdr);
    bool targetHdr() const { return m_hdrTarget; }
    bool hdrTargetAvailable() const { return m_pipelineHdr != nullptr; }
    /// Glyph atlas uploads since init (stays flat while GpuDrawData::atlasGeneration does not change).
    u64 atlasUploads() const { return m_atlasUploads; }
    /// Bytes written into mapped slot memory by upload() since init (cmds, tiles, images, atlas). An unchanged frame
    /// (same GpuDrawData::contentVersion) writes nothing; a changed one writes only the 4 KB pages that differ from
    /// what the slot already holds.
    u64 uploadedBytes() const { return m_uploadedBytes; }

private:
    struct Slot {
        void* buffer = nullptr; ///< VkBuffer
        void* memory = nullptr; ///< VkDeviceMemory
        u8* mapped = nullptr;
        void* set = nullptr; ///< VkDescriptorSet
        void* blurSet = nullptr; ///< VkDescriptorSet of ui.backdrop
        GpuPush push{};
        std::vector<GpuPass> passes;
        const u8* atlasPtr = nullptr;
        u32 atlasSize = 0;
        u32 atlasGeneration = ~0u;
        u64 contentVersion = 0; ///< GpuDrawData::contentVersion last uploaded (process-unique)
        std::vector<u8> shadow[3]; ///< CPU copy of the mapped cmds / tiles / images (page diff source)
    };
    u64 m_uploadedBytes = 0;
    UiVkDesc m_desc{};
    u64 m_offsets[5] = {};  ///< cmds, tiles, atlas, images, end
    void* m_setLayout = nullptr;
    void* m_layout = nullptr;
    void* m_pipeline = nullptr;
    void* m_pipelineHdr = nullptr; ///< ui_draw_list_hdr (RGBA16F target)
    bool m_hdrTarget = false;
    void* m_pool = nullptr;
    Slot m_slots[kMaxSlots]{};
    void* m_external[kMaxExternal]{}; ///< VkImageView per external slot (null: dummy)
    void* m_sampler = nullptr;        ///< nearest, clamp
    void* m_dummy = nullptr;          ///< 1x1 VkImage bound to unused external slots
    void* m_dummyMem = nullptr;
    void* m_dummyView = nullptr;
    bool m_dummyReady = false;        ///< layout transitioned (first record)
    void* m_blurSetLayout = nullptr;
    void* m_blurLayout = nullptr;
    void* m_blurPipeline = nullptr;
    void* m_blurPipelineHdr = nullptr;
    void* m_scratch = nullptr; ///< VkBuffer, device-local floats
    void* m_scratchMem = nullptr;
    u64 m_atlasUploads = 0;
};

/// "ui.encode_pq" / "ui.encode_scrgb": converts an RGBA8 sRGB canvas (or, with `hdrSource`, the RGBA16F extended-range
/// canvas of UiVkRenderer::setTargetHdr, where values above 1 are brighter than SDR white) (storage image, GENERAL) into an HDR output
/// image over a box: HDR10 (rgb10_a2: Rec.2020 + PQ, as color::toHdr10) or scRGB (rgba16f: linear, 1.0 = 80 nits).
/// Both images must be in GENERAL layout with STORAGE usage; barriers around the dispatch are the caller's.
class UiVkEncoder {
public:
    enum class Mode : u8 { Hdr10, ScRgb };
    UiVkEncoder() = default;
    ~UiVkEncoder() { destroy(); }
    UiVkEncoder(const UiVkEncoder&) = delete;
    UiVkEncoder& operator=(const UiVkEncoder&) = delete;

    static bool kernelAvailable(Mode mode, bool hdrSource = false);
    /// `slots`: descriptor sets, one per frame in flight (record() into a slot whose last use has completed).
    bool init(void* device, Mode mode, u32 slots = 2, bool hdrSource = false);
    bool hdrSource() const { return m_hdrSource; }
    void destroy();
    bool valid() const { return m_pipeline != nullptr; }
    Mode mode() const { return m_mode; }
    /// VkFormat value of the output image (VK_FORMAT_A2B10G10R10_UNORM_PACK32 or VK_FORMAT_R16G16B16A16_SFLOAT).
    u32 outputFormat() const;
    void record(void* commandBuffer, u32 slot, void* srcView, void* dstView, int x0, int y0, int x1, int y1,
                f32 sdrWhiteNits);

private:
    void* m_device = nullptr;
    void* m_setLayout = nullptr;
    void* m_layout = nullptr;
    void* m_pipeline = nullptr;
    void* m_pool = nullptr;
    void* m_sets[UiVkRenderer::kMaxSlots]{};
    u32 m_slots = 0;
    Mode m_mode = Mode::Hdr10;
    bool m_hdrSource = false;
};

} // namespace fuse::ui::render
