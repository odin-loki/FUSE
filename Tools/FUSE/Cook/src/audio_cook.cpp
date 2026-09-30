// Audio cook chain — see fuse/cook/audio_cook.hpp.
// MP-B7.9-AUDIO-IMPORT / AP-W8.3 / UNI-U7-AUDIO-1 (cook half of MP-B7.2-OGG-RUNTIME).

#include <fuse/cook/audio_cook.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

#if defined(FUSE_COOK_HAS_VORBIS)
#define OV_EXCLUDE_STATIC_CALLBACKS
#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#include <vorbis/vorbisfile.h>
#endif

#if defined(FUSE_COOK_HAS_FLAC)
#include <FLAC/format.h>
#include <FLAC/stream_decoder.h>
#endif

namespace fuse::cook {

namespace {

constexpr f64 kPi = std::numbers::pi;

void set_error(std::string* error, const std::string& text) {
    if (error != nullptr) {
        *error = text;
    }
}

u32 read_le16(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8u);
}

u32 read_le32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8u) | (static_cast<u32>(p[2]) << 16u)
         | (static_cast<u32>(p[3]) << 24u);
}

f64 db_to_linear(f64 db) {
    return std::pow(10.0, db / 20.0);
}

f32 linear_to_dbfs(f64 linear) {
    return linear > 1e-12 ? static_cast<f32>(20.0 * std::log10(linear)) : -144.0f;
}

bool valid_buffer(const AudioBuffer& b) {
    return b.channels > 0 && b.sample_rate > 0 && b.frames > 0
        && b.samples.size() == static_cast<usize>(b.frames) * b.channels;
}

// ---- K-weighting (ITU-R BS.1770-4) --------------------------------------------------------------------

struct Biquad {
    f64 b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
};

/// Stage 1 (high shelf, +4 dB) and stage 2 (RLB high-pass) for an arbitrary rate; the analogue
/// prototypes are the ones the 48 kHz coefficients of BS.1770 Table 1/2 are derived from.
void k_weighting(u32 rate, Biquad& shelf, Biquad& highpass) {
    const f64 fs = static_cast<f64>(rate);
    {
        const f64 f0 = 1681.974450955533;
        const f64 gain_db = 3.999843853973347;
        const f64 q = 0.7071752369554196;
        const f64 k = std::tan(kPi * f0 / fs);
        const f64 vh = std::pow(10.0, gain_db / 20.0);
        const f64 vb = std::pow(vh, 0.4996667741545416);
        const f64 a0 = 1.0 + k / q + k * k;
        shelf.b0 = (vh + vb * k / q + k * k) / a0;
        shelf.b1 = 2.0 * (k * k - vh) / a0;
        shelf.b2 = (vh - vb * k / q + k * k) / a0;
        shelf.a1 = 2.0 * (k * k - 1.0) / a0;
        shelf.a2 = (1.0 - k / q + k * k) / a0;
    }
    {
        const f64 f0 = 38.13547087602444;
        const f64 q = 0.5003270373238773;
        const f64 k = std::tan(kPi * f0 / fs);
        const f64 a0 = 1.0 + k / q + k * k;
        highpass.b0 = 1.0;
        highpass.b1 = -2.0;
        highpass.b2 = 1.0;
        highpass.a1 = 2.0 * (k * k - 1.0) / a0;
        highpass.a2 = (1.0 - k / q + k * k) / a0;
    }
}

f64 channel_weight(u32 channel, u32 channels) {
    // 5.1 (L R C LFE Ls Rs): LFE excluded, surrounds +1.5 dB.
    if (channels == 6) {
        if (channel == 3) {
            return 0.0;
        }
        if (channel >= 4) {
            return 1.41;
        }
    }
    return 1.0;
}

f64 block_loudness(f64 mean_square) {
    return mean_square > 0.0 ? -0.691 + 10.0 * std::log10(mean_square) : kAudioSilenceLufs;
}

// ---- windowed sinc ------------------------------------------------------------------------------------

