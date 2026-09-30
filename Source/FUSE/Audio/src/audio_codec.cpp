#include <fuse/audio/audio_codec.hpp>

#include "vorbis_memory_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(FUSE_HAS_OGG_VORBIS)
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#endif

#if defined(FUSE_HAS_FLAC)
#include <FLAC/format.h>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#endif

namespace fuse::audio {

bool ogg_vorbis_available() {
#if defined(FUSE_HAS_OGG_VORBIS)
    return true;
#else
    return false;
#endif
}

bool flac_available() {
#if defined(FUSE_HAS_FLAC)
    return true;
#else
    return false;
#endif
}

const char* vorbis_library_version() {
#if defined(FUSE_HAS_OGG_VORBIS)
    return vorbis_version_string();
#else
    return "";
#endif
}

const char* flac_library_version() {
#if defined(FUSE_HAS_FLAC)
    return FLAC__VERSION_STRING;
#else
    return "";
#endif
}

// ---- Ogg Vorbis ------------------------------------------------------------------------------

bool probe_ogg_vorbis(const u8* data, usize size, u32& frames, u32& channels, u32& sample_rate) {
#if defined(FUSE_HAS_OGG_VORBIS)
    if (data == nullptr || size == 0) {
        return false;
    }
    detail::MemoryReader reader{data, size, 0};
    OggVorbis_File vf;
    if (ov_open_callbacks(&reader, &vf, nullptr, 0, detail::memory_callbacks()) != 0) {
        return false;
    }
    const vorbis_info* info = ov_info(&vf, -1);
    const ogg_int64_t total = ov_pcm_total(&vf, -1);
    const bool ok = info != nullptr && info->channels > 0 && info->rate > 0 && total > 0
        && total <= static_cast<ogg_int64_t>(UINT32_MAX);
    if (ok) {
        frames = static_cast<u32>(total);
        channels = static_cast<u32>(info->channels);
        sample_rate = static_cast<u32>(info->rate);
    }
    ov_clear(&vf);
    return ok;
#else
    (void)data;
    (void)size;
    (void)frames;
    (void)channels;
    (void)sample_rate;
    return false;
#endif
}

bool decode_ogg_vorbis(const u8* data, usize size, DecodedAudio& out) {
#if defined(FUSE_HAS_OGG_VORBIS)
    if (data == nullptr || size == 0) {
        return false;
    }
    detail::MemoryReader reader{data, size, 0};
    OggVorbis_File vf;
    if (ov_open_callbacks(&reader, &vf, nullptr, 0, detail::memory_callbacks()) != 0) {
        return false;
    }
    const vorbis_info* info = ov_info(&vf, -1);
    if (info == nullptr || info->channels <= 0 || info->rate <= 0) {
        ov_clear(&vf);
        return false;
    }
    const u32 channels = static_cast<u32>(info->channels);
    const u32 rate = static_cast<u32>(info->rate); // `info` is owned by vf: read it before ov_clear
    const ogg_int64_t total = ov_pcm_total(&vf, -1);
    out.samples.clear();
    if (total > 0) {
        out.samples.reserve(static_cast<usize>(total) * channels);
    }
    bool failed = false;
    for (;;) {
        float** pcm = nullptr;
        int bitstream = 0;
        const long got = ov_read_float(&vf, &pcm, 4096, &bitstream);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            if (got == OV_HOLE) {
                continue; // recoverable gap in the page sequence
            }
            failed = true;
            break;
        }
        const vorbis_info* link = ov_info(&vf, bitstream);
        if (link == nullptr || static_cast<u32>(link->channels) != channels) {
            failed = true; // chained streams with a different layout are not supported
            break;
        }
        for (long i = 0; i < got; ++i) {
            for (u32 ch = 0; ch < channels; ++ch) {
                out.samples.push_back(pcm[ch][i]);
            }
        }
    }
    ov_clear(&vf);
    if (failed || out.samples.empty()) {
        out.samples.clear();
        return false;
    }
    out.channels = channels;
    out.sample_rate = rate;
    out.frames = static_cast<u32>(out.samples.size() / channels);
    return true;
#else
    (void)data;
    (void)size;
    (void)out;
    return false;
#endif
}

