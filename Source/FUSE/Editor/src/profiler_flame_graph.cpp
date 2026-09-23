#include <fuse/editor/profiler_flame_graph.hpp>

#include <algorithm>
#include <unordered_map>

namespace fuse::editor {

void ProfilerFlameGraph::buildFromProfiler() {
    std::vector<profiler::ProfileEvent> events;
    const u32 count = profiler::eventCount();
    events.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        profiler::ProfileEvent event{};
        if (profiler::tryEventAt(i, event)) {
            events.push_back(event);
        }
    }
    build(events);
}

void ProfilerFlameGraph::build(const std::vector<profiler::ProfileEvent>& events) {
    m_slices.clear();
    m_nodes.clear();
    m_roots.clear();
    m_threadCount = 0;
    m_maxDepth = 0;
    m_unmatchedEnds = 0;
    m_openScopes = 0;
    m_startNs = 0;
    m_endNs = 0;

    struct Open {
        u32 slice = 0;
        u32 scopeId = 0;
    };
    std::unordered_map<u32, std::vector<Open>> stacks; // per thread

    const auto parentOf = [&](u32 thread) -> u32 {
        const std::vector<Open>& stack = stacks[thread];
        return stack.empty() ? UINT32_MAX : stack.back().slice;
    };
    const auto addSlice = [&](const profiler::ProfileEvent& event, u64 start, u64 duration) {
        ProfilerSlice slice{};
        slice.name = event.name != nullptr ? event.name : "";
        slice.threadId = event.threadId;
        slice.parent = parentOf(event.threadId);
        slice.depth = slice.parent == UINT32_MAX ? 0u : m_slices[slice.parent].depth + 1u;
        slice.startNs = start;
        slice.durationNs = duration;
        const u32 index = static_cast<u32>(m_slices.size());
        if (slice.parent != UINT32_MAX) {
            m_slices[slice.parent].children.push_back(index);
        }
        m_maxDepth = std::max(m_maxDepth, slice.depth);
        m_slices.push_back(std::move(slice));
        return index;
    };

    bool first = true;
    for (const profiler::ProfileEvent& event : events) {
        if (first || event.timestampNs < m_startNs) {
            m_startNs = event.timestampNs;
        }
        m_endNs = std::max(m_endNs, event.timestampNs + event.durationNs);
        first = false;

        switch (event.phase) {
        case profiler::EventPhase::Begin: {
            const u32 index = addSlice(event, event.timestampNs, 0u);
            stacks[event.threadId].push_back({index, event.scopeId});
            break;
        }
        case profiler::EventPhase::End: {
            std::vector<Open>& stack = stacks[event.threadId];
            auto match = std::find_if(stack.rbegin(), stack.rend(),
                                      [&](const Open& open) { return open.scopeId == event.scopeId; });
            if (match == stack.rend()) {
                ++m_unmatchedEnds; // its Begin was overwritten in the ring
                break;
            }
            // Scopes opened after the matched one but never closed (should not happen with RAII
            // scopes) are closed at this End so the tree stays well formed.
            while (!stack.empty()) {
                const Open open = stack.back();
                stack.pop_back();
                ProfilerSlice& slice = m_slices[open.slice];
                slice.durationNs = event.timestampNs >= slice.startNs ? event.timestampNs - slice.startNs : 0u;
                if (open.scopeId == event.scopeId) {
                    break;
                }
            }
            break;
        }
        case profiler::EventPhase::GpuComplete:
        case profiler::EventPhase::CudaComplete: {
            const u64 start = event.timestampNs;
            (void)addSlice(event, start, event.durationNs);
            break;
        }
        case profiler::EventPhase::FlowStart:
        case profiler::EventPhase::FlowFinish:
        case profiler::EventPhase::Counter:
            break;
        }
    }

    // Scopes still open at capture time have no duration yet: drop them (and their subtrees are
    // kept only if closed, which cannot happen under an open parent, so drop recursively).
    std::vector<u8> keep(m_slices.size(), 1u);
    for (auto& [thread, stack] : stacks) {
        (void)thread;
        m_openScopes += static_cast<u32>(stack.size());
        for (const Open& open : stack) {
            keep[open.slice] = 0u;
        }
    }
    if (m_openScopes > 0u) {
        std::vector<u32> remap(m_slices.size(), UINT32_MAX);
        std::vector<ProfilerSlice> kept;
        for (u32 i = 0; i < m_slices.size(); ++i) {
            const u32 parent = m_slices[i].parent;
            if (keep[i] == 0u || (parent != UINT32_MAX && keep[parent] == 0u)) {
                keep[i] = 0u;
                continue;
            }
            remap[i] = static_cast<u32>(kept.size());
            kept.push_back(m_slices[i]);
        }
        for (ProfilerSlice& slice : kept) {
            slice.parent = slice.parent == UINT32_MAX ? UINT32_MAX : remap[slice.parent];
            std::vector<u32> children;
            for (u32 child : slice.children) {
                if (remap[child] != UINT32_MAX) {
                    children.push_back(remap[child]);
                }
            }
            slice.children = std::move(children);
        }
        m_slices = std::move(kept);
        m_maxDepth = 0;
        for (const ProfilerSlice& slice : m_slices) {
            m_maxDepth = std::max(m_maxDepth, slice.depth);
        }
    }

    std::vector<u32> threads;
    for (ProfilerSlice& slice : m_slices) {
        u64 childTotal = 0;
        for (u32 child : slice.children) {
            childTotal += m_slices[child].durationNs;
        }
        slice.selfNs = slice.durationNs > childTotal ? slice.durationNs - childTotal : 0u;
        if (std::find(threads.begin(), threads.end(), slice.threadId) == threads.end()) {
            threads.push_back(slice.threadId);
        }
    }
    m_threadCount = static_cast<u32>(threads.size());

    for (u32 i = 0; i < m_slices.size(); ++i) {
        if (m_slices[i].parent == UINT32_MAX) {
            mergeSlice_(i, UINT32_MAX);
        }
    }
    const auto byName = [&](u32 a, u32 b) { return m_nodes[a].name < m_nodes[b].name; };
    std::sort(m_roots.begin(), m_roots.end(), byName);
    for (FlameNode& node : m_nodes) {
        std::sort(node.children.begin(), node.children.end(), byName);
    }
}

