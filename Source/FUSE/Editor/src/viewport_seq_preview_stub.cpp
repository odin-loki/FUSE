#include <fuse/editor/viewport_seq_preview_stub.hpp>

#include <fuse/cinematics/timeline_loader.hpp>

namespace fuse::editor {

ViewportSeqPreviewSample sample_viewport_seq_preview(const std::string& seqAssetText,
                                                     fuse::cinematics::TimelineMs timeMs) {
    ViewportSeqPreviewSample sample{};
    if (seqAssetText.empty()) {
        return sample;
    }

    fuse::cinematics::Timeline timeline;
    fuse::cinematics::SeqScrubPreview preview;
    std::string error;
    if (!fuse::cinematics::scrub_seq_preview(seqAssetText, timeMs, timeline, preview, &error)) {
        return sample;
    }

    sample.time_ms = preview.time_ms;
    sample.valid = preview.valid;
    sample.mount_point = preview.mount_point;
    sample.bone_name = preview.bone_name;
    sample.mount_yaw_deg = preview.mount_yaw_deg;
    sample.mount_pitch_deg = preview.mount_pitch_deg;
    sample.mount_roll_deg = preview.mount_roll_deg;
    sample.sprite_x = preview.sprite_x;
    sample.sprite_y = preview.sprite_y;
    return sample;
}

} // namespace fuse::editor
