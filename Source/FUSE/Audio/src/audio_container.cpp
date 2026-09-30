#include <fuse/audio/audio_container.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace fuse::audio {

namespace {

constexpr std::string_view kMagicPrefix = "FUSEAUDIO_";

bool parse_u32(std::string_view text, u32& out) {
    if (text.empty() || text.size() > 10) {
        return false;
    }
    std::uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10u + static_cast<std::uint64_t>(c - '0');
    }
    if (value > UINT32_MAX) {
        return false;
    }
    out = static_cast<u32>(value);
    return true;
}

} // namespace

bool is_fuseaudio(const u8* bytes, usize size) {
    return bytes != nullptr && size >= kMagicPrefix.size()
        && std::memcmp(bytes, kMagicPrefix.data(), kMagicPrefix.size()) == 0;
}

bool parse_fuseaudio(const u8* bytes, usize size, FuseAudioContainer& out) {
    out = {};
    if (!is_fuseaudio(bytes, size)) {
        return false;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes), size);
    usize pos = 0;
    bool first = true;
    bool have_data = false;
    bool have_kind = false;
    // The header is short; cap the scan so a binary blob without DATA is rejected quickly.
    constexpr usize kMaxHeader = 4096;
    while (pos < size && pos < kMaxHeader) {
        const usize eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            return false;
        }
        std::string_view line = text.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        pos = eol + 1;
        if (first) {
            first = false;
            if (line == "FUSEAUDIO_OGG") {
                out.payload = FuseAudioPayload::Ogg;
            } else if (line == "FUSEAUDIO_PCM_F32") {
                out.payload = FuseAudioPayload::PcmF32;
            } else if (line == "FUSEAUDIO_FLAC") {
                out.payload = FuseAudioPayload::Flac;
            } else {
                return false; // FUSEAUDIO_STUB or unknown
            }
            have_kind = true;
            continue;
        }
        if (line == "DATA") {
            have_data = true;
            break;
        }
        const usize eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, eq);
        const std::string_view value = line.substr(eq + 1);
        u32 parsed = 0;
        if (!parse_u32(value, parsed)) {
            continue; // non-numeric keys (format=, encoder=, ...) are informational
        }
        if (key == "rate") {
            out.sample_rate = parsed;
        } else if (key == "channels") {
            out.channels = parsed;
        } else if (key == "samples" || key == "frames") {
            out.frames = parsed;
        } else if (key == "loop_start") {
            out.loop_start = parsed;
        } else if (key == "loop_end") {
            out.loop_end = parsed;
        }
    }
    if (!have_kind || !have_data || pos > size) {
        return false;
    }
    out.data = bytes + pos;
    out.size = size - pos;
    if (out.size == 0) {
        return false;
    }
    if (out.payload == FuseAudioPayload::PcmF32) {
        if (out.channels == 0 || out.sample_rate == 0 || out.frames == 0
            || out.size != static_cast<usize>(out.frames) * out.channels * sizeof(float)) {
            return false;
        }
    }
    return true;
}

std::vector<u8> write_fuseaudio(FuseAudioPayload payload, u32 sample_rate, u32 channels, u32 frames,
                                const u8* data, usize size, u32 loop_start, u32 loop_end) {
    std::string header;
    switch (payload) {
    case FuseAudioPayload::Ogg:
        header = "FUSEAUDIO_OGG\n";
        break;
    case FuseAudioPayload::PcmF32:
        header = "FUSEAUDIO_PCM_F32\n";
        break;
    case FuseAudioPayload::Flac:
        header = "FUSEAUDIO_FLAC\n";
        break;
    }
    header += "rate=" + std::to_string(sample_rate) + "\n";
    header += "channels=" + std::to_string(channels) + "\n";
    header += "samples=" + std::to_string(frames) + "\n";
    if (loop_start != 0 || loop_end != 0) {
        header += "loop_start=" + std::to_string(loop_start) + "\n";
        header += "loop_end=" + std::to_string(loop_end) + "\n";
    }
    header += "DATA\n";
    std::vector<u8> out(header.begin(), header.end());
    if (data != nullptr && size > 0) {
        out.insert(out.end(), data, data + size);
    }
    return out;
}

} // namespace fuse::audio
