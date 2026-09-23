#include <fuse/audio/audio_clip.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace fuse::audio {

namespace {

constexpr u16 kWaveFormatPcm = 0x0001;
constexpr u16 kWaveFormatFloat = 0x0003;
constexpr u16 kWaveFormatExtensible = 0xFFFE;

bool read_exact(FILE* file, void* dst, usize bytes) {
    return std::fread(dst, 1, bytes, file) == bytes;
}

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

    // RIFF chunk walk: fmt and data may appear in any order with arbitrary chunks
    // (LIST, fact, ...) between them; odd-sized chunks carry one pad byte.
    unsigned char riff[12];
    if (!read_exact(file, riff, sizeof(riff)) || std::memcmp(riff, "RIFF", 4) != 0
        || std::memcmp(riff + 8, "WAVE", 4) != 0) {
        std::fclose(file);
        return false;
    }

    WavFormat fmt;
    bool have_fmt = false;
    std::vector<unsigned char> data;
    bool have_data = false;

    unsigned char chunk_header[8];
    while (!have_data && read_exact(file, chunk_header, sizeof(chunk_header))) {
        const u32 chunk_size = read_u32_le(chunk_header + 4);
        if (std::memcmp(chunk_header, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                break;
            }
            std::vector<unsigned char> body(chunk_size);
            if (!read_exact(file, body.data(), body.size())) {
                break;
            }
            fmt.format = read_u16_le(body.data());
            fmt.channels = read_u16_le(body.data() + 2);
            fmt.sample_rate = read_u32_le(body.data() + 4);
            fmt.block_align = read_u16_le(body.data() + 12);
            fmt.bits_per_sample = read_u16_le(body.data() + 14);
            if (fmt.format == kWaveFormatExtensible && chunk_size >= 26) {
                // SubFormat GUID starts at offset 24; its first two bytes are the format tag.
                fmt.format = read_u16_le(body.data() + 24);
            }
            have_fmt = true;
        } else if (std::memcmp(chunk_header, "data", 4) == 0) {
            if (!have_fmt) {
                break;
            }
            data.resize(chunk_size);
            // Tolerate truncated files (common with streaming writers): keep what was read.
            data.resize(std::fread(data.data(), 1, chunk_size, file));
            have_data = true;
            break;
        } else {
            if (std::fseek(file, static_cast<long>(chunk_size), SEEK_CUR) != 0) {
                break;
            }
        }
        if ((chunk_size & 1u) != 0u) {
            std::fseek(file, 1, SEEK_CUR);
        }
    }
    std::fclose(file);

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

} // namespace fuse::audio
