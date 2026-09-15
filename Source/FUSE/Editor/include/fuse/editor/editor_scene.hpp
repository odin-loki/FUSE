#pragma once

#include <fuse/ecs/registry.hpp>

namespace fuse::editor {

/// Thin ECS wrapper for editor panels — owns a registry instance (B6.6–B6.8 stub).
class EditorScene {
public:
    void init(usize maxEntities = ecs::kMaxEntities) { m_registry.init(maxEntities); }
    void destroy() { m_registry.destroy(); }

    ecs::Registry& registry() { return m_registry; }
    const ecs::Registry& registry() const { return m_registry; }

private:
    ecs::Registry m_registry;
};

} // namespace fuse::editor