f64 bessel_i0(f64 x) {
    f64 sum = 1.0;
    f64 term = 1.0;
    const f64 half = x * 0.5;
    for (int k = 1; k < 64; ++k) {
        term *= (half / k) * (half / k);
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

u64 gcd_u64(u64 a, u64 b) {
    while (b != 0) {
        const u64 t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// ---- WAV ----------------------------------------------------------------------------------------------

constexpr u32 kWaveFormatPcm = 1u;
constexpr u32 kWaveFormatFloat = 3u;
constexpr u32 kWaveFormatExtensible = 0xFFFEu;

// ---- xiph decoders -------------------------------------------------------------------------------------

#if defined(FUSE_COOK_HAS_VORBIS)
struct MemReader {
    const u8* data = nullptr;
    usize size = 0;
    usize pos = 0;
};

size_t mem_read(void* ptr, size_t size, size_t nmemb, void* source) {
    auto* r = static_cast<MemReader*>(source);
    if (size == 0 || r->pos >= r->size) {
        return 0;
    }
    const usize items = std::min<usize>(nmemb, (r->size - r->pos) / size);
    std::memcpy(ptr, r->data + r->pos, items * size);
    r->pos += items * size;
    return items;
}

int mem_seek(void* source, ogg_int64_t offset, int whence) {
    auto* r = static_cast<MemReader*>(source);
    ogg_int64_t base = 0;
    if (whence == SEEK_CUR) {
        base = static_cast<ogg_int64_t>(r->pos);
    } else if (whence == SEEK_END) {
        base = static_cast<ogg_int64_t>(r->size);
    }
    const ogg_int64_t target = base + offset;
    if (target < 0 || target > static_cast<ogg_int64_t>(r->size)) {
        return -1;
    }
    r->pos = static_cast<usize>(target);
    return 0;
}

long mem_tell(void* source) {
    return static_cast<long>(static_cast<MemReader*>(source)->pos);
}
#endif

#if defined(FUSE_COOK_HAS_FLAC)
struct FlacState {
    const u8* data = nullptr;
    usize size = 0;
    usize pos = 0;
    AudioBuffer* out = nullptr;
    u32 bits = 0;
    u32 channels = 0;
    u32 rate = 0;
    bool error = false;
};

FLAC__StreamDecoderReadStatus flac_read(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes,
                                        void* client) {
    auto* s = static_cast<FlacState*>(client);
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
    auto* s = static_cast<FlacState*>(client);
    const u32 channels = frame->header.channels;
    const u32 bits = frame->header.bits_per_sample != 0 ? frame->header.bits_per_sample : s->bits;
    if (channels == 0 || bits == 0 || bits > 32 || (s->channels != 0 && channels != s->channels)) {
        s->error = true;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    s->channels = channels;
    if (frame->header.sample_rate != 0) {
        s->rate = frame->header.sample_rate;
    }
    const f64 scale = 1.0 / static_cast<f64>(u64{1} << (bits - 1u));
    for (u32 i = 0; i < frame->header.blocksize; ++i) {
        for (u32 ch = 0; ch < channels; ++ch) {
            s->out->samples.push_back(static_cast<float>(static_cast<f64>(buffer[ch][i]) * scale));
        }
    }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void flac_metadata(const FLAC__StreamDecoder*, const FLAC__StreamMetadata* metadata, void* client) {
    auto* s = static_cast<FlacState*>(client);
    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        s->bits = metadata->data.stream_info.bits_per_sample;
        s->channels = metadata->data.stream_info.channels;
        s->rate = metadata->data.stream_info.sample_rate;
        const FLAC__uint64 total = metadata->data.stream_info.total_samples;
        if (total > 0 && total < (FLAC__uint64{1} << 31u)) {
            s->out->samples.reserve(static_cast<usize>(total) * s->channels);
        }
    }
}

void flac_error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* client) {
    static_cast<FlacState*>(client)->error = true;
}
#endif

bool read_file_bytes(const std::string& path, std::vector<u8>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    if (length < 0) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<usize>(length));
    if (length > 0) {
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
    }
    return static_cast<bool>(in) || in.eof();
}

bool write_file_bytes(const std::string& path, const std::vector<u8>& bytes) {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

std::string format_fixed(f64 value) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.2f", value);
    return text;
}

/// Mono sum used for zero-crossing searches.
f64 mono_at(const AudioBuffer& b, u32 frame) {
    f64 sum = 0.0;
    const usize base = static_cast<usize>(frame) * b.channels;
    for (u32 c = 0; c < b.channels; ++c) {
        sum += b.samples[base + c];
    }
    return sum;
}

bool rising_zero_crossing(const AudioBuffer& b, u32 frame) {
    return frame > 0 && mono_at(b, frame - 1u) < 0.0 && mono_at(b, frame) >= 0.0;
}

} // namespace

// ---- availability / sniffing -------------------------------------------------------------------------

bool audio_cook_vorbis_available() {
#if defined(FUSE_COOK_HAS_VORBIS)
    return true;
#else
    return false;
#endif
}

bool audio_cook_flac_available() {
#if defined(FUSE_COOK_HAS_FLAC)
    return true;
#else
    return false;
#endif
}

AudioSourceFormat sniff_audio_format(const u8* bytes, usize size) {
    if (bytes == nullptr || size < 4) {
        return AudioSourceFormat::Unknown;
    }
    if (size >= 12 && std::memcmp(bytes, "RIFF", 4) == 0 && std::memcmp(bytes + 8, "WAVE", 4) == 0) {
        return AudioSourceFormat::Wav;
    }
    if (std::memcmp(bytes, "fLaC", 4) == 0) {
        return AudioSourceFormat::Flac;
    }
    if (std::memcmp(bytes, "OggS", 4) == 0) {
        // First page carries the identification header: 0x01 "vorbis" at the start of the packet.
        if (size >= 27) {
            const usize segments = bytes[26];
            const usize packet = 27 + segments;
            if (size >= packet + 7 && bytes[packet] == 0x01 && std::memcmp(bytes + packet + 1, "vorbis", 6) == 0) {
                return AudioSourceFormat::OggVorbis;
            }
        }
        return AudioSourceFormat::Unknown;
    }
    if (size >= 3 && std::memcmp(bytes, "ID3", 3) == 0) {
        return AudioSourceFormat::Mp3;
    }
    if (bytes[0] == 0xFF && (bytes[1] & 0xE0u) == 0xE0u && (bytes[1] & 0x06u) != 0) {
        return AudioSourceFormat::Mp3;
    }
    return AudioSourceFormat::Unknown;
}

const char* audio_source_format_name(AudioSourceFormat format) {
    switch (format) {
    case AudioSourceFormat::Wav:
        return "wav";
    case AudioSourceFormat::Flac:
        return "flac";
    case AudioSourceFormat::OggVorbis:
        return "ogg";
    case AudioSourceFormat::Mp3:
        return "mp3";
    case AudioSourceFormat::Unknown:
        break;
    }
    return "unknown";
}

const char* audio_class_name(AudioClass audio_class) {
    switch (audio_class) {
    case AudioClass::Bed:
        return "bed";
    case AudioClass::OneShot:
        return "oneshot";
    case AudioClass::Auto:
        break;
    }
    return "auto";
}

// ---- decoders -----------------------------------------------------------------------------------------

bool decode_wav(const u8* bytes, usize size, AudioBuffer& out, std::string* error) {
    out = {};
    if (sniff_audio_format(bytes, size) != AudioSourceFormat::Wav) {
        set_error(error, "not a RIFF/WAVE file");
        return false;
    }
    u32 format_tag = 0;
    u32 channels = 0;
    u32 rate = 0;
    u32 block_align = 0;
    u32 bits = 0;
    bool have_fmt = false;
    const u8* data = nullptr;
    usize data_size = 0;

    usize pos = 12;
    while (pos + 8 <= size) {
        const u8* id = bytes + pos;
        const usize chunk = read_le32(bytes + pos + 4);
        const usize body = pos + 8;
        const usize available = size - body;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            if (chunk < 16 || available < 16) {
                set_error(error, "truncated fmt chunk");
                return false;
            }
            format_tag = read_le16(bytes + body);
            channels = read_le16(bytes + body + 2);
            rate = read_le32(bytes + body + 4);
            block_align = read_le16(bytes + body + 12);
            bits = read_le16(bytes + body + 14);
            if (format_tag == kWaveFormatExtensible) {
                if (chunk < 40 || available < 40) {
                    set_error(error, "truncated WAVE_FORMAT_EXTENSIBLE fmt chunk");
                    return false;
                }
                format_tag = read_le16(bytes + body + 24); // SubFormat GUID, first two bytes
            }
            have_fmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = bytes + body;
            data_size = std::min<usize>(chunk, available); // streamed WAVs may carry 0xFFFFFFFF
            break;
        }
        if (chunk > available) {
            break;
        }
        pos = body + chunk + (chunk & 1u); // chunks are word aligned
    }
    if (!have_fmt || data == nullptr) {
        set_error(error, "missing fmt or data chunk");
        return false;
    }
    if (channels == 0 || channels > 64 || rate == 0 || rate > 768000 || block_align == 0
        || block_align % channels != 0) {
        set_error(error, "invalid WAV format header");
        return false;
    }
    const u32 container = block_align / channels; // bytes per stored sample
    const bool is_float = format_tag == kWaveFormatFloat;
    if (format_tag != kWaveFormatPcm && !is_float) {
        set_error(error, "unsupported WAV sample format " + std::to_string(format_tag));
        return false;
    }
    if ((is_float && container != 4 && container != 8) || (!is_float && (container < 1 || container > 4))
        || bits == 0 || bits > container * 8u) {
        set_error(error, "unsupported WAV sample width " + std::to_string(bits) + " bits");
        return false;
    }
    const usize frames = data_size / block_align;
    if (frames == 0 || frames > 0xFFFFFFFFull / channels) {
        set_error(error, "WAV has no sample frames");
        return false;
    }
    out.channels = channels;
    out.sample_rate = rate;
    out.frames = static_cast<u32>(frames);
    out.samples.resize(frames * channels);
    const f64 int_scale = 1.0 / static_cast<f64>(u64{1} << (container * 8u - 1u));
    for (usize i = 0; i < frames * channels; ++i) {
        const u8* p = data + i * container;
        f64 value = 0.0;
        if (is_float) {
            if (container == 4) {
                const u32 raw = read_le32(p);
                value = static_cast<f64>(std::bit_cast<float>(raw));
            } else {
                const u64 raw = static_cast<u64>(read_le32(p)) | (static_cast<u64>(read_le32(p + 4)) << 32u);
                value = std::bit_cast<f64>(raw);
            }
            if (!std::isfinite(value)) {
                value = 0.0;
            }
        } else if (container == 1) {
            value = (static_cast<f64>(p[0]) - 128.0) / 128.0; // 8-bit WAV is unsigned
        } else {
            u32 raw = 0;
            for (u32 b = 0; b < container; ++b) {
                raw |= static_cast<u32>(p[b]) << (8u * b);
            }
            const u32 shift = 32u - container * 8u;
            const s32 signed_value = static_cast<s32>(raw << shift) >> shift; // sign-extend
            value = static_cast<f64>(signed_value) * int_scale;
        }
        out.samples[i] = static_cast<float>(value);
    }
    return true;
}

bool decode_flac_stream(const u8* bytes, usize size, AudioBuffer& out, std::string* error) {
    out = {};
#if defined(FUSE_COOK_HAS_FLAC)
    if (bytes == nullptr || size == 0) {
        set_error(error, "empty FLAC stream");
        return false;
    }
    FlacState state;
    state.data = bytes;
    state.size = size;
    state.out = &out;
    FLAC__StreamDecoder* decoder = FLAC__stream_decoder_new();
    if (decoder == nullptr) {
        set_error(error, "FLAC decoder allocation failed");
        return false;
    }
    bool ok = FLAC__stream_decoder_init_stream(decoder, &flac_read, nullptr, nullptr, nullptr, nullptr, &flac_write,
                                               &flac_metadata, &flac_error, &state)
           == FLAC__STREAM_DECODER_INIT_STATUS_OK;
    ok = ok && FLAC__stream_decoder_process_until_end_of_stream(decoder) != 0;
    FLAC__stream_decoder_finish(decoder);
    FLAC__stream_decoder_delete(decoder);
    if (!ok || state.error || state.channels == 0 || state.rate == 0 || out.samples.empty()) {
        out = {};
        set_error(error, "FLAC decode failed");
        return false;
    }
    out.channels = state.channels;
    out.sample_rate = state.rate;
    out.frames = static_cast<u32>(out.samples.size() / state.channels);
    return true;
#else
    (void)bytes;
    (void)size;
    set_error(error, "FLAC decoder not built (vendored libFLAC unavailable)");
    return false;
#endif
}

bool decode_ogg_vorbis_stream(const u8* bytes, usize size, AudioBuffer& out, std::string* error) {
    out = {};
#if defined(FUSE_COOK_HAS_VORBIS)
    if (bytes == nullptr || size == 0) {
        set_error(error, "empty Ogg stream");
        return false;
    }
    MemReader reader;
    reader.data = bytes;
    reader.size = size;
    ov_callbacks callbacks{};
    callbacks.read_func = &mem_read;
    callbacks.seek_func = &mem_seek;
    callbacks.close_func = nullptr;
    callbacks.tell_func = &mem_tell;
    OggVorbis_File vf;
    if (ov_open_callbacks(&reader, &vf, nullptr, 0, callbacks) != 0) {
        set_error(error, "not an Ogg Vorbis stream");
        return false;
    }
    const vorbis_info* info = ov_info(&vf, -1);
    if (info == nullptr || info->channels <= 0 || info->rate <= 0) {
        ov_clear(&vf);
        set_error(error, "Ogg Vorbis stream has no valid header");
        return false;
    }
    const u32 channels = static_cast<u32>(info->channels);
    out.channels = channels;
    out.sample_rate = static_cast<u32>(info->rate);
    const ogg_int64_t total = ov_pcm_total(&vf, -1);
    if (total > 0 && total < (ogg_int64_t{1} << 31)) {
        out.samples.reserve(static_cast<usize>(total) * channels);
    }
    bool ok = true;
    for (;;) {
        float** pcm = nullptr;
        int section = 0;
        const long got = ov_read_float(&vf, &pcm, 4096, &section);
        if (got == 0) {
            break;
        }
        if (got < 0) {
            if (got == OV_HOLE) {
                continue; // recoverable gap in the page sequence
            }
            ok = false;
            break;
        }
        const vorbis_info* link = ov_info(&vf, section);
        if (link == nullptr || static_cast<u32>(link->channels) != channels) {
            ok = false; // chained streams with a different layout are not supported
            break;
        }
        for (long i = 0; i < got; ++i) {
            for (u32 c = 0; c < channels; ++c) {
                out.samples.push_back(pcm[c][i]);
            }
        }
    }
    ov_clear(&vf);
    if (!ok || out.samples.empty()) {
        out = {};
        set_error(error, "Ogg Vorbis decode failed");
        return false;
    }
    out.frames = static_cast<u32>(out.samples.size() / channels);
    return true;
#else
    (void)bytes;
    (void)size;
    set_error(error, "Ogg Vorbis decoder not built (vendored libvorbis unavailable)");
    return false;
#endif
}

bool decode_audio(const u8* bytes, usize size, AudioBuffer& out, std::string* error, AudioSourceFormat* detected) {
    const AudioSourceFormat format = sniff_audio_format(bytes, size);
    if (detected != nullptr) {
        *detected = format;
    }
    switch (format) {
    case AudioSourceFormat::Wav:
        return decode_wav(bytes, size, out, error);
    case AudioSourceFormat::Flac:
        return decode_flac_stream(bytes, size, out, error);
    case AudioSourceFormat::OggVorbis:
        return decode_ogg_vorbis_stream(bytes, size, out, error);
    case AudioSourceFormat::Mp3:
        out = {};
        set_error(error, "MP3 sources are not supported (no MP3 decoder vendored); convert to WAV/FLAC/Ogg");
        return false;
    case AudioSourceFormat::Unknown:
        break;
    }
    out = {};
    set_error(error, "unrecognised audio container (expected WAV, FLAC or Ogg Vorbis)");
    return false;
}

bool decode_audio_file(const std::string& path, AudioBuffer& out, std::string* error, AudioSourceFormat* detected) {
    std::vector<u8> bytes;
    if (path.empty() || !read_file_bytes(path, bytes)) {
        out = {};
        if (detected != nullptr) {
            *detected = AudioSourceFormat::Unknown;
        }
        set_error(error, "source unreadable");
        return false;
    }
    return decode_audio(bytes.data(), bytes.size(), out, error, detected);
}

// ---- processing stages --------------------------------------------------------------------------------

void downmix_to_mono(AudioBuffer& buffer) {
    if (buffer.channels <= 1 || !valid_buffer(buffer)) {
        return;
    }
    const u32 channels = buffer.channels;
    const f64 inv = 1.0 / static_cast<f64>(channels);
    for (u32 f = 0; f < buffer.frames; ++f) {
        f64 sum = 0.0;
        for (u32 c = 0; c < channels; ++c) {
            sum += buffer.samples[static_cast<usize>(f) * channels + c];
        }
        buffer.samples[f] = static_cast<float>(sum * inv);
    }
    buffer.samples.resize(buffer.frames);
    buffer.channels = 1;
}

AudioBuffer resample_audio(const AudioBuffer& in, u32 target_rate) {
    if (!valid_buffer(in) || target_rate == 0 || target_rate == in.sample_rate) {
        return in;
    }
    const u64 g = gcd_u64(in.sample_rate, target_rate);
    const u64 up = target_rate / g;    // L
    const u64 down = in.sample_rate / g; // M
    constexpr f64 kZeroCrossings = 32.0;
    constexpr f64 kRolloff = 0.945;
    constexpr f64 kBeta = 9.0; // Kaiser: ~-90 dB stopband
    // Cutoff as a fraction of the input Nyquist frequency.
    const f64 cutoff = std::min(1.0, static_cast<f64>(up) / static_cast<f64>(down)) * kRolloff;
    const f64 half_width = kZeroCrossings / cutoff; // in input samples
    const int radius = static_cast<int>(std::ceil(half_width));
    const int taps = 2 * radius;
    const bool exact = up <= 2048u;
    const u32 phases = exact ? static_cast<u32>(up) : 2048u;
    const f64 inv_i0 = 1.0 / bessel_i0(kBeta);

    // table[p][j] = h(phase_p - k), k = j - radius + 1; each phase normalised to unity DC gain.
    std::vector<f64> table(static_cast<usize>(phases + 1u) * taps);
    for (u32 p = 0; p <= phases; ++p) {
        const f64 phase = static_cast<f64>(p) / static_cast<f64>(phases);
        f64 sum = 0.0;
        f64* row = table.data() + static_cast<usize>(p) * taps;
        for (int j = 0; j < taps; ++j) {
            const f64 t = phase - static_cast<f64>(j - radius + 1);
            f64 h = 0.0;
            if (std::abs(t) < half_width) {
                const f64 x = cutoff * t;
                const f64 sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
                const f64 r = t / half_width;
                const f64 window = bessel_i0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) * inv_i0;
                h = cutoff * sinc * window;
            }
            row[j] = h;
            sum += h;
        }
        if (sum != 0.0) {
            for (int j = 0; j < taps; ++j) {
                row[j] /= sum;
            }
        }
    }

    AudioBuffer out;
    out.channels = in.channels;
    out.sample_rate = target_rate;
    const u64 out_frames = (static_cast<u64>(in.frames) * up + down - 1u) / down;
    out.frames = static_cast<u32>(std::min<u64>(out_frames, 0xFFFFFFFFull / in.channels));
    out.samples.assign(static_cast<usize>(out.frames) * in.channels, 0.0f);
    std::vector<f64> coef(static_cast<usize>(taps));
    const s64 in_frames = in.frames;
    const u32 channels = in.channels;
    for (u64 n = 0; n < out.frames; ++n) {
        const u64 pos = n * down;
        const s64 q = static_cast<s64>(pos / up);
        const u64 rem = pos % up;
        const f64* row = nullptr;
        if (exact) {
            row = table.data() + static_cast<usize>(rem) * taps;
        } else {
            const f64 fp = static_cast<f64>(rem) / static_cast<f64>(up) * phases;
            const u32 p0 = std::min(static_cast<u32>(fp), phases - 1u);
            const f64 frac = fp - p0;
            const f64* a = table.data() + static_cast<usize>(p0) * taps;
            const f64* b = a + taps;
            for (int j = 0; j < taps; ++j) {
                coef[static_cast<usize>(j)] = a[j] + (b[j] - a[j]) * frac;
            }
            row = coef.data();
        }
        const s64 first = q - radius + 1;
        const int j0 = static_cast<int>(std::max<s64>(0, -first));
        const int j1 = static_cast<int>(std::min<s64>(taps, in_frames - first));
        for (u32 c = 0; c < channels; ++c) {
            f64 acc = 0.0;
            for (int j = j0; j < j1; ++j) {
                acc += row[j] * in.samples[static_cast<usize>(first + j) * channels + c];
            }
            out.samples[static_cast<usize>(n) * channels + c] = static_cast<float>(acc);
        }
    }
    return out;
}

