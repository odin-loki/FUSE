// FUSE Relight RL-4.1: the renderer's WP-0.4 bindless heap behind BindlessImageRegistry (bindless_images.hpp). Inside
// d3d9.dll it is the heap of the renderer adopted from DXVK's device (renderer_context.hpp).
#pragma once

#include <fuse/relight/render/frame/bindless_images.hpp>
#include <fuse/renderer/vk/bindless.hpp>

namespace fuse::relight::render::frame {

/// The renderer's BindlessDescriptors (initialised by its owner; not owned).
class RendererBindlessHeap final : public IBindlessHeap {
public:
    explicit RendererBindlessHeap(renderer::BindlessDescriptors& heap) : m_heap(heap) {}
    renderer::BindlessSlotHandle registerTexture(const ExternalImageDesc& image, std::uint64_t view) override;
    bool retire(renderer::BindlessSlotHandle slot, std::uint64_t serial) override;
    std::uint32_t collect(std::uint64_t completedSerial) override;
    bool validate(renderer::BindlessSlotHandle slot) const override;
    std::uint32_t shaderHandle(renderer::BindlessSlotHandle slot) const override;
    bool gpuDescriptors() const override { return m_heap.vulkanDescriptorsReady(); }

private:
    renderer::BindlessDescriptors& m_heap;
};

} // namespace fuse::relight::render::frame
