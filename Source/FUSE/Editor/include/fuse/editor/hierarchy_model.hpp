#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

/// Flattened hierarchy row for headless editor panels (B6.5).
struct HierarchyNode {
    Handle<Object> handle = Handle<Object>::invalid();
    std::string name;
    u32 depth = 0;
};

/// Qt-free scene tree model over fuse::Object parent/child links.
class HierarchyModel {
public:
    void setRoot(Object* root);
    Object* root() const { return m_root; }

    void setSearchQuery(std::string query);
    const std::string& searchQuery() const { return m_searchQuery; }

    void rebuild();

    const std::vector<HierarchyNode>& flatNodes() const { return m_nodes; }
    std::vector<HierarchyNode> visibleNodes() const;
    std::vector<HierarchyNode> childrenOf(Handle<Object> parent) const;

    bool nodePassesFilter(const HierarchyNode& node) const;

private:
    void visit_(Object* node, u32 depth);

    Object* m_root = nullptr;
    std::string m_searchQuery;
    std::vector<HierarchyNode> m_nodes;
};

} // namespace fuse::editor
