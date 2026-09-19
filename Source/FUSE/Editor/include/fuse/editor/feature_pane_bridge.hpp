#pragma once

#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/property_inspector.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::editor {

/// WP-08 feature-pane hook — panes post EditorCommands through EditorHost instead of
/// touching scene state directly from the UI thread.
class FeaturePaneBridge {
public:
    explicit FeaturePaneBridge(EditorHost& host);

    EditorHost& host() { return m_host; }
    const EditorHost& host() const { return m_host; }

    PropertyInspector& propertyInspector() { return m_inspector; }
    const PropertyInspector& propertyInspector() const { return m_inspector; }

    void syncPropertyPane();
    void postPlayRequested();
    void postStopRequested();
    void postSelectEntity(ecs::EntityID entity);
    void postSetProperty(ecs::EntityID entity, const std::string& propertyName,
                         const std::string& propertyValue);
    void postDeleteEntity(ecs::EntityID entity);
    void postReparentEntity(ecs::EntityID entity, ecs::EntityID newParent);
    void postUndoRequested();
    void postRedoRequested();

    /// Game-thread property edit helpers — record undo via host `CommandStack`.
    bool editTransformPosition(const ecs::vec3& position);
    void undoPropertyEdit();
    void redoPropertyEdit();

private:
    EditorHost& m_host;
    PropertyInspector m_inspector;
};

} // namespace fuse::editor
