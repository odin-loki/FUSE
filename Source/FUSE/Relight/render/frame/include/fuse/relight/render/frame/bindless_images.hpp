// FUSE Relight RL-4.1: external-image bindless registration (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.3).
//
// Game textures stay DXVK images. BindlessImageRegistry puts each one into the FUSE bindless registry as an
// *external, non-owned* sampled-image slot: FUSE creates only an image view (through IImageViewFactory; none
// without a device) and never destroys the VkImage. Images FUSE owns (passthrough swap twins, FUSE's frame
// images) register the same way with `owned` set, so a shader sees one handle space.
//
// The heap behind the registry (IBindlessHeap):
//   RendererBindlessHeap  the renderer's WP-0.4 BindlessDescriptors (GPU descriptor set / descriptor buffer, or
//                         its CPU heap): descriptors are written when the heap has a GPU backend and a view exists
//                         (bindless_renderer_heap.hpp; RendererContext brings it up on the adopted device);
//   CpuBindlessHeap       a generation-checked slot table with the same 32-bit shader handles
//                         (renderer::packBindlessShaderHandle), when the device could not be adopted (DXVK runs on
//                         its own device: relight.device.import off, or no bootstrap).
//
// Lifetime: the tap reports DXVK's image destruction (onImageDestroy); release() retires the slot at once
// (generation bump: a stale shader handle no longer validates) but keeps the descriptor index and the view
// until the GPU work that could still read them completed: collect(completedSerial) reclaims every slot
// retired at a serial <= completedSerial and destroys its view. Serials are the frame orchestrator's acquire
// timeline values (the host signals them after all its prior work). A texture registered again with another
// VkImage (DXVK recreated it) retires the old slot first.
//
// Single-threaded (the tap's event thread).
#pragma once

#include <fuse/relight/tap/relight_tap.hpp>
#include <fuse/renderer/vk/bindless.hpp>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace fuse::relight::render::frame {

struct ExternalImageDesc {
    tap::ResourceId texture = tap::kNoResource;
    std::uint64_t vkImage = 0;
    std::uint32_t vkFormat = 0;
    std::uint32_t viewType = 1; ///< VkImageViewType (1 = 2D, 3 = cube, 2 = 3D)
    std::uint32_t width = 1, height = 1, depth = 1;
    std::uint32_t mipLevels = 1, arrayLayers = 1;
    bool owned = false; ///< FUSE-owned image (swap twin / frame image) rather than a DXVK image
};

/// The bindless heap the registry writes to.
class IBindlessHeap {
public:
    virtual ~IBindlessHeap() = default;
    /// A sampled-image slot for `image` (view: raw VkImageView, 0 = none). Invalid when full.
    virtual renderer::BindlessSlotHandle registerTexture(const ExternalImageDesc& image, std::uint64_t view) = 0;
    /// Invalidates `slot` now; its index is reusable once collect(completed >= serial). False when stale.
    virtual bool retire(renderer::BindlessSlotHandle slot, std::uint64_t serial) = 0;
    virtual std::uint32_t collect(std::uint64_t completedSerial) = 0;
    virtual bool validate(renderer::BindlessSlotHandle slot) const = 0;
    /// The 32-bit shader handle (0 when stale).
    virtual std::uint32_t shaderHandle(renderer::BindlessSlotHandle slot) const = 0;
    virtual bool gpuDescriptors() const = 0;
};

/// Slots and generations only (see the header comment). Capacity renderer::kMaxTextures.
class CpuBindlessHeap final : public IBindlessHeap {
public:
    renderer::BindlessSlotHandle registerTexture(const ExternalImageDesc& image, std::uint64_t view) override;
    bool retire(renderer::BindlessSlotHandle slot, std::uint64_t serial) override;
    std::uint32_t collect(std::uint64_t completedSerial) override;
    bool validate(renderer::BindlessSlotHandle slot) const override;
    std::uint32_t shaderHandle(renderer::BindlessSlotHandle slot) const override;
    bool gpuDescriptors() const override { return false; }

private:
    struct Slot {
        std::uint32_t generation = 0;
        bool occupied = false;
        bool retired = false;
        std::uint64_t serial = 0;
    };
    std::vector<Slot> m_slots;
    std::vector<std::uint32_t> m_free;
};

/// Creates / destroys the sampled views the registry writes into descriptors. Raw VkImageView values.
class IImageViewFactory {
public:
    virtual ~IImageViewFactory() = default;
    virtual std::uint64_t createView(const ExternalImageDesc& image) = 0; ///< 0 on failure
    virtual void destroyView(std::uint64_t view) = 0;
};

struct BindlessImageStats {
    std::uint32_t live = 0;       ///< registered textures
    std::uint32_t external = 0;   ///< of which DXVK-owned
    std::uint32_t owned = 0;      ///< of which FUSE-owned
    std::uint32_t retired = 0;    ///< released, waiting for their serial
    std::uint32_t registered = 0; ///< registrations since creation
    std::uint32_t released = 0;   ///< releases since creation
    std::uint32_t reclaimed = 0;  ///< slots reclaimed by collect() since creation
    std::uint32_t viewFailures = 0;
    std::uint32_t heapFull = 0;
};

class BindlessImageRegistry {
public:
    /// `heap` and `views` (may be null: no descriptors) are not owned.
    BindlessImageRegistry(IBindlessHeap& heap, IImageViewFactory* views);
    ~BindlessImageRegistry();
    BindlessImageRegistry(const BindlessImageRegistry&) = delete;
    BindlessImageRegistry& operator=(const BindlessImageRegistry&) = delete;

    /// Registers (or refreshes) `image` for image.texture. Same VkImage: the existing slot. Invalid handle
    /// when vkImage is 0 or the heap is full. `serial`: retire serial of a replaced slot.
    renderer::BindlessSlotHandle registerImage(const ExternalImageDesc& image, std::uint64_t serial);
    /// Retires the texture's slot at `serial` (see the header comment). False when not registered.
    bool release(tap::ResourceId texture, std::uint64_t serial);
    /// Reclaims slots (and destroys views) retired at serials <= completedSerial. Returns the count.
    std::uint32_t collect(std::uint64_t completedSerial);
    /// Releases every slot and reclaims them now (the caller guarantees the GPU is idle).
    void releaseAll();

    renderer::BindlessSlotHandle handleOf(tap::ResourceId texture) const;
    /// The 32-bit shader handle of a registered texture, 0 when none.
    std::uint32_t shaderHandle(tap::ResourceId texture) const;
    /// The view written for a texture (0 without a view factory).
    std::uint64_t viewOf(tap::ResourceId texture) const;
    const BindlessImageStats& stats() const { return m_stats; }
    const IBindlessHeap& heap() const { return m_heap; }

private:
    struct Entry {
        renderer::BindlessSlotHandle slot;
        std::uint64_t vkImage = 0;
        std::uint64_t view = 0;
        bool owned = false;
    };
    struct Retired {
        std::uint64_t view = 0;
        std::uint64_t serial = 0;
    };
    void retireEntry(const Entry& e, std::uint64_t serial);

    IBindlessHeap& m_heap;
    IImageViewFactory* m_views;
    std::unordered_map<tap::ResourceId, Entry> m_entries;
    std::vector<Retired> m_retired;
    BindlessImageStats m_stats;
};

} // namespace fuse::relight::render::frame
