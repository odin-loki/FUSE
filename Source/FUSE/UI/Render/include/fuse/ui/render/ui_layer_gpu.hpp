#pragma once
// "ui.draw_list" on the render graph: the UiVkRenderer kernel as one compute pass that blends a packed UI draw list
// onto an RGBA8 (R8G8B8A8_UNORM) graph image with StorageReadWrite access (composite over the scene / sprite layer, or
// a cleared overlay). Mirrors hybrid::SpriteLayerGpu: per-ring-slot host-mapped buffers, no steady-state allocations.
#if defined(FUSE_HAS_VULKAN_RHI)

#include "fuse/ui/render/ui_vk.hpp"

#include <fuse/renderer/rg/graph.hpp>

namespace fuse::renderer {
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::ui::render {

namespace rg = ::fuse::renderer::rg;

class UiLayerGpu {
public:
    UiLayerGpu() = default;
    UiLayerGpu(const UiLayerGpu&) = delete;
    UiLayerGpu& operator=(const UiLayerGpu&) = delete;

    /// False without a Vulkan device / the embedded kernel (the stub backend always fails).
    bool init(renderer::VulkanDevice* device, UiVkDesc limits = {});
    void destroy() { m_vk.destroy(); }
    bool valid() const { return m_vk.valid(); }

    /// Selects the ring slot of the frame (serial % framesInFlight).
    void beginFrame(u64 serial) { m_slot = u32(serial % m_frames); }
    /// Uploads `data` into the slot and records "ui.draw_list" over `target` (an RGBA8 storage-capable image of
    /// data.push extent). Returns `target`; invalid when not valid() or the upload overflowed the limits.
    rg::TextureRef addPass(rg::Graph& graph, rg::TextureRef target, const GpuDrawData& data);
    /// Convenience: a new RGBA8 transient for the overlay. Its contents are undefined until a clear pass writes it.
    static rg::TextureRef createTarget(rg::Graph& graph, u32 width, u32 height);

private:
    struct Record {
        UiLayerGpu* self = nullptr;
        rg::TextureRef target;
        u32 slot = 0;
    };
    static void record(const rg::PassContext& context, void* user);

    UiVkRenderer m_vk;
    Record m_record{};
    u32 m_slot = 0;
    u32 m_frames = 1;
};

} // namespace fuse::ui::render

#endif // defined(FUSE_HAS_VULKAN_RHI)
