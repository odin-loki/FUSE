// Audio cook chain gate (MP-B7.9-AUDIO-IMPORT / AP-W8.3 / UNI-U7-AUDIO-1; FUSE_ASSET_PLAN §5 `asset_audio`):
//   * decode WAV 8/16/24/32-bit int, 32-bit float, WAVE_FORMAT_EXTENSIBLE, native FLAC, Ogg Vorbis
//   * resample accuracy (sine frequency / amplitude error after 44.1 -> 48 kHz and 96 -> 48 kHz)
//   * BS.1770 integrated loudness vs an independent reference implemented here (48 kHz Table 1/2
//     coefficients, direct block sums) and the EBU Tech 3341 test signals; normalised cooks land
//     within +-1 LU of the -23 / -16 LUFS targets (PCM and after Vorbis encode + decode)
//   * peak ceiling through the limiter, silence trim
//   * Vorbis encode + decode round-trip SNR
//   * loop-seam continuity (zero-crossing crossfade), also after Vorbis encode + decode
//   * byte-identical output across two cooks
//   * the `.fuseaudio` header parses in the runtime (fuse::audio::parse_fuseaudio) when fuse_audio is built
//   * `fuse_cook --audio` CLI (argv[1] = fuse_cook path)
#include <fuse/cook/audio_cook.hpp>
#include <fuse/cook/cook_stub_writer.hpp>

#if defined(FUSE_AUDIO_COOK_TEST_HAS_RUNTIME)
#include <fuse/audio/audio_codec.hpp>
#include <fuse/audio/audio_container.hpp>
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

namespace {

using namespace fuse;
using namespace fuse::cook;

constexpr double kPi = std::numbers::pi;
int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

std::filesystem::path temp_dir() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fuse_asset_audio_cook";
    std::filesystem::create_directories(dir);
    return dir;
}

std::string temp_path(const std::string& name) {
    return (temp_dir() / name).string();
}

std::vector<u8> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const std::vector<u8>& bytes) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
}

std::string fmt(double v) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.4f", v);
    return text;
}

// ---- signal generators --------------------------------------------------------------------------------

AudioBuffer make_buffer(u32 rate, u32 channels, u32 frames) {
    AudioBuffer b;
    b.sample_rate = rate;
    b.channels = channels;
    b.frames = frames;
    b.samples.assign(static_cast<usize>(frames) * channels, 0.0f);
    return b;
}

/// Adds a sine (peak amplitude `amp`) to every channel over [begin, end) frames.
void add_sine(AudioBuffer& b, double freq, double amp, u32 begin = 0, u32 end = 0xFFFFFFFFu, double phase = 0.0) {
    end = std::min(end, b.frames);
    for (u32 f = begin; f < end; ++f) {
        const double v = amp * std::sin(2.0 * kPi * freq * f / b.sample_rate + phase);
        for (u32 c = 0; c < b.channels; ++c) {
            b.samples[static_cast<usize>(f) * b.channels + c] += static_cast<float>(v);
        }
    }
}

struct Lcg {
    u64 state;
    double next() { // uniform [-1, 1)
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(state >> 11u) / static_cast<double>(1ull << 52u) - 1.0;
    }
};

void add_noise(AudioBuffer& b, double amp, u64 seed) {
    Lcg rng{seed};
    for (float& s : b.samples) {
        s += static_cast<float>(amp * rng.next());
    }
}

// ---- WAV writer ---------------------------------------------------------------------------------------

void put16(std::vector<u8>& v, u32 x) {
    v.push_back(static_cast<u8>(x));
    v.push_back(static_cast<u8>(x >> 8u));
}

void put32(std::vector<u8>& v, u32 x) {
    put16(v, x & 0xFFFFu);
    put16(v, x >> 16u);
}

