// "ui.draw_list" render-graph pass: see include/fuse/ui/render/ui_layer_gpu.hpp.
#if defined(FUSE_HAS_VULKAN_RHI)

#include "fuse/ui/render/ui_layer_gpu.hpp"

#include <fuse/renderer/vk/device.hpp>

namespace fuse::ui::render {

bool UiLayerGpu::init(renderer::VulkanDevice* device, UiVkDesc limits) {
    if (device == nullptr || !device->isValid() || device->nativeHandle() == nullptr) return false;
    limits.device = device->nativeHandle();
    limits.physicalDevice = device->nativePhysicalDevice();
    if (limits.framesInFlight == 0 || limits.framesInFlight > UiVkRenderer::kMaxSlots)
        limits.framesInFlight = UiVkRenderer::kMaxSlots;
    m_frames = limits.framesInFlight;
    return m_vk.init(limits);
}

rg::TextureRef UiLayerGpu::createTarget(rg::Graph& graph, u32 width, u32 height) {
    rg::ImageDesc d{};
    d.width = width;
    d.height = height;
    d.format = 37u; // VK_FORMAT_R8G8B8A8_UNORM, the kernel's rgba8 image2D
    d.name = "ui.draw_list.target";
    return graph.createImage(d);
}

rg::TextureRef UiLayerGpu::addPass(rg::Graph& graph, rg::TextureRef target, const GpuDrawData& data) {
    if (!valid() || !m_vk.upload(m_slot, data)) return {};
    m_record = Record{this, target, m_slot};
    graph.addPass("ui.draw_list", &UiLayerGpu::record, &m_record)
        .use(target, rg::Access::StorageReadWrite, {}, rg::kStageCompute);
    return target;
}

void UiLayerGpu::record(const rg::PassContext& context, void* user) {
    const Record& r = *static_cast<const Record*>(user);
    r.self->m_vk.record(context.commandBuffer, context.imageView(r.target), r.slot);
}

} // namespace fuse::ui::render

#endif // defined(FUSE_HAS_VULKAN_RHI)
