#pragma once

#include <fuse/fx/effect_descriptor.hpp>
#include <fuse/types.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::fx {

inline constexpr u32 kInvalidEffectGraphNode = static_cast<u32>(-1);

/// Effect group node lifecycle — ore analogue: `afxEffectGroup` child enable states.
enum class EffectNodeState : u8 {
    Pending = 0,
    Active,
    Done,
};

struct EffectGraphNode {
    u32 id = 0;
    std::string effectId;
    u32 parentId = kInvalidEffectGraphNode;
    std::vector<u32> children;
    EffectNodeState state = EffectNodeState::Pending;
    float elapsed = 0.f;
    bool enabled = true;
};

/// Hierarchical effect group driver — ore analogue: `afxEffectGroupData` / `afxEffectVector`.
class EffectGraph {
public:
    static constexpr u32 kInvalidNode = kInvalidEffectGraphNode;

    u32 addNode(const std::string& effectId, u32 parentId = kInvalidEffectGraphNode);
    void activate();
    void tick(float dt, const std::unordered_map<std::string, EffectDescriptor>& registry);

    u32 activeCount() const;
    u32 completedCount() const { return m_completedCount; }
    u32 tickCount() const { return m_tickCount; }
    bool activated() const { return m_activated; }

    const std::vector<EffectGraphNode>& nodes() const { return m_nodes; }
    EffectGraphNode* findNode(u32 id);
    const EffectGraphNode* findNode(u32 id) const;

private:
    void activateNode(u32 nodeId);
    void finishNode(EffectGraphNode& node);

    std::vector<EffectGraphNode> m_nodes;
    u32 m_nextId = 1;
    u32 m_completedCount = 0;
    u32 m_tickCount = 0;
    bool m_activated = false;
};

} // namespace fuse::fx
