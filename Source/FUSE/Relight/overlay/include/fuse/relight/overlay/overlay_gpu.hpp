// FUSE Relight RL-6.1: the developer overlay's GPU pass (an IFrameRecorder for RL-4.1's FrameGpu).
//
// At Present, with the overlay shown, FrameOrchestrator::postComposite hands FUSE a copy of the back buffer (the
// target image, like the back buffer) and composites it back after FUSE's graph. The graph (RG v2: declared accesses,
// FrameGpu records every barrier) is three passes:
//
//   overlay.copy_in    target[region] -> region image (R32_UINT: the back buffer's texels as raw words)
//   overlay.compose    compute (shaders/overlay_compose.{slang,comp}): debug view (optional) + the UI layer
//   overlay.copy_out   region image -> target[region]
//
// The region is the panel rectangle, or the whole frame with a debug view; the rest of the target keeps the back
// buffer's texels, so the composite changes the panel rectangle only. The UI layer (raster.hpp, CPU) is uploaded into
// a host-visible ring slot per frame (3 slots; a slot is reused once the submission that read it completed). The
// debug buffer is the frame renderer's image (frame_renderer.hpp DebugImage), imported into the graph in its resting
// layout and sampled with texelFetch.
//
// Vulkan through fuse_rhi's volk globals (the renderer RL-4.1 adopted on the host's device). Images come from an
// IOverlayDevice (FrameGpu + importImage in the tap, plain Vulkan in the harness).
#pragma once

#include <fuse/relight/overlay/overlay_types.h>
#include <fuse/relight/overlay/ui.hpp>
#include <fuse/relight/render/frame/frame_gpu.hpp>
#include <fuse/relight/render/frame/frame_renderer.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fuse::relight::overlay {

class IOverlayDevice {
public:
    virtual ~IOverlayDevice() = default;
    /// A device-local optimal image like `like` with `usage`, in GENERAL (`general`), handed to the host when
    /// `hostImport` (GpuImage::host set; the target image).
    virtual bool createImage(const tap::HostImageInfo& like, std::uint32_t usage, bool hostImport,
                             render::frame::GpuImage& out) = 0;
    virtual void destroyImage(render::frame::GpuImage& image) = 0;
    /// Returns once the submission tagged `serial` completed (0: nothing to wait for).
    virtual void waitSerial(std::uint64_t serial) = 0;
};

/// One overlay frame.
struct OverlayFrameDesc {
    std::uint32_t frameW = 0, frameH = 0, frameFormat = 0; ///< the back buffer (VkFormat)
    Rect panel;                                            ///< layer rectangle (empty: no layer)
    const std::vector<Color>* layer = nullptr;             ///< panel.w * panel.h texels
    const render::frame::DebugImage* debug = nullptr;      ///< the debug view's buffer (null: none)
    std::uint64_t serial = 0;                              ///< expected tag of the submission (submitted() fixes it)
};

class OverlayGpu final : public render::frame::IFrameRecorder {
public:
    enum class Language : std::uint8_t { Slang, Glsl };
    static constexpr std::uint32_t kSlots = 3;

    OverlayGpu();
    ~OverlayGpu() override;
    OverlayGpu(const OverlayGpu&) = delete;
    OverlayGpu& operator=(const OverlayGpu&) = delete;

    /// Slang when it was compiled (else GLSL).
    static Language defaultLanguage();
    static bool languageAvailable(Language language);
    /// 8-bit RGBA / BGRA back buffers (UNORM / SRGB); `bgra` = the channel order.
    static bool supportedFormat(std::uint32_t vkFormat, bool& bgra);

    /// Pipeline, layouts, sampler, descriptor pool (VkDevice / VkPhysicalDevice values; volk globals loaded).
    bool init(std::uint64_t device, std::uint64_t physicalDevice, IOverlayDevice& images, Language language);
    /// Waits for every slot and destroys everything (images through the IOverlayDevice).
    void shutdown();
    bool ready() const { return m_ready; }

    /// CPU half of a frame: (re)creates the target / region images for the back buffer, uploads the layer and writes
    /// the slot's descriptor set. False (lastError) when there is nothing to draw or it cannot.
    bool prepare(const OverlayFrameDesc& frame);
    /// The submission of the prepared frame was tagged `serial` (0: it was not submitted): the ring slot and the
    /// debug view are reused / destroyed only after it completed.
    void submitted(std::uint64_t serial);
    /// The image the host copies the back buffer into and composites back (valid after prepare()).
    const render::frame::GpuImage& target() const { return m_target; }
    const OverlayPush& push() const { return m_push; }
    const std::string& lastError() const { return m_error; }
    std::uint32_t imageRebuilds() const { return m_rebuilds; }

    // ---- IFrameRecorder ------------------------------------------------------------------------------------
    bool declare(renderer::rg::Graph& graph, renderer::rg::TextureRef output,
                 const render::frame::GpuImage& outputImage) override;
    Image image(std::uint32_t resource) const override;
    std::uint64_t buffer(std::uint32_t resource) const override;
    void record(std::uint32_t pass, std::uint64_t commandBuffer) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    IOverlayDevice* m_images = nullptr;
    bool m_ready = false;
    render::frame::GpuImage m_target, m_region, m_dummy;
    OverlayPush m_push;
    std::string m_error;
    std::uint32_t m_rebuilds = 0;
};

} // namespace fuse::relight::overlay
