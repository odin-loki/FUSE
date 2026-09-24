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

    ReplacementEngine& engine() { return m_engine; }
    const ReplacedFrame& lastFrame() const { return m_lastFrame; }
    /// Per draw of the last frame; nullopt for draws that are not committed.
    const std::vector<std::optional<ReplacedDraw>>& lastDraws() const { return m_lastDraws; }

private:
    struct Scene;
    ReplacementEngine m_engine;
    hash::HashRule m_assetRule;
    std::unique_ptr<Scene> m_scene;
    ReplacedFrame m_lastFrame;
    std::vector<std::optional<ReplacedDraw>> m_lastDraws;
};

/// The processor createTapForDevice installs; null when disabled or there is nothing to load (see above).
std::unique_ptr<tap::IFrameProcessor> createCaptureReplaceProcessor(unsigned deviceOrdinal);

} // namespace fuse::relight::replace
