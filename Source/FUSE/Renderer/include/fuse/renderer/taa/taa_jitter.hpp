#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/taa/taa_types.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

static constexpr u32 kTaaDefaultJitterSequenceLength = 8;
static constexpr u32 kTaaMaxJitterSequenceLength = 64;

/// Halton (2,3) sequence helpers — CPU reference for projection jitter (B5.9 deepen).
struct TaaJitterLayout {
    static f32 halton(u32 index, u32 base);
    static bool validateSequenceLength(u32 length);
    /// True when viewport dimensions are non-zero for NDC jitter (B5.9 deepen).
    static bool validateViewportDimensions(u32 width, u32 height);
    /// True when jitter slot index is within the active sequence (B5.9 deepen).
    static bool jitterIndexInRange(u32 index, u32 sequenceLength);
    /// True when NDC jitter can be produced for viewport and sequence (B5.9 deepen).
    static bool canProduceNdcOffset(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Returns the jitter cycle length after validation (0 when invalid).
    static u32 sequencePeriod(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Maps a monotonic frame counter into the active Halton slot.
    static u32 frameIndexInSequence(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    static fuse::math::Vec2 haltonPixelOffset(u32 index, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    static fuse::math::Vec2 haltonNdcOffset(u32 index, u32 width, u32 height,
                                            u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Halton offset for a monotonic frame counter (wraps via `frameIndexInSequence`).
    static fuse::math::Vec2 offsetForFrameIndex(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// NDC jitter for a monotonic frame counter (wraps via `frameIndexInSequence`).
    static fuse::math::Vec2 ndcOffsetForFrameIndex(u32 frameIndex, u32 width, u32 height,
                                                   u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Fills a Halton (2,3) table; returns false when `out` is null or length is invalid.
    static bool fillHaltonSequence(u32 length, fuse::math::Vec2* out);
};

/// Sub-pixel jitter state — advances through a Halton sequence each frame (B5.9).
class TaaJitter {
public:
    explicit TaaJitter(const TaaJitterDesc& desc = {});

    fuse::math::Vec2 currentPixelOffset() const;
    fuse::math::Vec2 currentNdcOffset(u32 width, u32 height) const;

    void advance();
    void reset();
    /// Align jitter state to a monotonic frame counter (wraps with sequence period).
    void syncToFrameIndex(u32 frameIndex);
    /// True when jitter slot and monotonic counter match a frame index (B5.9 deepen).
    bool isSyncedToFrameIndex(u32 frameIndex) const;

    u32 index() const { return m_index; }
    /// True when the jitter sequence can advance (B5.9 deepen).
    bool canAdvance() const;
    /// True when NDC jitter can be produced for the given viewport (B5.9 deepen).
    bool canProduceNdcOffset(u32 width, u32 height) const;
    /// Monotonic frame counter — incremented by `advance`, set by `syncToFrameIndex`, cleared by `reset`.
    u32 monotonicFrameIndex() const { return m_monotonicFrame; }
    u32 sequenceLength() const { return m_sequenceLength; }

    static fuse::math::Vec2 haltonPixelOffset(u32 index);
    static fuse::math::Vec2 haltonNdcOffset(u32 index, u32 width, u32 height);

private:
    u32 m_sequenceLength = kTaaDefaultJitterSequenceLength;
    u32 m_index = 0;
    u32 m_monotonicFrame = 0;
};

/// Jitter sync preflight snapshot (B5.9 deepen).
struct TaaJitterSyncPreflight {
    u32 frame_index = 0;
    u32 expected_slot = 0;
    u32 actual_slot = 0;
    bool slot_matches = false;
    bool monotonic_matches = false;
    bool sequence_valid = false;
    bool viewport_valid = false;
    bool can_produce_ndc = false;

    bool synced() const;
};

TaaJitterSyncPreflight preflightTaaJitterSync(const TaaJitter& jitter, u32 frameIndex, u32 width, u32 height);

} // namespace fuse::renderer