/// kind: 8, 16, 24, 32 (int PCM), 33 (32-bit float); extensible wraps the fmt in WAVE_FORMAT_EXTENSIBLE.
std::vector<u8> make_wav(const AudioBuffer& b, int kind, bool extensible = false) {
    const bool is_float = kind == 33;
    const u32 bytes_per = is_float ? 4u : static_cast<u32>(kind) / 8u;
    const u32 bits = is_float ? 32u : static_cast<u32>(kind);
    std::vector<u8> data;
    for (float s : b.samples) {
        const double v = std::clamp(static_cast<double>(s), -1.0, 1.0);
        if (is_float) {
            put32(data, std::bit_cast<u32>(static_cast<float>(v)));
        } else if (bytes_per == 1) {
            data.push_back(static_cast<u8>(std::clamp(std::lround(v * 127.0) + 128, 0L, 255L)));
        } else {
            const double scale = static_cast<double>(1ull << (bits - 1u)) - 1.0;
            const s64 q = std::llround(v * scale);
            const u64 raw = static_cast<u64>(q);
            for (u32 i = 0; i < bytes_per; ++i) {
                data.push_back(static_cast<u8>(raw >> (8u * i)));
            }
        }
    }
    std::vector<u8> fmt_chunk;
    put16(fmt_chunk, extensible ? 0xFFFEu : (is_float ? 3u : 1u));
    put16(fmt_chunk, b.channels);
    put32(fmt_chunk, b.sample_rate);
    put32(fmt_chunk, b.sample_rate * b.channels * bytes_per);
    put16(fmt_chunk, b.channels * bytes_per);
    put16(fmt_chunk, bits);
    if (extensible) {
        put16(fmt_chunk, 22);
        put16(fmt_chunk, bits);
        put32(fmt_chunk, 0);
        put16(fmt_chunk, is_float ? 3u : 1u); // SubFormat GUID (KSDATAFORMAT_SUBTYPE_PCM / IEEE_FLOAT)
        const u8 guid_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        fmt_chunk.insert(fmt_chunk.end(), std::begin(guid_tail), std::end(guid_tail));
    }
    std::vector<u8> out;
    const u8 riff[4] = {'R', 'I', 'F', 'F'};
    out.insert(out.end(), riff, riff + 4);
    put32(out, static_cast<u32>(4 + 8 + fmt_chunk.size() + 8 + 8 + data.size() + (data.size() & 1u)));
    const char* wave_fmt = "WAVEfmt ";
    out.insert(out.end(), wave_fmt, wave_fmt + 8);
    put32(out, static_cast<u32>(fmt_chunk.size()));
    out.insert(out.end(), fmt_chunk.begin(), fmt_chunk.end());
    // An odd-sized unknown chunk before the data exercises the word-alignment padding.
    const char* junk = "junk";
    out.insert(out.end(), junk, junk + 4);
    put32(out, 3);
    out.push_back(1);
    out.push_back(2);
    out.push_back(3);
    out.push_back(0); // pad byte
    const char* data_id = "data";
    out.insert(out.end(), data_id, data_id + 4);
    put32(out, static_cast<u32>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// ---- reference BS.1770-4 (48 kHz only, Table 1 / Table 2 coefficients, direct block sums) --------------

double reference_loudness_48k(const AudioBuffer& b) {
    if (b.sample_rate != 48000u) {
        return -999.0;
    }
    const double s1b[3] = {1.53512485958697, -2.69169618940638, 1.19839281085285};
    const double s1a[3] = {1.0, -1.69065929318241, 0.73248077421585};
    const double s2b[3] = {1.0, -2.0, 1.0};
    const double s2a[3] = {1.0, -1.99004745483398, 0.99007225036621};
    std::vector<std::vector<double>> filtered(b.channels, std::vector<double>(b.frames));
    for (u32 c = 0; c < b.channels; ++c) {
        double x[3] = {0, 0, 0}, y[3] = {0, 0, 0}, z[3] = {0, 0, 0};
        for (u32 n = 0; n < b.frames; ++n) {
            x[2] = x[1];
            x[1] = x[0];
            x[0] = b.samples[static_cast<usize>(n) * b.channels + c];
            y[2] = y[1];
            y[1] = y[0];
            y[0] = s1b[0] * x[0] + s1b[1] * x[1] + s1b[2] * x[2] - s1a[1] * y[1] - s1a[2] * y[2];
            z[2] = z[1];
            z[1] = z[0];
            z[0] = s2b[0] * y[0] + s2b[1] * y[1] + s2b[2] * y[2] - s2a[1] * z[1] - s2a[2] * z[2];
            filtered[c][n] = z[0];
        }
    }
    const u32 block = 19200;
    const u32 step = 4800;
    std::vector<double> blocks;
    for (u32 start = 0; start + block <= b.frames; start += step) {
        double sum = 0.0;
        for (u32 c = 0; c < b.channels; ++c) {
            double ms = 0.0;
            for (u32 n = start; n < start + block; ++n) {
                ms += filtered[c][n] * filtered[c][n];
            }
            sum += ms / block; // G = 1 for L, R, C
        }
        blocks.push_back(sum);
    }
    auto loud = [](double z) { return -0.691 + 10.0 * std::log10(z); };
    double s = 0.0;
    int count = 0;
    for (double z : blocks) {
        if (z > 0.0 && loud(z) > -70.0) {
            s += z;
            ++count;
        }
    }
    if (count == 0) {
        return -999.0;
    }
    const double gate = loud(s / count) - 10.0;
    s = 0.0;
    count = 0;
    for (double z : blocks) {
        if (z > 0.0 && loud(z) > -70.0 && loud(z) > gate) {
            s += z;
            ++count;
        }
    }
    return count > 0 ? loud(s / count) : -999.0;
}

// ---- container helpers --------------------------------------------------------------------------------

struct Cooked {
    std::string kind;
    u32 rate = 0, channels = 0, frames = 0, loop_start = 0, loop_end = 0;
    bool has_loop = false;
    std::vector<u8> payload;
};

bool parse_cooked(const std::vector<u8>& bytes, Cooked& out) {
    const std::string text(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(std::min<usize>(bytes.size(), 4096)));
    usize pos = 0;
    bool first = true;
    while (pos < text.size()) {
        const usize eol = text.find('\n', pos);
        if (eol == std::string::npos) {
            return false;
        }
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (first) {
            out.kind = line;
            first = false;
            continue;
        }
        if (line == "DATA") {
            out.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos), bytes.end());
            return true;
        }
        const usize eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const unsigned long v = std::strtoul(line.c_str() + eq + 1, nullptr, 10);
        if (key == "rate") {
            out.rate = static_cast<u32>(v);
        } else if (key == "channels") {
            out.channels = static_cast<u32>(v);
        } else if (key == "samples") {
            out.frames = static_cast<u32>(v);
        } else if (key == "loop_start") {
            out.loop_start = static_cast<u32>(v);
            out.has_loop = true;
        } else if (key == "loop_end") {
            out.loop_end = static_cast<u32>(v);
        }
    }
    return false;
}

