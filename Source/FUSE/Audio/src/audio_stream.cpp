#include <fuse/audio/audio_stream.hpp>

#include "vorbis_memory_io.hpp"

#include <algorithm>

namespace fuse::audio {

namespace {
/// Frames kept behind the read position when the window slides (interpolation reads i0 and i0+1,
/// and a slow voice can revisit the previous frame).
constexpr u32 kKeepBehind = 4;
} // namespace

struct StreamDecoder::Impl {
#if defined(FUSE_HAS_OGG_VORBIS)
    detail::MemoryReader reader{};
    OggVorbis_File vf{};
#endif
    bool opened = false;
};

StreamDecoder::StreamDecoder() : m_impl(std::make_unique<Impl>()) {}

StreamDecoder::~StreamDecoder() {
    close();
}

bool StreamDecoder::is_open() const {
    return m_impl->opened;
}

void StreamDecoder::close() {
#if defined(FUSE_HAS_OGG_VORBIS)
    if (m_impl->opened) {
        ov_clear(&m_impl->vf);
    }
#endif
    m_impl->opened = false;
    m_data.reset();
    m_window.clear();
    m_head.clear();
    m_windowStart = m_windowCount = m_decodePos = 0;
    m_headStart = m_headCount = 0;
    m_channels = m_frames = 0;
    m_eof = false;
}

bool StreamDecoder::open(std::shared_ptr<const AudioStreamData> data, u32 loop_start, u32 loop_end) {
    close();
#if defined(FUSE_HAS_OGG_VORBIS)
    if (!data || data->encoded.empty() || data->channels == 0 || data->frames == 0) {
        return false;
    }
    m_data = std::move(data);
    m_impl->reader = detail::MemoryReader{m_data->encoded.data(), m_data->encoded.size(), 0};
    if (ov_open_callbacks(&m_impl->reader, &m_impl->vf, nullptr, 0, detail::memory_callbacks()) != 0) {
        m_data.reset();
        return false;
    }
    m_impl->opened = true;
    m_channels = m_data->channels;
    m_frames = m_data->frames;
    m_window.assign(static_cast<usize>(kWindowFrames) * m_channels, 0.f);
    m_head.assign(static_cast<usize>(kHeadFrames) * m_channels, 0.f);
    m_stats = {};

    // Head cache: the first kHeadFrames of the loop region, decoded once.
    const u32 end = (loop_end == 0 || loop_end > m_frames) ? m_frames : loop_end;
    const u32 start = std::min(loop_start, end > 0 ? end - 1 : 0);
    if (start > 0 && ov_pcm_seek(&m_impl->vf, static_cast<ogg_int64_t>(start)) != 0) {
        close();
        return false;
    }
    m_headStart = start;
    m_headCount = 0;
    const u32 head_target = std::min(kHeadFrames, end - start);
    while (m_headCount < head_target) {
        float** pcm = nullptr;
        int bitstream = 0;
        const long got = ov_read_float(&m_impl->vf, &pcm, static_cast<int>(head_target - m_headCount), &bitstream);
        if (got == OV_HOLE) {
            continue;
        }
        if (got <= 0) {
            break;
        }
        for (long i = 0; i < got; ++i) {
            for (u32 ch = 0; ch < m_channels; ++ch) {
                m_head[(static_cast<usize>(m_headCount) + static_cast<usize>(i)) * m_channels + ch] = pcm[ch][i];
            }
        }
        m_headCount += static_cast<u32>(got);
        m_stats.decoded_frames += static_cast<u64>(got);
    }
    m_decodePos = m_headStart + m_headCount;
    m_windowStart = m_decodePos;
    m_windowCount = 0;
    m_eof = m_decodePos >= m_frames;
    return true;
#else
    (void)data;
    (void)loop_start;
    (void)loop_end;
    return false;
#endif
}

bool StreamDecoder::seek_to(u32 frame) {
#if defined(FUSE_HAS_OGG_VORBIS)
    if (!m_impl->opened || ov_pcm_seek(&m_impl->vf, static_cast<ogg_int64_t>(frame)) != 0) {
        return false;
    }
    ++m_stats.seeks;
    m_decodePos = frame;
    m_windowStart = frame;
    m_windowCount = 0;
    m_eof = false;
    return true;
#else
    (void)frame;
    return false;
#endif
}

u32 StreamDecoder::decode_into(float* dst, u32 max_frames, u32 dst_capacity_frames, u32 ring_start) {
#if defined(FUSE_HAS_OGG_VORBIS)
    float** pcm = nullptr;
    int bitstream = 0;
    long got = OV_HOLE;
    while (got == OV_HOLE) {
        got = ov_read_float(&m_impl->vf, &pcm, static_cast<int>(max_frames), &bitstream);
    }
    if (got <= 0) {
        return 0;
    }
    for (long i = 0; i < got; ++i) {
        const usize slot = (static_cast<usize>(ring_start) + static_cast<usize>(i)) % dst_capacity_frames;
        for (u32 ch = 0; ch < m_channels; ++ch) {
            dst[slot * m_channels + ch] = pcm[ch][i];
        }
    }
    return static_cast<u32>(got);
#else
    (void)dst;
    (void)max_frames;
    (void)dst_capacity_frames;
    (void)ring_start;
    return 0;
#endif
}

bool StreamDecoder::refill(u32 frame) {
    if (!m_impl->opened || frame >= m_frames) {
        return false;
    }
    if (frame < m_windowStart || frame > m_decodePos + kLookaheadFrames) {
        if (!seek_to(frame)) {
            return false;
        }
    }
    // Drop what is more than kKeepBehind frames behind the read position.
    const u32 keep_from = std::min(frame > kKeepBehind ? frame - kKeepBehind : 0u, m_decodePos);
    if (keep_from > m_windowStart) {
        m_windowStart = keep_from;
        m_windowCount = m_decodePos - m_windowStart;
    }
    const u32 target = std::min(m_frames, frame + kLookaheadFrames);
    while (m_decodePos < target && !m_eof) {
        const u32 got = decode_into(m_window.data(), std::min<u32>(4096, target - m_decodePos), kWindowFrames,
                                    m_decodePos % kWindowFrames);
        if (got == 0) {
            m_eof = true;
            break;
        }
        m_decodePos += got;
        m_windowCount += got;
        if (m_windowCount > kWindowFrames) {
            m_windowStart += m_windowCount - kWindowFrames;
            m_windowCount = kWindowFrames;
        }
        m_stats.decoded_frames += got;
    }
    return frame >= m_windowStart && frame < m_windowStart + m_windowCount;
}

float StreamDecoder::sample(u32 frame, u32 channel) {
    if (frame >= m_frames || m_channels == 0) {
        return 0.f;
    }
    const u32 ch = std::min(channel, m_channels - 1);
    if (frame >= m_headStart && frame - m_headStart < m_headCount) {
        return m_head[static_cast<usize>(frame - m_headStart) * m_channels + ch];
    }
    if (frame < m_windowStart || frame >= m_windowStart + m_windowCount) {
        if (!refill(frame)) {
            ++m_stats.underruns;
            return 0.f;
        }
    }
    return m_window[static_cast<usize>(frame % kWindowFrames) * m_channels + ch];
}

} // namespace fuse::audio
