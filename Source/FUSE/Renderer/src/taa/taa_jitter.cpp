#include <fuse/renderer/taa/taa_jitter.hpp>

#include <cmath>

namespace fuse::renderer {

f32 TaaJitterLayout::halton(u32 index, u32 base) {
    if (base < 2u) {
        return 0.f;
    }

    f32 result = 0.f;
    f32 fraction = 1.f;
    while (index > 0u) {
        fraction /= static_cast<f32>(base);
        result += fraction * static_cast<f32>(index % base);
        index /= base;
    }
    return result;
}

const char* taaJitterGuardRejectReasonLabel(TaaJitterGuardRejectReason reason) {
    switch (reason) {
    case TaaJitterGuardRejectReason::None:
        return "none";
    case TaaJitterGuardRejectReason::InvalidSequence:
        return "invalid_sequence";
    case TaaJitterGuardRejectReason::InvalidViewport:
        return "invalid_viewport";
    }
    return "unknown";
}

TaaJitterGuardRejectReason classifyTaaJitterSyncReject(u32 sequenceLength) {
    if (!TaaJitterLayout::validateSequenceLength(sequenceLength)) {
        return TaaJitterGuardRejectReason::InvalidSequence;
    }
    return TaaJitterGuardRejectReason::None;
}

TaaJitterGuardRejectReason classifyTaaJitterNdcReject(u32 width, u32 height, u32 sequenceLength) {
    const TaaJitterGuardRejectReason syncReject = classifyTaaJitterSyncReject(sequenceLength);
    if (syncReject != TaaJitterGuardRejectReason::None) {
        return syncReject;
    }
    if (!TaaJitterLayout::validateViewportDimensions(width, height)) {
        return TaaJitterGuardRejectReason::InvalidViewport;
    }
    return TaaJitterGuardRejectReason::None;
}

bool preflightTaaJitterSync(u32 /*frameIndex*/, u32 sequenceLength, TaaJitterGuardRejectReason* reason) {
    const TaaJitterGuardRejectReason reject = classifyTaaJitterSyncReject(sequenceLength);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaJitterGuardRejectReason::None;
}

bool tryPreflightTaaJitterSync(u32 frameIndex, u32 sequenceLength, TaaJitterGuardRejectReason& reason) {
    return preflightTaaJitterSync(frameIndex, sequenceLength, &reason);
}

bool shouldSkipTaaJitterSync(u32 frameIndex, u32 sequenceLength) {
    return !preflightTaaJitterSync(frameIndex, sequenceLength);
}

bool preflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength, TaaJitterGuardRejectReason* reason) {
    const TaaJitterGuardRejectReason reject = classifyTaaJitterNdcReject(width, height, sequenceLength);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaJitterGuardRejectReason::None;
}

bool tryPreflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength, TaaJitterGuardRejectReason& reason) {
    return preflightTaaJitterNdc(width, height, sequenceLength, &reason);
}

bool shouldSkipTaaJitterNdc(u32 width, u32 height, u32 sequenceLength) {
    return !preflightTaaJitterNdc(width, height, sequenceLength);
}

TaaJitterGuardRejectReason classifyTaaJitterAdvanceReject(u32 sequenceLength) {
    return classifyTaaJitterSyncReject(sequenceLength);
}

bool preflightTaaJitterAdvance(u32 sequenceLength, TaaJitterGuardRejectReason* reason) {
    const TaaJitterGuardRejectReason reject = classifyTaaJitterAdvanceReject(sequenceLength);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaJitterGuardRejectReason::None;
}

bool tryPreflightTaaJitterAdvance(u32 sequenceLength, TaaJitterGuardRejectReason& reason) {
    return preflightTaaJitterAdvance(sequenceLength, &reason);
}

bool shouldSkipTaaJitterAdvance(u32 sequenceLength) {
    return !preflightTaaJitterAdvance(sequenceLength);
}

bool TaaJitterLayout::validateSequenceLength(u32 length) {
    return length > 0u && length <= kTaaMaxJitterSequenceLength;
}