bool encode_ogg_vorbis(const float* interleaved, u32 frames, u32 channels, u32 sample_rate, float quality,
                       std::vector<u8>& out) {
#if defined(FUSE_HAS_OGG_VORBIS)
    out.clear();
    if (interleaved == nullptr || frames == 0 || channels == 0 || channels > 255 || sample_rate == 0) {
        return false;
    }
    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, static_cast<long>(channels), static_cast<long>(sample_rate),
                               std::clamp(quality, -0.1f, 1.f)) != 0) {
        vorbis_info_clear(&vi);
        return false;
    }
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "FUSE fuse_audio");
    vorbis_dsp_state vd;
    vorbis_block vb;
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);

    ogg_stream_state os;
    ogg_stream_init(&os, static_cast<int>(0x46555345)); // "FUSE"

    auto append_page = [&out](const ogg_page& page) {
        out.insert(out.end(), page.header, page.header + page.header_len);
        out.insert(out.end(), page.body, page.body + page.body_len);
    };

    ogg_packet header;
    ogg_packet header_comment;
    ogg_packet header_code;
    vorbis_analysis_headerout(&vd, &vc, &header, &header_comment, &header_code);
    ogg_stream_packetin(&os, &header);
    ogg_stream_packetin(&os, &header_comment);
    ogg_stream_packetin(&os, &header_code);
    ogg_page page;
    while (ogg_stream_flush(&os, &page) != 0) {
        append_page(page);
    }

    auto drain = [&]() {
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);
            ogg_packet packet;
            while (vorbis_bitrate_flushpacket(&vd, &packet) != 0) {
                ogg_stream_packetin(&os, &packet);
                while (ogg_stream_pageout(&os, &page) != 0) {
                    append_page(page);
                }
            }
        }
    };

    constexpr u32 kChunk = 1024;
    for (u32 offset = 0; offset < frames; offset += kChunk) {
        const u32 count = std::min(kChunk, frames - offset);
        float** buffer = vorbis_analysis_buffer(&vd, static_cast<int>(count));
        for (u32 i = 0; i < count; ++i) {
            for (u32 ch = 0; ch < channels; ++ch) {
                buffer[ch][i] = interleaved[static_cast<usize>(offset + i) * channels + ch];
            }
        }
        vorbis_analysis_wrote(&vd, static_cast<int>(count));
        drain();
    }
    vorbis_analysis_wrote(&vd, 0);
    drain();
    while (ogg_stream_flush(&os, &page) != 0) {
        append_page(page);
    }

    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return !out.empty();
#else
    (void)interleaved;
    (void)frames;
    (void)channels;
    (void)sample_rate;
    (void)quality;
    out.clear();
    return false;
#endif
}

// ---- FLAC ------------------------------------------------------------------------------------

