#pragma once
// E03 "hybrid.sprite_layer" kernel on the render graph (see sprite_layer.hpp for the quad layout and CPU reference).
// One compute pass writes a graph-transient RGBA16F image (premultiplied) from the frame's quads; the quads live in a
// persistently mapped CpuToGpu storage buffer, one region per ring slot (serial % framesInFlight), and the pass's
// classic two-binding descriptor set (one per ring slot) is rewritten in the pass callback. Steady state: no heap
// allocations (fixed quad capacity, fixed records).
#if defined(FUSE_HAS_VULKAN_RHI)

#include <fuse/hybrid/sprite_layer.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {
class GpuAllocator;
class VulkanDevice;
} // namespace fuse::renderer

namespace fuse::hybrid {

namespace rg = ::fuse::renderer::rg;

struct SpriteLayerGpuDesc {
    renderer::VulkanDevice* device = nullptr;
    renderer::GpuAllocator* allocator = nullptr;
    u32 framesInFlight = 3;
    u32 maxQuads = 1024; ///< per frame (sprites + UI)
};

class SpriteLayerGpu {
public:
    static constexpr u32 kMaxSlots = 4u;

    SpriteLayerGpu() = default;
    ~SpriteLayerGpu();
    SpriteLayerGpu(const SpriteLayerGpu&) = delete;
    SpriteLayerGpu& operator=(const SpriteLayerGpu&) = delete;

    /// False without a Vulkan device / the embedded kernel (the stub backend always fails).
    bool init(const SpriteLayerGpuDesc& desc);
    /// The caller must have retired every frame that used the layer.
    void destroy();
    bool valid() const { return m_pipeline != nullptr; }
    u32 maxQuads() const { return m_desc.maxQuads; }

    /// Selects the ring slot of the frame (serial % framesInFlight): its quads are written by addPass.
    void beginFrame(u64 serial);
    /// Copies `count` quads (clamped to maxQuads) into the slot and records "hybrid.sprite_layer" into a new
    /// width x height RGBA16F transient (Sampled-readable), which is returned (invalid when not valid() or the extent
    /// is empty). One pass per frame.
    rg::TextureRef addPass(rg::Graph& graph, const SpriteQuad* quads, u32 count, u32 width, u32 height);

    u32 lastQuadCount() const { return m_record.count; }

private:
    struct Record {
        SpriteLayerGpu* self = nullptr;
        void* set = nullptr; ///< VkDescriptorSet
        rg::TextureRef target;
        rg::BufferRef quads;
        u64 offset = 0;
        u64 bytes = 0;
        u32 extent[2] = {0u, 0u};
        u32 count = 0;
    };
    static void record(const rg::PassContext& context, void* user);

    SpriteLayerGpuDesc m_desc{};
    renderer::Buffer m_buffer{};
    u64 m_slotBytes = 0;
    void* m_setLayout = nullptr;
    void* m_layout = nullptr;
    void* m_pipeline = nullptr;
    void* m_pool = nullptr;
    void* m_sets[kMaxSlots] = {};
    Record m_record{};
    u32 m_slot = 0;
};

} // namespace fuse::hybrid

#endif // defined(FUSE_HAS_VULKAN_RHI)
