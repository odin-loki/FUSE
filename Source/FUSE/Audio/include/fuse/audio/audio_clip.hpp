#pragma once

#include <fuse/audio/audio_stream.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <vector>

namespace fuse::audio {

struct AudioClipLoadOptions {
    /// Ogg Vorbis clips longer than this (seconds) stay encoded and stream (see AudioStreamData);
    /// <= 0 decodes everything to PCM at load.
    float stream_threshold_seconds = 0.f;
};

/// Interleaved PCM clip (mono or stereo, f32 samples), or a streaming clip (encoded Ogg Vorbis
/// in `stream`, `samples` empty) that voices decode on the fly.
struct AudioClip {
    std::vector<float> samples;
    u32 sample_rate = 0;
    u32 channel_count = 0;
    float duration = 0.f;

    /// Non-null for streaming clips.
    std::shared_ptr<const AudioStreamData> stream;

    /// Loop region used by looping voices, in frames: [loop_start, loop_end). loop_end == 0 means
    /// the clip end. The defaults loop the whole clip.
    u32 loop_start = 0;
    u32 loop_end = 0;

    bool load_wav(const char* path);
    bool load_wav_memory(const u8* data, usize size);
    bool load_from_pcm(const float* interleaved, u32 frame_count, u32 channels, u32 rate);
    /// Ogg Vorbis from memory: decoded to PCM, or kept encoded for streaming when `stream`.
    bool load_ogg(const u8* data, usize size, bool stream = false);
    /// Native FLAC from memory (decoded to PCM).
    bool load_flac(const u8* data, usize size);
    /// Cooked `.fuseaudio` container (FUSEAUDIO_OGG / FUSEAUDIO_PCM_F32 / FUSEAUDIO_FLAC).
    bool load_fuseaudio(const u8* data, usize size, const AudioClipLoadOptions& options = {});
    /// Sniff the format (RIFF/WAVE, OggS, fLaC, FUSEAUDIO_) and load.
    bool load_memory(const u8* data, usize size, const AudioClipLoadOptions& options = {});
    /// Read `path` through the VFS when a mount resolves it (fuse::io::VirtualFileSystem), else
    /// from the filesystem, then load_memory().
    bool load_file(const char* path, const AudioClipLoadOptions& options = {});

    u32 frame_count() const {
        if (stream) {
            return stream->frames;
        }
        if (channel_count == 0) {
            return 0;
        }
        return static_cast<u32>(samples.size()) / channel_count;
    }

    bool empty() const { return samples.empty() && !stream; }
    bool is_streaming() const { return stream != nullptr; }

    u32 loop_end_frame() const {
        const u32 frames = frame_count();
        return (loop_end == 0 || loop_end > frames) ? frames : loop_end;
    }
    u32 loop_start_frame() const {
        const u32 end = loop_end_frame();
        return loop_start < end ? loop_start : 0;
    }
};

} // namespace fuse::audio