/// Decoded PCM of a cooked file (PCM_F32 payload directly, OGG through the cook's own decoder).
bool load_cooked_pcm(const std::string& path, AudioBuffer& out, Cooked* info = nullptr) {
    Cooked c;
    if (!parse_cooked(read_file(path), c)) {
        return false;
    }
    if (info != nullptr) {
        *info = c;
    }
    if (c.kind == "FUSEAUDIO_PCM_F32") {
        if (c.payload.size() != static_cast<usize>(c.frames) * c.channels * 4u) {
            return false;
        }
        out = make_buffer(c.rate, c.channels, c.frames);
        for (usize i = 0; i < out.samples.size(); ++i) {
            const u32 bits = static_cast<u32>(c.payload[i * 4]) | (static_cast<u32>(c.payload[i * 4 + 1]) << 8u)
                           | (static_cast<u32>(c.payload[i * 4 + 2]) << 16u)
                           | (static_cast<u32>(c.payload[i * 4 + 3]) << 24u);
            out.samples[i] = std::bit_cast<float>(bits);
        }
        return true;
    }
    if (c.kind == "FUSEAUDIO_OGG") {
        return decode_ogg_vorbis_stream(c.payload.data(), c.payload.size(), out) && out.frames == c.frames
            && out.channels == c.channels && out.sample_rate == c.rate;
    }
    return false;
}

double max_abs_diff(const AudioBuffer& a, const AudioBuffer& b) {
    if (a.samples.size() != b.samples.size()) {
        return 1e9;
    }
    double m = 0.0;
    for (usize i = 0; i < a.samples.size(); ++i) {
        m = std::max(m, std::abs(static_cast<double>(a.samples[i]) - b.samples[i]));
    }
    return m;
}

// ---- tests --------------------------------------------------------------------------------------------

void test_decoders() {
    AudioBuffer src = make_buffer(44100, 2, 4410);
    add_sine(src, 440.0, 0.5);
    add_noise(src, 0.2, 7);
    const struct {
        int kind;
        bool ext;
        double tol;
        const char* name;
    } cases[] = {{8, false, 1.0 / 100.0, "u8"},       {16, false, 5e-5, "s16"}, {24, false, 1e-6, "s24"},
                 {32, false, 1e-7, "s32"},             {33, false, 0.0, "f32"},           {24, true, 1e-6, "s24 extensible"},
                 {33, true, 0.0, "f32 extensible"}};
    for (const auto& c : cases) {
        const std::vector<u8> wav = make_wav(src, c.kind, c.ext);
        AudioBuffer out;
        std::string error;
        AudioSourceFormat detected = AudioSourceFormat::Unknown;
        const bool ok = decode_audio(wav.data(), wav.size(), out, &error, &detected);
        check(ok && detected == AudioSourceFormat::Wav, std::string("decode WAV ") + c.name + ": " + error);
        check(out.frames == src.frames && out.channels == 2 && out.sample_rate == 44100,
              std::string("WAV ") + c.name + " shape");
        const double diff = max_abs_diff(out, src);
        check(diff <= c.tol + 1e-9, std::string("WAV ") + c.name + " sample error " + fmt(diff));
    }
    // Truncated / foreign inputs are refused with a reason.
    {
        std::vector<u8> wav = make_wav(src, 16);
        wav.resize(30);
        AudioBuffer out;
        std::string error;
        check(!decode_audio(wav.data(), wav.size(), out, &error) && !error.empty(), "truncated WAV refused");
        const u8 mp3[] = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0};
        AudioSourceFormat detected = AudioSourceFormat::Unknown;
        check(!decode_audio(mp3, sizeof(mp3), out, &error, &detected) && detected == AudioSourceFormat::Mp3
                  && error.find("MP3") != std::string::npos,
              "MP3 refused with a reason");
    }

    // Ogg Vorbis: encode here, decode through the cook's decoder.
    check(audio_cook_vorbis_available(), "vendored libvorbis linked into the cook");
    std::vector<u8> ogg;
    check(encode_vorbis_stream(src, 0.5f, ogg) && sniff_audio_format(ogg.data(), ogg.size()) == AudioSourceFormat::OggVorbis,
          "Vorbis encode + sniff");
    AudioBuffer from_ogg;
    std::string error;
    check(decode_audio(ogg.data(), ogg.size(), from_ogg, &error) && from_ogg.frames == src.frames
              && from_ogg.channels == 2 && from_ogg.sample_rate == 44100,
          "Ogg Vorbis decode (exact frame count): " + error);

