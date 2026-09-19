#include <fuse/editor/feature_pane_bridge.hpp>

#include <fuse/handle.hpp>
#include <fuse/object.hpp>

namespace fuse::editor {

FeaturePaneBridge::FeaturePaneBridge(EditorHost& host) : m_host(host) {}

void FeaturePaneBridge::syncPropertyPane() {
    m_inspector.sync(m_host.editorState(), m_host.editorScene());
}

void FeaturePaneBridge::postPlayRequested() {
    EditorCommand cmd;
    cmd.kind = CommandKind::StartPlay;
    m_host.postFromUi(std::move(cmd));
}

void FeaturePaneBridge::postStopRequested() {
    EditorCommand cmd;
    cmd.kind = CommandKind::StopPlay;
    m_host.postFromUi(std::move(cmd));
}

void FeaturePaneBridge::postSelectEntity(ecs::EntityID entity) {
    EditorCommand cmd;
    cmd.kind = CommandKind::SelectEntity;
    cmd.target = Handle<Object>(entity.index, entity.generation);
    m_host.postFromUi(std::move(cmd));
}

void FeaturePaneBridge::postSetProperty(ecs::EntityID entity, const std::string& propertyName,
                                        const std::string& propertyValue) {
    EditorCommand cmd;
    cmd.kind = CommandKind::SetProperty;
    cmd.target = Handle<Object>(entity.index, entity.generation);
    cmd.propertyName = propertyName;
    cmd.propertyValue = propertyValue;
    m_host.postFromUi(std::move(cmd));
}

void FeaturePaneBridge::postDeleteEntity(ecs::EntityID entity) {
    EditorCommand cmd;
    cmd.kind = CommandKind::DeleteObject;
    cmd.target = Handle<Object>(entity.index, entity.generation);
    m_host.postFromUi(std::move(cmd));
}

void FeaturePaneBridge::postReparentEntity(ecs::EntityID entity, ecs::EntityID newParent) {
    EditorCommand cmd;
    cmd.kind = CommandKind::ReparentObject;
    cmd.target = Handle<Object>(entity.index, entity.generation);
    cmd.parent = Handle<Object>(newParent.index, newParent.generation);
    m_host.postFromUi(std::move(cmd));
}

void FeaturePaneBridge::postUndoRequested() {
    EditorCommand cmd;
    cmd.kind = CommandKind::Undo;
    m_host.postFromUi(std::move(cmd));
}

void FeaturePaneBridge::postRedoRequested() {
    EditorCommand cmd;
    cmd.kind = CommandKind::Redo;
    m_host.postFromUi(std::move(cmd));
}

bool FeaturePaneBridge::editTransformPosition(const ecs::vec3& position) {
    syncPropertyPane();
    return m_inspector.setTransformPosition(position, m_host.editorScene(), m_host.commandStack());
}

void FeaturePaneBridge::undoPropertyEdit() {
    m_host.undoPropertyEdit();
}

void FeaturePaneBridge::redoPropertyEdit() {
    m_host.redoPropertyEdit();
}

} // namespace fuse::editor