f64 measure_integrated_loudness(const AudioBuffer& buffer) {
    if (!valid_buffer(buffer)) {
        return kAudioSilenceLufs;
    }
    Biquad shelf;
    Biquad highpass;
    k_weighting(buffer.sample_rate, shelf, highpass);
    const u32 channels = buffer.channels;
    const u32 frames = buffer.frames;
    // Weighted K-filtered energy per frame, summed over channels.
    std::vector<f64> energy(frames, 0.0);
    for (u32 c = 0; c < channels; ++c) {
        const f64 w = channel_weight(c, channels);
        if (w == 0.0) {
            continue;
        }
        f64 x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0; // stage 1 state
        f64 z1 = 0.0, z2 = 0.0, v1 = 0.0, v2 = 0.0; // stage 2 state
        for (u32 f = 0; f < frames; ++f) {
            const f64 x = buffer.samples[static_cast<usize>(f) * channels + c];
            const f64 y = shelf.b0 * x + shelf.b1 * x1 + shelf.b2 * x2 - shelf.a1 * y1 - shelf.a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            const f64 v = highpass.b0 * y + highpass.b1 * z1 + highpass.b2 * z2 - highpass.a1 * v1 - highpass.a2 * v2;
            z2 = z1;
            z1 = y;
            v2 = v1;
            v1 = v;
            energy[f] += w * v * v;
        }
    }
    const u32 step = std::max<u32>(1u, static_cast<u32>(std::lround(buffer.sample_rate * 0.1)));
    const u32 block = step * 4u;
    std::vector<f64> blocks;
    if (frames < block) {
        f64 sum = 0.0;
        for (f64 e : energy) {
            sum += e;
        }
        blocks.push_back(sum / frames);
    } else {
        // Sums of 100 ms segments; each 400 ms block is four consecutive segments.
        const u32 segments = frames / step;
        std::vector<f64> seg(segments, 0.0);
        for (u32 s = 0; s < segments; ++s) {
            f64 sum = 0.0;
            for (u32 i = 0; i < step; ++i) {
                sum += energy[static_cast<usize>(s) * step + i];
            }
            seg[s] = sum;
        }
        for (u32 s = 0; s + 4u <= segments; ++s) {
            blocks.push_back((seg[s] + seg[s + 1u] + seg[s + 2u] + seg[s + 3u]) / block);
        }
    }
    // Absolute gate (-70 LUFS), then relative gate (-10 LU below the absolute-gated loudness).
    f64 abs_sum = 0.0;
    usize abs_count = 0;
    for (f64 z : blocks) {
        if (block_loudness(z) > -70.0) {
            abs_sum += z;
            ++abs_count;
        }
    }
    if (abs_count == 0) {
        return kAudioSilenceLufs;
    }
    const f64 relative_gate = block_loudness(abs_sum / static_cast<f64>(abs_count)) - 10.0;
    f64 rel_sum = 0.0;
    usize rel_count = 0;
    for (f64 z : blocks) {
        const f64 l = block_loudness(z);
        if (l > -70.0 && l > relative_gate) {
            rel_sum += z;
            ++rel_count;
        }
    }
    if (rel_count == 0) {
        return kAudioSilenceLufs;
    }
    return block_loudness(rel_sum / static_cast<f64>(rel_count));
}