#if defined(FUSE_AUDIO_COOK_TEST_HAS_RUNTIME)
    // Native FLAC (24-bit) produced by the runtime's encoder, decoded by the cook.
    check(audio_cook_flac_available(), "vendored libFLAC linked into the cook");
    std::vector<u8> flac;
    check(fuse::audio::encode_flac(src.samples.data(), src.frames, src.channels, src.sample_rate, 24, flac),
          "FLAC encode (runtime)");
    AudioBuffer from_flac;
    AudioSourceFormat detected = AudioSourceFormat::Unknown;
    check(decode_audio(flac.data(), flac.size(), from_flac, &error, &detected) && detected == AudioSourceFormat::Flac,
          "FLAC decode: " + error);
    check(from_flac.frames == src.frames && from_flac.channels == 2 && max_abs_diff(from_flac, src) < 2e-7,
          "FLAC 24-bit round trip " + fmt(max_abs_diff(from_flac, src)));
#endif
}

void test_resample() {
    // 44.1 kHz -> 48 kHz (L/M = 160/147): a 1 kHz sine must come out as the same sine at 48 kHz.
    {
        AudioBuffer src = make_buffer(44100, 1, 44100);
        add_sine(src, 1000.0, 0.8);
        const AudioBuffer out = resample_audio(src, 48000);
        check(out.sample_rate == 48000 && out.frames == 48000, "44.1k -> 48k frame count " + std::to_string(out.frames));
        double err = 0.0;
        double peak = 0.0;
        for (u32 n = 256; n + 256 < out.frames; ++n) {
            const double ideal = 0.8 * std::sin(2.0 * kPi * 1000.0 * n / 48000.0);
            err = std::max(err, std::abs(out.samples[n] - ideal));
            peak = std::max(peak, static_cast<double>(std::abs(out.samples[n])));
        }
        std::printf("  resample 44.1k->48k: max error %.2e, amplitude %.6f\n", err, peak);
        check(err < 1e-4, "44.1k -> 48k sine error " + fmt(err));
        check(std::abs(peak - 0.8) < 1e-3, "44.1k -> 48k amplitude " + fmt(peak));
        // Frequency from the rising zero-crossing count over the interior (exactly 1000 Hz).
        u32 first = 0, last = 0, crossings = 0;
        for (u32 n = 257; n + 256 < out.frames; ++n) {
            if (out.samples[n - 1] < 0.0f && out.samples[n] >= 0.0f) {
                if (crossings == 0) {
                    first = n;
                }
                last = n;
                ++crossings;
            }
        }
        const double freq = crossings > 1 ? (crossings - 1) * 48000.0 / (last - first) : 0.0;
        check(std::abs(freq - 1000.0) < 0.5, "44.1k -> 48k frequency " + fmt(freq));
    }
    // 96 kHz -> 48 kHz: the band above the new Nyquist (30 kHz) is removed, 1 kHz is kept.
    {
        AudioBuffer src = make_buffer(96000, 2, 96000);
        add_sine(src, 1000.0, 0.5);
        add_sine(src, 30000.0, 0.3);
        const AudioBuffer out = resample_audio(src, 48000);
        check(out.frames == 48000 && out.channels == 2, "96k -> 48k shape");
        double err = 0.0;
        for (u32 n = 256; n + 256 < out.frames; ++n) {
            const double ideal = 0.5 * std::sin(2.0 * kPi * 1000.0 * n / 48000.0);
            err = std::max(err, std::abs(out.samples[static_cast<usize>(n) * 2] - ideal));
        }
        std::printf("  resample 96k->48k: max error vs the 1 kHz tone %.2e (30 kHz alias rejected)\n", err);
        check(err < 3e-4, "96k -> 48k error incl. 30 kHz alias " + fmt(err));
    }
    // 22.05 kHz -> 48 kHz (L/M = 320/147).
    {
        AudioBuffer src = make_buffer(22050, 1, 22050);
        add_sine(src, 3000.0, 0.7);
        const AudioBuffer out = resample_audio(src, 48000);
        double err = 0.0;
        for (u32 n = 512; n + 512 < out.frames; ++n) {
            err = std::max(err, std::abs(out.samples[n] - 0.7 * std::sin(2.0 * kPi * 3000.0 * n / 48000.0)));
        }
        check(out.frames == 48000 && err < 1e-4, "22.05k -> 48k 3 kHz sine error " + fmt(err));
    }
}

