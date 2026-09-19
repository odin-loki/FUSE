#pragma once

#include <fuse/cinematics/cue_preview.hpp>
#include <fuse/cinematics/timeline_loader.hpp>
#include <fuse/cinematics/types.hpp>
#include <fuse/editor/editor_host.hpp>

#include <string>
#include <vector>

namespace fuse::editor {

struct SeqPreviewPaneSample {
    fuse::cinematics::TimelineMs time_ms = 0;
    bool valid = false;
    bool has_actor_events = false;
    std::string actor_id;
    std::string mount_point;
    float mount_yaw_deg = 0.f;
    float mount_pitch_deg = 0.f;
    float mount_roll_deg = 0.f;
    std::string bone_name;
    float sprite_x = 0.f;
    float sprite_y = 0.f;
    float camera_fov = 0.f;
};

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
    [[nodiscard]] bool scrubPreviewAtMs(fuse::cinematics::TimelineMs timeMs,
                                        fuse::cinematics::SeqScrubPreview& outPreview) const;

    /// Qt seq preview pane stub — posts scrub time through EditorHost for game-thread sampling.
    bool postScrubPreviewAtMs(fuse::cinematics::TimelineMs timeMs);
    [[nodiscard]] SeqPreviewPaneSample previewPaneSampleAtMs(fuse::cinematics::TimelineMs timeMs) const;

    /// Wire Qt seq preview pane — import asset + post initial scrub through EditorHost.
    bool wirePreviewPaneToHost(fuse::cinematics::TimelineMs initialTimeMs = 0);
    [[nodiscard]] const SeqPreviewPaneSample& lastWiredPreviewSample() const { return m_lastWiredPreviewSample; }
    [[nodiscard]] u32 previewPaneWireCount() const { return m_previewPaneWireCount; }
    [[nodiscard]] u32 previewPaneScrubCount() const { return m_previewPaneScrubCount; }
    [[nodiscard]] u32 scrubPreviewPostCount() const { return m_scrubPreviewPostCount; }

private:
    EditorHost& m_host;
    std::string m_lastAssetText;
    u32 m_importCount = 0;
    u32 m_scrubPreviewPostCount = 0;
    u32 m_previewPaneWireCount = 0;
    u32 m_previewPaneScrubCount = 0;
    SeqPreviewPaneSample m_lastWiredPreviewSample{};
};

} // namespace fuse::editor
