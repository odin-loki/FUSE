#pragma once

namespace fuse::audio {

/// Output speaker layout. Channel count is the interleaved width of one output frame.
/// This does not open a device or choose an output backend.
enum class ChannelLayout {
    Mono,
    Stereo,
    Quad,
    Surround51,
};

/// Interleaved channels in one frame: Mono 1, Stereo 2, Quad 4, Surround51 6.
inline constexpr int channel_count(ChannelLayout layout) {
    switch (layout) {
    case ChannelLayout::Mono:
        return 1;
    case ChannelLayout::Stereo:
        return 2;
    case ChannelLayout::Quad:
        return 4;
    case ChannelLayout::Surround51:
        return 6;
    }
    return 0;
}

/// True only for Surround51, the layout that carries a low-frequency effects channel.
inline constexpr bool layout_has_lfe(ChannelLayout layout) {
    return layout == ChannelLayout::Surround51;
}

} // namespace fuse::audio
