#pragma once

#include <fuse/types.hpp>

#include <memory>
#include <vector>

namespace fuse::audio {

/// Encoded clip kept in memory for streaming playback (MP-B7.2-OGG-RUNTIME). Shared by every
/// voice that plays the clip; each voice decodes it independently through a StreamDecoder.
struct AudioStreamData {
    std::vector<u8> encoded; ///< Complete Ogg Vorbis stream.
    u32 frames = 0;
    u32 channels = 0;
    u32 sample_rate = 0;
};

/// Counters for one streaming voice.
struct StreamDecoderStats {
    u64 decoded_frames = 0;
    u64 seeks = 0;
    u64 underruns = 0; ///< Frames requested that could not be decoded (served as silence).
};

/// Per-voice streaming decoder with bounded memory.
///
/// Holds two fixed buffers allocated at open(): a ring "window" of kWindowFrames frames that
/// slides forward over the clip as playback advances (refilled kLookaheadFrames ahead of the read
/// position), and a "head" cache of the first kHeadFrames frames of the loop region, so a looping
/// voice can wrap from loop_end back to loop_start inside one mix block without stalling. A
/// request outside both (a seek, or the continuation after the head on the next loop pass)
/// re-positions the decoder with ov_pcm_seek. Decoding runs on the thread that mixes (the audio
/// thread of the engine); nothing allocates after open() once libvorbis is warm.
class StreamDecoder {
public:
    static constexpr u32 kWindowFrames = 16384;
    static constexpr u32 kHeadFrames = 8192;
    static constexpr u32 kLookaheadFrames = 4096;

    StreamDecoder();
    ~StreamDecoder();
    StreamDecoder(const StreamDecoder&) = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;

    /// Open `data` for playback with the loop region starting at `loop_start` (frames) and ending
    /// at `loop_end` (exclusive; 0 = clip end). Returns false when the stream cannot be decoded.
    bool open(std::shared_ptr<const AudioStreamData> data, u32 loop_start, u32 loop_end);
    void close();
    bool is_open() const;

    /// Sample `channel` of clip frame `frame` (decoding / seeking as needed). Frames past the end
    /// of the clip, or that could not be decoded, read as 0.
    float sample(u32 frame, u32 channel);

    const StreamDecoderStats& stats() const { return m_stats; }
    u32 channels() const { return m_channels; }

private:
    struct Impl;
    bool refill(u32 frame);
    bool seek_to(u32 frame);
    u32 decode_into(float* dst, u32 max_frames, u32 dst_capacity_frames, u32 ring_start);

    std::unique_ptr<Impl> m_impl;
    std::shared_ptr<const AudioStreamData> m_data;
    u32 m_channels = 0;
    u32 m_frames = 0;

    std::vector<float> m_window; ///< kWindowFrames * channels, ring indexed by frame % kWindowFrames
    u32 m_windowStart = 0;       ///< First clip frame held in the window.
    u32 m_windowCount = 0;       ///< Frames held (contiguous from m_windowStart).
    u32 m_decodePos = 0;         ///< Clip frame the decoder produces next (== start + count).
    bool m_eof = false;

    std::vector<float> m_head;   ///< Loop-region head cache, kHeadFrames * channels
    u32 m_headStart = 0;
    u32 m_headCount = 0;

    StreamDecoderStats m_stats{};
};

} // namespace fuse::audio
