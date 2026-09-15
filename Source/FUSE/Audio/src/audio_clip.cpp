#include <fuse/audio/audio_clip.hpp>

#include <cstdio>
#include <cstring>
#include <vector>

namespace fuse::audio {

namespace {

struct WavHeader {
    char riff[4];
    u32 chunk_size;
    char wave[4];
    char fmt[4];
    u32 fmt_size;
    u16 audio_format;
    u16 num_channels;
    u32 sample_rate;
    u32 byte_rate;
    u16 block_align;
    u16 bits_per_sample;
};

bool read_exact(FILE* file, void* dst, usize bytes) {
    return std::fread(dst, 1, bytes, file) == bytes;
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

    WavHeader header{};
    if (!read_exact(file, &header, sizeof(header))) {
        std::fclose(file);
        return false;
    }

    if (std::strncmp(header.riff, "RIFF", 4) != 0 || std::strncmp(header.wave, "WAVE", 4) != 0) {
        std::fclose(file);
        return false;
    }

    while (std::strncmp(header.fmt, "fmt ", 4) != 0) {
        char chunk_id[4];
        u32 chunk_size = 0;
        if (!read_exact(file, chunk_id, 4) || !read_exact(file, &chunk_size, 4)) {
            std::fclose(file);
            return false;
        }
        std::fseek(file, chunk_size, SEEK_CUR);
        if (!read_exact(file, header.fmt, 4)) {
            std::fclose(file);
            return false;
        }
    }

    if (header.fmt_size > 16) {
        std::fseek(file, static_cast<long>(header.fmt_size - 16), SEEK_CUR);
    }

    char chunk_id[4];
    u32 data_size = 0;
    while (std::fread(chunk_id, 1, 4, file) == 4) {
        if (!read_exact(file, &data_size, 4)) {
            break;
        }
        if (std::strncmp(chunk_id, "data", 4) == 0) {
            break;
        }
        std::fseek(file, data_size, SEEK_CUR);
    }

    if (data_size == 0 || header.num_channels == 0 || header.sample_rate == 0) {
        std::fclose(file);
        return false;
    }

    const u32 bytes_per_sample = static_cast<u32>(header.bits_per_sample / 8);
    const u32 frame_count = data_size / (bytes_per_sample * header.num_channels);
    std::vector<float> pcm(static_cast<usize>(frame_count) * header.num_channels);

    if (header.bits_per_sample == 16) {
        std::vector<std::int16_t> raw(static_cast<usize>(frame_count) * header.num_channels);
        if (!read_exact(file, raw.data(), raw.size() * sizeof(std::int16_t))) {
            std::fclose(file);
            return false;
        }
        for (usize i = 0; i < raw.size(); ++i) {
            pcm[i] = static_cast<float>(raw[i]) / 32768.f;
        }
    } else if (header.bits_per_sample == 32 && header.audio_format == 3) {
        if (!read_exact(file, pcm.data(), pcm.size() * sizeof(float))) {
            std::fclose(file);
            return false;
        }
    } else {
        std::fclose(file);
        return false;
    }

    std::fclose(file);
    return load_from_pcm(pcm.data(), frame_count, header.num_channels, header.sample_rate);
}

} // namespace fuse::audio
