// FUSE Relight RL-4.1: the renderer's bindless heap behind BindlessImageRegistry (see bindless_renderer_heap.hpp).
#include <fuse/relight/render/frame/bindless_renderer_heap.hpp>

#include <fuse/renderer/resources.hpp>

namespace fuse::relight::render::frame {

namespace rr = fuse::renderer;

// ---- RendererBindlessHeap ----------------------------------------------------------------------------------

rr::BindlessSlotHandle RendererBindlessHeap::registerTexture(const ExternalImageDesc& image, std::uint64_t view) {
    rr::Texture tex;
    tex.image = reinterpret_cast<void*>(static_cast<std::uintptr_t>(image.vkImage));
    tex.view = reinterpret_cast<void*>(static_cast<std::uintptr_t>(view));
    tex.desc.width = image.width;
    tex.desc.height = image.height;
    tex.desc.depth = image.depth;
    tex.desc.mipLevels = image.mipLevels;
    tex.desc.arrayLayers = image.arrayLayers;
    tex.desc.format = static_cast<rr::GpuFormat>(image.vkFormat);
    tex.desc.usage = rr::ImageUsage::Sampled;
    tex.desc.cubeMap = image.viewType == 3u; // VK_IMAGE_VIEW_TYPE_CUBE
    // Non-owned: no allocation. The descriptor (GPU backend + a view) samples the image in
    // SHADER_READ_ONLY_OPTIMAL, the layout DXVK keeps sampled textures in between its submissions.
    return m_heap.registerTextureSlot(tex, false);
}

bool RendererBindlessHeap::retire(rr::BindlessSlotHandle slot, std::uint64_t serial) {
    return m_heap.retireSlot(slot, serial);
}

std::uint32_t RendererBindlessHeap::collect(std::uint64_t completedSerial) { return m_heap.collectRetired(completedSerial); }

bool RendererBindlessHeap::validate(rr::BindlessSlotHandle slot) const { return m_heap.validateSlot(slot); }

std::uint32_t RendererBindlessHeap::shaderHandle(rr::BindlessSlotHandle slot) const { return m_heap.shaderHandle(slot); }

} // namespace fuse::relight::render::frame
