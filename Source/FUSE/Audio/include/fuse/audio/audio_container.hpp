#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::audio {

/// Cooked audio container (".fuseaudio", UNI-U7-AUDIO-1 / AP-W8.3).
///
/// Layout: an ASCII header of '\n'-terminated lines, then the payload bytes:
///
///     FUSEAUDIO_OGG | FUSEAUDIO_PCM_F32 | FUSEAUDIO_FLAC      (magic line = payload kind)
///     rate=<Hz>
///     channels=<n>
///     samples=<frames per channel>     (frames= is accepted as a synonym)
///     loop_start=<frame>               (optional)
///     loop_end=<frame>                 (optional, exclusive; 0 / absent = clip end)
///     <other key=value lines are ignored: hook=, format=, encoder=, wav=, ...>
///     DATA
///     <payload: Ogg Vorbis stream | interleaved little-endian f32 | native FLAC stream>
///
/// FUSEAUDIO_OGG is what Tools/FUSE/Cook writes today (cook_stub_writer.cpp); FUSEAUDIO_STUB
/// (placeholder when no encoder was linked) is rejected by the parser.
enum class FuseAudioPayload : u8 { Ogg, PcmF32, Flac };

struct FuseAudioContainer {
    FuseAudioPayload payload = FuseAudioPayload::Ogg;
    u32 sample_rate = 0;
    u32 channels = 0;
    u32 frames = 0;
    u32 loop_start = 0;
    u32 loop_end = 0;
    const u8* data = nullptr; ///< Points into the parsed buffer.
    usize size = 0;
};

/// True when `bytes` starts with a FUSEAUDIO_ magic line.
bool is_fuseaudio(const u8* bytes, usize size);

/// Parse the header; `out.data/size` reference the payload inside `bytes`. Returns false for
/// stubs, unknown kinds, missing DATA line, or a PCM_F32 payload whose size disagrees with the
/// header.
bool parse_fuseaudio(const u8* bytes, usize size, FuseAudioContainer& out);

/// Build a container (header + payload).
std::vector<u8> write_fuseaudio(FuseAudioPayload payload, u32 sample_rate, u32 channels, u32 frames,
                                const u8* data, usize size, u32 loop_start = 0, u32 loop_end = 0);

} // namespace fuse::audio
