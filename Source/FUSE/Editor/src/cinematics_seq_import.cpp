#include <fuse/cinematics/cue_preview.hpp>
#include <fuse/cinematics/timeline_loader.hpp>

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
        "motion outpost_intro 0,0,0,0 15000,10,0,5 30000,20,5,10\n"
        "actor agent_3d mount 2000 cockpit 15\n"
        "actor agent_3d unmount 28000\n";
    return postImportAsset(kAssetText);
}

std::vector<fuse::cinematics::CuePreviewEntry> CinematicsSeqImport::previewAtMs(
    fuse::cinematics::TimelineMs timeMs) const {
    fuse::cinematics::Timeline timeline;
    std::string error;
    if (!fuse::cinematics::load_timeline_from_asset(m_lastAssetText, timeline, &error)) {
        return {};
    }
    return fuse::cinematics::preview_cues_at(timeline, timeMs);
}

bool CinematicsSeqImport::scrubPreviewAtMs(fuse::cinematics::TimelineMs timeMs,
                                           fuse::cinematics::SeqScrubPreview& outPreview) const {
    if (m_lastAssetText.empty()) {
        return false;
    }

    fuse::cinematics::Timeline timeline;
    std::string error;
    return fuse::cinematics::scrub_seq_preview(m_lastAssetText, timeMs, timeline, outPreview, &error);
}

bool CinematicsSeqImport::postScrubPreviewAtMs(fuse::cinematics::TimelineMs timeMs) {
    if (m_lastAssetText.empty()) {
        return false;
    }

    ++m_scrubPreviewPostCount;
    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.propertyName = "cinematics.seq_scrub_preview_ms";
    command.propertyValue = std::to_string(timeMs);
    m_host.postFromUi(std::move(command));
    return true;
}

SeqPreviewPaneSample CinematicsSeqImport::previewPaneSampleAtMs(fuse::cinematics::TimelineMs timeMs) const {
    SeqPreviewPaneSample sample{};
    fuse::cinematics::SeqScrubPreview preview;
    if (!scrubPreviewAtMs(timeMs, preview)) {
        return sample;
    }

    sample.time_ms = preview.time_ms;
    sample.valid = preview.valid;
    sample.has_actor_events = preview.has_actor_events;
    sample.actor_id = preview.actor_id;
    sample.mount_point = preview.mount_point;
    sample.mount_yaw_deg = preview.mount_yaw_deg;
    sample.mount_pitch_deg = preview.mount_pitch_deg;
    sample.mount_roll_deg = preview.mount_roll_deg;
    sample.bone_name = preview.bone_name;
    sample.sprite_x = preview.sprite_x;
    sample.sprite_y = preview.sprite_y;
    sample.camera_fov = preview.camera_fov;
    return sample;
}

bool CinematicsSeqImport::wirePreviewPaneToHost(fuse::cinematics::TimelineMs initialTimeMs) {
    if (!postImportEmbeddedOutpostIntro()) {
        return false;
    }

    EditorCommand wireCommand;
    wireCommand.kind = CommandKind::SetProperty;
    wireCommand.propertyName = "cinematics.seq_preview_pane_wire";
    wireCommand.propertyValue = std::to_string(initialTimeMs);
    m_host.postFromUi(std::move(wireCommand));

    ++m_previewPaneWireCount;
    m_lastWiredPreviewSample = previewPaneSampleAtMs(initialTimeMs);

    if (postScrubPreviewAtMs(28'000)) {
        ++m_previewPaneScrubCount;
    }

    return m_lastWiredPreviewSample.valid;
}

bool CinematicsSeqImport::wireTimelineHostToHost(fuse::cinematics::TimelineMs initialTimeMs) {
    if (!postImportEmbeddedOutpostIntro()) {
        return false;
    }

    EditorCommand wireCommand;
    wireCommand.kind = CommandKind::SetProperty;
    wireCommand.propertyName = "cinematics.seq_timeline_host_wire";
    wireCommand.propertyValue = std::to_string(initialTimeMs);
    m_host.postFromUi(std::move(wireCommand));

    ++m_timelineHostWireCount;
    fuse::cinematics::TimelineHostStub hostStub;
    hostStub.loadSeqAssetText(m_lastAssetText);
    hostStub.scrubToMs(initialTimeMs);
    m_lastTimelineHostSample = hostStub.lastOverlaySample();
    return m_lastTimelineHostSample.valid;
}

bool CinematicsSeqImport::postViewportSeqPreviewAtMs(fuse::cinematics::TimelineMs timeMs) {
    if (m_lastAssetText.empty()) {
        return false;
    }

    ++m_viewportSeqPreviewPostCount;
    EditorCommand command;
    command.kind = CommandKind::SetProperty;
    command.propertyName = "cinematics.viewport_seq_preview_ms";
    command.propertyValue = std::to_string(timeMs);
    m_host.postFromUi(std::move(command));
    return true;
}

} // namespace fuse::editor
