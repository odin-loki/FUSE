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

} // namespace fuse::cinematics
