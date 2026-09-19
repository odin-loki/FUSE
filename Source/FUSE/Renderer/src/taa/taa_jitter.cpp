#include <fuse/renderer/taa/taa_jitter.hpp>

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
}

} // namespace fuse::renderer
