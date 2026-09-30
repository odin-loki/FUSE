#include <fuse/audio/audio_clip.hpp>

#include <fuse/audio/audio_codec.hpp>
#include <fuse/audio/audio_container.hpp>
#include <fuse/io/vfs.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <fstream>
#include <string>
#include <cstdio>
#include <cstring>
#include <vector>

namespace fuse::audio {

namespace {

constexpr u16 kWaveFormatPcm = 0x0001;
constexpr u16 kWaveFormatFloat = 0x0003;
constexpr u16 kWaveFormatExtensible = 0xFFFE;

u16 read_u16_le(const unsigned char* p) {
    return static_cast<u16>(p[0] | (p[1] << 8));
}

u32 read_u32_le(const unsigned char* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16)
        | (static_cast<u32>(p[3]) << 24);
}

struct WavFormat {
    u16 format = 0;
    u16 channels = 0;
    u32 sample_rate = 0;
    u16 block_align = 0;
    u16 bits_per_sample = 0;
};

/// Decode little-endian samples to [-1, 1). Returns false for unsupported encodings.
bool decode_samples(const WavFormat& fmt, const std::vector<unsigned char>& raw, usize sample_count,
                    std::vector<float>& out) {
    out.resize(sample_count);
    const u32 bytes = fmt.bits_per_sample / 8u;
    for (usize i = 0; i < sample_count; ++i) {
        const unsigned char* p = raw.data() + i * bytes;
        if (fmt.format == kWaveFormatFloat) {
            if (fmt.bits_per_sample != 32) {
                return false;
            }
            float value = 0.f;
            std::memcpy(&value, p, sizeof(float));
            out[i] = value;
            continue;
        }
        switch (fmt.bits_per_sample) {
        case 8:
            out[i] = (static_cast<float>(p[0]) - 128.f) / 128.f;
            break;
        case 16:
            out[i] = static_cast<float>(static_cast<std::int16_t>(read_u16_le(p))) / 32768.f;
            break;
        case 24: {
            std::int32_t v = static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
            if ((v & 0x800000) != 0) {
                v -= 0x1000000;
            }
            out[i] = static_cast<float>(v) / 8388608.f;
            break;
        }
        case 32:
            out[i] = static_cast<float>(static_cast<double>(static_cast<std::int32_t>(read_u32_le(p)))
                                        / 2147483648.0);
            break;
        default:
            return false;
        }
    }
    return true;
}

} // namespace

bool AudioClip::load_from_pcm(const float* interleaved, u32 frame_count, u32 channels, u32 rate) {
    if (interleaved == nullptr || frame_count == 0 || channels == 0 || rate == 0) {
        return false;
    }

    samples.assign(interleaved, interleaved + static_cast<usize>(frame_count) * channels);
    stream.reset();
    loop_start = 0;
    loop_end = 0;
    sample_rate = rate;
    channel_count = channels;
    duration = static_cast<float>(frame_count) / static_cast<float>(rate);
    return true;
}

