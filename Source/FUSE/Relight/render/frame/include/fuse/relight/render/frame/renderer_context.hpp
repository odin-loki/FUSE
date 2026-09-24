// FUSE Relight RL-4.1: the FUSE renderer on the host's Vulkan device, inside d3d9.dll
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.2 AD-2, §2.3).
//
// RendererContext adopts the device DXVK runs on (FUSE's RL-1.1 bootstrap created it; the host describes it through
// IFrameHost::deviceCreateInfo) and brings up the renderer objects on it:
//
//   loader     volk is initialised from the host's vkGetInstanceProcAddr (vkloader::initializeWithProcAddr), never
//              from DllMain: d3d9.dll / d3d8.dll link fuse_rhi with FUSE_RHI_VOLK_NO_AUTO_INIT, so nothing opens
//              vulkan-1.dll before the first attach (loaderReport() records how volk was loaded);
//   device     VulkanDevice::adopt (non-owning): the host's instance, physical device, device, enabled extensions,
//              feature chain and graphics queue family; the renderer's tier comes from what is enabled
//              (FUSE_RENDER_TIER_MAX caps it);
//   heap       the WP-0.4 BindlessDescriptors with GPU descriptors (descriptor buffer or descriptor set), behind
//              BindlessImageRegistry through RendererBindlessHeap, with sampled views of DXVK's images (views());
//   scene      the WP-1.1 GpuScene with GPU tables (direct-copy uploads through an UploadQueue on the shared
//              graphics queue) and the GpuSceneAdapter feeding it (sink()).
//
// Queue: every submission the renderer objects make (the upload queue's batches) happens between
// IFrameHost::lockQueue / unlockQueue, i.e. under the RL-1.1 queueCallback lock DXVK takes around its own queue use.
// Serials: the acquire timeline values of the frame orchestrator (a resource retired while preparing the frame that
// signals acquire A is reclaimed once acquire A + 1 completed: the host signals it after waiting for FUSE's frame A).
//
// No Vulkan header here (see frame_gpu.hpp).
#pragma once

#include <fuse/relight/render/frame/bindless_images.hpp>
#include <fuse/relight/render/frame/scene_feed.hpp>
#include <fuse/relight/tap/frame_host.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace fuse::renderer {
class BindlessDescriptors;
class GpuAllocator;
class UploadQueue;
class VulkanDevice;
namespace gpu_scene {
class GpuScene;
}
} // namespace fuse::renderer

namespace fuse::relight::render::frame {

class GpuSceneAdapter;

/// How fuse_rhi's volk loader came up in this process.
struct LoaderReport {
    bool autoInitLinked = false;     ///< the static auto-initialisation ran (FUSE_RHI_VOLK_NO_AUTO_INIT not set)
    bool loadedBeforeAttach = false; ///< volk already had a loader when the first attach began
    bool throughProcAddr = false;    ///< volk was initialised from the host's vkGetInstanceProcAddr
    bool loaded = false;             ///< volk has a loader now
};

struct RendererContextStats {
    bool adopted = false;
    std::string error;                  ///< why attach failed
    std::string device;                 ///< VkPhysicalDeviceProperties::deviceName
    std::string bindlessBackend = "none";
    bool gpuDescriptors = false;        ///< the heap writes Vulkan descriptors
    bool gpuScene = false;              ///< the GPU scene has GPU tables
    std::uint32_t tier = 0;             ///< effective renderer tier (RendererCaps::tier)
    std::uint32_t hardwareTier = 0;
    std::uint64_t uploadBatches = 0;    ///< upload-queue submissions (under the host's queue lock)
    std::uint64_t queueLocks = 0;       ///< lockQueue / unlockQueue pairs taken by the context
};

class RendererContext {
public:
    RendererContext();
    ~RendererContext();
    RendererContext(const RendererContext&) = delete;
    RendererContext& operator=(const RendererContext&) = delete;

    /// See the header comment. `textureHandle`: bindless shader handle of a game texture (the registry's), used
    /// for the GPU scene's material rows. False (stats().error) when the host cannot describe its device or a
    /// piece fails; nothing is left half-created.
    bool attach(tap::IFrameHost& host, GpuSceneAdapterTextureFn textureHandle);
    /// The caller made the GPU idle (host waitIdle + FUSE's own submissions). Destroys every renderer object.
    void detach();
    bool attached() const;

    renderer::VulkanDevice& device();
    renderer::GpuAllocator& allocator();
    renderer::BindlessDescriptors& bindless();
    renderer::gpu_scene::GpuScene& scene();
    GpuSceneAdapter& adapter();
    /// The heap / view factory for BindlessImageRegistry.
    IBindlessHeap& heap();
    IImageViewFactory& views();
    /// The scene sink (the adapter; endFrame commits the GPU scene and flushes its uploads under the queue lock).
    IGpuSceneSink& sink();

    /// Stamps the serial retired resources get from now on (bindless heap and GPU scene).
    void setRetireSerial(std::uint64_t serial);
    /// Submits pending uploads under the host's queue lock. False when a submission failed.
    bool flushUploads();
    /// Reclaims every GPU scene buffer / bindless slot retired at serials <= completed.
    void collect(std::uint64_t completed);

    /// The host's queue lock, counted (for the renderer's own submissions).
    void lockQueue();
    void unlockQueue();

    const RendererContextStats& stats() const;
    /// Process-wide: how volk was loaded (see LoaderReport).
    static LoaderReport loaderReport();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fuse::relight::render::frame
