#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/taa/taa_types.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

static constexpr u32 kTaaDefaultJitterSequenceLength = 8;
static constexpr u32 kTaaMaxJitterSequenceLength = 64;

/// Why jitter sync or NDC production preflight rejected the request (B5.9 deepen).
enum class TaaJitterGuardRejectReason : u8 {
    None = 0,
    InvalidSequence,
    InvalidViewport,
};

/// Human-readable label for jitter guard reject reasons (B5.9 deepen).
const char* taaJitterGuardRejectReasonLabel(TaaJitterGuardRejectReason reason);
/// Classify why jitter sync to a frame counter would be rejected (B5.9 deepen).
TaaJitterGuardRejectReason classifyTaaJitterSyncReject(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// Classify why NDC jitter production would be rejected (B5.9 deepen).
TaaJitterGuardRejectReason classifyTaaJitterNdcReject(u32 width, u32 height,
                                                      u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// True when jitter can sync to `frameIndex` for the given sequence (B5.9 deepen).
bool preflightTaaJitterSync(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength,
                             TaaJitterGuardRejectReason* reason = nullptr);
/// Jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaJitterSync(u32 frameIndex, u32 sequenceLength, TaaJitterGuardRejectReason& reason);
/// Early-out when jitter sync preflight would reject (B5.9 deepen).
bool shouldSkipTaaJitterSync(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// True when NDC jitter can be produced for viewport and sequence (B5.9 deepen).
bool preflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength,
                            TaaJitterGuardRejectReason* reason = nullptr);
/// Jitter NDC preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength, TaaJitterGuardRejectReason& reason);
/// Early-out when NDC jitter preflight would reject (B5.9 deepen).
bool shouldSkipTaaJitterNdc(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// Classify why jitter advance would be rejected (B5.9 deepen).
TaaJitterGuardRejectReason classifyTaaJitterAdvanceReject(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// True when jitter can advance for the given sequence (B5.9 deepen).
bool preflightTaaJitterAdvance(u32 sequenceLength = kTaaDefaultJitterSequenceLength,
                                 TaaJitterGuardRejectReason* reason = nullptr);
/// Jitter advance preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaJitterAdvance(u32 sequenceLength, TaaJitterGuardRejectReason& reason);
/// Early-out when jitter advance preflight would reject (B5.9 deepen).
bool shouldSkipTaaJitterAdvance(u32 sequenceLength = kTaaDefaultJitterSequenceLength);

/// Halton (2,3) sequence helpers — CPU reference for projection jitter (B5.9 deepen).
struct TaaJitterLayout {
    static f32 halton(u32 index, u32 base);
    static bool validateSequenceLength(u32 length);
    /// True when viewport dimensions are non-zero for NDC jitter (B5.9 deepen).
    static bool validateViewportDimensions(u32 width, u32 height);
    /// True when jitter slot index is within the active sequence (B5.9 deepen).
    static bool jitterIndexInRange(u32 index, u32 sequenceLength);
    /// True when a monotonic frame counter may be synced into the active sequence (B5.9 deepen).
    static bool canSyncToFrameIndex(u32 sequenceLength);
    /// True when `index` is the slot `frameIndex` maps to for the active sequence (B5.9 deepen).
    static bool jitterIndexMatchesFrame(u32 index, u32 frameIndex, u32 sequenceLength);
    /// True when NDC jitter can be produced for viewport and sequence (B5.9 deepen).
    static bool canProduceNdcOffset(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when jitter can align to a monotonic frame counter (B5.9 deepen).
    static bool canSyncToFrameIndex(u32 /*frameIndex*/, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when `slot` is the active Halton index for `frameIndex` (B5.9 deepen).
    static bool jitterSlotMatchesFrameIndex(u32 frameIndex, u32 slot, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when NDC jitter can be computed for the viewport (B5.9 deepen).
    static bool canComputeNdcOffset(u32 width, u32 height);
    /// True when jitter state can be aligned to a monotonic frame counter (B5.9 deepen).
    static bool canSyncToFrameIndex(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when two NDC jitter offsets are approximately equal (B5.9 deepen).
    static bool ndcOffsetsMatch(const fuse::math::Vec2& a, const fuse::math::Vec2& b, f32 epsilon = 1e-6f);
    /// Returns the jitter cycle length after validation (0 when invalid).
    static u32 sequencePeriod(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Maps a monotonic frame counter into the active Halton slot.
    static u32 frameIndexInSequence(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when `slot` is the jitter index for `frameIndex` (B5.9 deepen).
    static bool slotMatchesFrameIndex(u32 slot, u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    static fuse::math::Vec2 haltonPixelOffset(u32 index, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    static fuse::math::Vec2 haltonNdcOffset(u32 index, u32 width, u32 height,
                                            u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Returns zero when viewport dimensions are invalid (B5.9 deepen guard).
    static fuse::math::Vec2 safeHaltonNdcOffset(u32 index, u32 width, u32 height,
                                                u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Halton offset for a monotonic frame counter (wraps via `frameIndexInSequence`).
    static fuse::math::Vec2 offsetForFrameIndex(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Halton offset for a frame counter only when the sequence is valid (B5.9 deepen).
    static bool offsetForFrameIndexIfReady(u32 frameIndex, u32 sequenceLength, fuse::math::Vec2& out);
    /// NDC jitter for a monotonic frame counter (wraps via `frameIndexInSequence`).
    static fuse::math::Vec2 ndcOffsetForFrameIndex(u32 frameIndex, u32 width, u32 height,
                                                   u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// NDC jitter for a frame counter only when viewport and sequence are valid (B5.9 deepen).
    static bool ndcOffsetForFrameIndexIfReady(u32 frameIndex, u32 width, u32 height, u32 sequenceLength,
                                              fuse::math::Vec2& out);
    /// Returns zero when viewport dimensions are invalid (B5.9 deepen guard).
    /// NDC jitter for a frame counter; returns zero when viewport dimensions are invalid (B5.9 deepen).
    static fuse::math::Vec2 safeNdcOffsetForFrameIndex(u32 frameIndex, u32 width, u32 height,
                                                       u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when `slot` is the Halton slot for a monotonic frame counter (B5.9 deepen).
    static bool monotonicFrameMatchesSlot(u32 frameIndex, u32 slot, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when `observedFrameIndex` maps to the same jitter slot as `expectedFrameIndex`.
    static bool jitterSyncMatches(u32 observedFrameIndex, u32 expectedFrameIndex, u32 sequenceLength);
    /// Fills a Halton (2,3) table; returns false when `out` is null or length is invalid.
    static bool fillHaltonSequence(u32 length, fuse::math::Vec2* out);
    /// True when `slot` is the expected Halton slot for a monotonic frame counter (B5.9 deepen).
    static bool monotonicFrameMatchesSlot(u32 monotonicFrame, u32 slot, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
};

/// Jitter sync classification for monotonic frame alignment (B5.9 deepen).
enum class TaaJitterSyncStatus : u8 {
    Synced = 0,
    SlotMismatch,
    MonotonicMismatch,
};
/// Classify jitter sync against an expected monotonic frame counter (B5.9 deepen).
TaaJitterSyncStatus classifyTaaJitterSync(u32 monotonicFrame, u32 slot, u32 expectedFrameIndex,
                                          u32 sequenceLength = kTaaDefaultJitterSequenceLength);

/// Sub-pixel jitter state — advances through a Halton sequence each frame (B5.9).
class TaaJitter {
public:
    explicit TaaJitter(const TaaJitterDesc& desc = {});

    fuse::math::Vec2 currentPixelOffset() const;
    fuse::math::Vec2 currentNdcOffset(u32 width, u32 height) const;
    /// NDC offset only when viewport and sequence are valid; returns false when blocked (B5.9 deepen).
    bool currentNdcOffsetIfReady(u32 width, u32 height, fuse::math::Vec2& out) const;
    /// True when viewport dimensions allow NDC jitter output (B5.9 deepen).
    bool canProvideNdcOffset(u32 width, u32 height) const;

    void advance();
    /// Advance only when the sequence is valid; returns false when blocked (B5.9 deepen).
    bool advanceIfReady();
    /// Advance only when the jitter sequence is valid; returns false when blocked (B5.9 deepen).
    bool advanceIfPossible();
    void reset();
    /// Align jitter state to a monotonic frame counter (wraps with sequence period).
    void syncToFrameIndex(u32 frameIndex);
    /// Sync only when the sequence is valid; returns false when blocked (B5.9 deepen).
    bool syncToFrameIndexIfReady(u32 frameIndex);
    /// True when monotonic frame counter and slot match `frameIndex` (B5.9 deepen).
    bool isAlignedToFrameIndex(u32 frameIndex) const;
    /// True when jitter slot and monotonic counter match a frame index (B5.9 deepen).
    bool isSyncedToFrameIndex(u32 frameIndex) const;
    /// True when monotonic frame counter matches the requested frame (B5.9 deepen).
    /// True when the active slot matches the monotonic frame counter (B5.9 deepen).
    bool slotMatchesMonotonicFrame() const;
    /// True when monotonic frame and slot match `frameIndex` (B5.9 deepen guard).
    /// True when monotonic frame counter and slot index match `frameIndex` (B5.9 deepen).

    u32 index() const { return m_index; }
    /// True when monotonic frame counter matches `frameIndex` (B5.9 deepen).
    bool isSyncedToFrameIndex(u32 frameIndex) const;
    /// True when jitter must be resynced to `frameIndex` (B5.9 deepen).
    bool needsSyncToFrameIndex(u32 frameIndex) const;
    /// True when the active slot matches the slot for `frameIndex` (B5.9 deepen).
    bool slotMatchesFrame(u32 frameIndex) const;
    /// True when the jitter sequence can advance (B5.9 deepen).
    bool canAdvance() const;
    /// True when NDC jitter can be produced for the given viewport (B5.9 deepen).
    bool canProduceNdcOffset(u32 width, u32 height) const;
    /// True when jitter can align to a monotonic frame counter (B5.9 deepen).
    bool canSyncToFrameIndex(u32 frameIndex) const;
    /// True when jitter slot and monotonic counter match `frameIndex` (B5.9 deepen).
    bool isSyncedToFrameIndex(u32 frameIndex) const;
    /// True when jitter state matches `frameIndex` (B5.9 deepen).
    /// Expected Halton slot for `frameIndex` with the active sequence length (B5.9 deepen).
    u32 expectedSlotForFrameIndex(u32 frameIndex) const;
    /// True when slot and monotonic counter align with `frameIndex` (B5.9 deepen).
    bool slotMatchesFrameIndex(u32 frameIndex) const;
    /// True when the active sequence can accept `syncToFrameIndex` (B5.9 deepen).
    bool canSyncToFrameIndex() const;
    /// True when monotonic frame and slot align with `frameIndex` (B5.9 deepen).
    /// Monotonic frame counter — incremented by `advance`, set by `syncToFrameIndex`, cleared by `reset`.
    u32 monotonicFrameIndex() const { return m_monotonicFrame; }
    /// True when jitter state matches the expected monotonic frame counter (B5.9 deepen).
    bool isSyncedToFrameIndex(u32 frameIndex) const;
    /// Classify sync status against an expected monotonic frame counter (B5.9 deepen).
    TaaJitterSyncStatus syncStatusForFrameIndex(u32 frameIndex) const;
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
/// True when `slot` matches the wrapped frame counter for the active sequence (B5.9 deepen).
bool taaJitterSlotMatchesFrame(u32 frameIndex, u32 slot, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// Preflight jitter sync for viewport and frame counter without mutating state (B5.9 deepen).
bool preflightTaaJitterSync(const TaaJitter& jitter, u32 frameIndex, u32 width, u32 height,
                            TaaJitterSyncRejectReason* reason = nullptr);
/// True when `jitter` monotonic frame and slot align with `frameIndex` (B5.9 deepen).
bool taaJitterFrameSynced(const TaaJitter& jitter, u32 frameIndex);
/// Classify why jitter sync preflight would reject (B5.9 deepen).
TaaJitterSyncRejectReason classifyTaaJitterSyncReject(const TaaJitter& jitter, u32 frameIndex, u32 width,
                                                      u32 height);
/// Preflight jitter sync — returns true when jitter matches `frameIndex` for the viewport (B5.9 deepen).
bool taaJitterSyncPreflight(const TaaJitter& jitter, u32 frameIndex, u32 width, u32 height,

} // namespace fuse::renderer
