#include <fuse/cinematics/timeline_host_stub.hpp>

namespace fuse::cinematics {

bool TimelineHostStub::loadSeqAssetText(const std::string& seqText) {
    if (seqText.empty()) {
        return false;
    }

    std::string error;
    if (!load_timeline_from_asset(seqText, m_timeline, &error)) {
        return false;
    }

    m_seqAssetText = seqText;
    ++m_overlayWireCount;
    return true;
}

bool TimelineHostStub::wireFromTimeline(const Timeline& timeline, const std::string& seqText, TimelineMs timeMs) {
    if (seqText.empty()) {
        return false;
    }

    m_seqAssetText = seqText;
    ++m_overlayWireCount;
    m_scrubTimeMs = timeMs;
    m_hostPlaybackMs = timeMs;
    m_timeline.scrub_to(timeline.playhead().time_ms());
    if (!load_timeline_from_asset(seqText, m_timeline, nullptr)) {
        return false;
    }
    m_timeline.scrub_to(timeMs);
    ++m_scrubCount;
    m_lastOverlaySample = sampleOverlayAt(timeMs);
    return m_lastOverlaySample.valid;
}

bool TimelineHostStub::scrubToMs(TimelineMs timeMs) {
    if (m_seqAssetText.empty()) {
        return false;
    }

    m_scrubTimeMs = timeMs;
    m_hostPlaybackMs = timeMs;
    m_timeline.scrub_to(timeMs);
    ++m_scrubCount;

    m_lastOverlaySample = sampleOverlayAt(timeMs);
    return m_lastOverlaySample.valid;
}

bool TimelineHostStub::setPlaying(bool playing) {
    m_playing = playing;
    if (playing) {
        m_timeline.play();
    } else {
        m_timeline.pause();
    }
    return true;
}

bool TimelineHostStub::advanceHostPlayback(TimelineMs deltaMs) {
    if (m_seqAssetText.empty() || deltaMs == 0) {
        return false;
    }

    m_hostPlaybackMs += deltaMs;
    m_scrubTimeMs = m_hostPlaybackMs;
    m_timeline.scrub_to(m_hostPlaybackMs);
    ++m_hostAdvanceCount;
    ++m_scrubCount;
    m_lastOverlaySample = sampleOverlayAt(m_hostPlaybackMs);
    return m_lastOverlaySample.valid;
}

bool TimelineHostStub::syncToExternalTimeline(Timeline& timeline) {
    if (m_seqAssetText.empty()) {
        return false;
    }

    timeline.scrub_to(m_hostPlaybackMs);
    if (m_playing) {
        timeline.play();
    } else {
        timeline.pause();
    }
    ++m_hostSyncCount;
    return true;
}

TimelineHostOverlaySample TimelineHostStub::sampleOverlayAt(TimelineMs timeMs) const {
    TimelineHostOverlaySample sample{};
    if (m_seqAssetText.empty()) {
        return sample;
    }

    Timeline timeline;
    SeqScrubPreview preview;
    std::string error;
    if (!scrub_seq_preview(m_seqAssetText, timeMs, timeline, preview, &error)) {
        return sample;
    }

    sample.time_ms = preview.time_ms;
    sample.valid = preview.valid;
    sample.playing = m_playing;
    sample.seq_asset_text = m_seqAssetText;
    sample.scrub_preview = preview;
    return sample;
}

TimelineHostActorMountSample TimelineHostStub::sampleActorMountAt(TimelineMs timeMs) const {
    TimelineHostActorMountSample sample{};
    const TimelineHostOverlaySample overlay = sampleOverlayAt(timeMs);
    if (!overlay.valid || !overlay.scrub_preview.has_actor_events) {
        return sample;
    }

    sample.actor_id = overlay.scrub_preview.actor_id;
    sample.mount_id = overlay.scrub_preview.mount_point;
    sample.mount_yaw_deg = overlay.scrub_preview.mount_yaw_deg;
    sample.valid = !sample.actor_id.empty();
    return sample;
}

} // namespace fuse::cinematics
