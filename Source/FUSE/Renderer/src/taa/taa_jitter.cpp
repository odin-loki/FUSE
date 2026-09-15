#include <fuse/renderer/taa/taa_jitter.hpp>

namespace fuse::renderer {
namespace {

constexpr f32 kHaltonX[8] = {0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f, 0.0625f};
constexpr f32 kHaltonY[8] = {0.333f, 0.667f, 0.111f, 0.444f, 0.778f, 0.222f, 0.556f, 0.889f};

} // namespace

TaaJitter::TaaJitter(const TaaJitterDesc& desc)
    : m_sequenceLength(desc.sequence_length > 0u ? desc.sequence_length : 8u) {}

fuse::math::Vec2 TaaJitter::haltonPixelOffset(u32 index) {
    const u32 slot = index % 8u;
    return {kHaltonX[slot], kHaltonY[slot]};
}

fuse::math::Vec2 TaaJitter::haltonNdcOffset(u32 index, u32 width, u32 height) {
    const fuse::math::Vec2 pixel = haltonPixelOffset(index);
    const f32 safeWidth = width > 0u ? static_cast<f32>(width) : 1.f;
    const f32 safeHeight = height > 0u ? static_cast<f32>(height) : 1.f;
    return {(pixel.x - 0.5f) * 2.f / safeWidth, (pixel.y - 0.5f) * 2.f / safeHeight};
}

fuse::math::Vec2 TaaJitter::currentPixelOffset() const {
    return haltonPixelOffset(m_index);
}

fuse::math::Vec2 TaaJitter::currentNdcOffset(u32 width, u32 height) const {
    return haltonNdcOffset(m_index, width, height);
}

void TaaJitter::advance() {
    m_index = (m_index + 1u) % m_sequenceLength;
}

void TaaJitter::reset() {
    m_index = 0u;
}

} // namespace fuse::renderer
