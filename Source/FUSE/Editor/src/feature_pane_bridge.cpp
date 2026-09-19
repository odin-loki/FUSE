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

} // namespace fuse::editor
