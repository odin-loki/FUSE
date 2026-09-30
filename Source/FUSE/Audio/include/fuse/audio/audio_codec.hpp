#pragma once

#include <fuse/types.hpp>

#include <vector>

namespace fuse::audio {

/// Whole-clip decode result: interleaved f32 in [-1, 1].
struct DecodedAudio {
    std::vector<float> samples;
    u32 frames = 0;
    u32 channels = 0;
    u32 sample_rate = 0;
};

/// Codec entry points backed by the vendored xiph libraries (cmake/FuseXiph.cmake). When a codec
/// was not built (FUSE_HAS_OGG_VORBIS / FUSE_HAS_FLAC undefined) the functions return false.
bool ogg_vorbis_available();
bool flac_available();

/// Library version strings ("Xiph.Org libVorbis 1.3.7", "1.5.0"), or "" when not built.
const char* vorbis_library_version();
const char* flac_library_version();

/// Decode a complete Ogg Vorbis stream held in memory (vorbisfile over memory callbacks).
bool decode_ogg_vorbis(const u8* data, usize size, DecodedAudio& out);
/// Read the stream header only: total PCM frames, channels and rate.
bool probe_ogg_vorbis(const u8* data, usize size, u32& frames, u32& channels, u32& sample_rate);
/// VBR-encode interleaved f32 PCM to an Ogg Vorbis stream (quality -0.1 .. 1.0).
bool encode_ogg_vorbis(const float* interleaved, u32 frames, u32 channels, u32 sample_rate, float quality,
                       std::vector<u8>& out);

/// Decode a complete native FLAC stream held in memory.
bool decode_flac(const u8* data, usize size, DecodedAudio& out);
/// Encode interleaved f32 PCM to native FLAC at `bits_per_sample` (16 or 24), compression level 5.
bool encode_flac(const float* interleaved, u32 frames, u32 channels, u32 sample_rate, u32 bits_per_sample,
                 std::vector<u8>& out);

} // namespace fuse::audio
