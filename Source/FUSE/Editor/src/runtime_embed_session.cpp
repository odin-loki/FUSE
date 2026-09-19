#include <fuse/editor/runtime_embed_session.hpp>

namespace fuse::editor {

void RuntimeEmbedSession::reset() {
    projectRoot.clear();
    loadedWorldPath.clear();
    worldEntityCount = 0;
    mirroredEditorEntityCount = 0;
    headlessPresentTicks = 0;
    worldLoaded = false;
    headlessGpuReady = false;
}

} // namespace fuse::editor
