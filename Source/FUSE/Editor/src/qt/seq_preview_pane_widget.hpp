#pragma once

#include <fuse/editor/cinematics_seq_import.hpp>
#include <fuse/editor/editor_host.hpp>

#include <QWidget>

namespace fuse::editor::qt {

/// Qt seq preview pane stub (U5 wave 16) — wires CinematicsSeqImport scrub through EditorHost.
class SeqPreviewPaneWidget final : public QWidget {
    Q_OBJECT

public:
    explicit SeqPreviewPaneWidget(EditorHost& host, QWidget* parent = nullptr);

    CinematicsSeqImport& seqImport() { return m_seqImport; }
    const CinematicsSeqImport& seqImport() const { return m_seqImport; }

    void wireEmbeddedOutpostIntro(fuse::cinematics::TimelineMs initialTimeMs = 2'500);
    void scrubToMs(fuse::cinematics::TimelineMs timeMs);

    [[nodiscard]] u32 scrubPostCount() const { return m_scrubPostCount; }

protected:
    void showEvent(QShowEvent* event) override;

private:
    EditorHost& m_host;
    CinematicsSeqImport m_seqImport;
    u32 m_scrubPostCount = 0;
};

} // namespace fuse::editor::qt