f32 sample_peak(const AudioBuffer& buffer) {
    f32 peak = 0.0f;
    for (float s : buffer.samples) {
        peak = std::max(peak, std::abs(s));
    }
    return peak;
}

void apply_gain(AudioBuffer& buffer, f64 gain_linear) {
    for (float& s : buffer.samples) {
        s = static_cast<float>(static_cast<f64>(s) * gain_linear);
    }
}

bool limit_peaks(AudioBuffer& buffer, f32 ceiling_linear) {
    if (!valid_buffer(buffer) || ceiling_linear <= 0.0f || sample_peak(buffer) <= ceiling_linear) {
        return false;
    }
    const u32 frames = buffer.frames;
    const u32 channels = buffer.channels;
    const u32 lookahead = std::max<u32>(1u, static_cast<u32>(std::lround(buffer.sample_rate * 0.0015)));
    const f64 release_step = 1.0 / std::max(1.0, buffer.sample_rate * 0.05);
    const f64 ceiling = ceiling_linear;

    // Required gain per frame.
    std::vector<f64> need(frames, 1.0);
    for (u32 f = 0; f < frames; ++f) {
        f64 peak = 0.0;
        for (u32 c = 0; c < channels; ++c) {
            peak = std::max(peak, static_cast<f64>(std::abs(buffer.samples[static_cast<usize>(f) * channels + c])));
        }
        if (peak > ceiling) {
            need[f] = ceiling / peak;
        }
    }
    // m[i] = min(need[i .. i + lookahead]) (monotonic deque, scanning backwards).
    std::vector<f64> gain(frames, 1.0);
    std::vector<u32> deque(frames);
    usize head = 0;
    usize tail = 0;
    for (u32 i = frames; i-- > 0;) {
        while (tail > head && need[deque[tail - 1]] >= need[i]) {
            --tail;
        }
        deque[tail++] = i;
        while (deque[head] > i + lookahead) {
            ++head;
        }
        gain[i] = need[deque[head]];
    }
    // Release: the gain recovers at most `release_step` per frame.
    f64 prev = 1.0;
    for (u32 i = 0; i < frames; ++i) {
        prev = std::min(gain[i], prev + release_step);
        gain[i] = prev;
    }
    // Box smoothing over the look-ahead span. Every frame of the box ending at a peak has that peak in
    // its look-ahead window, so the smoothed gain never exceeds the gain the peak needs.
    f64 running = 0.0;
    std::vector<f64> smooth(frames);
    for (u32 i = 0; i < frames; ++i) {
        running += gain[i];
        if (i >= lookahead) {
            running -= gain[i - lookahead];
            smooth[i] = running / lookahead;
        } else {
            smooth[i] = (running + gain[0] * static_cast<f64>(lookahead - 1u - i)) / lookahead;
        }
    }
    for (u32 f = 0; f < frames; ++f) {
        for (u32 c = 0; c < channels; ++c) {
            float& s = buffer.samples[static_cast<usize>(f) * channels + c];
            f64 v = static_cast<f64>(s) * smooth[f];
            v = std::clamp(v, -ceiling, ceiling); // float rounding guard
            s = static_cast<float>(v);
        }
    }
    return true;
}

