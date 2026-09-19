#pragma once

#include <fuse/cinematics/cue_preview.hpp>
#include <fuse/cinematics/types.hpp>
#include <fuse/editor/editor_host.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

/// WP-08 / U5 stub — posts `.seq` asset text through EditorHost for game-thread import.
class CinematicsSeqImport {
public:
    explicit CinematicsSeqImport(EditorHost& host);

    EditorHost& host() { return m_host; }
    const EditorHost& host() const { return m_host; }

    [[nodiscard]] const std::string& lastAssetText() const { return m_lastAssetText; }
    [[nodiscard]] u32 importCount() const { return m_importCount; }

    bool postImportAsset(const std::string& seqText);
    bool postImportEmbeddedOutpostIntro();
    [[nodiscard]] std::vector<fuse::cinematics::CuePreviewEntry> previewAtMs(
        fuse::cinematics::TimelineMs timeMs) const;

private:
    EditorHost& m_host;
    std::string m_lastAssetText;
    u32 m_importCount = 0;
};

} // namespace fuse::editor
