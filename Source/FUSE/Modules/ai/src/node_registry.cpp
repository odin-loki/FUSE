#include <fuse/ai/node_registry.hpp>

namespace fuse::ai {

namespace {

u32 firstChild(const NodeLoadSpec& spec) {
    return spec.childIndices.empty() ? 0 : spec.childIndices[0];
}

u32 secondChild(const NodeLoadSpec& spec) {
    return spec.childIndices.size() >= 2u ? spec.childIndices[1] : 0;
}

BehaviorNode makeSequence(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::Sequence;
    node.childA = firstChild(spec);
    node.childB = secondChild(spec);
    return node;
}

BehaviorNode makeSelector(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::Selector;
    node.childA = firstChild(spec);
    node.childB = secondChild(spec);
    return node;
}

BehaviorNode makeInverter(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::Inverter;
    node.childA = firstChild(spec);
    return node;
}

BehaviorNode makeLoop(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::Loop;
    node.loopCount = spec.loopCount;
    node.childA = firstChild(spec);
    return node;
}

BehaviorNode makeSucceedAlways(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::SucceedAlways;
    node.childA = firstChild(spec);
    return node;
}

BehaviorNode makeRoot(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::Root;
    node.childA = firstChild(spec);
    return node;
}

BehaviorNode makeConditionDistanceLess(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::ConditionDistanceLess;
    node.threshold = spec.threshold;
    return node;
}

BehaviorNode makeActionSetFlag(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::ActionSetFlag;
    node.flagIndex = spec.flagIndex;
    node.scriptHook = spec.scriptHook;
    return node;
}

BehaviorNode makeActionMoveToward(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::ActionMoveToward;
    node.threshold = spec.threshold;
    node.flagIndex = spec.flagIndex;
    node.scriptHook = spec.scriptHook;
    return node;
}

} // namespace

NodeRegistry& NodeRegistry::instance() {
    static NodeRegistry registry;
    static bool initialized = false;
    if (!initialized) {
        registry.registerBuiltins();
        initialized = true;
    }
    return registry;
}

void NodeRegistry::registerFactory(std::string typeId, NodeFactory factory) {
    m_factories[std::move(typeId)] = std::move(factory);
}

bool NodeRegistry::hasFactory(const std::string& typeId) const {
    return m_factories.find(typeId) != m_factories.end();
}

bool NodeRegistry::buildNode(const NodeLoadSpec& spec, BehaviorNode& outNode) const {
    const auto it = m_factories.find(spec.typeId);
    if (it == m_factories.end()) {
        return false;
    }
    outNode = it->second(spec);
    return true;
}

void NodeRegistry::registerBuiltins() {
    // BadBehaviour composite/ — third_party/addons/BadBehaviour/Engine/source/BadBehavior/composite/
    registerFactory("bb.sequence", makeSequence);
    registerFactory("bb.selector", makeSelector);

    // BadBehaviour decorator/ — third_party/addons/BadBehaviour/Engine/source/BadBehavior/decorator/
    registerFactory("bb.inverter", makeInverter);
    registerFactory("bb.loop", makeLoop);
    registerFactory("bb.succeed_always", makeSucceedAlways);
    registerFactory("bb.root", makeRoot);

    // BadBehaviour leaf/ + FUSE demo conditions
    registerFactory("bb.condition.distance_less", makeConditionDistanceLess);
    registerFactory("bb.action.set_flag", makeActionSetFlag);

    // GuideBot navigation leaf stub — third_party/addons/GuideBot/.../guideBot/actionMove.h
    registerFactory("gb.action.move_toward", makeActionMoveToward);
}

std::vector<std::string> NodeRegistry::registeredTypeIds() const {
    std::vector<std::string> ids;
    ids.reserve(m_factories.size());
    for (const auto& entry : m_factories) {
        ids.push_back(entry.first);
    }
    return ids;
}

bool loadTreeFromSpecs(const std::vector<NodeLoadSpec>& specs,
                       u32 rootIndex,
                       BehaviorTree& outTree) {
    NodeRegistry& registry = NodeRegistry::instance();
    for (const NodeLoadSpec& spec : specs) {
        BehaviorNode node;
        if (!registry.buildNode(spec, node)) {
            return false;
        }
        outTree.addNode(node);
    }
    if (rootIndex >= specs.size()) {
        return false;
    }
    outTree.setRoot(rootIndex);
    return true;
}

} // namespace fuse::ai
