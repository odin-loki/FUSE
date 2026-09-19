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
    /// Halton offset for a frame counter only when the sequence is valid (B5.9 deepen).
    static bool offsetForFrameIndexIfReady(u32 frameIndex, u32 sequenceLength, fuse::math::Vec2& out);
    /// NDC jitter for a monotonic frame counter (wraps via `frameIndexInSequence`).
    static fuse::math::Vec2 ndcOffsetForFrameIndex(u32 frameIndex, u32 width, u32 height,
                                                   u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// NDC jitter for a frame counter only when viewport and sequence are valid (B5.9 deepen).
    static bool ndcOffsetForFrameIndexIfReady(u32 frameIndex, u32 width, u32 height, u32 sequenceLength,
                                              fuse::math::Vec2& out);
    /// Fills a Halton (2,3) table; returns false when `out` is null or length is invalid.
    static bool fillHaltonSequence(u32 length, fuse::math::Vec2* out);
};

/// Sub-pixel jitter state — advances through a Halton sequence each frame (B5.9).
class TaaJitter {
public:
    explicit TaaJitter(const TaaJitterDesc& desc = {});

    fuse::math::Vec2 currentPixelOffset() const;
    fuse::math::Vec2 currentNdcOffset(u32 width, u32 height) const;
    /// NDC offset only when viewport and sequence are valid; returns false when blocked (B5.9 deepen).
    bool currentNdcOffsetIfReady(u32 width, u32 height, fuse::math::Vec2& out) const;

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

// --- deepen additive from deepen-b59-taa-guards-8293 ---
struct TaaJitterSyncPreflight {
TaaJitterSyncPreflight preflightTaaJitterSync(const TaaJitter& jitter, u32 frameIndex, u32 width, u32 height);

// --- deepen additive from deepen-b59-taa-guards-94f6 ---
bool preflightTaaJitterSync(const TaaJitter& jitter, u32 frameIndex, u32 width, u32 height,
                            TaaJitterSyncRejectReason* reason = nullptr);

// --- deepen additive from deepen-b59-taa-guards-5b8c ---
TaaJitterSyncRejectReason classifyTaaJitterSyncReject(const TaaJitter& jitter, u32 frameIndex, u32 width,
bool taaJitterSyncPreflight(const TaaJitter& jitter, u32 frameIndex, u32 width, u32 height,

// --- deepen additive from deepen-b59-taa-guards-27d7 ---
    bool preflightSync(u32 frameIndex, u32 width, u32 height, TaaJitterSyncBlockReason* reason = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-117f ---
bool preflightTaaJitterSync(const TaaJitter& jitter, u32 frameIndex, TaaJitterSyncBlockReason* reason = nullptr);

// --- deepen additive from deepen-fuse-b59-taa-cd32 ---
enum class TaaJitterSyncRejectReason : u8 {
const char* taaJitterSyncRejectReasonLabel(TaaJitterSyncRejectReason reason);
TaaJitterSyncRejectReason classifyTaaJitterSyncReject(u32 /*frameIndex*/, u32 sequenceLength);
bool canPreflightTaaJitterSync(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
enum class TaaJitterNdcRejectReason : u8 {
const char* taaJitterNdcRejectReasonLabel(TaaJitterNdcRejectReason reason);
TaaJitterNdcRejectReason classifyTaaJitterNdcReject(u32 width, u32 height,
                             TaaJitterNdcRejectReason* reason = nullptr);
bool canPreflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    TaaJitterSyncRejectReason classifySyncReject(u32 frameIndex) const;
    bool preflightSyncToFrameIndex(u32 frameIndex, TaaJitterSyncRejectReason* reason = nullptr) const;
    TaaJitterNdcRejectReason classifyNdcReject(u32 width, u32 height) const;
    bool preflightCurrentNdcOffset(u32 width, u32 height, TaaJitterNdcRejectReason* reason = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-1d2e ---
bool taaJitterSyncBlockReasonIsBlocking(TaaJitterSyncBlockReason reason);
bool preflightTaaJitterSync(u32 frameIndex, u32 sequenceLength, TaaJitterSyncBlockReason* reason = nullptr);
    bool preflightSync(u32 frameIndex, TaaJitterSyncBlockReason* reason = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-3c58 ---
    static TaaJitterSyncRejectReason classifySyncReject(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    static bool preflightSyncToFrameIndex(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength,
    bool preflightSyncToFrameIndex(u32 frameIndex, TaaJitterSyncRejectReason* reason = nullptr);

// --- deepen additive from deepen-b59-taa-guards-53dc ---
bool preflightTaaJitterAlignment(u32 expectedFrameIndex, const TaaJitter& jitter);

// --- deepen additive from deepen-b59-taa-guards-efe8 ---
    bool tryAdvanceIfReady(TaaJitterAdvanceBlockReason& outReason);
    bool tryAdvanceIfViewportReady(u32 width, u32 height, TaaJitterAdvanceBlockReason& outReason);
    bool trySyncToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncBlockReason& outReason);
    bool trySyncToFrameIndexIfViewportReady(u32 frameIndex, u32 width, u32 height,
