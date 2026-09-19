#include <fuse/editor/cinematics_seq_import.hpp>

namespace fuse::editor {

CinematicsSeqImport::CinematicsSeqImport(EditorHost& host) : m_host(host) {}

bool CinematicsSeqImport::postImportAsset(const std::string& seqText) {
    if (seqText.empty()) {
        return false;
    }

    m_lastAssetText = seqText;
    ++m_importCount;

    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.propertyName = "cinematics.seq_asset";
    command.propertyValue = seqText;
    m_host.postFromUi(std::move(command));
    return true;
}

bool CinematicsSeqImport::postImportEmbeddedOutpostIntro() {
    static const char* kAssetText =
        "# Outpost intro 30s sequence\n"
        "duration_ms=30000\n"
        "sprite hud_sprite 0,-20,0,1 15000,0,10,1 30000,40,20,1\n"
        "camera 0,0,0,8,55 15000,0,30,12,70 30000,0,60,15,85\n"
        "actor agent_3d mount 2000 cockpit\n"
        "actor agent_3d unmount 28000\n";
    return postImportAsset(kAssetText);
}

} // namespace fuse::editor
