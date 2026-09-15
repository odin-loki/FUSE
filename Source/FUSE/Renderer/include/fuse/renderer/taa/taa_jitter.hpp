#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/taa/taa_types.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

static constexpr u32 kTaaDefaultJitterSequenceLength = 8;
static constexpr u32 kTaaMaxJitterSequenceLength = 64;

/// Halton (2,3) sequence helpers — CPU reference for projection jitter (B5.9 deepen).
struct TaaJitterLayout {
    static f32 halton(u32 index, u32 base);
    static bool validateSequenceLength(u32 length);
    /// Returns the jitter cycle length after validation (0 when invalid).
    static u32 sequencePeriod(u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Maps a monotonic frame counter into the active Halton slot.
    static u32 frameIndexInSequence(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    static fuse::math::Vec2 haltonPixelOffset(u32 index, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    static fuse::math::Vec2 haltonNdcOffset(u32 index, u32 width, u32 height,
                                            u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    /// Halton offset for a monotonic frame counter (wraps via `frameIndexInSequence`).
    static fuse::math::Vec2 offsetForFrameIndex(u32 frameIndex, u32 sequenceLength = kTaaDefaultJitterSequenceLength);
    static void fillHaltonSequence(u32 length, fuse::math::Vec2* out);
};

/// Sub-pixel jitter state — advances through a Halton sequence each frame (B5.9).
class TaaJitter {
public:
    explicit TaaJitter(const TaaJitterDesc& desc = {});

    fuse::math::Vec2 currentPixelOffset() const;
    fuse::math::Vec2 currentNdcOffset(u32 width, u32 height) const;

    void advance();
    void reset();

    u32 index() const { return m_index; }
    u32 sequenceLength() const { return m_sequenceLength; }

    static fuse::math::Vec2 haltonPixelOffset(u32 index);
    static fuse::math::Vec2 haltonNdcOffset(u32 index, u32 width, u32 height);

private:
    u32 m_sequenceLength = kTaaDefaultJitterSequenceLength;
    u32 m_index = 0;
};

} // namespace fuse::renderer
