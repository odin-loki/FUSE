#include <fuse/fx/effect_graph.hpp>

namespace fuse::fx {

u32 EffectGraph::addNode(const std::string& effectId, u32 parentId) {
    EffectGraphNode node;
    node.id = m_nextId++;
    node.effectId = effectId;
    node.parentId = parentId;

    if (parentId != kInvalidNode) {
        if (EffectGraphNode* parent = findNode(parentId)) {
            parent->children.push_back(node.id);
        }
    }

    m_nodes.push_back(node);
    return node.id;
}

void EffectGraph::activate() {
    m_activated = true;
    for (EffectGraphNode& node : m_nodes) {
        if (node.parentId == kInvalidNode) {
            activateNode(node.id);
        }
    }
}

void EffectGraph::tick(float dt, const std::unordered_map<std::string, EffectDescriptor>& registry) {
    if (!m_activated || m_nodes.empty()) {
        return;
    }

    ++m_tickCount;

    std::vector<u32> finishedThisTick;
    for (EffectGraphNode& node : m_nodes) {
        if (node.state != EffectNodeState::Active) {
            continue;
        }

        if (!node.enabled) {
            finishedThisTick.push_back(node.id);
            continue;
        }

        const auto it = registry.find(node.effectId);
        if (it == registry.end()) {
            finishedThisTick.push_back(node.id);
            continue;
        }

        node.elapsed += dt;
        const float duration = it->second.duration;
        if (duration <= 0.f || node.elapsed >= duration) {
            finishedThisTick.push_back(node.id);
        }
    }

    for (u32 nodeId : finishedThisTick) {
        if (EffectGraphNode* node = findNode(nodeId)) {
            finishNode(*node);
        }
    }
}

u32 EffectGraph::activeCount() const {
    u32 count = 0;
    for (const EffectGraphNode& node : m_nodes) {
        if (node.state == EffectNodeState::Active) {
            ++count;
        }
    }
    return count;
}

EffectGraphNode* EffectGraph::findNode(u32 id) {
    for (EffectGraphNode& node : m_nodes) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

const EffectGraphNode* EffectGraph::findNode(u32 id) const {
    for (const EffectGraphNode& node : m_nodes) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

void EffectGraph::activateNode(u32 nodeId) {
    if (EffectGraphNode* node = findNode(nodeId)) {
        node->state = EffectNodeState::Active;
        node->elapsed = 0.f;
    }
}

void EffectGraph::finishNode(EffectGraphNode& node) {
    if (node.state == EffectNodeState::Done) {
        return;
    }

    node.state = EffectNodeState::Done;
    ++m_completedCount;

    for (u32 childId : node.children) {
        if (EffectGraphNode* child = findNode(childId)) {
            if (child->state == EffectNodeState::Pending) {
                activateNode(childId);
            }
        }
    }
}

} // namespace fuse::fx
