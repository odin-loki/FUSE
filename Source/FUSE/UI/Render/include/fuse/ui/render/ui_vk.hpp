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
    u32 maxTileWords = 1u << 18;
    u32 maxImageWords = 1u << 20;
    u32 maxAtlasSize = 1024;
};

class UiVkRenderer {
public:
    static constexpr u32 kMaxSlots = 4;

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

    /// True when the kernel SPIR-V was compiled into this build.
    static bool kernelAvailable();

private:
    struct Slot {
        void* buffer = nullptr; ///< VkBuffer
        void* memory = nullptr; ///< VkDeviceMemory
        u8* mapped = nullptr;
        void* set = nullptr; ///< VkDescriptorSet
        GpuPush push{};
    };
    UiVkDesc m_desc{};
    u64 m_offsets[5] = {};  ///< cmds, tiles, atlas, images, end
    void* m_setLayout = nullptr;
    void* m_layout = nullptr;
    void* m_pipeline = nullptr;
    void* m_pool = nullptr;
    Slot m_slots[kMaxSlots]{};
};

} // namespace fuse::ui::render
