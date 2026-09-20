#pragma once

// Ore: Verve VController timeline host stub (Qt seq overlay ↔ game-thread host without Torque)

#include <fuse/cinematics/timeline.hpp>
#include <fuse/cinematics/timeline_loader.hpp>
#include <fuse/cinematics/types.hpp>

#include <string>

namespace fuse::cinematics {

struct TimelineHostOverlaySample {
    TimelineMs time_ms = 0;
    bool valid = false;
    bool playing = false;
    std::string seq_asset_text;
    SeqScrubPreview scrub_preview{};
};

/// Headless timeline host stub — bridges Qt seq overlay scrub posts to timeline sampling.
class TimelineHostStub {
public:
    bool loadSeqAssetText(const std::string& seqText);
    bool wireFromTimeline(const Timeline& timeline, const std::string& seqText, TimelineMs timeMs);
    bool scrubToMs(TimelineMs timeMs);
    bool setPlaying(bool playing);

    [[nodiscard]] const Timeline& timeline() const { return m_timeline; }
    [[nodiscard]] TimelineMs scrubTimeMs() const { return m_scrubTimeMs; }
    [[nodiscard]] bool playing() const { return m_playing; }
    [[nodiscard]] u32 scrubCount() const { return m_scrubCount; }
    [[nodiscard]] u32 overlayWireCount() const { return m_overlayWireCount; }
    [[nodiscard]] const TimelineHostOverlaySample& lastOverlaySample() const { return m_lastOverlaySample; }

    [[nodiscard]] TimelineHostOverlaySample sampleOverlayAt(TimelineMs timeMs) const;

private:
    Timeline m_timeline;
    TimelineMs m_scrubTimeMs = 0;
    std::string m_seqAssetText;
    bool m_playing = false;
    u32 m_scrubCount = 0;
    u32 m_overlayWireCount = 0;
    TimelineHostOverlaySample m_lastOverlaySample{};
};

} // namespace fuse::cinematics