void test_loudness() {
    // EBU Tech 3341 case 1 / 2: stereo 1 kHz sine at -23 / -33 dBFS -> -23.0 / -33.0 LUFS (+-0.1).
    for (double level : {-23.0, -33.0}) {
        AudioBuffer b = make_buffer(48000, 2, 48000 * 5);
        add_sine(b, 1000.0, std::pow(10.0, level / 20.0));
        const double lib = measure_integrated_loudness(b);
        const double ref = reference_loudness_48k(b);
        check(std::abs(lib - level) < 0.1, "EBU 3341 sine " + fmt(level) + " dBFS -> " + fmt(lib) + " LUFS");
        check(std::abs(ref - level) < 0.1, "reference BS.1770 sine " + fmt(level) + " -> " + fmt(ref));
    }
    // EBU 3341 case 3: 10 s at -36, 60 s at -23, 10 s at -36 dBFS -> -23.0 LUFS (relative gate).
    {
        AudioBuffer b = make_buffer(48000, 2, 48000 * 80);
        add_sine(b, 1000.0, std::pow(10.0, -36.0 / 20.0), 0, 48000 * 10);
        add_sine(b, 1000.0, std::pow(10.0, -23.0 / 20.0), 48000 * 10, 48000 * 70);
        add_sine(b, 1000.0, std::pow(10.0, -36.0 / 20.0), 48000 * 70, 48000 * 80);
        const double lib = measure_integrated_loudness(b);
        const double ref = reference_loudness_48k(b);
        check(std::abs(lib + 23.0) < 0.1, "EBU 3341 case 3 -> " + fmt(lib));
        check(std::abs(lib - ref) < 0.02, "EBU 3341 case 3 vs reference " + fmt(ref));
    }
    // Library vs reference on assorted test tones (mono / stereo, low / high frequency, noise, gaps).
    struct Tone {
        u32 channels;
        double freq;
        double amp;
        double noise;
        bool gap;
    };
    const Tone tones[] = {{1, 100.0, 0.3, 0.0, false},   {2, 60.0, 0.5, 0.0, false},  {2, 5000.0, 0.1, 0.0, false},
                          {1, 12000.0, 0.2, 0.0, false}, {2, 440.0, 0.05, 0.1, true}, {2, 0.0, 0.0, 0.3, false}};
    for (const Tone& t : tones) {
        AudioBuffer b = make_buffer(48000, t.channels, 48000 * 3);
        if (t.freq > 0.0) {
            add_sine(b, t.freq, t.amp);
        }
        if (t.noise > 0.0) {
            add_noise(b, t.noise, 99);
        }
        if (t.gap) {
            std::fill(b.samples.begin(), b.samples.begin() + static_cast<std::ptrdiff_t>(48000 * t.channels), 0.0f);
        }
        const double lib = measure_integrated_loudness(b);
        const double ref = reference_loudness_48k(b);
        check(std::abs(lib - ref) < 0.02, "loudness vs reference (" + fmt(t.freq) + " Hz): " + fmt(lib) + " vs " + fmt(ref));
    }

    // Normalisation through the cook: beds -> -23 LUFS, one-shots -> -16 LUFS, +-1 LU (reference meter),
    // both in the PCM_F32 output and after Vorbis encode + decode.
    struct Case {
        AudioClass cls;
        double target;
        double amp;
        double freq;
        u32 seconds;
    };
    const Case cases[] = {{AudioClass::Bed, -23.0, 0.05, 220.0, 10}, {AudioClass::Bed, -23.0, 0.9, 3000.0, 9},
                          {AudioClass::OneShot, -16.0, 0.01, 800.0, 2}, {AudioClass::OneShot, -16.0, 0.7, 150.0, 1}};
    int index = 0;
    for (const Case& c : cases) {
        for (AudioCookFormat format : {AudioCookFormat::PcmF32, AudioCookFormat::OggVorbis}) {
            AudioBuffer b = make_buffer(44100, 2, 44100 * c.seconds);
            add_sine(b, c.freq, c.amp);
            add_noise(b, c.amp * 0.2, 1234);
            AudioCookOptions options;
            options.audio_class = c.cls;
            options.format = format;
            const std::string path = temp_path("norm_" + std::to_string(index++) + ".fuseaudio");
            AudioCookReport report;
            const CookStubWriteResult r = cook_audio_buffer(b, path, options, &report);
            check(r.ok, "normalise cook: " + r.note);
            AudioBuffer cooked;
            check(load_cooked_pcm(path, cooked), "normalise cook output decodes");
            const double ref = reference_loudness_48k(cooked);
            std::printf("  normalise %s %s: source %.2f -> %.2f LUFS (reference %.2f), peak %.2f dBFS%s\n",
                        audio_class_name(c.cls), format == AudioCookFormat::PcmF32 ? "pcm" : "ogg", report.source_lufs,
                        report.output_lufs, ref, static_cast<double>(report.output_peak_dbfs),
                        report.peak_limited ? " (limited)" : "");
            check(cooked.sample_rate == 48000, "normalised output resampled to 48 kHz");
            check(std::abs(ref - c.target) <= 1.0, "normalised loudness within +-1 LU: " + fmt(ref));
            if (format == AudioCookFormat::PcmF32) {
                check(std::abs(ref - c.target) <= 0.2, "PCM normalised loudness within +-0.2 LU: " + fmt(ref));
                check(sample_peak(cooked) <= std::pow(10.0, -1.0 / 20.0) + 1e-6, "peak ceiling -1 dBFS held");
            }
        }
    }

    // Peak-heavy one-shot: -16 LUFS needs more than the ceiling allows; the limiter holds -1 dBFS and
    // the loudness still lands on target.
    {
        AudioBuffer b = make_buffer(48000, 1, 48000);
        add_sine(b, 200.0, 0.02);
        for (u32 k = 0; k < 10; ++k) {
            for (u32 i = 0; i < 48; ++i) {
                b.samples[k * 4800u + 100u + i] += (i % 2 == 0 ? 0.9f : -0.9f);
            }
        }
        AudioCookOptions options;
        options.audio_class = AudioClass::OneShot;
        options.format = AudioCookFormat::PcmF32;
        options.trim_silence = false;
        AudioCookReport report;
        const std::string path = temp_path("limited.fuseaudio");
        check(cook_audio_buffer(b, path, options, &report).ok, "limited cook");
        AudioBuffer cooked;
        check(load_cooked_pcm(path, cooked), "limited output decodes");
        const double ref = reference_loudness_48k(cooked);
        check(report.peak_limited, "limiter engaged on the peaky one-shot");
        check(sample_peak(cooked) <= std::pow(10.0, -1.0 / 20.0) + 1e-6,
              "limited peak " + fmt(20.0 * std::log10(sample_peak(cooked))) + " dBFS");
        check(std::abs(ref + 16.0) <= 1.0, "limited loudness within +-1 LU: " + fmt(ref));
    }
    // Peak normalisation option.
    {
        AudioBuffer b = make_buffer(48000, 2, 48000);
        add_sine(b, 500.0, 0.25);
        AudioCookOptions options;
        options.normalise = AudioNormalise::Peak;
        options.peak_target_dbfs = -3.0f;
        options.format = AudioCookFormat::PcmF32;
        const std::string path = temp_path("peak.fuseaudio");
        check(cook_audio_buffer(b, path, options).ok, "peak-normalise cook");
        AudioBuffer cooked;
        check(load_cooked_pcm(path, cooked) && std::abs(20.0 * std::log10(sample_peak(cooked)) + 3.0) < 0.01,
              "peak normalised to -3 dBFS");
    }
}