#if defined(FUSE_HAS_FLAC)
namespace {

struct FlacDecodeState {
    const u8* data = nullptr;
    usize size = 0;
    usize pos = 0;
    DecodedAudio* out = nullptr;
    u32 bits = 0;
    u32 channels = 0;
    u32 rate = 0;
    bool error = false;
};

FLAC__StreamDecoderReadStatus flac_read(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes,
                                        void* client) {
    auto* s = static_cast<FlacDecodeState*>(client);
    if (s->pos >= s->size) {
        *bytes = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    const usize n = std::min<usize>(*bytes, s->size - s->pos);
    std::memcpy(buffer, s->data + s->pos, n);
    s->pos += n;
    *bytes = n;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus flac_write(const FLAC__StreamDecoder*, const FLAC__Frame* frame,
                                          const FLAC__int32* const buffer[], void* client) {
    auto* s = static_cast<FlacDecodeState*>(client);
    const u32 channels = frame->header.channels;
    const u32 bits = frame->header.bits_per_sample != 0 ? frame->header.bits_per_sample : s->bits;
    if (channels == 0 || bits == 0 || bits > 32 || (s->channels != 0 && channels != s->channels)) {
        s->error = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    s->channels = channels;
    s->rate = frame->header.sample_rate != 0 ? frame->header.sample_rate : s->rate;
    const double scale = 1.0 / static_cast<double>(std::uint64_t{1} << (bits - 1));
    for (u32 i = 0; i < frame->header.blocksize; ++i) {
        for (u32 ch = 0; ch < channels; ++ch) {
            s->out->samples.push_back(static_cast<float>(static_cast<double>(buffer[ch][i]) * scale));
        }
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void flac_metadata(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* metadata, void* client) {
    auto* s = static_cast<FlacDecodeState*>(client);
    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        s->bits = metadata->data.stream_info.bits_per_sample;
        s->channels = metadata->data.stream_info.channels;
        s->rate = metadata->data.stream_info.sample_rate;
        const FLAC__uint64 total = metadata->data.stream_info.total_samples;
        if (total > 0 && total < (FLAC__uint64{1} << 31)) {
            s->out->samples.reserve(static_cast<usize>(total) * s->channels);
        }
    }
}

void flac_error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* client) {
    static_cast<FlacDecodeState*>(client)->error = true;
}

struct FlacEncodeState {
    std::vector<u8>* out = nullptr;
    usize pos = 0;
};

FLAC__StreamEncoderWriteStatus flac_enc_write(const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes,
                                              uint32_t, uint32_t, void* client) {
    auto* s = static_cast<FlacEncodeState*>(client);
    std::vector<u8>& out = *s->out;
    if (s->pos + bytes > out.size()) {
        out.resize(s->pos + bytes);
    }
    std::memcpy(out.data() + s->pos, buffer, bytes);
    s->pos += bytes;
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

FLAC__StreamEncoderSeekStatus flac_enc_seek(const FLAC__StreamEncoder*, FLAC__uint64 offset, void* client) {
    auto* s = static_cast<FlacEncodeState*>(client);
    if (offset > s->out->size()) {
        return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    }
    s->pos = static_cast<usize>(offset);
    return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}

FLAC__StreamEncoderTellStatus flac_enc_tell(const FLAC__StreamEncoder*, FLAC__uint64* offset, void* client) {
    *offset = static_cast<FLAC__uint64>(static_cast<FlacEncodeState*>(client)->pos);
    return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

} // namespace
#endif

bool decode_flac(const u8* data, usize size, DecodedAudio& out) {
#if defined(FUSE_HAS_FLAC)
    if (data == nullptr || size < 4) {
        return false;
    }
    FLAC__StreamDecoder* decoder = FLAC__stream_decoder_new();
    if (decoder == nullptr) {
        return false;
    }
    out.samples.clear();
    FlacDecodeState state;
    state.data = data;
    state.size = size;
    state.out = &out;
    bool ok = FLAC__stream_decoder_init_stream(decoder, &flac_read, nullptr, nullptr, nullptr, nullptr, &flac_write,
                                               &flac_metadata, &flac_error, &state)
        == FLAC__STREAM_DECODER_INIT_STATUS_OK;
    ok = ok && FLAC__stream_decoder_process_until_end_of_stream(decoder) != 0;
    FLAC__stream_decoder_finish(decoder);
    FLAC__stream_decoder_delete(decoder);
    ok = ok && !state.error && state.channels > 0 && state.rate > 0 && !out.samples.empty();
    if (!ok) {
        out.samples.clear();
        return false;
    }
    out.channels = state.channels;
    out.sample_rate = state.rate;
    out.frames = static_cast<u32>(out.samples.size() / state.channels);
    return true;
#else
    (void)data;
    (void)size;
    (void)out;
    return false;
#endif
}

bool encode_flac(const float* interleaved, u32 frames, u32 channels, u32 sample_rate, u32 bits_per_sample,
                 std::vector<u8>& out) {
#if defined(FUSE_HAS_FLAC)
    out.clear();
    if (interleaved == nullptr || frames == 0 || channels == 0 || channels > 8 || sample_rate == 0
        || (bits_per_sample != 16 && bits_per_sample != 24)) {
        return false;
    }
    FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
    if (encoder == nullptr) {
        return false;
    }
    bool ok = FLAC__stream_encoder_set_channels(encoder, channels) != 0;
    ok = ok && FLAC__stream_encoder_set_bits_per_sample(encoder, bits_per_sample) != 0;
    ok = ok && FLAC__stream_encoder_set_sample_rate(encoder, sample_rate) != 0;
    ok = ok && FLAC__stream_encoder_set_compression_level(encoder, 5) != 0;
    ok = ok && FLAC__stream_encoder_set_total_samples_estimate(encoder, frames) != 0;
    FlacEncodeState state;
    state.out = &out;
    ok = ok && FLAC__stream_encoder_init_stream(encoder, &flac_enc_write, &flac_enc_seek, &flac_enc_tell, nullptr,
                                                &state)
        == FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    if (ok) {
        const double full_scale = static_cast<double>((std::int64_t{1} << (bits_per_sample - 1)) - 1);
        constexpr u32 kChunk = 4096;
        std::vector<FLAC__int32> block(static_cast<usize>(kChunk) * channels);
        for (u32 offset = 0; ok && offset < frames; offset += kChunk) {
            const u32 count = std::min(kChunk, frames - offset);
            for (usize i = 0; i < static_cast<usize>(count) * channels; ++i) {
                const double v = std::clamp(static_cast<double>(interleaved[static_cast<usize>(offset) * channels + i]),
                                            -1.0, 1.0);
                block[i] = static_cast<FLAC__int32>(std::lrint(v * full_scale));
            }
            ok = FLAC__stream_encoder_process_interleaved(encoder, block.data(), count) != 0;
        }
        ok = FLAC__stream_encoder_finish(encoder) != 0 && ok;
    }
    FLAC__stream_encoder_delete(encoder);
    if (!ok) {
        out.clear();
    }
    return ok && !out.empty();
#else
    (void)interleaved;
    (void)frames;
    (void)channels;
    (void)sample_rate;
    (void)bits_per_sample;
    out.clear();
    return false;
#endif
}

} // namespace fuse::audio
