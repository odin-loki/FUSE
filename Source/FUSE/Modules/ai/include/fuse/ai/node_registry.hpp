#pragma once

#include <fuse/ai/behavior_tree.hpp>
#include <fuse/types.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::ai {

/// Serialized node spec for load path — ore analogue: BadBehaviour CONOBJECT registration.
/// Ore: third_party/addons/BadBehaviour/Engine/source/BadBehavior/core/Core.h (Node/LeafNode)
struct NodeLoadSpec {
    std::string typeId;
    float threshold = 0.f;
    u32 flagIndex = 0;
    u32 loopCount = 1;
    std::string scriptHook;
    std::vector<u32> childIndices;
    u32 scalarSlot = BehaviorNode::kNoScalarSlot;
    u32 successThreshold = 0;
    u32 failThreshold = 1;
    bool abortOnFail = false;
    bool requireBoundBlackboard = false;
    bool requireAllyContext = false;
};

/// Factory signature — builds a flat BehaviorNode from a load spec.
using NodeFactory = std::function<BehaviorNode(const NodeLoadSpec& spec)>;

/// Type-id registry mirroring BadBehaviour node registration without SimObject/Con::.
class NodeRegistry {
public:
    static NodeRegistry& instance();

    void registerFactory(std::string typeId, NodeFactory factory);
    bool hasFactory(const std::string& typeId) const;

    /// Returns false when typeId is unknown.
    bool buildNode(const NodeLoadSpec& spec, BehaviorNode& outNode) const;

    /// Register built-in ore nodes (Sequence, Selector, Inverter, Loop, …).
    void registerBuiltins();

    std::vector<std::string> registeredTypeIds() const;

private:
    NodeRegistry() = default;

    std::unordered_map<std::string, NodeFactory> m_factories;
};

/// Build a BehaviorTree from ordered node specs; rootIndex selects the root node.
bool loadTreeFromSpecs(const std::vector<NodeLoadSpec>& specs,
                       u32 rootIndex,
                       BehaviorTree& outTree);

} // namespace fuse::ai