void test_trim() {
    AudioBuffer b = make_buffer(48000, 2, 48000 * 2);
    add_sine(b, 700.0, 0.5, 24000, 72000); // 0.5 s silence, 1 s tone, 0.5 s silence
    u32 head = 0, tail = 0;
    trim_silence(b, -60.0f, 48, 960, &head, &tail);
    // The tone's first/last samples above -60 dB are frames 24001 and 71999 (frame 24000 is a zero).
    check(head == 24001 - 48 && tail == 24000 - 960, "trim head/tail " + std::to_string(head) + "/" + std::to_string(tail));
    check(b.frames == 72000 + 960 - (24001 - 48), "trimmed length " + std::to_string(b.frames));
}

void test_round_trip_snr() {
    AudioBuffer src = make_buffer(48000, 2, 48000 * 3);
    for (u32 f = 0; f < src.frames; ++f) {
        const double t = static_cast<double>(f) / 48000.0;
        src.samples[f * 2u] = static_cast<float>(0.3 * std::sin(2 * kPi * 440 * t) + 0.15 * std::sin(2 * kPi * 1234.5 * t)
                                                 + 0.08 * std::sin(2 * kPi * 3000 * t));
        src.samples[f * 2u + 1u] = static_cast<float>(0.3 * std::sin(2 * kPi * 330 * t + 1.0)
                                                      + 0.1 * std::sin(2 * kPi * 2222 * t));
    }
    for (AudioClass cls : {AudioClass::OneShot, AudioClass::Bed}) {
        AudioCookOptions options;
        options.audio_class = cls;
        options.normalise = AudioNormalise::None;
        options.trim_silence = false;
        const std::string path = temp_path(std::string("snr_") + audio_class_name(cls) + ".fuseaudio");
        AudioCookReport report;
        check(cook_audio_buffer(src, path, options, &report).ok, "SNR cook");
        AudioBuffer decoded;
        Cooked info;
        check(load_cooked_pcm(path, decoded, &info) && info.kind == "FUSEAUDIO_OGG", "SNR cook decodes");
        double signal = 0.0, noise = 0.0;
        for (usize i = 0; i < std::min(src.samples.size(), decoded.samples.size()); ++i) {
            signal += static_cast<double>(src.samples[i]) * src.samples[i];
            const double d = static_cast<double>(src.samples[i]) - decoded.samples[i];
            noise += d * d;
        }
        const double snr = 10.0 * std::log10(signal / std::max(noise, 1e-30));
        const u64 bytes = read_file(path).size();
        std::printf("  Vorbis q%.0f round trip: SNR %.1f dB, %.1f kbit/s\n", report.ogg_quality * 10.0f, snr,
                    bytes * 8.0 / 3.0 / 1000.0);
        check(decoded.frames == src.frames, "Vorbis round trip keeps the frame count");
        check(report.ogg_quality == (cls == AudioClass::Bed ? kAudioBedVorbisQuality : kAudioOneShotVorbisQuality),
              "class quality (q4 bed / q5 one-shot)");
        check(snr >= 20.0, "Vorbis round-trip SNR " + fmt(snr) + " dB");
    }
}

