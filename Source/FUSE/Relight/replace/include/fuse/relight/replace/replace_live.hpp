// FUSE Relight RL-3.4: the replacement engine inside d3d9.dll (capture mode).
//
// CaptureReplaceProcessor is CaptureTap's frame processor (tap/capture/capture_tap.hpp IFrameProcessor). At every
// flushed frame it:
//   1. runs ReplacementEngine::beginFrame (hot reload: the notifications since the previous frame);
//   2. feeds the committed draws to its own RL-1.7 SceneModel (as the live capture export does), so every draw has
//      its stable instance id, and looks each up (ReplacementEngine::replaceDraw: the draw's geometry hash
//      components, legacy key inputs, stage-0 texture hash, categories with the geometry lists applied,
//      objectToWorld);
//   3. ends the frame with TranslateTap's game lights (ReplacementEngine::endFrame) and returns the record lines
//      (replace_json.hpp). The last ReplacedFrame and ReplacedDraws stay available (lastFrame / lastDraws) for an
//      in-process consumer (the renderer, RL-4.x).
// Injection-time feed (RL-4.x, render/frame RenderTap): processPending() runs steps 1-2 early for the draws recorded
// before the injection point, so the renderer's GPU scene gets this frame's replaced draws and (previewLights) lights
// when FUSE renders it; processFrame for the same frame then continues after them. The record is identical to
// processing the whole frame at the flush (same calls, same order).
// The replacements are advisory while Relight does not render (the tap still returns DrawDecision::Raster), so
// DXVK's output is unchanged; the record shows what the renderer will draw.
//
// createCaptureReplaceProcessor (called by createTapForDevice) builds one from the options (replace_options.hpp):
// null when relight.replace.enable is off, or when no search root or explicit mod exists (nothing to load or
// watch). The game id defaults to the executable's name without extension.
#pragma once

#include <fuse/relight/replace/replacement_engine.hpp>
#include <fuse/relight/tap/capture_tap.hpp>

#include <memory>
#include <vector>

namespace fuse::relight::replace {

class CaptureReplaceProcessor final : public tap::IFrameProcessor {
public:
    /// `assetRule`: the capture's rtx.geometryAssetHashRuleString (GeometryCapture config), for SceneModel.
    CaptureReplaceProcessor(EngineConfig config, hash::HashRule assetRule);
    ~CaptureReplaceProcessor() override;

    Output processFrame(std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws,
                        const scene::TranslatedFrame* translatedFrame, bool presented) override;

    /// The injection-time half (see the header comment): beginFrame (first call of a frame) and the per-draw lookups
    /// of draws [processed so far, count) of the current frame's pending draws. `classifications`: per draw in
    /// [0, count), as the flush will have them (the geometry categories applied). Returns the number of draws of the
    /// frame processed so far; processFrame for `frame` continues from there.
    std::size_t processPending(std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws, std::size_t count,
                               const std::vector<scene::DrawClassification>& classifications);
    /// The frame's light list as it stands (ReplacementEngine::previewLights of `gameLights`).
    std::vector<ReplacedLight> previewLights(const std::vector<scene::LightRecord>& gameLights) const {
        return m_engine.previewLights(gameLights);
    }

    ReplacementEngine& engine() { return m_engine; }
    const ReplacedFrame& lastFrame() const { return m_lastFrame; }
    /// Per draw of the last frame; nullopt for draws that are not committed.
    const std::vector<std::optional<ReplacedDraw>>& lastDraws() const { return m_lastDraws; }

private:
    struct Scene;
    /// Step 2 for one draw (no-op for draws that are not translated and committed).
    void processDraw(std::size_t index, const tap::CaptureDrawRecord& r, const scene::DrawClassification& cls,
                     std::string& json);
    ReplacementEngine m_engine;
    hash::HashRule m_assetRule;
    std::unique_ptr<Scene> m_scene;
    ReplacedFrame m_lastFrame;
    std::vector<std::optional<ReplacedDraw>> m_lastDraws;
    // The frame processPending started (processFrame completes it).
    std::optional<std::uint64_t> m_pendingFrame;
    std::size_t m_pendingCount = 0;
    std::vector<std::string> m_pendingJson;
};

/// The processor createTapForDevice installs; null when disabled or there is nothing to load (see above).
std::unique_ptr<tap::IFrameProcessor> createCaptureReplaceProcessor(unsigned deviceOrdinal);

} // namespace fuse::relight::replace
