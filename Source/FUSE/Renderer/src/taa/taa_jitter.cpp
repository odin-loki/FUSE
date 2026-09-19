#include <fuse/renderer/taa/taa_jitter.hpp>

namespace fuse::renderer {

const char* taaJitterSyncBlockReasonLabel(TaaJitterSyncBlockReason reason) {
    switch (reason) {
    case TaaJitterSyncBlockReason::None:
        return "none";
    case TaaJitterSyncBlockReason::InvalidSequence:
        return "invalid_sequence";
    case TaaJitterSyncBlockReason::InvalidViewport:
        return "invalid_viewport";
    }
    return "unknown";
}

TaaJitterSyncBlockReason classifyTaaJitterSyncBlock(u32 sequenceLength) {
    if (!TaaJitterLayout::validateSequenceLength(sequenceLength)) {
        return TaaJitterSyncBlockReason::InvalidSequence;
    }
    return TaaJitterSyncBlockReason::None;
}

TaaJitterSyncBlockReason classifyTaaJitterSyncViewportBlock(u32 width, u32 height, u32 sequenceLength) {
    if (!TaaJitterLayout::validateViewportDimensions(width, height)) {
        return TaaJitterSyncBlockReason::InvalidViewport;
    }
    return classifyTaaJitterSyncBlock(sequenceLength);
}

const char* taaJitterAdvanceBlockReasonLabel(TaaJitterAdvanceBlockReason reason) {
    switch (reason) {
    case TaaJitterAdvanceBlockReason::None:
        return "none";
    case TaaJitterAdvanceBlockReason::InvalidSequence:
        return "invalid_sequence";
    case TaaJitterAdvanceBlockReason::InvalidViewport:
        return "invalid_viewport";
    }
    return "unknown";
}

TaaJitterAdvanceBlockReason classifyTaaJitterAdvanceBlock(u32 width, u32 height, u32 sequenceLength) {
    if (!TaaJitterLayout::validateSequenceLength(sequenceLength)) {
        return TaaJitterAdvanceBlockReason::InvalidSequence;
    }
    if (!TaaJitterLayout::validateViewportDimensions(width, height)) {
        return TaaJitterAdvanceBlockReason::InvalidViewport;
    }
    return TaaJitterAdvanceBlockReason::None;
}

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

fuse::math::Vec2 TaaJitterLayout::ndcOffsetForFrameIndex(u32 frameIndex, u32 width, u32 height,
                                                         u32 sequenceLength) {
    const u32 slot = frameIndexInSequence(frameIndex, sequenceLength);
    return haltonNdcOffset(slot, width, height, sequenceLength);
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

bool TaaJitter::syncToFrameIndexIfViewportReady(u32 frameIndex, u32 width, u32 height) {
    if (classifyTaaJitterSyncViewportBlock(width, height, m_sequenceLength) != TaaJitterSyncBlockReason::None) {
        return false;
    }
    syncToFrameIndex(frameIndex);
    return true;
}

bool TaaJitter::trySyncToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncBlockReason& outReason) {
    outReason = classifyTaaJitterSyncBlock(m_sequenceLength);
    if (outReason != TaaJitterSyncBlockReason::None) {
        return false;
    }
    syncToFrameIndex(frameIndex);
    return true;
}

bool TaaJitter::trySyncToFrameIndexIfViewportReady(u32 frameIndex, u32 width, u32 height,
                                                   TaaJitterSyncBlockReason& outReason) {
    outReason = classifyTaaJitterSyncViewportBlock(width, height, m_sequenceLength);
    if (outReason != TaaJitterSyncBlockReason::None) {
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

bool TaaJitter::advanceIfViewportReady(u32 width, u32 height) {
    if (classifyTaaJitterAdvanceBlock(width, height, m_sequenceLength) != TaaJitterAdvanceBlockReason::None) {
        return false;
    }
    advance();
    return true;
}

bool TaaJitter::tryAdvanceIfReady(TaaJitterAdvanceBlockReason& outReason) {
    if (classifyTaaJitterSyncBlock(m_sequenceLength) == TaaJitterSyncBlockReason::InvalidSequence) {
        outReason = TaaJitterAdvanceBlockReason::InvalidSequence;
        return false;
    }
    outReason = TaaJitterAdvanceBlockReason::None;
    advance();
    return true;
}

bool TaaJitter::tryAdvanceIfViewportReady(u32 width, u32 height, TaaJitterAdvanceBlockReason& outReason) {
    outReason = classifyTaaJitterAdvanceBlock(width, height, m_sequenceLength);
    if (outReason != TaaJitterAdvanceBlockReason::None) {
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