void test_loop() {
    // A 440 Hz tone cut at 1.37 s (602.8 cycles) clicks when wrapped as is.
    AudioBuffer tone = make_buffer(48000, 2, static_cast<u32>(48000 * 1.37));
    add_sine(tone, 440.0, 0.5, 0, 0xFFFFFFFFu, 0.3);
    const float naive = loop_seam_ratio(tone);
    AudioBuffer looped = tone;
    u32 crossfade = 0;
    check(make_seamless_loop(looped, 2400, &crossfade) && crossfade > 0, "make_seamless_loop (tone)");
    const float fixed_ratio = loop_seam_ratio(looped);
    std::printf("  loop seam (tone): naive %.2f -> %.3f, crossfade %u frames\n", static_cast<double>(naive),
                static_cast<double>(fixed_ratio), crossfade);
    check(naive > kAudioLoopSeamClickRatio, "naive wrap is detected as a click " + fmt(naive));
    check(fixed_ratio <= 1.1f, "tone loop seam continuous " + fmt(fixed_ratio));
    // Wrapping the looped tone keeps the waveform a clean 440 Hz sine across the seam: compare
    // the frames around the wrap with an ideal sine fitted by phase continuation.
    {
        const u32 n = looped.frames;
        double worst = 0.0;
        for (u32 i = 1; i < 32; ++i) {
            const double a = looped.samples[static_cast<usize>((n - i) % n) * 2];
            const double b2 = looped.samples[static_cast<usize>((n - i - 1) % n) * 2];
            worst = std::max(worst, std::abs(a - b2));
        }
        check(worst <= 2.0 * kPi * 440.0 / 48000.0 * 0.5 * 1.01, "tail slope bounded by the sine slope");
    }

    // Noise bed (uncorrelated head/tail -> equal-power crossfade).
    AudioBuffer bed = make_buffer(48000, 2, 48000 * 3);
    add_noise(bed, 0.3, 4242);
    AudioBuffer bed_loop = bed;
    check(make_seamless_loop(bed_loop, 2400), "make_seamless_loop (noise bed)");
    check(loop_seam_ratio(bed_loop) <= kAudioLoopSeamClickRatio, "noise bed loop seam " + fmt(loop_seam_ratio(bed_loop)));

    // Through the cook: loop points in the header, continuity in PCM and after Vorbis encode + decode.
    for (AudioCookFormat format : {AudioCookFormat::PcmF32, AudioCookFormat::OggVorbis}) {
        AudioCookOptions options;
        options.audio_class = AudioClass::Bed;
        options.make_loop = true;
        options.format = format;
        AudioCookReport report;
        const std::string path = temp_path(format == AudioCookFormat::PcmF32 ? "loop_pcm.fuseaudio" : "loop_ogg.fuseaudio");
        const CookStubWriteResult r = cook_audio_buffer(tone, path, options, &report);
        check(r.ok && report.looped && !report.loop_seam_click, "loop cook: " + r.note);
        AudioBuffer decoded;
        Cooked info;
        check(load_cooked_pcm(path, decoded, &info), "loop cook decodes");
        check(info.has_loop && info.loop_start == 0 && info.loop_end == info.frames, "loop points in the header");
        const float ratio = loop_seam_ratio(decoded);
        std::printf("  loop seam through the cook (%s): %.3f\n", format == AudioCookFormat::PcmF32 ? "pcm" : "ogg",
                    static_cast<double>(ratio));
        check(ratio <= kAudioLoopSeamClickRatio, "cooked loop seam continuity " + fmt(ratio));
    }
}

