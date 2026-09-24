// FUSE Relight RL-4.1 -> RL-4.2: the hook through which the frame orchestration drives a renderer of the captured
// scene at the injection point (relight.frame.mode = raster: Relight/render/raster, RL-4.2).
//
// At the injection point RenderTap takes the capture tap's lock (CaptureTap::visitPendingDraws), feeds the draws
// recorded so far to the GPU scene, and hands the same draws to IFrameRenderer::prepare (CPU work: the renderer copies
// what it needs; nothing may be kept past the call). The orchestrator then records the renderer's frame graph
// (IFrameRecorder) into FUSE's frame submission. Vulkan-free.
#pragma once

#include <fuse/relight/render/frame/frame_gpu.hpp>
#include <fuse/relight/render/frame/frame_options.hpp>
#include <fuse/relight/render/frame/scene_feed.hpp>
#include <fuse/relight/scene/lights/legacy_light.hpp>
#include <fuse/relight/tap/capture_tap.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fuse::relight::replace {
struct MaterialDef;
}

namespace fuse::relight::render::frame {

class BindlessImageRegistry;
class RendererContext;

/// The frame so far, at the injection point (valid during IFrameRenderer::prepare only).
struct FrameInputs {
    std::uint64_t frame = 0;
    const std::vector<tap::CaptureDrawRecord>* draws = nullptr; ///< the capture tap's pending draws
    std::uint32_t count = 0;                                    ///< draws before the injection point
    /// Per draw in [0, count): the classification with the geometry categories applied (as the flush would).
    const std::vector<scene::DrawClassification>* classifications = nullptr;
    /// Per draw in [0, count): a scene draw (translated, committed, captured geometry).
    const std::vector<std::uint8_t>* sceneDraw = nullptr;
    const std::vector<scene::LightRecord>* lights = nullptr;    ///< the frame's game lights so far
    /// The lights the GPU scene received at this injection (with RL-3.4: the replaced list), keyed as the GPU scene
    /// keys them; null when the scene was fed at the flush (then `lights`).
    const std::vector<AdapterLight>* sceneLights = nullptr;
    bool haveClear = false;
    std::uint32_t clearColor = 0;                               ///< the back buffer's last clear (D3DCOLOR)
    tap::CaptureTap* capture = nullptr;                         ///< texture tracker, replacement processor
    const tap::HostImageInfo* backBuffer = nullptr;
    /// RL-3.2 material replacements: the replacement engine's material for a legacy material hash (null: none, or
    /// no engine).
    std::function<const replace::MaterialDef*(hash::Hash64)> replacementMaterial;
};

class IFrameRenderer : public IFrameRecorder {
public:
    ~IFrameRenderer() override = default;
    /// The device's renderer came up (or went away: detach). `registry` names every game texture's bindless slot.
    virtual bool attach(RendererContext& context, tap::IFrameHost& host, BindlessImageRegistry& registry) = 0;
    virtual void detach() = 0;
    /// CPU half of the frame (see the header comment). False: nothing to render this frame (passthrough).
    /// `retireSerial`: the serial resources released now are tagged with.
    virtual bool prepare(const FrameInputs& in, std::uint64_t retireSerial) = 0;
    /// Reclaims per-frame resources whose serial completed.
    virtual void collect(std::uint64_t completedSerial) = 0;
    /// Members (no braces) of the frame record's "raster" object for the last prepared frame.
    virtual std::string recordJson() const = 0;
    /// Members of the header record's "raster" object (configuration, tier, features).
    virtual std::string headerJson() const = 0;
    virtual const std::string& lastError() const = 0;
};

} // namespace fuse::relight::render::frame
