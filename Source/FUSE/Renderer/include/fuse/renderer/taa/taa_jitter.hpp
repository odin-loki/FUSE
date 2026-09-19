#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/taa/taa_types.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

static constexpr u32 kTaaDefaultJitterSequenceLength = 8;
static constexpr u32 kTaaMaxJitterSequenceLength = 64;

/// Why jitter sync preflight rejected the request (B5.9 deepen).
enum class TaaJitterSyncRejectReason : u8 {
    None = 0,
    InvalidSequence,
};
/// Human-readable label for jitter sync reject reasons (B5.9 deepen).
const char* taaJitterSyncRejectReasonLabel(TaaJitterSyncRejectReason reason);
/// Classify why jitter sync would be rejected for a frame counter (B5.9 deepen).
TaaJitterSyncRejectReason classifyTaaJitterSyncReject(u32 /*frameIndex*/, u32 sequenceLength);
/// True when jitter can align to a monotonic frame counter (B5.9 deepen).
bool preflightTaaJitterSync(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength,
                              TaaJitterSyncRejectReason* reason = nullptr);
/// Convenience wrapper — true when `preflightTaaJitterSync` would pass (B5.9 deepen).
bool canPreflightTaaJitterSync(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);

/// Why jitter NDC preflight rejected the request (B5.9 deepen).
enum class TaaJitterNdcRejectReason : u8 {
    None = 0,
    InvalidSequence,
    InvalidViewport,
};
/// Human-readable label for jitter NDC reject reasons (B5.9 deepen).
const char* taaJitterNdcRejectReasonLabel(TaaJitterNdcRejectReason reason);
/// Classify why NDC jitter production would be rejected (B5.9 deepen).
TaaJitterNdcRejectReason classifyTaaJitterNdcReject(u32 width, u32 height,
                                                      u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// True when NDC jitter can be produced for viewport and sequence (B5.9 deepen).
bool preflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength,
                             TaaJitterNdcRejectReason* reason = nullptr);
/// Convenience wrapper — true when `preflightTaaJitterNdc` would pass (B5.9 deepen).
bool canPreflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength);

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
    /// True when jitter can align to a monotonic frame counter (B5.9 deepen).
    static bool canSyncToFrameIndex(u32 /*frameIndex*/, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when `slot` is the active Halton index for `frameIndex` (B5.9 deepen).
    static bool jitterSlotMatchesFrameIndex(u32 frameIndex, u32 slot, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
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
    /// Advance only when the sequence is valid; returns false when blocked (B5.9 deepen).
    bool advanceIfReady();
    void reset();
    /// Align jitter state to a monotonic frame counter (wraps with sequence period).
    void syncToFrameIndex(u32 frameIndex);
    /// Sync only when the sequence is valid; returns false when blocked (B5.9 deepen).
    bool syncToFrameIndexIfReady(u32 frameIndex);
    /// True when monotonic frame counter and slot match `frameIndex` (B5.9 deepen).
    bool isAlignedToFrameIndex(u32 frameIndex) const;

    u32 index() const { return m_index; }
    /// True when the jitter sequence can advance (B5.9 deepen).
    bool canAdvance() const;
    /// True when NDC jitter can be produced for the given viewport (B5.9 deepen).
    bool canProduceNdcOffset(u32 width, u32 height) const;
    /// True when jitter can align to a monotonic frame counter (B5.9 deepen).
    bool canSyncToFrameIndex(u32 frameIndex) const;
    /// Classify why sync would be rejected for `frameIndex` (B5.9 deepen).
    TaaJitterSyncRejectReason classifySyncReject(u32 frameIndex) const;
    /// Preflight sync without mutating state (B5.9 deepen).
    bool preflightSyncToFrameIndex(u32 frameIndex, TaaJitterSyncRejectReason* reason = nullptr) const;
    /// Classify why NDC offset production would be rejected (B5.9 deepen).
    TaaJitterNdcRejectReason classifyNdcReject(u32 width, u32 height) const;
    /// Preflight NDC offset production without mutating state (B5.9 deepen).
    bool preflightCurrentNdcOffset(u32 width, u32 height, TaaJitterNdcRejectReason* reason = nullptr) const;
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

} // namespace fuse::renderer