bool AudioClip::load_wav(const char* path) {
    if (path == nullptr) {
        return false;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    std::vector<unsigned char> bytes;
    unsigned char chunk[16384];
    for (;;) {
        const usize got = std::fread(chunk, 1, sizeof(chunk), file);
        bytes.insert(bytes.end(), chunk, chunk + got);
        if (got < sizeof(chunk)) {
            break;
        }
    }
    std::fclose(file);
    return load_wav_memory(bytes.data(), bytes.size());
}

bool AudioClip::load_wav_memory(const u8* bytes, usize size) {
    if (bytes == nullptr) {
        return false;
    }

    // RIFF chunk walk: fmt and data may appear in any order with arbitrary chunks
    // (LIST, fact, ...) between them; odd-sized chunks carry one pad byte.
    if (size < 12 || std::memcmp(bytes, "RIFF", 4) != 0 || std::memcmp(bytes + 8, "WAVE", 4) != 0) {
        return false;
    }

    WavFormat fmt;
    bool have_fmt = false;
    std::vector<unsigned char> data;
    bool have_data = false;

    usize pos = 12;
    while (!have_data && pos + 8 <= size) {
        const unsigned char* chunk_header = bytes + pos;
        const u32 chunk_size = read_u32_le(chunk_header + 4);
        pos += 8;
        if (std::memcmp(chunk_header, "fmt ", 4) == 0) {
            if (chunk_size < 16 || pos + chunk_size > size) {
                break;
            }
            const unsigned char* body = bytes + pos;
            fmt.format = read_u16_le(body);
            fmt.channels = read_u16_le(body + 2);
            fmt.sample_rate = read_u32_le(body + 4);
            fmt.block_align = read_u16_le(body + 12);
            fmt.bits_per_sample = read_u16_le(body + 14);
            if (fmt.format == kWaveFormatExtensible && chunk_size >= 26) {
                // SubFormat GUID starts at offset 24; its first two bytes are the format tag.
                fmt.format = read_u16_le(body + 24);
            }
            have_fmt = true;
        } else if (std::memcmp(chunk_header, "data", 4) == 0) {
            if (!have_fmt) {
                break;
            }
            // Tolerate truncated files (common with streaming writers, and the 0xFFFFFFFF size a
            // writer leaves until it finalises the header): keep what is there.
            const usize avail = std::min<usize>(chunk_size, size - pos);
            data.assign(bytes + pos, bytes + pos + avail);
            have_data = true;
            break;
        }
        if (static_cast<usize>(chunk_size) > size - pos) {
            break;
        }
        pos += chunk_size;
        if ((chunk_size & 1u) != 0u) {
            ++pos;
        }
    }

    if (!have_fmt || !have_data || fmt.channels == 0 || fmt.sample_rate == 0
        || (fmt.format != kWaveFormatPcm && fmt.format != kWaveFormatFloat)
        || fmt.bits_per_sample == 0 || (fmt.bits_per_sample % 8) != 0
        || fmt.bits_per_sample > 32) {
        return false;
    }

    const u32 bytes_per_frame = static_cast<u32>(fmt.bits_per_sample / 8) * fmt.channels;
    const u32 frame_count = static_cast<u32>(data.size() / bytes_per_frame);
    if (frame_count == 0) {
        return false;
    }

    std::vector<float> pcm;
    if (!decode_samples(fmt, data, static_cast<usize>(frame_count) * fmt.channels, pcm)) {
        return false;
    }
    return load_from_pcm(pcm.data(), frame_count, fmt.channels, fmt.sample_rate);
}

bool AudioClip::load_ogg(const u8* data, usize size, bool streaming) {
    if (streaming) {
        u32 frames = 0;
        u32 channels = 0;
        u32 rate = 0;
        if (!probe_ogg_vorbis(data, size, frames, channels, rate)) {
            return false;
        }
        auto shared = std::make_shared<AudioStreamData>();
        shared->encoded.assign(data, data + size);
        shared->frames = frames;
        shared->channels = channels;
        shared->sample_rate = rate;
        samples.clear();
        samples.shrink_to_fit();
        stream = std::move(shared);
        sample_rate = rate;
        channel_count = channels;
        duration = static_cast<float>(frames) / static_cast<float>(rate);
        loop_start = 0;
        loop_end = 0;
        return true;
    }
    DecodedAudio decoded;
    if (!decode_ogg_vorbis(data, size, decoded)) {
        return false;
    }
    return load_from_pcm(decoded.samples.data(), decoded.frames, decoded.channels, decoded.sample_rate);
}

bool AudioClip::load_flac(const u8* data, usize size) {
    DecodedAudio decoded;
    if (!decode_flac(data, size, decoded)) {
        return false;
    }
    return load_from_pcm(decoded.samples.data(), decoded.frames, decoded.channels, decoded.sample_rate);
}

bool AudioClip::load_fuseaudio(const u8* data, usize size, const AudioClipLoadOptions& options) {
    FuseAudioContainer container;
    if (!parse_fuseaudio(data, size, container)) {
        return false;
    }
    bool ok = false;
    switch (container.payload) {
    case FuseAudioPayload::Ogg: {
        u32 frames = container.frames;
        u32 channels = container.channels;
        u32 rate = container.sample_rate;
        if (!probe_ogg_vorbis(container.data, container.size, frames, channels, rate)) {
            return false;
        }
        const bool streaming = options.stream_threshold_seconds > 0.f
            && static_cast<double>(frames) > static_cast<double>(options.stream_threshold_seconds) * rate;
        ok = load_ogg(container.data, container.size, streaming);
        break;
    }
    case FuseAudioPayload::PcmF32: {
        static_assert(std::endian::native == std::endian::little,
                      "FUSEAUDIO_PCM_F32 payloads are little-endian f32; add a byte swap for this target");
        std::vector<float> pcm(static_cast<usize>(container.frames) * container.channels);
        std::memcpy(pcm.data(), container.data, pcm.size() * sizeof(float));
        ok = load_from_pcm(pcm.data(), container.frames, container.channels, container.sample_rate);
        break;
    }
    case FuseAudioPayload::Flac:
        ok = load_flac(container.data, container.size);
        break;
    }
    if (ok) {
        loop_start = container.loop_start;
        loop_end = container.loop_end;
    }
    return ok;
}

bool AudioClip::load_memory(const u8* data, usize size, const AudioClipLoadOptions& options) {
    if (data == nullptr || size < 4) {
        return false;
    }
    if (is_fuseaudio(data, size)) {
        return load_fuseaudio(data, size, options);
    }
    if (std::memcmp(data, "RIFF", 4) == 0) {
        return load_wav_memory(data, size);
    }
    if (std::memcmp(data, "OggS", 4) == 0) {
        u32 frames = 0;
        u32 channels = 0;
        u32 rate = 0;
        const bool streaming = options.stream_threshold_seconds > 0.f
            && probe_ogg_vorbis(data, size, frames, channels, rate)
            && static_cast<double>(frames) > static_cast<double>(options.stream_threshold_seconds) * rate;
        return load_ogg(data, size, streaming);
    }
    if (std::memcmp(data, "fLaC", 4) == 0) {
        return load_flac(data, size);
    }
    return false;
}

bool AudioClip::load_file(const char* path, const AudioClipLoadOptions& options) {
    if (path == nullptr || *path == '\0') {
        return false;
    }
    std::vector<u8> bytes;
    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();
    std::string physical;
    if (!vfs.resolve(path, physical) || !vfs.readFileSync(path, bytes)) {
        bytes.clear();
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return false;
        }
        in.seekg(0, std::ios::end);
        const std::streamoff length = in.tellg();
        if (length <= 0) {
            return false;
        }
        in.seekg(0, std::ios::beg);
        bytes.resize(static_cast<usize>(length));
        in.read(reinterpret_cast<char*>(bytes.data()), length);
        if (!in) {
            return false;
        }
    }
    return load_memory(bytes.data(), bytes.size(), options);
}

} // namespace fuse::audio
