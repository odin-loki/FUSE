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

BehaviorNode makeParallel(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::Parallel;
    node.childA = firstChild(spec);
    node.childB = secondChild(spec);
    node.parallelPolicy.successThreshold = spec.successThreshold;
    node.parallelPolicy.failThreshold = spec.failThreshold;
    node.parallelPolicy.abortOnFail = spec.abortOnFail;
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

BehaviorNode makeActionBlackboardSet(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::ActionBlackboardSet;
    node.flagIndex = spec.flagIndex;
    node.threshold = spec.threshold;
    node.scriptHook = spec.scriptHook;
    return node;
}

BehaviorNode makeConditionBlackboardGet(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::ConditionBlackboardGet;
    node.flagIndex = spec.flagIndex;
    return node;
}

BehaviorNode makeConditionDistanceGreater(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::ConditionDistanceGreater;
    node.threshold = spec.threshold;
    return node;
}

BehaviorNode makeActionWait(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::ActionWait;
    node.threshold = spec.threshold;
    node.loopCount = spec.loopCount;
    node.scriptHook = spec.scriptHook;
    return node;
}

BehaviorNode makeActionDistance(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::ActionDistance;
    node.threshold = spec.threshold;
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

BehaviorNode makeConditionAlliesInRadius(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::ConditionAlliesInRadius;
    node.threshold = spec.threshold;
    node.loopCount = spec.loopCount;
    return node;
}

BehaviorNode makeActionNearestAlly(const NodeLoadSpec& spec) {
    BehaviorNode node;
    node.kind = NodeKind::ActionNearestAlly;
    node.threshold = spec.threshold;
    node.flagIndex = spec.flagIndex;
    node.scalarSlot = spec.scalarSlot;
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
    registerFactory("bb.parallel", makeParallel);

    // BadBehaviour decorator/ — third_party/addons/BadBehaviour/Engine/source/BadBehavior/decorator/
    registerFactory("bb.inverter", makeInverter);
    registerFactory("bb.loop", makeLoop);
    registerFactory("bb.succeed_always", makeSucceedAlways);
    registerFactory("bb.root", makeRoot);

    // BadBehaviour leaf/ + FUSE demo conditions/actions
    registerFactory("bb.condition.distance_less", makeConditionDistanceLess);
    registerFactory("bb.condition.distance_greater", makeConditionDistanceGreater);
    registerFactory("bb.condition.blackboard_get", makeConditionBlackboardGet);
    registerFactory("bb.action.set_flag", makeActionSetFlag);
    registerFactory("bb.action.blackboard_set", makeActionBlackboardSet);
    registerFactory("bb.action.wait", makeActionWait);
    registerFactory("bb.action.distance", makeActionDistance);

    // GuideBot navigation leaf stub — third_party/addons/GuideBot/.../guideBot/actionMove.h
    registerFactory("gb.action.move_toward", makeActionMoveToward);

    // FUSE spatial query leaves — blackboard ally lookup stubs (BadBehaviour ScriptedBehavior pattern)
    registerFactory("bb.condition.allies_in_radius", makeConditionAlliesInRadius);
    registerFactory("bb.action.nearest_ally", makeActionNearestAlly);
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