void test_determinism_and_wrapper() {
    AudioBuffer src = make_buffer(44100, 2, 44100 * 2);
    add_sine(src, 523.25, 0.4);
    add_noise(src, 0.05, 555);
    const std::string wav_path = temp_path("det_source.wav");
    write_file(wav_path, make_wav(src, 24));
    for (const char* format : {"ogg", "pcm_f32"}) {
        const std::string a = temp_path(std::string("det_a_") + format + ".fuseaudio");
        const std::string b = temp_path(std::string("det_b_") + format + ".fuseaudio");
        const CookStubWriteResult ra = tryCookAudioOgg(wav_path, a, 48000, format);
        const CookStubWriteResult rb = tryCookAudioOgg(wav_path, b, 48000, format);
        check(ra.ok && rb.ok, std::string("tryCookAudioOgg ") + format + ": " + ra.note);
        const std::vector<u8> ba = read_file(a);
        const std::vector<u8> bb = read_file(b);
        check(!ba.empty() && ba == bb, std::string("byte-identical cooks (") + format + ")");
        Cooked info;
        check(parse_cooked(ba, info) && info.rate == 48000 && info.channels == 2
                  && info.kind == (std::strcmp(format, "ogg") == 0 ? "FUSEAUDIO_OGG" : "FUSEAUDIO_PCM_F32"),
              std::string("wrapper container header (") + format + ")");
#if defined(FUSE_AUDIO_COOK_TEST_HAS_RUNTIME)
        // The runtime parses the cooked container and decodes the payload to the header's frame count.
        fuse::audio::FuseAudioContainer container;
        check(fuse::audio::parse_fuseaudio(ba.data(), ba.size(), container) && container.sample_rate == 48000
                  && container.channels == 2 && container.frames == info.frames,
              std::string("runtime parse_fuseaudio (") + format + ")");
        if (container.payload == fuse::audio::FuseAudioPayload::Ogg) {
            fuse::audio::DecodedAudio decoded;
            check(fuse::audio::decode_ogg_vorbis(container.data, container.size, decoded)
                      && decoded.frames == container.frames,
                  "runtime Vorbis decode of the cooked payload");
        }
#endif
    }
    // write_audio_stub (AssetCooker path) now produces the real container for a valid source.
    const std::string via_stub = temp_path("det_stub.fuseaudio");
    check(write_audio_stub(wav_path, via_stub, 48000, "ogg").ok, "write_audio_stub real cook");
    check(read_file(via_stub) == read_file(temp_path("det_a_ogg.fuseaudio")), "write_audio_stub == tryCookAudioOgg bytes");
    // Undecodable source: the hook fails with a classified reason (the lenient stub keeps `hook=ogg...`).
    write_file(temp_path("bad.wav"), {'R', 'I', 'F', 'F'});
    const CookStubWriteResult bad = tryCookAudioOgg(temp_path("bad.wav"), temp_path("bad.fuseaudio"), 48000, "ogg");
    check(!bad.ok && bad.failure == CookFailure::MalformedSource && bad.note.rfind("ogg", 0) == 0,
          "malformed source refused: " + bad.note);
    // Source formats all reach the chain from files: FLAC and Ogg inputs.
    std::vector<u8> ogg;
    check(encode_vorbis_stream(src, 0.6f, ogg), "Ogg source encode");
    write_file(temp_path("src.ogg"), ogg);
    AudioCookReport report;
    AudioCookOptions options;
    check(cook_audio_file(temp_path("src.ogg"), temp_path("from_ogg.fuseaudio"), options, &report).ok
              && report.source_format == AudioSourceFormat::OggVorbis && report.output_rate == 48000,
          "cook from an Ogg Vorbis source");
#if defined(FUSE_AUDIO_COOK_TEST_HAS_RUNTIME)
    std::vector<u8> flac;
    check(fuse::audio::encode_flac(src.samples.data(), src.frames, src.channels, src.sample_rate, 16, flac), "FLAC source");
    write_file(temp_path("src.flac"), flac);
    check(cook_audio_file(temp_path("src.flac"), temp_path("from_flac.fuseaudio"), options, &report).ok
              && report.source_format == AudioSourceFormat::Flac && report.output_rate == 48000,
          "cook from a FLAC source");
#endif
}

void test_cli(const char* fuse_cook) {
    if (fuse_cook == nullptr) {
        std::printf("  fuse_cook CLI check skipped (no executable path given)\n");
        return;
    }
    const std::filesystem::path dir = temp_dir() / "cli";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    AudioBuffer src = make_buffer(44100, 2, 44100 * 3);
    add_sine(src, 330.0, 0.3);
    add_noise(src, 0.05, 31);
    write_file((dir / "bed.wav").string(), make_wav(src, 16));
    const std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::current_path(dir); // fuse_cook keeps its cook cache in the working directory
    auto run = [&](const std::string& args) {
        const std::string command = "\"" + std::string(fuse_cook) + "\" " + args;
        return std::system(command.c_str());
    };
    check(run("--audio --input bed.wav --output bed.fuseaudio") == 0, "fuse_cook --audio (AssetCooker path)");
    check(run("--audio --bed --loop --input bed.wav --output bed_loop.fuseaudio") == 0, "fuse_cook --audio --bed --loop");
    check(run("--audio --oneshot --mono --pcm --rate 32000 --input bed.wav --output shot.fuseaudio") == 0,
          "fuse_cook --audio --oneshot --mono --pcm --rate");
    std::filesystem::current_path(previous);

    Cooked info;
    check(parse_cooked(read_file((dir / "bed.fuseaudio").string()), info) && info.kind == "FUSEAUDIO_OGG"
              && info.rate == 48000,
          "CLI default output is FUSEAUDIO_OGG at 48 kHz");
    AudioBuffer loop;
    check(load_cooked_pcm((dir / "bed_loop.fuseaudio").string(), loop, &info) && info.has_loop
              && std::abs(reference_loudness_48k(loop) + 23.0) <= 1.0,
          "CLI bed loop: loop points and -23 LUFS");
    AudioBuffer shot;
    check(load_cooked_pcm((dir / "shot.fuseaudio").string(), shot, &info) && info.kind == "FUSEAUDIO_PCM_F32"
              && shot.channels == 1 && shot.sample_rate == 32000,
          "CLI one-shot: mono PCM_F32 at 32 kHz");
}

} // namespace

int main(int argc, char** argv) {
    test_decoders();
    test_resample();
    test_loudness();
    test_trim();
    test_round_trip_snr();
    test_loop();
    test_determinism_and_wrapper();
    test_cli(argc > 1 ? argv[1] : nullptr);
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_asset_audio_cook: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_asset_audio_cook: all checks passed\n");
    return EXIT_SUCCESS;
}
