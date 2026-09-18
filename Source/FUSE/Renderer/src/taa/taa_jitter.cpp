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
}

bool TaaJitterLayout::canProduceNdcOffset(u32 width, u32 height, u32 sequenceLength) {
    return validateViewportDimensions(width, height) && validateSequenceLength(sequenceLength);
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

bool TaaJitter::isSyncedToFrameIndex(u32 frameIndex) const {
    return m_monotonicFrame == frameIndex;
}

u32 TaaJitter::expectedSlotForFrameIndex(u32 frameIndex) const {
    return TaaJitterLayout::frameIndexInSequence(frameIndex, m_sequenceLength);
}

bool TaaJitter::slotMatchesFrameIndex(u32 frameIndex) const {
    return isSyncedToFrameIndex(frameIndex) && m_index == expectedSlotForFrameIndex(frameIndex);
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
