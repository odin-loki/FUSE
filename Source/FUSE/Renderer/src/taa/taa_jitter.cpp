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
}

bool TaaJitterLayout::canSyncToFrameIndex(u32 /*frameIndex*/, u32 sequenceLength) {
    return validateSequenceLength(sequenceLength);
}

bool TaaJitterLayout::jitterSlotMatchesFrameIndex(u32 frameIndex, u32 slot, u32 sequenceLength) {
    const u32 period = sequencePeriod(sequenceLength);
    if (period == 0u) {
        return false;
    }
    return frameIndexInSequence(frameIndex, sequenceLength) == slot;
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
}

bool TaaJitter::canAdvance() const {
    return TaaJitterLayout::validateSequenceLength(m_sequenceLength);
}

bool TaaJitter::canProduceNdcOffset(u32 width, u32 height) const {
    return TaaJitterLayout::canProduceNdcOffset(width, height, m_sequenceLength);
}

bool TaaJitter::canSyncToFrameIndex(u32 frameIndex) const {
    return TaaJitterLayout::canSyncToFrameIndex(frameIndex, m_sequenceLength);
}

bool TaaJitter::isAlignedToFrameIndex(u32 frameIndex) const {
    return m_monotonicFrame == frameIndex &&
           TaaJitterLayout::jitterSlotMatchesFrameIndex(frameIndex, m_index, m_sequenceLength);
}

bool TaaJitter::syncToFrameIndexIfReady(u32 frameIndex) {
    if (!canSyncToFrameIndex(frameIndex)) {
        return false;
    }
    syncToFrameIndex(frameIndex);
    return true;
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

void TaaJitter::reset() {
    m_index = 0u;
    m_monotonicFrame = 0u;
}

void TaaJitter::syncToFrameIndex(u32 frameIndex) {
    m_monotonicFrame = frameIndex;
    m_index = TaaJitterLayout::frameIndexInSequence(frameIndex, m_sequenceLength);
}

} // namespace fuse::renderer

// --- deepen additive from deepen-b59-taa-guards-8293 ---
bool TaaJitterSyncPreflight::synced() const {
TaaJitterSyncPreflight preflightTaaJitterSync(const TaaJitter& jitter, u32 frameIndex, u32 width, u32 height) {
    TaaJitterSyncPreflight preflight{};

// --- deepen additive from deepen-b59-taa-guards-94f6 ---
const char* taaJitterSyncRejectReasonLabel(TaaJitterSyncRejectReason reason) {
    case TaaJitterSyncRejectReason::None:
    case TaaJitterSyncRejectReason::InvalidSequence:
    case TaaJitterSyncRejectReason::InvalidViewport:
    case TaaJitterSyncRejectReason::FrameIndexMismatch:
bool preflightTaaJitterSync(const TaaJitter& jitter, u32 frameIndex, u32 width, u32 height,
                            TaaJitterSyncRejectReason* reason) {
            *reason = TaaJitterSyncRejectReason::InvalidSequence;
            *reason = TaaJitterSyncRejectReason::InvalidViewport;
            *reason = TaaJitterSyncRejectReason::FrameIndexMismatch;
        *reason = TaaJitterSyncRejectReason::None;

// --- deepen additive from deepen-b59-taa-guards-5b8c ---
TaaJitterSyncRejectReason classifyTaaJitterSyncReject(const TaaJitter& jitter, u32 frameIndex, u32 width,
        return TaaJitterSyncRejectReason::InvalidSequenceLength;
        return TaaJitterSyncRejectReason::InvalidViewport;
        return TaaJitterSyncRejectReason::FrameIndexMismatch;
        return TaaJitterSyncRejectReason::SlotIndexMismatch;
    return TaaJitterSyncRejectReason::None;
bool taaJitterSyncPreflight(const TaaJitter& jitter, u32 frameIndex, u32 width, u32 height,
    const TaaJitterSyncRejectReason reject = classifyTaaJitterSyncReject(jitter, frameIndex, width, height);
    return reject == TaaJitterSyncRejectReason::None;

// --- deepen additive from deepen-b59-taa-guards-27d7 ---
bool TaaJitter::preflightSync(u32 frameIndex, u32 width, u32 height, TaaJitterSyncBlockReason* reason) const {
    return preflightTaaJitterSync(frameIndex, width, height, m_sequenceLength, reason);

// --- deepen additive from deepen-b59-taa-guards-117f ---
bool preflightTaaJitterSync(const TaaJitter& jitter, u32 frameIndex, TaaJitterSyncBlockReason* reason) {

// --- deepen additive from deepen-fuse-b59-taa-cd32 ---
TaaJitterSyncRejectReason classifyTaaJitterSyncReject(u32 /*frameIndex*/, u32 sequenceLength) {
        return TaaJitterSyncRejectReason::InvalidSequence;
bool preflightTaaJitterSync(u32 frameIndex, u32 sequenceLength, TaaJitterSyncRejectReason* reason) {
    const TaaJitterSyncRejectReason reject = classifyTaaJitterSyncReject(frameIndex, sequenceLength);
bool canPreflightTaaJitterSync(u32 frameIndex, u32 sequenceLength) {
    return preflightTaaJitterSync(frameIndex, sequenceLength);
const char* taaJitterNdcRejectReasonLabel(TaaJitterNdcRejectReason reason) {
    case TaaJitterNdcRejectReason::None:
    case TaaJitterNdcRejectReason::InvalidSequence:
    case TaaJitterNdcRejectReason::InvalidViewport:
TaaJitterNdcRejectReason classifyTaaJitterNdcReject(u32 width, u32 height, u32 sequenceLength) {
        return TaaJitterNdcRejectReason::InvalidSequence;
        return TaaJitterNdcRejectReason::InvalidViewport;
    return TaaJitterNdcRejectReason::None;
bool preflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength, TaaJitterNdcRejectReason* reason) {
    const TaaJitterNdcRejectReason reject = classifyTaaJitterNdcReject(width, height, sequenceLength);
    return reject == TaaJitterNdcRejectReason::None;
bool canPreflightTaaJitterNdc(u32 width, u32 height, u32 sequenceLength) {
    return preflightTaaJitterNdc(width, height, sequenceLength);
TaaJitterSyncRejectReason TaaJitter::classifySyncReject(u32 frameIndex) const {
    return classifyTaaJitterSyncReject(frameIndex, m_sequenceLength);
bool TaaJitter::preflightSyncToFrameIndex(u32 frameIndex, TaaJitterSyncRejectReason* reason) const {
    return preflightTaaJitterSync(frameIndex, m_sequenceLength, reason);
TaaJitterNdcRejectReason TaaJitter::classifyNdcReject(u32 width, u32 height) const {
    return classifyTaaJitterNdcReject(width, height, m_sequenceLength);
bool TaaJitter::preflightCurrentNdcOffset(u32 width, u32 height, TaaJitterNdcRejectReason* reason) const {
    return preflightTaaJitterNdc(width, height, m_sequenceLength, reason);

// --- deepen additive from deepen-b59-taa-guards-1d2e ---
bool taaJitterSyncBlockReasonIsBlocking(TaaJitterSyncBlockReason reason) {
bool preflightTaaJitterSync(u32 frameIndex, u32 sequenceLength, TaaJitterSyncBlockReason* reason) {
bool TaaJitter::preflightSync(u32 frameIndex, TaaJitterSyncBlockReason* reason) const {