bool TaaJitterLayout::validateViewportDimensions(u32 width, u32 height) {
    return width > 0u && height > 0u;
}

bool TaaJitterLayout::jitterIndexInRange(u32 index, u32 sequenceLength) {
    const u32 period = sequencePeriod(sequenceLength);
    return period > 0u && index < period;
}

bool TaaJitterLayout::canSyncToFrameIndex(u32 sequenceLength) {
    return validateSequenceLength(sequenceLength);
bool TaaJitterLayout::jitterIndexMatchesFrame(u32 index, u32 frameIndex, u32 sequenceLength) {
    return index == frameIndexInSequence(frameIndex, sequenceLength);
}

bool TaaJitterLayout::canProduceNdcOffset(u32 width, u32 height, u32 sequenceLength) {
    return validateViewportDimensions(width, height) && validateSequenceLength(sequenceLength);

bool TaaJitterLayout::canSyncToFrameIndex(u32 /*frameIndex*/, u32 sequenceLength) {
    return validateSequenceLength(sequenceLength);

bool TaaJitterLayout::jitterSlotMatchesFrameIndex(u32 frameIndex, u32 slot, u32 sequenceLength) {
    if (period == 0u) {
        return false;
    return frameIndexInSequence(frameIndex, sequenceLength) == slot;
bool TaaJitterLayout::canComputeNdcOffset(u32 width, u32 height) {
    return validateViewportDimensions(width, height);
}

bool TaaJitterLayout::canSyncToFrameIndex(u32 sequenceLength) {
    return validateSequenceLength(sequenceLength);
}

bool TaaJitterLayout::ndcOffsetsMatch(const fuse::math::Vec2& a, const fuse::math::Vec2& b, f32 epsilon) {
    return std::fabs(a.x - b.x) <= epsilon && std::fabs(a.y - b.y) <= epsilon;
}

bool TaaJitterLayout::canSyncToFrameIndex(u32 /*frameIndex*/, u32 sequenceLength) {
    return validateSequenceLength(sequenceLength);
}

bool TaaJitterLayout::canSyncAndProduceNdc(u32 width, u32 height, u32 sequenceLength) {
    return canProduceNdcOffset(width, height, sequenceLength) && canSyncToFrameIndex(0u, sequenceLength);
}

bool TaaJitterLayout::jitterSlotMatchesFrameIndex(u32 frameIndex, u32 slot, u32 sequenceLength) {
    const u32 period = sequencePeriod(sequenceLength);
    if (period == 0u) {
        return false;
    return frameIndexInSequence(frameIndex, sequenceLength) == slot;

u32 TaaJitterLayout::sequencePeriod(u32 sequenceLength) {
    return validateSequenceLength(sequenceLength) ? sequenceLength : 0u;
}

u32 TaaJitterLayout::frameIndexInSequence(u32 frameIndex, u32 sequenceLength) {
    const u32 period = sequencePeriod(sequenceLength);
    if (period == 0u) {
        return 0u;
    }
    return frameIndex % period;
}

bool TaaJitterLayout::slotMatchesFrameIndex(u32 slot, u32 frameIndex, u32 sequenceLength) {
    if (!validateSequenceLength(sequenceLength)) {
        return false;
    }
    return slot == frameIndexInSequence(frameIndex, sequenceLength);
u32 TaaJitterLayout::expectedSlotForMonotonicFrame(u32 monotonicFrame, u32 sequenceLength) {
    return frameIndexInSequence(monotonicFrame, sequenceLength);

bool TaaJitterLayout::monotonicFrameMatchesSlot(u32 monotonicFrame, u32 slot, u32 sequenceLength) {
    return expectedSlotForMonotonicFrame(monotonicFrame, sequenceLength) == slot;
bool TaaJitterLayout::jitterIndexMatchesFrameIndex(u32 frameIndex, u32 index, u32 sequenceLength) {
    return frameIndexInSequence(frameIndex, sequenceLength) == index;
}

fuse::math::Vec2 TaaJitterLayout::haltonPixelOffset(u32 index, u32 sequenceLength) {
    const u32 safeLength = validateSequenceLength(sequenceLength) ? sequenceLength : kTaaDefaultJitterSequenceLength;
    const u32 slot = index % safeLength;
    return {halton(slot + 1u, 2u), halton(slot + 1u, 3u)};
}

fuse::math::Vec2 TaaJitterLayout::haltonNdcOffset(u32 index, u32 width, u32 height, u32 sequenceLength) {
    const fuse::math::Vec2 pixel = haltonPixelOffset(index, sequenceLength);
    const f32 safeWidth = width > 0u ? static_cast<f32>(width) : 1.f;
    const f32 safeHeight = height > 0u ? static_cast<f32>(height) : 1.f;
    return {(pixel.x - 0.5f) * 2.f / safeWidth, (pixel.y - 0.5f) * 2.f / safeHeight};
}

fuse::math::Vec2 TaaJitterLayout::safeHaltonNdcOffset(u32 index, u32 width, u32 height, u32 sequenceLength) {
    if (!validateViewportDimensions(width, height)) {
        return {};
    }
    return haltonNdcOffset(index, width, height, sequenceLength);
}

fuse::math::Vec2 TaaJitterLayout::offsetForFrameIndex(u32 frameIndex, u32 sequenceLength) {
    const u32 slot = frameIndexInSequence(frameIndex, sequenceLength);
    return haltonPixelOffset(slot, sequenceLength);
}

bool TaaJitterLayout::offsetForFrameIndexIfReady(u32 frameIndex, u32 sequenceLength, fuse::math::Vec2& out) {
    if (!validateSequenceLength(sequenceLength)) {
        return false;
    }
    out = offsetForFrameIndex(frameIndex, sequenceLength);
    return true;
}

fuse::math::Vec2 TaaJitterLayout::ndcOffsetForFrameIndex(u32 frameIndex, u32 width, u32 height,
                                                         u32 sequenceLength) {
    const u32 slot = frameIndexInSequence(frameIndex, sequenceLength);
    return haltonNdcOffset(slot, width, height, sequenceLength);
}

bool TaaJitterLayout::ndcOffsetForFrameIndexIfReady(u32 frameIndex, u32 width, u32 height, u32 sequenceLength,
                                                    fuse::math::Vec2& out) {
    if (!canProduceNdcOffset(width, height, sequenceLength)) {
        return false;
    }
    out = ndcOffsetForFrameIndex(frameIndex, width, height, sequenceLength);
    return true;
fuse::math::Vec2 TaaJitterLayout::safeNdcOffsetForFrameIndex(u32 frameIndex, u32 width, u32 height,
                                                             u32 sequenceLength) {
    if (!validateViewportDimensions(width, height)) {
        return {};
    if (!canComputeNdcOffset(width, height)) {
    }
    return ndcOffsetForFrameIndex(frameIndex, width, height, sequenceLength);
}

bool TaaJitterLayout::monotonicFrameMatchesSlot(u32 frameIndex, u32 slot, u32 sequenceLength) {
    return slot == frameIndexInSequence(frameIndex, sequenceLength);
bool TaaJitterLayout::jitterSyncMatches(u32 observedFrameIndex, u32 expectedFrameIndex, u32 sequenceLength) {
    const u32 period = sequencePeriod(sequenceLength);
    if (period == 0u) {
        return false;
    }
    return frameIndexInSequence(observedFrameIndex, sequenceLength) ==
           frameIndexInSequence(expectedFrameIndex, sequenceLength);
bool TaaJitterLayout::slotMatchesMonotonicFrame(u32 slot, u32 frameIndex, u32 sequenceLength) {
}

bool TaaJitterLayout::fillHaltonSequence(u32 length, fuse::math::Vec2* out) {
    if (out == nullptr || !validateSequenceLength(length)) {
        return false;
    }

    for (u32 i = 0u; i < length; ++i) {
        out[i] = haltonPixelOffset(i, length);
    }
    return true;
}

bool TaaJitterLayout::monotonicFrameMatchesSlot(u32 monotonicFrame, u32 slot, u32 sequenceLength) {
    const u32 expectedSlot = frameIndexInSequence(monotonicFrame, sequenceLength);
    return jitterIndexInRange(slot, sequenceLength) && slot == expectedSlot;
}

TaaJitterSyncStatus classifyTaaJitterSync(u32 monotonicFrame, u32 slot, u32 expectedFrameIndex,
                                          u32 sequenceLength) {
    if (monotonicFrame != expectedFrameIndex) {
        return TaaJitterSyncStatus::MonotonicMismatch;
    }
    if (!TaaJitterLayout::monotonicFrameMatchesSlot(monotonicFrame, slot, sequenceLength)) {
        return TaaJitterSyncStatus::SlotMismatch;
    }
    return TaaJitterSyncStatus::Synced;
}

TaaJitter::TaaJitter(const TaaJitterDesc& desc)
    : m_sequenceLength(TaaJitterLayout::validateSequenceLength(desc.sequence_length) ? desc.sequence_length
                                                                                     : kTaaDefaultJitterSequenceLength) {}

fuse::math::Vec2 TaaJitter::haltonPixelOffset(u32 index) {
    return TaaJitterLayout::haltonPixelOffset(index, kTaaDefaultJitterSequenceLength);
}

fuse::math::Vec2 TaaJitter::haltonNdcOffset(u32 index, u32 width, u32 height) {
    return TaaJitterLayout::haltonNdcOffset(index, width, height, kTaaDefaultJitterSequenceLength);
}

fuse::math::Vec2 TaaJitter::currentPixelOffset() const {
    return TaaJitterLayout::haltonPixelOffset(m_index, m_sequenceLength);
}

fuse::math::Vec2 TaaJitter::currentNdcOffset(u32 width, u32 height) const {
    if (!canProduceNdcOffset(width, height)) {
        return {};
    }
    return TaaJitterLayout::haltonNdcOffset(m_index, width, height, m_sequenceLength);
}

bool TaaJitter::currentNdcOffsetIfReady(u32 width, u32 height, fuse::math::Vec2& out) const {
    if (!canProduceNdcOffset(width, height)) {
        return false;
    }
    out = TaaJitterLayout::haltonNdcOffset(m_index, width, height, m_sequenceLength);
    return true;

bool TaaJitter::canAdvance() const {
    return TaaJitterLayout::validateSequenceLength(m_sequenceLength);

bool TaaJitter::canProduceNdcOffset(u32 width, u32 height) const {
    return TaaJitterLayout::canProduceNdcOffset(width, height, m_sequenceLength);

bool TaaJitter::canSyncToFrameIndex(u32 frameIndex) const {
    return TaaJitterLayout::canSyncToFrameIndex(frameIndex, m_sequenceLength);

TaaJitterSyncBlockReason TaaJitter::classifySyncBlock(u32 width, u32 height) const {
    return classifyTaaJitterSyncBlock(width, height, m_sequenceLength);
}

bool TaaJitter::preflightSync(u32 frameIndex, u32 width, u32 height, TaaJitterSyncBlockReason* reason) const {
    if (!canSyncToFrameIndex(frameIndex)) {
        if (reason != nullptr) {
            *reason = TaaJitterSyncBlockReason::InvalidSequence;
        }
        return false;
    }
    return preflightTaaJitterSync(frameIndex, width, height, m_sequenceLength, reason);
}

bool TaaJitter::isAlignedToFrameIndex(u32 frameIndex) const {
    return m_monotonicFrame == frameIndex &&
           TaaJitterLayout::jitterSlotMatchesFrameIndex(frameIndex, m_index, m_sequenceLength);

bool TaaJitter::syncToFrameIndexIfReady(u32 frameIndex) {
    if (!canSyncToFrameIndex(frameIndex)) {
    syncToFrameIndex(frameIndex);

bool TaaJitter::advanceIfReady() {
    if (!canAdvance()) {
    advance();
bool TaaJitter::canProvideNdcOffset(u32 width, u32 height) const {
    return TaaJitterLayout::canComputeNdcOffset(width, height);
}

bool TaaJitter::isSyncedToFrameIndex(u32 frameIndex) const {
    return m_monotonicFrame == frameIndex &&
           m_index == TaaJitterLayout::frameIndexInSequence(frameIndex, m_sequenceLength);
}

bool TaaJitter::isSyncedToFrameIndex(u32 frameIndex) const {
    return syncStatusForFrameIndex(frameIndex) == TaaJitterSyncStatus::Synced;
}

TaaJitterSyncStatus TaaJitter::syncStatusForFrameIndex(u32 frameIndex) const {
    return classifyTaaJitterSync(m_monotonicFrame, m_index, frameIndex, m_sequenceLength);
}

bool TaaJitter::isSyncedToFrameIndex(u32 frameIndex) const {
    return m_monotonicFrame == frameIndex;
}

u32 TaaJitter::expectedSlotForFrameIndex(u32 frameIndex) const {
    return TaaJitterLayout::frameIndexInSequence(frameIndex, m_sequenceLength);
}

bool TaaJitter::slotMatchesFrameIndex(u32 frameIndex) const {
    return isSyncedToFrameIndex(frameIndex) && m_index == expectedSlotForFrameIndex(frameIndex);
}

bool TaaJitter::isSyncedToFrameIndex(u32 frameIndex) const {
    return m_monotonicFrame == frameIndex;
}

bool TaaJitter::needsSyncToFrameIndex(u32 frameIndex) const {
    return !isSyncedToFrameIndex(frameIndex);
}

bool TaaJitter::slotMatchesFrame(u32 frameIndex) const {
    return TaaJitterLayout::jitterIndexMatchesFrame(m_index, frameIndex, m_sequenceLength);
}

bool TaaJitter::canSyncToFrameIndex() const {
    return TaaJitterLayout::validateSequenceLength(m_sequenceLength);
}

bool TaaJitter::isSyncedToFrameIndex(u32 frameIndex) const {
    return m_monotonicFrame == frameIndex &&
           TaaJitterLayout::slotMatchesFrameIndex(m_index, frameIndex, m_sequenceLength);
}

bool taaJitterFrameSynced(const TaaJitter& jitter, u32 frameIndex) {
    return jitter.isSyncedToFrameIndex(frameIndex);
}

bool TaaJitter::canSyncToFrameIndex(u32 frameIndex) const {
    return TaaJitterLayout::canSyncToFrameIndex(frameIndex, m_sequenceLength);
}

bool TaaJitter::advanceIfReady() {
    if (!canAdvance()) {
        return false;
    }
    advance();
    return true;
}

void TaaJitter::advance() {
    ++m_monotonicFrame;
    m_index = TaaJitterLayout::frameIndexInSequence(m_monotonicFrame, m_sequenceLength);
}

bool TaaJitter::advanceIfPossible() {
    if (!canAdvance()) {
        return false;
    }
    advance();
    return true;
}

bool TaaJitter::isSyncedToFrameIndex(u32 frameIndex) const {
    return m_monotonicFrame == frameIndex &&
           m_index == TaaJitterLayout::frameIndexInSequence(frameIndex, m_sequenceLength);
}

void TaaJitter::reset() {
    m_index = 0u;
    m_monotonicFrame = 0u;
}

void TaaJitter::syncToFrameIndex(u32 frameIndex) {
    m_monotonicFrame = frameIndex;
    m_index = TaaJitterLayout::frameIndexInSequence(frameIndex, m_sequenceLength);
}

bool TaaJitter::isSyncedToFrameIndex(u32 frameIndex) const {
    return m_monotonicFrame == frameIndex &&
           m_index == TaaJitterLayout::frameIndexInSequence(frameIndex, m_sequenceLength);
}

bool TaaJitterSyncPreflight::synced() const {
    return sequence_valid && monotonic_matches && slot_matches;

TaaJitterSyncPreflight preflightTaaJitterSync(const TaaJitter& jitter, u32 frameIndex, u32 width, u32 height) {
    TaaJitterSyncPreflight preflight{};
    preflight.frame_index = frameIndex;
    preflight.sequence_valid = jitter.canAdvance();
    preflight.viewport_valid = TaaJitterLayout::validateViewportDimensions(width, height);
    preflight.can_produce_ndc = jitter.canProduceNdcOffset(width, height);
    preflight.expected_slot = TaaJitterLayout::frameIndexInSequence(frameIndex, jitter.sequenceLength());
    preflight.actual_slot = jitter.index();
    preflight.slot_matches = preflight.actual_slot == preflight.expected_slot;
    preflight.monotonic_matches = jitter.monotonicFrameIndex() == frameIndex;
    return preflight;
    return m_monotonicFrame == frameIndex;

bool TaaJitter::slotMatchesMonotonicFrame() const {
    return TaaJitterLayout::monotonicFrameMatchesSlot(m_monotonicFrame, m_index, m_sequenceLength);
const char* taaJitterSyncRejectReasonLabel(TaaJitterSyncRejectReason reason) {
    switch (reason) {
    case TaaJitterSyncRejectReason::None:
        return "none";
    case TaaJitterSyncRejectReason::InvalidSequence:
        return "invalid_sequence";
    case TaaJitterSyncRejectReason::InvalidViewport:
        return "invalid_viewport";
    case TaaJitterSyncRejectReason::FrameIndexMismatch:
        return "frame_index_mismatch";
    return "unknown";

bool taaJitterSlotMatchesFrame(u32 frameIndex, u32 slot, u32 sequenceLength) {
    return TaaJitterLayout::frameIndexInSequence(frameIndex, sequenceLength) == slot;

bool preflightTaaJitterSync(const TaaJitter& jitter, u32 frameIndex, u32 width, u32 height,
                            TaaJitterSyncRejectReason* reason) {
    if (!jitter.canAdvance()) {
        if (reason != nullptr) {
            *reason = TaaJitterSyncRejectReason::InvalidSequence;
        return false;
    if (!jitter.canProduceNdcOffset(width, height)) {
            *reason = TaaJitterSyncRejectReason::InvalidViewport;
    if (!jitter.isSyncedToFrameIndex(frameIndex)) {
            *reason = TaaJitterSyncRejectReason::FrameIndexMismatch;
        *reason = TaaJitterSyncRejectReason::None;
    return true;
    if (m_monotonicFrame != frameIndex) {
    return m_index == TaaJitterLayout::frameIndexInSequence(frameIndex, m_sequenceLength);

TaaJitterSyncRejectReason classifyTaaJitterSyncReject(const TaaJitter& jitter, u32 frameIndex, u32 width,
                                                        u32 height) {
    if (!TaaJitterLayout::validateSequenceLength(jitter.sequenceLength())) {
        return TaaJitterSyncRejectReason::InvalidSequenceLength;
    if (!TaaJitterLayout::validateViewportDimensions(width, height)) {
        return TaaJitterSyncRejectReason::InvalidViewport;
    if (jitter.monotonicFrameIndex() != frameIndex) {
        return TaaJitterSyncRejectReason::FrameIndexMismatch;
        return TaaJitterSyncRejectReason::SlotIndexMismatch;
    return TaaJitterSyncRejectReason::None;

bool taaJitterSyncPreflight(const TaaJitter& jitter, u32 frameIndex, u32 width, u32 height,
    const TaaJitterSyncRejectReason reject = classifyTaaJitterSyncReject(jitter, frameIndex, width, height);
        *reason = reject;
    return reject == TaaJitterSyncRejectReason::None;
           TaaJitterLayout::slotMatchesMonotonicFrame(m_index, frameIndex, m_sequenceLength);
bool TaaJitter::syncToFrameIndexIfReady(u32 frameIndex) {
    if (!canSyncToFrameIndex(frameIndex)) {
    syncToFrameIndex(frameIndex);

           TaaJitterLayout::monotonicFrameMatchesSlot(frameIndex, m_index, m_sequenceLength);

           TaaJitterLayout::jitterIndexMatchesFrameIndex(frameIndex, m_index, m_sequenceLength);
}

} // namespace fuse::renderer
