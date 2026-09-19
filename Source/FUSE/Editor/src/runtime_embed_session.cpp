#include <fuse/editor/runtime_embed_session.hpp>

namespace fuse::editor {

void RuntimeEmbedSession::reset() {
    projectRoot.clear();
    loadedWorldPath.clear();
    wsiBackendName.clear();
    worldEntityCount = 0;
    mirroredEditorEntityCount = 0;
    headlessPresentTicks = 0;
    surfaceHandoffCount = 0;
    submittedFrames = 0;
    worldLoaded = false;
    headlessGpuReady = false;
    usesHeadlessGpuPath = false;
    surfaceHandoffPending = false;
    surfaceHandoffConsumed = false;
}

} // namespace fuse::editor