void trim_silence(AudioBuffer& buffer, f32 threshold_db, u32 head_pad_frames, u32 tail_pad_frames,
                  u32* trimmed_head, u32* trimmed_tail) {
    if (trimmed_head != nullptr) {
        *trimmed_head = 0;
    }
    if (trimmed_tail != nullptr) {
        *trimmed_tail = 0;
    }
    if (!valid_buffer(buffer)) {
        return;
    }
    const f32 peak = sample_peak(buffer);
    if (peak <= 0.0f) {
        return; // all silent: leave it alone
    }
    const f64 threshold = peak * db_to_linear(threshold_db);
    const u32 channels = buffer.channels;
    auto loud = [&](u32 frame) {
        for (u32 c = 0; c < channels; ++c) {
            if (std::abs(buffer.samples[static_cast<usize>(frame) * channels + c]) > threshold) {
                return true;
            }
        }
        return false;
    };
    u32 first = 0;
    while (first < buffer.frames && !loud(first)) {
        ++first;
    }
    u32 last = buffer.frames - 1u;
    while (last > first && !loud(last)) {
        --last;
    }
    const u32 begin = first > head_pad_frames ? first - head_pad_frames : 0u;
    const u64 end64 = std::min<u64>(static_cast<u64>(last) + 1u + tail_pad_frames, buffer.frames);
    const u32 end = static_cast<u32>(end64);
    if (begin == 0 && end == buffer.frames) {
        return;
    }
    if (trimmed_head != nullptr) {
        *trimmed_head = begin;
    }
    if (trimmed_tail != nullptr) {
        *trimmed_tail = buffer.frames - end;
    }
    buffer.samples.erase(buffer.samples.begin() + static_cast<std::ptrdiff_t>(static_cast<usize>(end) * channels),
                         buffer.samples.end());
    buffer.samples.erase(buffer.samples.begin(),
                         buffer.samples.begin() + static_cast<std::ptrdiff_t>(static_cast<usize>(begin) * channels));
    buffer.frames = end - begin;
}

