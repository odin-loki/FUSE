#pragma once

#include <fuse/profiler/profiler.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

/// One timed slice (a closed `fuse::profiler` scope or GPU / CUDA complete marker).
struct ProfilerSlice {
    std::string name;
    u32 threadId = 0;
    u32 depth = 0;           ///< 0 = root slice on its thread
    u64 startNs = 0;
    u64 durationNs = 0;
    u64 selfNs = 0;          ///< duration minus direct children
    u32 parent = UINT32_MAX; ///< index into `slices()`, UINT32_MAX for roots
    std::vector<u32> children;
};

/// Aggregated call-path node of a flame graph: identical name paths merge across calls.
struct FlameNode {
    std::string name;
    u64 totalNs = 0;
    u64 selfNs = 0;
    u32 callCount = 0;
    u32 depth = 0;
    std::vector<u32> children; ///< indices into `flameNodes()`, sorted by name
};

/// Qt-free model behind the profiler panel's flame graph (B6.10). Consumes the chronological
/// `fuse::profiler` event ring: per thread, Begin / End events are paired by scope id with a
/// stack (so nesting follows the actual call structure, not only the recorded depth), GPU / CUDA
/// complete events become leaf slices, and slices are merged by call path into flame nodes.
/// Ends whose Begin fell out of the ring and scopes still open at capture are counted, not drawn.
class ProfilerFlameGraph {
public:
    /// Rebuild from the current contents of the global profiler ring buffer.
    void buildFromProfiler();
    /// Rebuild from an explicit chronological event list (e.g. a saved capture).
    void build(const std::vector<profiler::ProfileEvent>& events);

    [[nodiscard]] const std::vector<ProfilerSlice>& slices() const { return m_slices; }
    [[nodiscard]] const std::vector<FlameNode>& flameNodes() const { return m_nodes; }
    /// Roots of the merged flame graph, one per distinct root scope name (all threads).
    [[nodiscard]] const std::vector<u32>& flameRoots() const { return m_roots; }
    [[nodiscard]] const FlameNode* findPath(const std::vector<std::string>& path) const;

    [[nodiscard]] u32 threadCount() const { return m_threadCount; }
    [[nodiscard]] u32 maxDepth() const { return m_maxDepth; }
    [[nodiscard]] u32 unmatchedEndCount() const { return m_unmatchedEnds; }
    [[nodiscard]] u32 openScopeCount() const { return m_openScopes; }
    [[nodiscard]] u64 captureStartNs() const { return m_startNs; }
    [[nodiscard]] u64 captureEndNs() const { return m_endNs; }

private:
    void mergeSlice_(u32 sliceIndex, u32 parentNode);

    std::vector<ProfilerSlice> m_slices;
    std::vector<FlameNode> m_nodes;
    std::vector<u32> m_roots;
    u32 m_threadCount = 0;
    u32 m_maxDepth = 0;
    u32 m_unmatchedEnds = 0;
    u32 m_openScopes = 0;
    u64 m_startNs = 0;
    u64 m_endNs = 0;
};

} // namespace fuse::editor
