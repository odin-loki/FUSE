#include <fuse/editor/hierarchy_model.hpp>

#include <fuse/scene/scene.hpp>

#include <cctype>

namespace fuse::editor {

namespace {

std::string toLower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool containsInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

} // namespace

void HierarchyModel::setRoot(Object* root) {
    m_root = root;
    rebuild();
}

void HierarchyModel::setScene(scene::Scene* scene) {
    m_scene = scene;
    rebuild();
}

void HierarchyModel::setSearchQuery(std::string query) {
    m_searchQuery = std::move(query);
}

void HierarchyModel::rebuild() {
    m_nodes.clear();
    if (m_root) {
        visit_(m_root, 0);
        return;
    }
    rebuildFromScene_();
}

std::vector<HierarchyNode> HierarchyModel::visibleNodes() const {
    if (m_searchQuery.empty()) {
        return m_nodes;
    }

    std::vector<HierarchyNode> filtered;
    for (const HierarchyNode& node : m_nodes) {
        if (nodePassesFilter(node)) {
            filtered.push_back(node);
        }
    }
    return filtered;
}

std::vector<HierarchyNode> HierarchyModel::childrenOf(Handle<Object> parent) const {
    std::vector<HierarchyNode> children;
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].handle != parent) {
            continue;
        }

        const u32 parentDepth = m_nodes[i].depth;
        for (size_t j = i + 1; j < m_nodes.size(); ++j) {
            if (m_nodes[j].depth <= parentDepth) {
                break;
            }
            if (m_nodes[j].depth == parentDepth + 1) {
                children.push_back(m_nodes[j]);
            }
        }
        break;
    }
    return children;
}

bool HierarchyModel::nodePassesFilter(const HierarchyNode& node) const {
    return containsInsensitive(node.name, m_searchQuery);
}

bool HierarchyModel::findNamed(const std::string& name, HierarchyNode& out) const {
    for (const HierarchyNode& node : m_nodes) {
        if (node.name == name) {
            out = node;
            return true;
        }
    }
    return false;
}

void HierarchyModel::rebuildFromScene_() {
    if (!m_scene) {
        return;
    }

    const u32 count = m_scene->entityCount();
    std::vector<u8> visiting(count, 0);
    std::vector<u8> visited(count, 0);

    auto isRoot = [this, count](u32 index) {
        const scene::SceneEntity* entity = m_scene->entityAt(index);
        if (entity == nullptr) {
            return true;
        }
        if (entity->parentIndex < 0) {
            return true;
        }
        const u32 parent = static_cast<u32>(entity->parentIndex);
        return parent >= count || parent == index;
    };

    for (u32 i = 0; i < count; ++i) {
        if (isRoot(i)) {
            visitSceneEntity_(i, 0, visiting, visited);
        }
    }

    for (u32 i = 0; i < count; ++i) {
        if (visited[i] == 0) {
            visitSceneEntity_(i, 0, visiting, visited);
        }
    }
}

void HierarchyModel::visitSceneEntity_(u32 index, u32 depth, std::vector<u8>& visiting,
                                      std::vector<u8>& visited) {
    if (!m_scene) {
        return;
    }

    const u32 count = m_scene->entityCount();
    if (index >= count || visiting[index] != 0) {
        return;
    }

    const scene::SceneEntity* entity = m_scene->entityAt(index);
    if (entity == nullptr) {
        return;
    }

    visiting[index] = 1;
    visited[index] = 1;
    m_nodes.push_back(HierarchyNode{Handle<Object>(index, 1u), entity->name, depth});

    for (u32 child = 0; child < count; ++child) {
        const scene::SceneEntity* childEntity = m_scene->entityAt(child);
        if (childEntity != nullptr && childEntity->parentIndex == static_cast<s32>(index)) {
            visitSceneEntity_(child, depth + 1, visiting, visited);
        }
    }

    visiting[index] = 0;
}

void HierarchyModel::visit_(Object* node, u32 depth) {
    if (!node) {
        return;
    }

    m_nodes.push_back(HierarchyNode{node->handle(), node->name(), depth});
    for (Object* child : node->children()) {
        visit_(child, depth + 1);
    }
}

} // namespace fuse::editor
