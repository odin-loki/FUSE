#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/taa/taa_types.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Why jitter sync to a monotonic frame counter is blocked (B5.9 deepen).
enum class TaaJitterSyncBlockReason : u8 {
    None = 0,
    InvalidSequence,
    DriftedFromFrame,
};
/// Why jitter sync preflight rejected the request (B5.9 deepen).
enum class TaaJitterSyncRejectReason : u8 {
/// Human-readable label for jitter sync reject reasons (B5.9 deepen).
const char* taaJitterSyncRejectReasonLabel(TaaJitterSyncRejectReason reason);
/// Why jitter sync to a frame index is blocked (B5.9 deepen).
    InvalidViewport,
/// Human-readable label for jitter sync block reasons (B5.9 deepen).
const char* taaJitterSyncBlockReasonLabel(TaaJitterSyncBlockReason reason);
/// Classify why jitter sync is blocked for the active sequence (B5.9 deepen).
TaaJitterSyncBlockReason classifyTaaJitterSyncBlock(u32 sequenceLength);
/// Classify why jitter sync is blocked for viewport + sequence (B5.9 deepen).
TaaJitterSyncBlockReason classifyTaaJitterSyncViewportBlock(u32 width, u32 height, u32 sequenceLength);
/// Why jitter advance is blocked (B5.9 deepen).
enum class TaaJitterAdvanceBlockReason : u8 {
/// Human-readable label for jitter advance block reasons (B5.9 deepen).
const char* taaJitterAdvanceBlockReasonLabel(TaaJitterAdvanceBlockReason reason);
/// Classify why jitter advance is blocked for viewport + sequence (B5.9 deepen).
TaaJitterAdvanceBlockReason classifyTaaJitterAdvanceBlock(u32 width, u32 height, u32 sequenceLength);
/// Why jitter sync-to-frame preflight rejected the request (B5.9 deepen).
/// Classify why jitter cannot sync to a monotonic frame counter (B5.9 deepen).
TaaJitterSyncRejectReason classifyTaaJitterSyncReject(u32 frameIndex, u32 sequenceLength);
/// True when jitter may align to `frameIndex` for the given sequence (B5.9 deepen).
bool preflightTaaJitterSync(u32 frameIndex, u32 sequenceLength, TaaJitterSyncRejectReason* reason = nullptr);
/// Why jitter sync/advance would be blocked (B5.9 deepen).

/// Classify why jitter sync to a monotonic frame counter would be blocked (B5.9 deepen).
TaaJitterSyncRejectReason classifyTaaJitterSyncReject(u32 sequenceLength);


static constexpr u32 kTaaDefaultJitterSequenceLength = 8;
static constexpr u32 kTaaMaxJitterSequenceLength = 64;

/// Why jitter sync or NDC production preflight rejected the request (B5.9 deepen).
enum class TaaJitterGuardRejectReason : u8 {
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
    InvalidViewport,

/// Human-readable label for jitter guard reject reasons (B5.9 deepen).
const char* taaJitterGuardRejectReasonLabel(TaaJitterGuardRejectReason reason);
/// Classify why jitter sync to a frame counter would be rejected (B5.9 deepen).
TaaJitterGuardRejectReason classifyTaaJitterSyncReject(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// Classify why NDC jitter production would be rejected (B5.9 deepen).
TaaJitterGuardRejectReason classifyTaaJitterNdcReject(u32 width, u32 height,
                                                      u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// True when jitter can sync to `frameIndex` for the given sequence (B5.9 deepen).
                             TaaJitterGuardRejectReason* reason = nullptr);
/// Jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaJitterSync(u32 frameIndex, u32 sequenceLength, TaaJitterGuardRejectReason& reason);
/// Early-out when jitter sync preflight would reject (B5.9 deepen).
bool shouldSkipTaaJitterSync(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// True when jitter can sync to `frameIndex` for the given sequence (B5.9 deepen).
bool taaJitterSyncReady(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength,
                        TaaJitterGuardRejectReason* reason = nullptr);
/// Early-out when jitter sync to a frame counter would be blocked (B5.9 deepen).
bool taaJitterSyncReady(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// Early-out when jitter sync to `frameIndex` would be rejected (B5.9 deepen).
bool shouldSkipTaaJitterSync(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// True when NDC jitter can be produced for viewport and sequence (B5.9 deepen).
bool preflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength,
/// Jitter NDC preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength, TaaJitterGuardRejectReason& reason);
/// Early-out when NDC jitter preflight would reject (B5.9 deepen).
bool shouldSkipTaaJitterNdc(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// Classify why jitter advance would be rejected (B5.9 deepen).
TaaJitterGuardRejectReason classifyTaaJitterAdvanceReject(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// True when jitter can advance for the given sequence (B5.9 deepen).
bool preflightTaaJitterAdvance(u32 sequenceLength = kTaaDefaultJitterSequenceLength,
/// Jitter advance preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaJitterAdvance(u32 sequenceLength, TaaJitterGuardRejectReason& reason);
/// Early-out when jitter advance preflight would reject (B5.9 deepen).
bool shouldSkipTaaJitterAdvance(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// Human-readable label for jitter NDC reject reasons (B5.9 deepen).
const char* taaJitterNdcRejectReasonLabel(TaaJitterNdcRejectReason reason);
TaaJitterNdcRejectReason classifyTaaJitterNdcReject(u32 width, u32 height,
                             TaaJitterNdcRejectReason* reason = nullptr);
/// Convenience wrapper — true when `preflightTaaJitterNdc` would pass (B5.9 deepen).
bool canPreflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// Why jitter sync preflight rejected alignment to a monotonic frame counter (B5.9 deepen).
enum class TaaJitterSyncBlockReason : u8 {

/// Human-readable label for jitter sync block reasons (B5.9 deepen).
const char* taaJitterSyncBlockReasonLabel(TaaJitterSyncBlockReason reason);
/// True when a jitter sync block reason prevents alignment (B5.9 deepen).
bool taaJitterSyncBlockReasonIsBlocking(TaaJitterSyncBlockReason reason);
/// Classify why jitter cannot align to `frameIndex` with `sequenceLength` (B5.9 deepen).
TaaJitterSyncBlockReason classifyTaaJitterSyncBlock(u32 frameIndex, u32 sequenceLength);
/// True when jitter can align to `frameIndex` with `sequenceLength` (B5.9 deepen).
bool preflightTaaJitterSync(u32 frameIndex, u32 sequenceLength, TaaJitterSyncBlockReason* reason = nullptr);
/// Why jitter sync preflight blocked the request (B5.9 deepen).
/// Classify why jitter cannot sync to a monotonic frame counter (B5.9 deepen).
TaaJitterSyncBlockReason classifyTaaJitterSyncBlock(u32 /*frameIndex*/, u32 width, u32 height,
/// True when jitter can sync to `frameIndex` for the given viewport and sequence (B5.9 deepen).
bool preflightTaaJitterSync(u32 frameIndex, u32 width, u32 height,
                            u32 sequenceLength = kTaaDefaultJitterSequenceLength,
                            TaaJitterSyncBlockReason* reason = nullptr);
    None = 0,
    InvalidSequence,
};
                            TaaJitterGuardRejectReason* reason = nullptr);
/// NDC jitter preflight with mandatory reject-reason output (B5.9 deepen).
/// Early-out when jitter sync preflight would reject (B5.9 deepen).
bool shouldSkipTaaJitterSync(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// Jitter NDC preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength, TaaJitterGuardRejectReason& reason);
/// Early-out when NDC jitter preflight would reject (B5.9 deepen).
/// Early-out when jitter sync to a frame counter would be rejected (B5.9 deepen).
/// Early-out when NDC jitter production would be rejected (B5.9 deepen).
bool shouldSkipTaaJitterNdc(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// True when jitter can sync to `frameIndex` for the given sequence (B5.9 deepen).
bool taaJitterSyncReady(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// True when NDC jitter can be produced for viewport and sequence (B5.9 deepen).
bool taaJitterNdcReady(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
bool taaJitterNdcReady(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength,
                       TaaJitterGuardRejectReason* reason = nullptr);
/// Early-out when NDC jitter production would be blocked (B5.9 deepen).
/// Classify why jitter advance would be rejected (B5.9 deepen).
TaaJitterGuardRejectReason classifyTaaJitterAdvanceReject(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// True when jitter can advance for the given sequence (B5.9 deepen).
bool preflightTaaJitterAdvance(u32 sequenceLength = kTaaDefaultJitterSequenceLength,
/// Jitter advance preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaJitterAdvance(u32 sequenceLength, TaaJitterGuardRejectReason& reason);
/// Early-out when jitter advance would be rejected (B5.9 deepen).
bool shouldSkipTaaJitterAdvance(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// Compute NDC jitter for a frame counter with reject-reason diagnostics (B5.9 deepen).
bool tryComputeTaaJitterNdcOffset(u32 frameIndex, u32 width, u32 height, u32 sequenceLength,
                                  fuse::math::Vec2& outOffset, TaaJitterGuardRejectReason& reason);
/// Early-out when jitter sync would be rejected for the sequence (B5.9 deepen).
bool shouldSkipTaaJitterSync(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
/// Jitter NDC preflight with mandatory reject-reason output (B5.9 deepen follow-up).
/// Early-out when jitter sync to a frame counter would be rejected (B5.9 deepen follow-up).
/// Early-out when NDC jitter production would be rejected (B5.9 deepen follow-up).
/// True when jitter can advance for the given sequence (B5.9 deepen follow-up).
/// Early-out when jitter advance would be rejected (B5.9 deepen follow-up).

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
    /// True when viewport and sequence are valid for sync + NDC production (B5.9 deepen).
    static bool canSyncAndProduceNdc(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Early-out when jitter sync would be blocked for the sequence (B5.9 deepen).
    static bool wouldSkipSyncToFrameIndex(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Classify why jitter sync would be rejected (B5.9 deepen).
    static TaaJitterSyncRejectReason classifyTaaJitterSyncReject(
        u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when `slot` is the active Halton index for `frameIndex` (B5.9 deepen).
    static bool jitterSlotMatchesFrameIndex(u32 frameIndex, u32 slot, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when NDC jitter can be computed for the viewport (B5.9 deepen).
    static bool canComputeNdcOffset(u32 width, u32 height);
    /// True when jitter state can be aligned to a monotonic frame counter (B5.9 deepen).
    static bool canSyncToFrameIndex(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when two NDC jitter offsets are approximately equal (B5.9 deepen).
    static bool ndcOffsetsMatch(const fuse::math::Vec2& a, const fuse::math::Vec2& b, f32 epsilon = 1e-6f);
    /// Expected Halton slot for a monotonic frame counter (alias of `frameIndexInSequence`).
    static u32 expectedSlotForFrameIndex(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Classify why jitter sync would be rejected for the given sequence (B5.9 deepen).
    static TaaJitterSyncRejectReason classifySyncReject(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when jitter can sync to `frameIndex` for the given sequence (B5.9 deepen).
    static bool preflightSyncToFrameIndex(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength,
                                          TaaJitterSyncRejectReason* reason = nullptr);
    /// Circular slot distance from the frame-index mapping (0 when aligned) (B5.9 deepen).
    static u32 frameIndexSlotDrift(u32 frameIndex, u32 slot, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when jitter slot differs from the frame-index mapping (B5.9 deepen).
    static bool needsSyncToFrameIndex(u32 frameIndex, u32 slot, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Returns the jitter cycle length after validation (0 when invalid).
    static u32 sequencePeriod(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Maps a monotonic frame counter into the active Halton slot.
    static u32 frameIndexInSequence(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when `slot` is the jitter index for `frameIndex` (B5.9 deepen).
    static bool slotMatchesFrameIndex(u32 slot, u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Expected Halton slot for a monotonic frame counter (alias for sync preflight) (B5.9 deepen).
    static u32 expectedSlotForMonotonicFrame(u32 monotonicFrame,
                                             u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// True when `slot` matches the expected Halton slot for `monotonicFrame` (B5.9 deepen).
    static bool monotonicFrameMatchesSlot(u32 monotonicFrame, u32 slot,
    /// True when `index` matches the slot for `frameIndex` (B5.9 deepen).
    static bool jitterIndexMatchesFrameIndex(u32 frameIndex, u32 index,
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
    /// True when slot index matches the expected slot for a monotonic frame counter (B5.9 deepen).
    static bool slotMatchesMonotonicFrame(u32 slot, u32 frameIndex,
    /// NDC jitter for a monotonic frame counter when viewport and sequence are valid (B5.9 deepen).
    static bool ndcOffsetForFrameIndexIfReady(u32 frameIndex, u32 width, u32 height, fuse::math::Vec2* out,
    /// NDC jitter for a monotonic frame counter with reject-reason diagnostics (B5.9 deepen).
    static bool tryNdcOffsetForFrameIndex(u32 frameIndex, u32 width, u32 height, u32 sequenceLength,
                                          fuse::math::Vec2& out, TaaJitterGuardRejectReason& reason);
    /// NDC jitter for a frame counter with guard preflight and reject-reason diagnostics (B5.9 deepen).
    static bool tryComputeNdcOffsetForFrameIndex(u32 frameIndex, u32 width, u32 height, u32 sequenceLength,
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
    /// Pixel offset only when the sequence is valid; returns false when blocked (B5.9 deepen).
    bool currentPixelOffsetIfReady(fuse::math::Vec2& out) const;
    fuse::math::Vec2 currentNdcOffset(u32 width, u32 height) const;
    /// NDC offset only when viewport and sequence are valid; returns false when blocked (B5.9 deepen).
    bool currentNdcOffsetIfReady(u32 width, u32 height, fuse::math::Vec2& out) const;
    /// True when viewport dimensions allow NDC jitter output (B5.9 deepen).
    bool canProvideNdcOffset(u32 width, u32 height) const;
    /// NDC offset with mandatory reject-reason diagnostics (B5.9 deepen).
    bool tryCurrentNdcOffset(u32 width, u32 height, fuse::math::Vec2& out, TaaJitterGuardRejectReason& reason) const;
    /// NDC offset with reject-reason diagnostics (B5.9 deepen).
    bool tryCurrentNdcOffsetIfReady(u32 width, u32 height, fuse::math::Vec2& out,
                                    TaaJitterGuardRejectReason& reason) const;

    void advance();
    /// Advance only when the sequence is valid; returns false when blocked (B5.9 deepen).
    bool advanceIfReady();
    /// Advance only when the jitter sequence is valid; returns false when blocked (B5.9 deepen).
    bool advanceIfPossible();
    /// Advance only when aligned to `expectedFrameIndex`; returns false when drifted (B5.9 deepen).
    bool advanceIfAlignedToFrameIndex(u32 expectedFrameIndex);
    /// Advance only when sequence and viewport are valid; returns false when blocked (B5.9 deepen).
    bool advanceIfViewportReady(u32 width, u32 height);
    /// Diagnose jitter advance; false when advance is blocked (B5.9 deepen).
    bool tryAdvanceIfReady(TaaJitterAdvanceBlockReason& outReason);
    /// Diagnose viewport-aware jitter advance; false when advance is blocked (B5.9 deepen).
    bool tryAdvanceIfViewportReady(u32 width, u32 height, TaaJitterAdvanceBlockReason& outReason);
    void reset();
    /// Align jitter state to a monotonic frame counter (wraps with sequence period).
    void syncToFrameIndex(u32 frameIndex);
    /// Sync only when the sequence is valid; returns false when blocked (B5.9 deepen).
    bool syncToFrameIndexIfReady(u32 frameIndex);
    /// Classify why sync to `frameIndex` would be blocked (B5.9 deepen).
    TaaJitterSyncBlockReason classifySyncBlock(u32 frameIndex) const;
    /// Sync preflight without mutating jitter state (B5.9 deepen).
    bool preflightSync(u32 frameIndex, TaaJitterSyncBlockReason* reason = nullptr) const;
    /// Sync only when sequence and viewport are valid; returns false when blocked (B5.9 deepen).
    bool syncToFrameIndexIfViewportReady(u32 frameIndex, u32 width, u32 height);
    /// Diagnose jitter sync; false when sync is blocked (B5.9 deepen).
    bool trySyncToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncBlockReason& outReason);
    /// Diagnose viewport-aware jitter sync; false when sync is blocked (B5.9 deepen).
    bool trySyncToFrameIndexIfViewportReady(u32 frameIndex, u32 width, u32 height,
                                            TaaJitterSyncBlockReason& outReason);
    /// Sync only when viewport and sequence preflight pass; returns false when blocked (B5.9 deepen).
    /// True when jitter can sync to `frameIndex` for the given viewport (B5.9 deepen).
    bool preflightSyncToFrameIndex(u32 frameIndex, u32 width, u32 height,
                                   TaaJitterSyncBlockReason* reason = nullptr) const;
    /// True when jitter is not aligned to `frameIndex` but could sync (B5.9 deepen).
    bool needsSyncToFrameIndex(u32 frameIndex) const;
    /// Sync only when misaligned; returns false when blocked (B5.9 deepen).
    bool syncToFrameIndexIfMisaligned(u32 frameIndex);
    /// Sync with required reject-reason diagnostics; returns false when blocked (B5.9 deepen).
    bool trySyncToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncRejectReason& outReason);
    /// Early-out when jitter sync would be blocked (B5.9 deepen).
    bool wouldSkipSyncToFrameIndex(u32 frameIndex) const;
    /// Sync with reject-reason diagnostics (B5.9 deepen).
    bool trySyncToFrameIndexIfReady(u32 frameIndex, TaaJitterGuardRejectReason& reason);
    /// True when monotonic frame counter and slot match `frameIndex` (B5.9 deepen).
    bool isAlignedToFrameIndex(u32 frameIndex) const;
    /// True when jitter slot and monotonic counter match a frame index (B5.9 deepen).
    bool isSyncedToFrameIndex(u32 frameIndex) const;
    /// True when monotonic frame counter matches the requested frame (B5.9 deepen).
    /// True when the active slot matches the monotonic frame counter (B5.9 deepen).
    bool slotMatchesMonotonicFrame() const;
    /// True when monotonic frame and slot match `frameIndex` (B5.9 deepen guard).
    /// True when monotonic frame counter and slot index match `frameIndex` (B5.9 deepen).
    /// True when jitter state matches the given monotonic frame counter (B5.9 deepen).
    /// Align jitter only when sync preflight passes; returns false when blocked (B5.9 deepen).
    /// True when jitter state matches the expected slot for `frameIndex` (B5.9 deepen).
    /// Align jitter only when the sequence is valid; returns false when blocked (B5.9 deepen).
    /// True when jitter index and monotonic counter match `frameIndex` (B5.9 deepen).
    /// True when jitter must resync to align with `frameIndex` (B5.9 deepen).
    bool needsResyncToFrameIndex(u32 frameIndex) const;
    /// Advance only when already aligned to `frameIndex`; returns false when desynced (B5.9 deepen).
    bool advanceIfAlignedToFrameIndex(u32 frameIndex);
    /// Classify why sync to `frameIndex` would be rejected (B5.9 deepen).
    TaaJitterSyncRejectReason classifySyncReject(u32 frameIndex) const;
    /// Sync only when preflight passes; returns false when blocked (B5.9 deepen).
    bool preflightSyncToFrameIndex(u32 frameIndex, TaaJitterSyncRejectReason* reason = nullptr);
    /// Circular slot distance from the frame-index mapping (0 when aligned) (B5.9 deepen).
    u32 frameIndexSlotDrift(u32 frameIndex) const;
    /// True when jitter slot differs from the frame-index mapping (B5.9 deepen).
    bool needsSyncToFrameIndex(u32 frameIndex) const;
    /// Sync with reject-reason diagnostics; returns false when blocked (B5.9 deepen).
    bool syncToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncRejectReason* reason);
    /// True when jitter state differs from the expected slot for `frameIndex` (B5.9 deepen).
    /// True when jitter state must resync to `frameIndex` before projection (B5.9 deepen).

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
    /// Classify why this jitter cannot sync for the given viewport (B5.9 deepen).
    TaaJitterSyncBlockReason classifySyncBlock(u32 width, u32 height) const;
    /// True when jitter can sync to `frameIndex` for the given viewport (B5.9 deepen).
    bool preflightSync(u32 frameIndex, u32 width, u32 height, TaaJitterSyncBlockReason* reason = nullptr) const;
    /// Classify why sync would be rejected for `frameIndex` (B5.9 deepen).
    TaaJitterSyncRejectReason classifySyncReject(u32 frameIndex) const;
    /// Preflight sync without mutating state (B5.9 deepen).
    bool preflightSyncToFrameIndex(u32 frameIndex, TaaJitterSyncRejectReason* reason = nullptr) const;
    /// Classify why NDC offset production would be rejected (B5.9 deepen).
    TaaJitterNdcRejectReason classifyNdcReject(u32 width, u32 height) const;
    /// Preflight NDC offset production without mutating state (B5.9 deepen).
    bool preflightCurrentNdcOffset(u32 width, u32 height, TaaJitterNdcRejectReason* reason = nullptr) const;
    /// Early-out when sync to `frameIndex` would be rejected (B5.9 deepen).
    bool shouldSkipSyncToFrameIndex(u32 frameIndex) const;
    /// Early-out when NDC jitter production would be rejected (B5.9 deepen).
    bool shouldSkipNdcOffset(u32 width, u32 height) const;
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
/// Why jitter frame-index sync preflight blocked the request (B5.9 deepen).
enum class TaaJitterSyncBlockReason : u8 {
    None = 0,
    InvalidSequence,
    Misaligned,
/// Human-readable label for jitter sync block reasons (B5.9 deepen).
const char* taaJitterSyncBlockReasonLabel(TaaJitterSyncBlockReason reason);
/// Classify why jitter is not aligned to `frameIndex` (B5.9 deepen).
TaaJitterSyncBlockReason classifyTaaJitterSyncBlock(const TaaJitter& jitter, u32 frameIndex);
/// True when jitter is aligned to `frameIndex` and the sequence is valid (B5.9 deepen).
bool preflightTaaJitterSync(const TaaJitter& jitter, u32 frameIndex, TaaJitterSyncBlockReason* reason = nullptr);
/// True when jitter can align to `frameIndex` without drift (B5.9 deepen).
bool preflightTaaJitterSync(const TaaJitter& jitter, u32 frameIndex,
                            TaaJitterSyncBlockReason* reason = nullptr);
/// True when jitter monotonic counter or slot differs from `frameIndex` (B5.9 deepen).
bool taaJitterNeedsResyncToFrameIndex(const TaaJitter& jitter, u32 frameIndex);
/// True when jitter monotonic counter and slot match `expectedFrameIndex` (B5.9 deepen).
bool preflightTaaJitterAlignment(u32 expectedFrameIndex, const TaaJitter& jitter);
/// True when jitter is not aligned to `frameIndex` and sync would change state (B5.9 deepen).
bool wouldResyncJitterToFrameIndex(const TaaJitter& jitter, u32 frameIndex);
bool jitterNeedsResyncToFrameIndex(const TaaJitter& jitter, u32 frameIndex);
/// Diagnose jitter sync preflight; false when sync would be blocked (B5.9 deepen).
bool trySyncJitterToFrameIndexIfReady(TaaJitter& jitter, u32 frameIndex, TaaJitterSyncRejectReason& outReason);

} // namespace fuse::renderer
