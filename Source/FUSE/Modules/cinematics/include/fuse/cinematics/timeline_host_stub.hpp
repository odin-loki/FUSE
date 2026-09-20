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

struct TimelineHostActorMountSample {
    std::string actor_id;
    std::string mount_id;
    f32 mount_yaw_deg = 0.f;
    bool valid = false;
};

/// Headless timeline host stub — bridges Qt seq overlay scrub posts to timeline sampling.
class TimelineHostStub {
public:
    bool loadSeqAssetText(const std::string& seqText);
    bool wireFromTimeline(const Timeline& timeline, const std::string& seqText, TimelineMs timeMs);
    bool scrubToMs(TimelineMs timeMs);
    bool setPlaying(bool playing);
    bool advanceHostPlayback(TimelineMs deltaMs);
    bool syncToExternalTimeline(Timeline& timeline);

    [[nodiscard]] const Timeline& timeline() const { return m_timeline; }
    [[nodiscard]] TimelineMs scrubTimeMs() const { return m_scrubTimeMs; }
    [[nodiscard]] TimelineMs hostPlaybackMs() const { return m_hostPlaybackMs; }
    [[nodiscard]] bool playing() const { return m_playing; }
    [[nodiscard]] u32 scrubCount() const { return m_scrubCount; }
    [[nodiscard]] u32 overlayWireCount() const { return m_overlayWireCount; }
    [[nodiscard]] u32 hostAdvanceCount() const { return m_hostAdvanceCount; }
    [[nodiscard]] u32 hostSyncCount() const { return m_hostSyncCount; }
    [[nodiscard]] const TimelineHostOverlaySample& lastOverlaySample() const { return m_lastOverlaySample; }

    [[nodiscard]] TimelineHostOverlaySample sampleOverlayAt(TimelineMs timeMs) const;
    [[nodiscard]] TimelineHostActorMountSample sampleActorMountAt(TimelineMs timeMs) const;

private:
    Timeline m_timeline;
    TimelineMs m_scrubTimeMs = 0;
    TimelineMs m_hostPlaybackMs = 0;
    std::string m_seqAssetText;
    bool m_playing = false;
    u32 m_scrubCount = 0;
    u32 m_overlayWireCount = 0;
    u32 m_hostAdvanceCount = 0;
    u32 m_hostSyncCount = 0;
    TimelineHostOverlaySample m_lastOverlaySample{};
};

} // namespace fuse::cinematics