bool make_seamless_loop(AudioBuffer& buffer, u32 crossfade_frames, u32* used_crossfade) {
    if (used_crossfade != nullptr) {
        *used_crossfade = 0;
    }
    if (!valid_buffer(buffer) || buffer.frames < 64u || crossfade_frames == 0) {
        return false;
    }
    const u32 channels = buffer.channels;
    const u32 search = std::max<u32>(1u, buffer.sample_rate / 50u); // 20 ms
    // Head: first rising zero crossing within the search window.
    u32 start = 0;
    for (u32 f = 1; f < std::min(search, buffer.frames / 8u); ++f) {
        if (rising_zero_crossing(buffer, f)) {
            start = f;
            break;
        }
    }
    const u32 usable = buffer.frames - start;
    const u32 crossfade = std::min(crossfade_frames, usable / 4u);
    if (crossfade == 0) {
        return false;
    }
    // Tail cut: rising zero crossing nearest to frames - crossfade, never leaving less than the crossfade.
    const u32 ideal = buffer.frames - crossfade;
    const u32 lowest = start + 2u * crossfade;
    u32 cut = ideal;
    for (u32 d = 0; d <= search; ++d) {
        if (ideal >= d && ideal - d >= lowest && rising_zero_crossing(buffer, ideal - d)) {
            cut = ideal - d;
            break;
        }
        if (ideal + d < buffer.frames && d > 0 && rising_zero_crossing(buffer, ideal + d)) {
            cut = ideal + d;
            break;
        }
    }
    const u32 fade = std::min(crossfade, buffer.frames - cut);
    if (fade == 0 || cut <= start + fade) {
        return false;
    }
    // Crossfade law from the head/tail correlation: equal gain for coherent material (tones), equal
    // power for uncorrelated material (noise beds).
    f64 dot = 0.0, head_energy = 0.0, tail_energy = 0.0;
    for (u32 i = 0; i < fade; ++i) {
        for (u32 c = 0; c < channels; ++c) {
            const f64 h = buffer.samples[static_cast<usize>(start + i) * channels + c];
            const f64 t = buffer.samples[static_cast<usize>(cut + i) * channels + c];
            dot += h * t;
            head_energy += h * h;
            tail_energy += t * t;
        }
    }
    const f64 denom = std::sqrt(head_energy * tail_energy);
    const bool coherent = denom > 0.0 && dot / denom > 0.5;

    AudioBuffer looped;
    looped.channels = channels;
    looped.sample_rate = buffer.sample_rate;
    looped.frames = cut - start;
    looped.samples.resize(static_cast<usize>(looped.frames) * channels);
    for (u32 i = 0; i < looped.frames; ++i) {
        f64 head_w = 1.0;
        f64 tail_w = 0.0;
        if (i < fade) {
            const f64 x = static_cast<f64>(i) / static_cast<f64>(fade);
            if (coherent) {
                head_w = x;
                tail_w = 1.0 - x;
            } else {
                head_w = std::sin(0.5 * kPi * x);
                tail_w = std::cos(0.5 * kPi * x);
            }
        }
        for (u32 c = 0; c < channels; ++c) {
            f64 v = head_w * buffer.samples[static_cast<usize>(start + i) * channels + c];
            if (tail_w != 0.0) {
                v += tail_w * buffer.samples[static_cast<usize>(cut + i) * channels + c];
            }
            looped.samples[static_cast<usize>(i) * channels + c] = static_cast<float>(v);
        }
    }
    buffer = std::move(looped);
    if (used_crossfade != nullptr) {
        *used_crossfade = fade;
    }
    return true;
}

