#pragma once

#include <fuse/handle.hpp>
#include <fuse/object.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace fuse::scene {
class Scene;
}

namespace fuse::editor {

/// Flattened hierarchy row for headless editor panels (B6.5).
struct HierarchyNode {
    Handle<Object> handle = Handle<Object>::invalid();
    std::string name;
    u32 depth = 0;
};

/// Qt-free scene tree model over fuse::Object parent/child links, or runtime
/// `scene::Scene` entities when no Object root is set.
class HierarchyModel {
public:
    void setRoot(Object* root);
    Object* root() const { return m_root; }

    void setScene(scene::Scene* scene);
    scene::Scene* scene() const { return m_scene; }

    void setSearchQuery(std::string query);
    const std::string& searchQuery() const { return m_searchQuery; }

    void rebuild();

    const std::vector<HierarchyNode>& flatNodes() const { return m_nodes; }
    std::vector<HierarchyNode> visibleNodes() const;
    std::vector<HierarchyNode> childrenOf(Handle<Object> parent) const;

    bool nodePassesFilter(const HierarchyNode& node) const;
    bool findNamed(const std::string& name, HierarchyNode& out) const;

private:
    void visit_(Object* node, u32 depth);
    void rebuildFromScene_();
    void visitSceneEntity_(u32 index, u32 depth, std::vector<u8>& visiting, std::vector<u8>& visited);

    Object* m_root = nullptr;
    scene::Scene* m_scene = nullptr;
    std::string m_searchQuery;
    std::vector<HierarchyNode> m_nodes;
};

} // namespace fuse::editor
