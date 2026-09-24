// FUSE Relight RL-3.5: the Logic graph runtime (docs/plans/FUSE_REMIX_PORT_PLAN.md, Wave R3; the per-frame part of
// dxvk-remix SceneManager::processReplacementGraphs / onFrameEnd and GraphManager at 0867d3c, MIT: semantics).
//
// Graphs belong to mesh replacements (graph_usd_parser.hpp ModGraphs). Every replacement *instance* that carries
// graphs (a draw instance whose mesh_<H> has OmniGraph prims) owns one GraphInstance per graph; the caller names
// the owners present in a frame and runFrame():
//   1. removes the instances of owners that are gone (or whose graphs changed: a mod reload);
//   2. adds instances for new owners; each new instance is updated once immediately (initialize callbacks, then
//      the first update, as GraphBatch::addInstance);
//   3. updates every instance of every graph (GraphManager::update: batches by graph hash, components in
//      topological order) with the frame's inputs;
//   4. applies the option layer requests (OptionManager::applyPendingValues), so option values driven by
//      RtxOptionLayerAction change for the next frame, as upstream's end of frame.
// Evaluation is deterministic: batches, instances and owners are ordered containers and the only inputs are the
// graph states and FrameInputs.
//
// rtx.graph.enable = false unloads every instance (losing state); rtx.graph.pauseGraphUpdates keeps state and
// skips step 3 (logic_options.hpp).
#pragma once

#include <fuse/relight/logic/component_list.hpp>
#include <fuse/relight/logic/graph_batch.hpp>
#include <fuse/relight/logic/graph_usd_parser.hpp>
#include <fuse/relight/logic/logic_context.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fuse::relight::logic {

/// One replacement instance with graphs, present in the frame.
struct GraphOwnerFrame {
    std::uint64_t owner = 0;                   ///< stable key (the RL-1.7 draw instance id in the live runtime)
    const ReplacementGraphs* graphs = nullptr; ///< must stay valid until the next setModGraphs()
    std::string mod;                           ///< reporting
};

struct LogicFrameReport {
    std::uint64_t frame = 0;
    bool enabled = true, paused = false;
    std::size_t graphStates = 0; ///< graphs loaded (all mods)
    std::size_t owners = 0, instances = 0, batches = 0;
    std::size_t added = 0, removed = 0; ///< instances this frame
    std::vector<HeldOptionLayer> layers;
    struct InstanceValues {
        std::uint64_t id = 0, owner = 0;
        std::string mod, graph;
        std::vector<std::pair<std::string, std::string>> outputs; ///< "<node>.<output>" -> formatted value
    };
    std::vector<InstanceValues> values; ///< when recorded, by instance id
};

struct LogicRunOptions {
    bool enabled = true;          ///< rtx.graph.enable
    bool paused = false;          ///< rtx.graph.pauseGraphUpdates
    bool applyOptionLayers = true; ///< run OptionManager::applyPendingValues at the end of the frame
    bool recordValues = true;     ///< fill LogicFrameReport::values
    /// Resolved from the rtx.graph.* / relight.logic.* options now.
    static LogicRunOptions fromOptions();
};

class LogicRuntime {
public:
    LogicRuntime();
    ~LogicRuntime();
    LogicRuntime(const LogicRuntime&) = delete;
    LogicRuntime& operator=(const LogicRuntime&) = delete;

    /// Replaces the loaded graphs (initial load or reload): every instance is removed first (graph state is lost,
    /// as upstream when a replacement changes).
    void setModGraphs(std::vector<ModGraphs> mods);
    const std::vector<ModGraphs>& modGraphs() const { return m_mods; }
    /// The graphs of mesh replacement `hash` in mod `mod` (nullptr: none).
    const ReplacementGraphs* find(const std::string& mod, std::uint64_t hash) const;

    LogicFrameReport runFrame(const FrameInputs& inputs, const std::vector<GraphOwnerFrame>& owners,
                              const LogicRunOptions& options = {});
    /// Removes every instance (releases their option layers).
    void clear();

    GraphManager& manager() { return m_manager; }
    const GraphManager& manager() const { return m_manager; }
    /// Instance ids of `owner` (one per graph, in the replacement's graph order).
    std::vector<std::uint64_t> instancesOf(std::uint64_t owner) const;

private:
    struct OwnerState {
        const ReplacementGraphs* graphs = nullptr;
        std::string mod;
        std::vector<std::uint64_t> instanceIds;
    };
    void removeOwner(std::map<std::uint64_t, OwnerState>::iterator it, std::size_t& removed);

    GraphManager m_manager;
    std::vector<ModGraphs> m_mods;
    std::map<std::uint64_t, OwnerState> m_owners;
};

/// JSON object members (no braces) summarising a frame: graphs, owners, instances, batches, added, removed,
/// layers [{config, priority, references, enabled, strength, threshold}], values [{id, owner, mod, graph,
/// outputs {...}}]. Paths are printed relative to `pathBase` when they lie below it.
std::string logicFrameJson(const LogicFrameReport& report, const std::string& pathBase = {});

} // namespace fuse::relight::logic
