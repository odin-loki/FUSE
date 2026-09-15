#include <fuse/editor/hierarchy_model.hpp>

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

void HierarchyModel::setSearchQuery(std::string query) {
    m_searchQuery = std::move(query);
}

void HierarchyModel::rebuild() {
    m_nodes.clear();
    if (!m_root) {
        return;
    }
    visit_(m_root, 0);
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