void ProfilerFlameGraph::mergeSlice_(u32 sliceIndex, u32 parentNode) {
    const ProfilerSlice& slice = m_slices[sliceIndex];
    std::vector<u32>& siblings = parentNode == UINT32_MAX ? m_roots : m_nodes[parentNode].children;
    u32 node = UINT32_MAX;
    for (u32 candidate : siblings) {
        if (m_nodes[candidate].name == slice.name) {
            node = candidate;
            break;
        }
    }
    if (node == UINT32_MAX) {
        FlameNode created{};
        created.name = slice.name;
        created.depth = parentNode == UINT32_MAX ? 0u : m_nodes[parentNode].depth + 1u;
        node = static_cast<u32>(m_nodes.size());
        m_nodes.push_back(std::move(created));
        // `siblings` may dangle after push_back when it aliases m_nodes storage.
        (parentNode == UINT32_MAX ? m_roots : m_nodes[parentNode].children).push_back(node);
    }
    m_nodes[node].totalNs += slice.durationNs;
    m_nodes[node].selfNs += slice.selfNs;
    ++m_nodes[node].callCount;
    const std::vector<u32> children = slice.children; // copy: recursion may grow m_nodes only
    for (u32 child : children) {
        mergeSlice_(child, node);
    }
}

const FlameNode* ProfilerFlameGraph::findPath(const std::vector<std::string>& path) const {
    const std::vector<u32>* level = &m_roots;
    const FlameNode* found = nullptr;
    for (const std::string& name : path) {
        found = nullptr;
        for (u32 index : *level) {
            if (m_nodes[index].name == name) {
                found = &m_nodes[index];
                break;
            }
        }
        if (found == nullptr) {
            return nullptr;
        }
        level = &found->children;
    }
    return found;
}

} // namespace fuse::editor
