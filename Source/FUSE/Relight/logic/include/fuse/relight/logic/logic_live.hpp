// FUSE Relight RL-3.5: Logic graphs inside d3d9.dll (capture mode).
//
// LogicFrameProcessor wraps RL-3.4's frame processor (replace/replace_live.hpp CaptureReplaceProcessor; the tap
// installs it through attachLogicProcessor). For every presented frame, after the replacement engine ran:
//   1. when the engine's generation changed (first load, hot reload), each loaded Remix USD mod of the stack is
//      re-read (RL-3.1 readStage) and its OmniGraph prims parsed (graph_usd_parser.hpp); every graph instance is
//      dropped, as upstream on a replacement change;
//   2. the frame's senses are gathered (logic_context.hpp FrameInputs): delta time (relight.logic.fixedDeltaTime
//      or the measured time between presented frames), the main camera, per asset hash / stage-0 texture hash
//      usage counts of the committed draws, the game lights' hashes, the fog hash, the keyboard (Windows builds);
//   3. every committed draw whose mesh replacement (the stack's winning mesh_<H>) has graphs is a graph owner,
//      keyed by its RL-1.7 instance id; its prim table is snapshotted from the replaced draw: "<root>/mesh" = the
//      original draw (objectToWorld, captured bounds, skinning bones), mesh parts (part transform, record bounds),
//      attached lights (world position);
//   4. LogicRuntime::runFrame (logic_runtime.hpp) updates the graphs and applies the option layer requests, which
//      take effect at the next frame (the replacement engine reads rtx.enableReplacement* at beginFrame);
//   5. the frame's "replace_frame" record line gets a "logic" member (logicFrameJson).
// FUSE-native (store) mods carry no graph topology yet: their graphs are listed by the importer but not run.
#pragma once

#include <fuse/relight/logic/logic_runtime.hpp>
#include <fuse/relight/tap/capture_tap.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>

namespace fuse::relight::replace {
class CaptureReplaceProcessor;
}

namespace fuse::relight::logic {

class LogicFrameProcessor final : public tap::IFrameProcessor {
public:
    /// `inner`: RL-3.4's processor (graphs run only when it is a replace::CaptureReplaceProcessor).
    explicit LogicFrameProcessor(std::unique_ptr<tap::IFrameProcessor> inner);
    ~LogicFrameProcessor() override;

    Output processFrame(std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws,
                        const scene::TranslatedFrame* translatedFrame, bool presented) override;

    LogicRuntime& runtime() { return m_runtime; }
    tap::IFrameProcessor* inner() { return m_inner.get(); }
    const LogicFrameReport& lastReport() const { return m_lastReport; }

private:
    void reloadGraphs();
    FrameInputs gatherInputs(std::uint64_t frame, const std::vector<tap::CaptureDrawRecord>& draws,
                             const scene::TranslatedFrame* translatedFrame);

    std::unique_ptr<tap::IFrameProcessor> m_inner;
    replace::CaptureReplaceProcessor* m_replace = nullptr;
    LogicRuntime m_runtime;
    std::uint64_t m_generation = ~0ull;
    std::string m_pathBase; ///< the working directory ('/' separators), for relative paths in the record
    bool m_haveLastTime = false;
    std::chrono::steady_clock::time_point m_lastTime;
    std::set<std::uint32_t> m_keysDown;
    LogicFrameReport m_lastReport;
};

/// Wraps `processor` with the Logic runtime; returns it unchanged when it is null or rtx.graph.enable is off at
/// device creation.
std::unique_ptr<tap::IFrameProcessor> attachLogicProcessor(std::unique_ptr<tap::IFrameProcessor> processor);

} // namespace fuse::relight::logic