f32 loop_seam_ratio(const AudioBuffer& buffer) {
    if (!valid_buffer(buffer) || buffer.frames < 4u) {
        return 0.0f;
    }
    const u32 channels = buffer.channels;
    const u32 window = std::min<u32>(64u, buffer.frames / 2u);
    f64 worst = 0.0;
    for (u32 c = 0; c < channels; ++c) {
        auto at = [&](u32 f) { return static_cast<f64>(buffer.samples[static_cast<usize>(f) * channels + c]); };
        const f64 jump = std::abs(at(0) - at(buffer.frames - 1u));
        f64 local = 0.0;
        for (u32 i = 1; i < window; ++i) {
            local = std::max(local, std::abs(at(i) - at(i - 1u)));
            const u32 t = buffer.frames - i;
            local = std::max(local, std::abs(at(t) - at(t - 1u)));
        }
        const f64 ratio = jump <= 1e-6 ? 0.0 : jump / std::max(local, 1e-6);
        worst = std::max(worst, ratio);
    }
    return static_cast<f32>(worst);
}

bool encode_vorbis_stream(const AudioBuffer& buffer, f32 quality, std::vector<u8>& out, u32 serial) {
    out.clear();
#if defined(FUSE_COOK_HAS_VORBIS)
    if (!valid_buffer(buffer) || buffer.channels > 255) {
        return false;
    }
    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, static_cast<long>(buffer.channels), static_cast<long>(buffer.sample_rate),
                               std::clamp(quality, -0.1f, 1.0f))
        != 0) {
        vorbis_info_clear(&vi);
        return false;
    }
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "FUSE fuse_cook");
    vorbis_dsp_state vd;
    vorbis_block vb;
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);
    ogg_stream_state os;
    ogg_stream_init(&os, static_cast<int>(serial));

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
        append_page(page); // audio starts on a fresh page (Vorbis I spec)
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
    const u32 channels = buffer.channels;
    for (u32 offset = 0; offset < buffer.frames; offset += kChunk) {
        const u32 count = std::min(kChunk, buffer.frames - offset);
        float** analysis = vorbis_analysis_buffer(&vd, static_cast<int>(count));
        for (u32 i = 0; i < count; ++i) {
            for (u32 c = 0; c < channels; ++c) {
                analysis[c][i] = buffer.samples[static_cast<usize>(offset + i) * channels + c];
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
    (void)buffer;
    (void)quality;
    (void)serial;
    return false;
#endif
}

// ---- full chain ---------------------------------------------------------------------------------------

CookStubWriteResult cook_audio_buffer(AudioBuffer buffer, const std::string& output_path,
                                      const AudioCookOptions& options, AudioCookReport* report) {
    CookStubWriteResult result;
    AudioCookReport local;
    AudioCookReport& rep = report != nullptr ? *report : local;
    if (output_path.empty()) {
        result.note = "ogg missing output path";
        result.failure = CookFailure::InvalidArgument;
        return result;
    }
    if (!valid_buffer(buffer)) {
        result.note = "ogg source has no audio frames";
        result.failure = CookFailure::MalformedSource;
        return result;
    }
    if (rep.source_rate == 0) {
        rep.source_rate = buffer.sample_rate;
        rep.source_channels = buffer.channels;
        rep.source_frames = buffer.frames;
    }
    if (options.format == AudioCookFormat::OggVorbis && !audio_cook_vorbis_available()) {
        result.note = "ogg encoder unavailable (vendored libvorbis not built)";
        result.failure = CookFailure::ImporterUnavailable;
        return result;
    }

    if (options.force_mono) {
        downmix_to_mono(buffer);
    }
    if (options.target_sample_rate != 0 && options.target_sample_rate != buffer.sample_rate) {
        buffer = resample_audio(buffer, options.target_sample_rate);
    }
    const u32 rate = buffer.sample_rate;
    if (options.trim_silence) {
        const u32 head_pad = static_cast<u32>(static_cast<u64>(rate) * options.trim_head_pad_ms / 1000u);
        const u32 tail_pad = static_cast<u32>(static_cast<u64>(rate) * options.trim_tail_pad_ms / 1000u);
        trim_silence(buffer, options.trim_threshold_db, head_pad, tail_pad, &rep.trimmed_head_frames,
                     &rep.trimmed_tail_frames);
    }
    rep.resolved_class = options.audio_class;
    if (rep.resolved_class == AudioClass::Auto) {
        const f64 seconds = static_cast<f64>(buffer.frames) / static_cast<f64>(rate);
        rep.resolved_class = seconds >= kAudioAutoBedSeconds ? AudioClass::Bed : AudioClass::OneShot;
    }
    const bool bed = rep.resolved_class == AudioClass::Bed;

    if (options.make_loop) {
        const u32 crossfade = static_cast<u32>(static_cast<u64>(rate) * options.loop_crossfade_ms / 1000u);
        rep.looped = make_seamless_loop(buffer, crossfade, &rep.loop_crossfade_frames);
        if (!rep.looped) {
            result.note = "ogg loop: clip too short for a seamless loop";
            result.failure = CookFailure::MalformedSource;
            return result;
        }
    }

    rep.source_lufs = measure_integrated_loudness(buffer);
    const f64 ceiling = db_to_linear(options.peak_ceiling_dbfs);
    if (options.normalise == AudioNormalise::Loudness) {
        rep.target_lufs = options.target_lufs != 0.0f ? options.target_lufs : (bed ? kAudioBedLufs : kAudioOneShotLufs);
        if (rep.source_lufs > kAudioSilenceLufs) {
            // Gain to the target, the limiter holds the ceiling. Limiting costs loudness, so re-measure and
            // raise the gain (secant steps on measured loudness vs gain; bounded passes and +24 dB of make-up).
            const f64 base_db = rep.target_lufs - rep.source_lufs;
            f64 gain_db = base_db;
            f64 prev_gain_db = 0.0;
            f64 prev_measured = 0.0;
            bool have_prev = false;
            AudioBuffer work;
            for (int pass = 0; pass < 12; ++pass) {
                work = buffer;
                apply_gain(work, db_to_linear(gain_db));
                const bool limited = limit_peaks(work, static_cast<f32>(ceiling));
                rep.peak_limited = rep.peak_limited || limited;
                const f64 measured = measure_integrated_loudness(work);
                if (!limited || std::abs(measured - rep.target_lufs) <= 0.05) {
                    break;
                }
                f64 slope = 1.0;
                if (have_prev && std::abs(gain_db - prev_gain_db) > 1e-6) {
                    slope = (measured - prev_measured) / (gain_db - prev_gain_db);
                }
                slope = std::clamp(slope, 0.05, 1.0);
                prev_gain_db = gain_db;
                prev_measured = measured;
                have_prev = true;
                gain_db = std::clamp(gain_db + (rep.target_lufs - measured) / slope, base_db - 24.0, base_db + 24.0);
                if (std::abs(gain_db - prev_gain_db) < 1e-4) {
                    break;
                }
            }
            rep.gain_db = gain_db;
            buffer = std::move(work);
        }
    } else if (options.normalise == AudioNormalise::Peak) {
        const f32 peak = sample_peak(buffer);
        if (peak > 0.0f) {
            const f64 gain = db_to_linear(options.peak_target_dbfs) / peak;
            rep.gain_db = 20.0 * std::log10(gain);
            apply_gain(buffer, gain);
        }
    }
    rep.output_lufs = measure_integrated_loudness(buffer);
    rep.output_peak_dbfs = linear_to_dbfs(sample_peak(buffer));
    if (rep.looped) {
        rep.loop_seam_ratio = loop_seam_ratio(buffer);
        rep.loop_seam_click = rep.loop_seam_ratio > kAudioLoopSeamClickRatio;
    }
    rep.output_rate = buffer.sample_rate;
    rep.output_channels = buffer.channels;
    rep.output_frames = buffer.frames;

    std::vector<u8> payload;
    const bool ogg = options.format == AudioCookFormat::OggVorbis;
    if (ogg) {
        rep.ogg_quality = options.ogg_quality >= -0.1f ? std::min(options.ogg_quality, 1.0f)
                                                       : (bed ? kAudioBedVorbisQuality : kAudioOneShotVorbisQuality);
        if (!encode_vorbis_stream(buffer, rep.ogg_quality, payload)) {
            result.note = "ogg vorbis encode failed";
            result.failure = CookFailure::WriteFailed;
            return result;
        }
    } else {
        payload.resize(buffer.samples.size() * sizeof(float));
        for (usize i = 0; i < buffer.samples.size(); ++i) {
            const u32 bits = std::bit_cast<u32>(buffer.samples[i]); // little-endian on every host
            payload[i * 4 + 0] = static_cast<u8>(bits);
            payload[i * 4 + 1] = static_cast<u8>(bits >> 8u);
            payload[i * 4 + 2] = static_cast<u8>(bits >> 16u);
            payload[i * 4 + 3] = static_cast<u8>(bits >> 24u);
        }
    }
    rep.payload_bytes = static_cast<u32>(payload.size());

    // `.fuseaudio` header (fuse::audio::parse_fuseaudio reads rate/channels/samples/loop_*).
    std::string header = ogg ? "FUSEAUDIO_OGG\n" : "FUSEAUDIO_PCM_F32\n";
    header += ogg ? "hook=ogg\n" : "hook=pcm_f32\n";
    header += "rate=" + std::to_string(buffer.sample_rate) + "\n";
    header += "channels=" + std::to_string(buffer.channels) + "\n";
    header += "samples=" + std::to_string(buffer.frames) + "\n";
    if (rep.looped) {
        header += "loop_start=0\n";
        header += "loop_end=" + std::to_string(buffer.frames) + "\n";
    }
    header += ogg ? "format=ogg\n" : "format=pcm_f32\n";
    header += ogg ? "encoder=vorbisenc_vbr_q" + format_fixed(rep.ogg_quality * 10.0f) + "\n" : "encoder=none\n";
    header += std::string("class=") + audio_class_name(rep.resolved_class) + "\n";
    header += std::string("source=") + audio_source_format_name(rep.source_format) + "\n";
    header += "source_rate=" + std::to_string(rep.source_rate) + "\n";
    header += std::string("normalise=")
            + (options.normalise == AudioNormalise::Loudness ? "loudness"
                                                             : (options.normalise == AudioNormalise::Peak ? "peak" : "none"))
            + "\n";
    header += "loudness_lufs=" + format_fixed(rep.output_lufs) + "\n";
    header += "gain_db=" + format_fixed(rep.gain_db) + "\n";
    header += "peak_dbfs=" + format_fixed(rep.output_peak_dbfs) + "\n";
    header += std::string("limited=") + (rep.peak_limited ? "yes" : "no") + "\n";
    header += "DATA\n";

    std::vector<u8> bytes(header.begin(), header.end());
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if (!write_file_bytes(output_path, bytes)) {
        result.note = "unable to write audio output";
        result.failure = CookFailure::WriteFailed;
        return result;
    }
    result.ok = true;
    result.byteCount = static_cast<u32>(bytes.size());
    result.note = std::string(ogg ? "ogg vorbis q" + format_fixed(rep.ogg_quality * 10.0f) : "pcm_f32") + ", "
                + std::to_string(buffer.frames) + " frames @ " + std::to_string(buffer.sample_rate) + " Hz, "
                + audio_class_name(rep.resolved_class) + ", " + format_fixed(rep.output_lufs) + " LUFS";
    if (rep.looped) {
        result.note += ", loop seam " + format_fixed(rep.loop_seam_ratio);
    }
    return result;
}

CookStubWriteResult cook_audio_file(const std::string& input_path, const std::string& output_path,
                                    const AudioCookOptions& options, AudioCookReport* report) {
    CookStubWriteResult result;
    if (input_path.empty() || output_path.empty()) {
        result.note = "ogg missing input or output path";
        result.failure = CookFailure::InvalidArgument;
        return result;
    }
    AudioCookReport local;
    AudioCookReport& rep = report != nullptr ? *report : local;
    rep = {};
    AudioBuffer buffer;
    std::string error;
    if (!decode_audio_file(input_path, buffer, &error, &rep.source_format)) {
        result.note = "ogg decode failed: " + error;
        result.failure = error == "source unreadable" ? CookFailure::InvalidArgument : CookFailure::MalformedSource;
        if (rep.source_format == AudioSourceFormat::Flac && !audio_cook_flac_available()) {
            result.failure = CookFailure::ImporterUnavailable;
        }
        return result;
    }
    rep.source_rate = buffer.sample_rate;
    rep.source_channels = buffer.channels;
    rep.source_frames = buffer.frames;
    return cook_audio_buffer(std::move(buffer), output_path, options, &rep);
}

} // namespace fuse::cook
