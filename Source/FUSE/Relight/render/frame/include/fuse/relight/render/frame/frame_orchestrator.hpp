// FUSE Relight RL-4.1: frame orchestration (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.3).
//
// FrameOrchestrator runs FUSE's part of a frame against an IFrameHost (frame_host.hpp):
//
//   inject()   at the injection point (the first UI draw, or Present without UI):
//                1. (re)create FUSE's frame images when the back buffer's size / format changed (the old ones
//                   are released to the host and destroyed once their acquire value completed);
//                2. passthrough (and raster without a recorder): host copies the back buffer into FUSE's input
//                   image;
//                3. host: flush + signal acquire = A (after everything recorded so far);
//                4. FUSE: frame graph (FrameGpu::submitFrame; raster: the RL-4.2 recorder's passes) waiting
//                   acquire >= A, signalling release = R;
//                5. host: wait release >= R, copy FUSE's output over the back buffer (the composite) - the
//                   host's following draws (the UI) land on top.
//   swapTexture() / onTextureDestroyed(): the passthrough texture swap. A FUSE-owned twin of a game texture
//              (created from IFrameHost::textureInfo) that DXVK samples instead of the texture and keeps in
//              sync with it; registered in the bindless registry as FUSE-owned.
//   collect()  destroys released images and reclaims bindless slots whose acquire value completed.
//
// Timeline values: A and R advance by one per injection (both start at 0; the first injection uses 1). A
// released image is tagged with the next A: every host command that could use it was recorded before that
// signal. Single-threaded: called from tap events (device lock held).
#pragma once

#include <fuse/relight/render/frame/bindless_images.hpp>
#include <fuse/relight/render/frame/frame_gpu.hpp>
#include <fuse/relight/render/frame/frame_options.hpp>
#include <fuse/relight/tap/frame_host.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::relight::render::frame {

struct InjectResult {
    bool injected = false;
    const char* pass = "none"; ///< what FUSE rendered: passthrough, solid or raster
    std::uint64_t acquire = 0, release = 0;
    FrameSubmitStats submit;
    std::string error; ///< why nothing was composited (empty when injected)
};

struct OrchestratorStats {
    std::uint64_t injections = 0;
    std::uint64_t failures = 0;
    std::uint64_t acquireValue = 0, releaseValue = 0;
    std::uint32_t frameImages = 0;       ///< live FUSE frame images (input + output)
    std::uint32_t imageRebuilds = 0;
    std::uint32_t swaps = 0;             ///< live passthrough swaps
    std::uint32_t swapsCreated = 0;
    std::uint32_t swapsRejected = 0;     ///< textures the host could not swap
    std::uint32_t pendingDestroy = 0;    ///< released images waiting for their acquire value
    std::uint32_t destroyed = 0;
};

class FrameOrchestrator {
public:
    /// `bindless`: where FUSE-owned images are registered (may be null). Not owned.
    FrameOrchestrator(FrameConfig config, BindlessImageRegistry* bindless);
    ~FrameOrchestrator();
    FrameOrchestrator(const FrameOrchestrator&) = delete;
    FrameOrchestrator& operator=(const FrameOrchestrator&) = delete;

    /// Attaches to a device's host (onDeviceCreate / onDeviceReset). False (with lastError) when the host's
    /// Vulkan device cannot be used; the orchestrator then stays inert.
    bool attach(tap::IFrameHost* host);
    /// Waits for the host and FUSE, releases and destroys every FUSE image (onDeviceDestroy / before reset).
    void detach();
    bool attached() const { return m_host != nullptr && m_gpu.ready(); }
    /// Where FUSE-owned images are registered from now on (the registry is recreated when the device's renderer
    /// comes up). Only while detached.
    void setBindless(BindlessImageRegistry* bindless) { m_bindless = bindless; }

    /// `recorder`: relight.frame.mode = raster's frame graph (RL-4.2), null when it has nothing to render (the
    /// frame is then passthrough). Ignored in the other modes.
    InjectResult inject(IFrameRecorder* recorder = nullptr);
    /// RL-6.1 (the developer overlay at Present): one more round on the same timelines after this frame's
    /// injection: the host copies the back buffer into `image` (an importImage'd GpuImage like the back buffer),
    /// `recorder`'s graph runs with `image` as its output, and the host composites `image` back.
    InjectResult postComposite(IFrameRecorder& recorder, const GpuImage& image);
    /// Creates the passthrough twin of `texture` and hands it to the host. False when not swappable.
    bool swapTexture(const tap::TextureDesc& texture);
    /// The texture is gone (the host already dropped its swap): the twin is retired.
    void onTextureDestroyed(tap::ResourceId texture);
    /// Destroys retired images / reclaims bindless slots whose acquire value completed. Returns the count.
    std::uint32_t collect();

    /// Serial for resources released now: the acquire value the next injection signals.
    std::uint64_t retireSerial() const { return m_acquire + 1; }
    const FrameConfig& config() const { return m_config; }
    const OrchestratorStats& stats() const { return m_stats; }
    const std::string& lastError() const { return m_error; }
    FrameGpu& gpu() { return m_gpu; }
    /// The FUSE image composited last (tests).
    const GpuImage& outputImage() const { return m_output; }

    /// True for textures the passthrough swap covers: sampled colour textures with a GPU image (not render
    /// targets / depth, not multisampled, not the back buffer, not D3DPOOL_SYSTEMMEM / SCRATCH).
    static bool swappable(const tap::TextureDesc& texture);

private:
    struct Retired {
        GpuImage image;
        std::uint64_t acquire = 0;
    };
    bool ensureFrameImages(const tap::HostImageInfo& backBuffer);
    void retire(GpuImage& image);
    ExternalImageDesc registryDesc(tap::ResourceId id, const GpuImage& image) const;

    FrameConfig m_config;
    BindlessImageRegistry* m_bindless;
    tap::IFrameHost* m_host = nullptr;
    FrameGpu m_gpu;
    GpuImage m_input, m_output;
    std::uint32_t m_frameFormat = 0, m_frameWidth = 0, m_frameHeight = 0;
    std::unordered_map<tap::ResourceId, GpuImage> m_swaps;
    std::vector<Retired> m_retired;
    std::uint64_t m_acquire = 0, m_release = 0;
    OrchestratorStats m_stats;
    std::string m_error;
};

/// Bindless ids for FUSE's own frame images (above every tap texture id).
inline constexpr tap::ResourceId kFrameInputId = 0xFFFFFF00u;
inline constexpr tap::ResourceId kFrameOutputId = 0xFFFFFF01u;
/// Bindless id of a swap twin: the texture id with the top bit set.
inline constexpr tap::ResourceId swapTwinId(tap::ResourceId texture) { return texture | 0x80000000u; }

} // namespace fuse::relight::render::frame
