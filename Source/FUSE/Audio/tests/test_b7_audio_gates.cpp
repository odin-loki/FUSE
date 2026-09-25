// B7.2 Spatial Audio Engine — Phase 7 deliverable gates (FUSE_MASTER_PLAN.md "#### Audio").
//
// Every check runs headless: the engine's offline mix buffer is the observable, and the output
// backend is read back through AudioEngine::backend(). References are computed independently
// (analytic formulas, direct double-precision convolution, clip sample indexing) rather than
// by calling the code under test.
//
// Run with `--perf` for the CPU budget gate (enforced only in NDEBUG builds).
// Run with `--require-openal` (e.g. under ALSOFT_DRIVERS=null) to fail unless the engine brings up
// a real OpenAL device + current context without error, instead of falling back to Null.

#include <fuse/audio/attenuation.hpp>
#include <fuse/audio/audio_clip.hpp>
#include <fuse/audio/audio_engine.hpp>
#include <fuse/audio/audio_registry.hpp>
#include <fuse/audio/conv_reverb_cpu.hpp>
#include <fuse/audio/occlusion.hpp>
#include <fuse/audio/reverb_cuda.hpp>
#include <fuse/core/init.hpp>
#include <fuse/core/sanitizer.hpp>

#if defined(FUSE_AUDIO_OPENAL)
#include <AL/al.h>
#include <AL/alc.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using fuse::u32;
namespace fa = fuse::audio;

namespace {

int g_failures = 0;
constexpr double kPi = 3.14159265358979323846;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(double value, double expected, double epsilon, const char* message) {
    if (!(std::fabs(value - expected) <= epsilon)) {
        std::fprintf(stderr, "FAIL: %s (got %.9g, expected %.9g, eps %.3g)\n", message, value,
                     expected, epsilon);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------------------------
// WAV writer (independent of the loader under test)

void put_u16(std::vector<unsigned char>& out, std::uint16_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xFF));
    out.push_back(static_cast<unsigned char>(v >> 8));
}

void put_u32(std::vector<unsigned char>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
    }
}

void put_chunk(std::vector<unsigned char>& out, const char* id,
               const std::vector<unsigned char>& body) {
    out.insert(out.end(), id, id + 4);
    put_u32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    if ((body.size() & 1u) != 0u) {
        out.push_back(0);
    }
}

struct WavSpec {
    std::uint16_t format = 1;
    std::uint16_t channels = 1;
    std::uint32_t rate = 48000;
    std::uint16_t bits = 16;
    bool extensible = false;
    bool junk_before_fmt = false; ///< Odd-sized LIST chunk ahead of fmt.
};

unsigned long currentProcessId() {
#if defined(_WIN32)
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::filesystem::path write_wav(const char* name, const WavSpec& spec,
                                const std::vector<unsigned char>& data) {
    std::vector<unsigned char> fmt;
    put_u16(fmt, spec.extensible ? 0xFFFE : spec.format);
    put_u16(fmt, spec.channels);
    put_u32(fmt, spec.rate);
    put_u32(fmt, spec.rate * spec.channels * spec.bits / 8u);
    put_u16(fmt, static_cast<std::uint16_t>(spec.channels * spec.bits / 8u));
    put_u16(fmt, spec.bits);
    if (spec.extensible) {
        put_u16(fmt, 22);
        put_u16(fmt, spec.bits);
        put_u32(fmt, spec.channels == 2 ? 0x3u : 0x4u);
        put_u16(fmt, spec.format);
        const unsigned char guid_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                             0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        fmt.insert(fmt.end(), guid_tail, guid_tail + 14);
    }

    std::vector<unsigned char> body;
    body.insert(body.end(), {'W', 'A', 'V', 'E'});
    if (spec.junk_before_fmt) {
        put_chunk(body, "LIST", {'I', 'N', 'F', 'O', 'x'});
    }
    put_chunk(body, "fmt ", fmt);
    if (spec.junk_before_fmt) {
        put_chunk(body, "fact", {1, 0, 0, 0});
    }
    put_chunk(body, "data", data);

    std::vector<unsigned char> file;
    file.insert(file.end(), {'R', 'I', 'F', 'F'});
    put_u32(file, static_cast<std::uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());

    // Per-process directory: ctest runs this binary twice at once (gates + --require-openal), and a
    // shared fixed path let one process truncate a WAV while the other was loading it.
    // Removed again at process exit so repeated runs don't leave one directory per pid behind.
    struct FixtureDir {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() / ("fuse_b7_audio_gates_" + std::to_string(currentProcessId()));
        ~FixtureDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };
    static const FixtureDir fixtureDir;
    const std::filesystem::path& dir = fixtureDir.path;
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return path;
}

// ---------------------------------------------------------------------------------------------
// Scene helpers

struct Scene {
    fa::AudioEngine engine;
    fa::AudioRegistry registry;
    fa::AudioListener* listener = nullptr;

    explicit Scene(u32 frames, u32 max_sources = 256, bool hrtf = true) {
        fa::AudioDesc desc;
        desc.frames_per_buf = frames;
        desc.max_sources = max_sources;
        desc.hrtf_enabled = hrtf;
        desc.cuda_reverb = false;
        engine.init(desc);
        const fa::EntityId id = registry.create_entity();
        listener = registry.set_listener(id);
        listener->forward = {0.f, 0.f, -1.f};
        listener->up = {0.f, 1.f, 0.f};
    }

    fa::AudioSource* add(fuse::Handle<fa::AudioClip> clip, const fa::Vec3& pos, float volume = 1.f,
                         bool spatial = true, bool looping = true) {
        const fa::EntityId id = registry.create_entity();
        registry.set_position(id, pos);
        fa::AudioSourceDesc desc;
        desc.clip = clip;
        desc.volume = volume;
        desc.spatial = spatial;
        desc.looping = looping;
        fa::AudioSource* source = registry.add_source(id, desc);
        source->playing = true;
        return source;
    }
};

fuse::Handle<fa::AudioClip> register_pcm(fa::AudioEngine& engine, const std::vector<float>& pcm,
                                       u32 channels, u32 rate) {
    fa::AudioClip clip;
    clip.load_from_pcm(pcm.data(), static_cast<u32>(pcm.size() / channels), channels, rate);
    return engine.register_clip(std::move(clip));
}

std::vector<float> sine(u32 frames, double freq, u32 rate, double amp = 1.0, double phase = 0.0) {
    std::vector<float> out(frames);
    for (u32 i = 0; i < frames; ++i) {
        out[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * i / rate + phase));
    }
    return out;
}

/// Amplitude of the component at `freq` (single-bin DFT over whole periods of `x`).
double tone_amplitude(const std::vector<double>& x, double freq, double rate) {
    double re = 0.0;
    double im = 0.0;
    for (size_t n = 0; n < x.size(); ++n) {
        const double w = 2.0 * kPi * freq * static_cast<double>(n) / rate;
        re += x[n] * std::cos(w);
        im -= x[n] * std::sin(w);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(x.size());
}

std::vector<double> channel(const std::vector<float>& stereo, u32 ch) {
    std::vector<double> out(stereo.size() / 2);
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = stereo[i * 2 + ch];
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Gate: Audio engine initialises OpenAL device and context without error

bool g_requireOpenAL = false;

void gateEngineInitialises() {
    for (int cycle = 0; cycle < 3; ++cycle) {
        fa::AudioEngine engine;
        fa::AudioDesc desc;
        desc.cuda_reverb = false;
        engine.init(desc);
        expectTrue(engine.is_initialized(), "engine initialises (OpenAL or headless null backend)");
#if defined(FUSE_AUDIO_OPENAL)
        if (engine.backend_kind() == fa::AudioBackendKind::OpenAL) {
            expectTrue(alGetError() == AL_NO_ERROR, "OpenAL context reports no error after init");
        }
#endif
        if (cycle == 0) {
            std::printf("[b7-audio] backend = %s\n",
                        engine.backend_kind() == fa::AudioBackendKind::OpenAL ? "OpenAL" : "Null");
        }
        if (g_requireOpenAL) {
            expectTrue(engine.backend_kind() == fa::AudioBackendKind::OpenAL,
                       "--require-openal: engine initialised the OpenAL backend (not Null)");
#if defined(FUSE_AUDIO_OPENAL)
            ALCcontext* context = alcGetCurrentContext();
            ALCdevice* device = context != nullptr ? alcGetContextsDevice(context) : nullptr;
            expectTrue(context != nullptr && device != nullptr, "--require-openal: current ALC context and device");
            if (device != nullptr) {
                expectTrue(alcGetError(device) == ALC_NO_ERROR, "--require-openal: ALC device reports no error");
                if (cycle == 0) {
                    const ALCchar* name = alcGetString(device, ALC_DEVICE_SPECIFIER);
                    std::printf("[b7-audio] OpenAL device = %s, renderer = %s\n", name != nullptr ? name : "?",
                                alGetString(AL_RENDERER) != nullptr ? alGetString(AL_RENDERER) : "?");
                }
            }
            expectTrue(alGetError() == AL_NO_ERROR, "--require-openal: AL reports no error");
#endif
        }
        engine.destroy();
        expectTrue(!engine.is_initialized(), "engine destroy releases backend");
#if defined(FUSE_AUDIO_OPENAL)
        if (g_requireOpenAL) {
            expectTrue(alcGetCurrentContext() == nullptr, "--require-openal: destroy releases the ALC context");
        }
#endif
    }
}

// ---------------------------------------------------------------------------------------------
// Gate: Mono and stereo WAV files load and play correctly

void gateWavLoadDecodesExactly() {
    // 16-bit mono and stereo, with an odd-sized LIST chunk before fmt and a fact chunk after.
    const std::vector<std::int16_t> mono16 = {0, 16384, -16384, 32767, -32768, 1, -1};
    std::vector<unsigned char> data;
    for (std::int16_t v : mono16) {
        put_u16(data, static_cast<std::uint16_t>(v));
    }
    WavSpec spec;
    spec.junk_before_fmt = true;
    fa::AudioClip mono;
    expectTrue(mono.load_wav(write_wav("mono16.wav", spec, data).string().c_str()),
               "16-bit mono WAV with LIST/fact chunks loads");
    expectTrue(mono.channel_count == 1 && mono.sample_rate == 48000 && mono.frame_count() == 7,
               "16-bit mono format fields");
    for (size_t i = 0; i < mono16.size() && i < mono.samples.size(); ++i) {
        expectNear(mono.samples[i], mono16[i] / 32768.0, 0.0, "16-bit mono sample decodes exactly");
    }

    const std::vector<std::int16_t> stereo16 = {1000, -2000, 3000, -4000, 5000, -6000};
    data.clear();
    for (std::int16_t v : stereo16) {
        put_u16(data, static_cast<std::uint16_t>(v));
    }
    spec = {};
    spec.channels = 2;
    spec.rate = 44100;
    fa::AudioClip stereo;
    expectTrue(stereo.load_wav(write_wav("stereo16.wav", spec, data).string().c_str()),
               "16-bit stereo WAV loads");
    expectTrue(stereo.channel_count == 2 && stereo.sample_rate == 44100 && stereo.frame_count() == 3,
               "16-bit stereo format fields");
    expectNear(stereo.duration, 3.0 / 44100.0, 1e-9, "stereo duration");
    for (size_t i = 0; i < stereo16.size() && i < stereo.samples.size(); ++i) {
        expectNear(stereo.samples[i], stereo16[i] / 32768.0, 0.0,
                   "16-bit stereo interleaving preserved");
    }

    // 24-bit PCM (extensible header), 8-bit unsigned, 32-bit float.
    data.clear();
    const std::int32_t s24[] = {0x7FFFFF, -0x800000, 0x123456, -1};
    for (std::int32_t v : s24) {
        const std::uint32_t u = static_cast<std::uint32_t>(v);
        data.push_back(u & 0xFF);
        data.push_back((u >> 8) & 0xFF);
        data.push_back((u >> 16) & 0xFF);
    }
    spec = {};
    spec.channels = 2;
    spec.bits = 24;
    spec.extensible = true;
    fa::AudioClip c24;
    expectTrue(c24.load_wav(write_wav("s24.wav", spec, data).string().c_str()),
               "24-bit WAVE_FORMAT_EXTENSIBLE loads");
    for (size_t i = 0; i < 4 && i < c24.samples.size(); ++i) {
        expectNear(c24.samples[i], s24[i] / 8388608.0, 1e-9, "24-bit sample decodes");
    }

    data = {0, 128, 255};
    spec = {};
    spec.bits = 8;
    fa::AudioClip c8;
    expectTrue(c8.load_wav(write_wav("u8.wav", spec, data).string().c_str()), "8-bit WAV loads");
    if (c8.samples.size() == 3) {
        expectNear(c8.samples[0], -1.0, 0.0, "8-bit 0 -> -1");
        expectNear(c8.samples[1], 0.0, 0.0, "8-bit 128 -> 0");
        expectNear(c8.samples[2], 127.0 / 128.0, 1e-9, "8-bit 255 -> 127/128");
    }

    data.clear();
    const float f32[] = {0.25f, -0.75f};
    data.resize(sizeof(f32));
    std::memcpy(data.data(), f32, sizeof(f32));
    spec = {};
    spec.format = 3;
    spec.bits = 32;
    fa::AudioClip cf;
    expectTrue(cf.load_wav(write_wav("f32.wav", spec, data).string().c_str()),
               "32-bit float WAV loads");
    expectTrue(cf.samples.size() == 2 && cf.samples[0] == 0.25f && cf.samples[1] == -0.75f,
               "32-bit float samples bit-exact");

    // Malformed inputs are rejected without crashing (bits=0 used to divide by zero).
    spec = {};
    spec.bits = 0;
    fa::AudioClip bad;
    expectTrue(!bad.load_wav(write_wav("bits0.wav", spec, {1, 2, 3, 4}).string().c_str()),
               "bits_per_sample=0 rejected");
    spec = {};
    spec.format = 2; // ADPCM
    expectTrue(!bad.load_wav(write_wav("adpcm.wav", spec, {1, 2, 3, 4}).string().c_str()),
               "compressed format rejected");
    expectTrue(!bad.load_wav("/nonexistent/fuse.wav"), "missing file rejected");
}

void gateWavPlaysThroughMixer() {
    const u32 frames = 256;
    Scene scene(frames);

    // Mono clip: both channels carry the clip samples exactly (2D source, unity gains).
    std::vector<float> mono_pcm = sine(frames, 375.0, 48000, 0.5);
    const auto mono_clip = register_pcm(scene.engine, mono_pcm, 1, 48000);
    fa::AudioSource* mono_src = scene.add(mono_clip, {}, 1.f, false, false);
    scene.engine.update(scene.registry, 1.f / 60.f);
    const std::vector<float>& out = scene.engine.last_mix_buffer();
    double max_err = 0.0;
    for (u32 i = 0; i < frames; ++i) {
        max_err = std::max<double>(max_err, std::fabs(out[i * 2] - mono_pcm[i]));
        max_err = std::max<double>(max_err, std::fabs(out[i * 2 + 1] - mono_pcm[i]));
    }
    expectNear(max_err, 0.0, 1e-7, "mono clip plays sample-exact on both channels");
    expectTrue(!mono_src->playing, "non-looping clip stops after its last frame");

    // Stereo clip: L and R stay distinct (previously down-mixed to mono).
    std::vector<float> stereo_pcm(frames * 2);
    for (u32 i = 0; i < frames; ++i) {
        stereo_pcm[i * 2] = 0.25f * std::sin(2.f * 3.14159265f * i / 64.f);
        stereo_pcm[i * 2 + 1] = -0.4f;
    }
    const auto stereo_clip = register_pcm(scene.engine, stereo_pcm, 2, 48000);
    scene.add(stereo_clip, {}, 1.f, false, false);
    scene.engine.update(scene.registry, 1.f / 60.f);
    max_err = 0.0;
    for (u32 i = 0; i < frames; ++i) {
        max_err = std::max<double>(max_err, std::fabs(out[i * 2] - stereo_pcm[i * 2]));
        max_err = std::max<double>(max_err, std::fabs(out[i * 2 + 1] - stereo_pcm[i * 2 + 1]));
    }
    expectNear(max_err, 0.0, 1e-7, "stereo clip plays with L/R preserved");

    // play_2d one-shot: plays the same way and is retired (backend source freed) when done.
    const u32 backend_before = scene.engine.backend().source_count();
    scene.engine.play_2d(stereo_clip, 0.5f);
    expectTrue(scene.engine.spatial_mixer().one_shots().size() == 1, "play_2d queues a voice");
    scene.engine.update(scene.registry, 1.f / 60.f);
    max_err = 0.0;
    for (u32 i = 0; i < frames; ++i) {
        max_err = std::max<double>(max_err, std::fabs(out[i * 2] - 0.5f * stereo_pcm[i * 2]));
    }
    expectNear(max_err, 0.0, 1e-7, "play_2d renders clip at requested volume");
    expectTrue(scene.engine.spatial_mixer().one_shots().empty(), "finished one-shot retired");
    expectTrue(scene.engine.backend().source_count() == backend_before,
               "finished one-shot releases its backend source");

    // Resampling: a 24 kHz clip plays at the correct speed on the 48 kHz bus.
    Scene rs(512);
    const auto slow = register_pcm(rs.engine, sine(2400, 1000.0, 24000), 1, 24000);
    rs.add(slow, {}, 1.f, false, true);
    rs.engine.update(rs.registry, 1.f / 60.f);
    const std::vector<double> l = channel(rs.engine.last_mix_buffer(), 0);
    expectNear(tone_amplitude(l, 1000.0, 48000.0), 1.0, 0.02,
               "24 kHz clip resampled: 1 kHz tone preserved on 48 kHz bus");
}

// ---------------------------------------------------------------------------------------------
// Gate: Spatial attenuation: source at max_distance has near-zero volume (gain readback)

double reference_attenuation(fa::AttenuationCurve curve, double d, double min_d, double max_d,
                             double r) {
    if (d <= min_d) {
        return 1.0;
    }
    if (d >= max_d) {
        return 0.0;
    }
    double g = 1.0;
    switch (curve) {
    case fa::AttenuationCurve::Linear:
        g = 1.0 - r * (d - min_d) / (max_d - min_d);
        break;
    case fa::AttenuationCurve::Logarithmic:
        g = min_d / (min_d + r * (d - min_d));
        break;
    case fa::AttenuationCurve::Exponential:
        g = std::pow(d / min_d, -r);
        break;
    case fa::AttenuationCurve::Inverse:
        g = min_d / (r * d);
        break;
    default:
        break;
    }
    return std::clamp(g, 0.0, 1.0);
}

void gateAttenuationCurvesAndReadback() {
    const fa::AttenuationCurve curves[] = {fa::AttenuationCurve::Linear,
                                           fa::AttenuationCurve::Logarithmic,
                                           fa::AttenuationCurve::Exponential,
                                           fa::AttenuationCurve::Inverse};
    for (fa::AttenuationCurve curve : curves) {
        for (float rolloff : {0.5f, 1.f, 2.f}) {
            fa::AttenuationParams p;
            p.curve = curve;
            p.min_dist = 2.f;
            p.max_dist = 40.f;
            p.rolloff = rolloff;
            double prev = 2.0;
            for (float d = 0.f; d <= 45.f; d += 0.25f) {
                const double got = fa::compute_attenuation(d, p);
                expectNear(got, reference_attenuation(curve, d, 2.0, 40.0, rolloff), 1e-5,
                           "attenuation curve matches analytic OpenAL-style formula");
                expectTrue(got <= prev + 1e-7, "attenuation is monotonically non-increasing");
                prev = got;
            }
        }
    }

    // Linear is a true linear ramp (it used to be min/d, identical to Logarithmic r=1).
    fa::AttenuationParams lin;
    lin.min_dist = 1.f;
    lin.max_dist = 11.f;
    expectNear(fa::compute_attenuation(6.f, lin), 0.5, 1e-6, "linear curve is 0.5 at mid-range");
    expectNear(fa::compute_attenuation(10.999f, lin), 1e-4, 1e-4,
               "linear curve reaches zero continuously at max_distance");

    // End-to-end gain readback through the output backend and the mix buffer.
    const u32 frames = 256;
    Scene scene(frames);
    const auto clip = register_pcm(scene.engine, std::vector<float>(frames, 0.5f), 1, 48000);
    fa::AudioSource* src = scene.add(clip, {0.f, 0.f, -50.f});
    src->desc.min_distance = 1.f;
    src->desc.max_distance = 50.f;
    scene.engine.update(scene.registry, 1.f / 60.f);
    const float gain_at_max = scene.engine.backend().read_source_gain(src->backend_source);
    expectTrue(src->backend_source != 0, "registry source owns a backend source");
    expectTrue(gain_at_max <= 1e-3f, "backend gain readback at max_distance is near zero");
    double peak = 0.0;
    for (float s : scene.engine.last_mix_buffer()) {
        peak = std::max(peak, static_cast<double>(std::fabs(s)));
    }
    expectTrue(peak <= 1e-3, "mix output at max_distance is near zero");

    // Mid-range readback equals volume * linear gain (1 - 24.5/49 = 0.5).
    scene.registry.set_position(scene.registry.source_entities().front(), {0.f, 0.f, -25.5f});
    src->playing = true;
    scene.engine.update(scene.registry, 1.f / 60.f);
    expectNear(scene.engine.backend().read_source_gain(src->backend_source), 0.5, 1e-5,
               "backend gain readback at mid-range matches linear curve");

    // Destroyed entities release their backend source.
    const u32 before = scene.engine.backend().source_count();
    scene.registry.destroy_entity(scene.registry.source_entities().front());
    scene.engine.update(scene.registry, 1.f / 60.f);
    expectTrue(scene.engine.backend().source_count() + 1 == before,
               "destroying a source entity frees its backend source");
}

// ---------------------------------------------------------------------------------------------
// Gate: play_at positions source at world position — panning matches camera orientation

struct StereoLevel {
    double left = 0.0;
    double right = 0.0;
};

StereoLevel play_at_level(const fa::Vec3& forward, const fa::Vec3& pos, fa::Vec3* backend_pos) {
    const u32 frames = 256;
    Scene scene(frames);
    scene.listener->forward = forward;
    const auto clip = register_pcm(scene.engine, std::vector<float>(4096, 0.5f), 1, 48000);
    scene.engine.play_at(clip, pos, 1.f);
    if (backend_pos != nullptr && !scene.engine.spatial_mixer().one_shots().empty()) {
        const u32 id = scene.engine.spatial_mixer().one_shots().front().backend_source;
        scene.engine.backend().read_source_position(id, backend_pos->x, backend_pos->y,
                                                    backend_pos->z);
    }
    scene.engine.update(scene.registry, 1.f / 60.f);
    const std::vector<float>& out = scene.engine.last_mix_buffer();
    // Steady-state sample (constant clip): gains are exactly L/R * 0.5.
    return {out[(frames - 1) * 2] / 0.5, out[(frames - 1) * 2 + 1] / 0.5};
}

void gatePlayAtPanningFollowsCamera() {
    const fa::Vec3 facing_neg_z{0.f, 0.f, -1.f};
    const fa::Vec3 facing_pos_z{0.f, 0.f, 1.f};
    const fa::Vec3 facing_pos_x{1.f, 0.f, 0.f};

    fa::Vec3 backend_pos{};
    // Source 1 m to the camera's right (distance == min_distance, so no attenuation or image
    // narrowing): equal-power pan at azimuth 90 degrees is L=0, R=1.
    const StereoLevel right = play_at_level(facing_neg_z, {1.f, 0.f, 0.f}, &backend_pos);
    expectNear(backend_pos.x, 1.0, 0.0, "play_at places backend source at world x");
    expectNear(backend_pos.y, 0.0, 0.0, "play_at places backend source at world y");
    expectNear(backend_pos.z, 0.0, 0.0, "play_at places backend source at world z");
    expectNear(right.left, 0.0, 1e-6, "source at camera right: left gain 0");
    expectNear(right.right, 1.0, 1e-6, "source at camera right: right gain 1");

    // Front-right at 45 degrees: pan = sin(45deg); L = sqrt((1-p)/2), R = sqrt((1+p)/2).
    const double p = std::sin(kPi / 4.0);
    const float h = static_cast<float>(std::sqrt(0.5));
    const StereoLevel diag = play_at_level(facing_neg_z, {h, 0.f, -h}, nullptr);
    expectNear(diag.left, std::sqrt(0.5 * (1.0 - p)), 1e-5, "45deg right: equal-power left gain");
    expectNear(diag.right, std::sqrt(0.5 * (1.0 + p)), 1e-5, "45deg right: equal-power right gain");

    // Mirror symmetry: swapping the source side swaps the channels exactly.
    const StereoLevel mirror = play_at_level(facing_neg_z, {-h, 0.f, -h}, nullptr);
    expectNear(mirror.left, diag.right, 1e-6, "mirrored source swaps channels (L)");
    expectNear(mirror.right, diag.left, 1e-6, "mirrored source swaps channels (R)");

    // Camera turned 180 degrees: the same world source is now on the left.
    const StereoLevel turned = play_at_level(facing_pos_z, {1.f, 0.f, 0.f}, nullptr);
    expectNear(turned.left, 1.0, 1e-6, "camera turned 180deg: source moves to left ear");
    expectNear(turned.right, 0.0, 1e-6, "camera turned 180deg: right ear silent");

    // Camera facing the source: centred, equal-power centre gain sqrt(1/2) on each ear.
    const StereoLevel ahead = play_at_level(facing_pos_x, {1.f, 0.f, 0.f}, nullptr);
    expectNear(ahead.left, std::sqrt(0.5), 1e-5, "camera facing source: centred (L)");
    expectNear(ahead.right, std::sqrt(0.5), 1e-5, "camera facing source: centred (R)");

    // Distance still applies to play_at voices (linear curve, min 1, max 50).
    const StereoLevel far = play_at_level(facing_pos_x, {25.5f, 0.f, 0.f}, nullptr);
    expectNear(far.left, std::sqrt(0.5) * 0.5, 1e-5, "play_at voice uses distance attenuation");
}

// ---------------------------------------------------------------------------------------------
// Gate: Convolution reverb CUDA FFT within -60 dB of reference CPU FFT

void gateConvolutionReverbAccuracy() {
    const u32 block = 512;
    const u32 blocks = 24;
    const u32 ir_len = 4800;
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> uni(-1.f, 1.f);

    std::vector<float> ir(ir_len);
    for (u32 i = 0; i < ir_len; ++i) {
        ir[i] = uni(rng) * std::exp(-static_cast<float>(i) / 900.f);
    }
    std::vector<float> input(block * blocks);
    for (float& s : input) {
        s = uni(rng);
    }

    // Independent reference: direct convolution in double precision.
    std::vector<double> ref(input.size(), 0.0);
    for (size_t n = 0; n < input.size(); ++n) {
        double acc = 0.0;
        const size_t kmax = std::min<size_t>(n, ir_len - 1);
        for (size_t k = 0; k <= kmax; ++k) {
            acc += static_cast<double>(input[n - k]) * ir[k];
        }
        ref[n] = acc;
    }
    double ref_peak = 0.0;
    for (double v : ref) {
        ref_peak = std::max(ref_peak, std::fabs(v));
    }

    auto error_db = [&](const std::vector<float>& got) {
        double err = 0.0;
        for (size_t n = 0; n < got.size(); ++n) {
            err = std::max<double>(err, std::fabs(got[n] - ref[n]));
        }
        return 20.0 * std::log10(std::max(err, 1e-30) / ref_peak);
    };

    fa::ConvReverbCpu cpu;
    cpu.init(ir.data(), ir_len, block);
    std::vector<float> cpu_out(input.size());
    for (u32 b = 0; b < blocks; ++b) {
        cpu.process(input.data() + b * block, cpu_out.data() + b * block, block);
    }
    const double cpu_db = error_db(cpu_out);
    std::printf("[b7-audio] CPU FFT reverb vs direct: %.1f dB (ir %u, %u blocks)\n", cpu_db, ir_len,
                blocks);
    expectTrue(cpu_db <= -60.0, "CPU overlap-add FFT reverb within -60 dB of direct convolution");

    // Oversized blocks are split instead of wrapping the circular convolution.
    fa::ConvReverbCpu split;
    split.init(ir.data(), ir_len, block);
    std::vector<float> split_out(input.size());
    for (u32 b = 0; b < blocks; b += 4) {
        split.process(input.data() + b * block, split_out.data() + b * block, block * 4);
    }
    expectTrue(error_db(split_out) <= -60.0, "blocks larger than the plan are split correctly");

    // GPU facade: without a CUDA backend it must produce the CPU reference output, not a
    // pass-through (previous behaviour).
    fa::ReverbCuda gpu;
    gpu.init(ir.data(), ir_len, block);
    std::vector<float> gpu_out(input.size());
    for (u32 b = 0; b < blocks; ++b) {
        gpu.process(input.data() + b * block, gpu_out.data() + b * block, block);
    }
    const double gpu_db = error_db(gpu_out);
    std::printf("[b7-audio] ReverbCuda (%s) vs direct: %.1f dB\n",
                gpu.available() ? "CUDA" : "CPU fallback", gpu_db);
    expectTrue(gpu_db <= -60.0, "ReverbCuda output within -60 dB of reference");
}

void gateReverbPreservesStereoImage() {
    const u32 frames = 256;
    Scene scene(frames);
    const auto clip = register_pcm(scene.engine, std::vector<float>(4096, 0.5f), 1, 48000);
    const float ir[] = {0.5f, 0.25f};
    const auto ir_clip = register_pcm(scene.engine, std::vector<float>(ir, ir + 2), 1, 48000);
    scene.add(clip, {1.f, 0.f, 0.f});
    fa::AudioEngine::ReverbZone zone;
    zone.bounds = {{-10.f, -10.f, -10.f}, {10.f, 10.f, 10.f}};
    zone.impulse_response = ir_clip;
    zone.wet_dry = 0.5f;
    scene.engine.add_reverb_zone(zone);
    scene.engine.update(scene.registry, 1.f / 60.f);
    const std::vector<float>& out = scene.engine.last_mix_buffer();
    // Dry: L=0, R=0.5. Wet: mono send 0.25 through IR sum 0.75 = 0.1875 on both channels.
    const size_t last = (frames - 1) * 2;
    expectNear(out[last], 0.5 * 0.1875, 1e-5, "reverb: left = dry 0 + wet share");
    expectNear(out[last + 1], 0.5 * 0.5 + 0.5 * 0.1875, 1e-5,
               "reverb: right keeps its dry panning (no mono collapse)");
}

// ---------------------------------------------------------------------------------------------
// Gate: 32 simultaneous spatial sources mix without crackling at 48 kHz

void gateThirtyTwoSourcesContinuous() {
    const u32 frames = 512;
    const u32 buffers = 200; // ~2.1 s
    const u32 source_count = 32;

    struct Voice {
        std::vector<float> pcm;
        fa::Vec3 pos;
        double gain_l = 0.0;
        double gain_r = 0.0;
        double bound = 0.0; ///< 4 sin^2(pi / period): max |second difference| of a unit sine.
    };
    std::vector<Voice> voices(source_count);
    for (u32 i = 0; i < source_count; ++i) {
        const u32 period = 24 + 3 * i;              // 2000 Hz .. ~407 Hz, integer samples
        const u32 cycles = 1 + (1500 + 97 * i) / period; // loop lengths not multiples of 512
        Voice& v = voices[i];
        v.pcm = sine(period * cycles, 48000.0 / period, 48000, 1.0, 0.37 * i);
        const float angle = 2.f * 3.14159265f * static_cast<float>(i) / source_count;
        const float radius = 2.f + static_cast<float>(i % 5);
        v.pos = {radius * std::cos(angle), 0.5f * static_cast<float>(i % 3) - 0.5f,
                 radius * std::sin(angle)};
        v.bound = 4.0 * std::pow(std::sin(kPi / period), 2.0);
    }

    // Per-voice steady-state gains from single-voice renders (first sample with |s| large).
    for (u32 i = 0; i < source_count; ++i) {
        Scene solo(frames);
        const auto clip = register_pcm(solo.engine, voices[i].pcm, 1, 48000);
        solo.add(clip, voices[i].pos, 0.03f);
        solo.engine.update(solo.registry, 1.f / 60.f);
        const std::vector<float>& out = solo.engine.last_mix_buffer();
        u32 best = 0;
        for (u32 n = 0; n < frames; ++n) {
            if (std::fabs(voices[i].pcm[n]) > std::fabs(voices[i].pcm[best])) {
                best = n;
            }
        }
        voices[i].gain_l = out[best * 2] / voices[i].pcm[best];
        voices[i].gain_r = out[best * 2 + 1] / voices[i].pcm[best];
    }

    Scene scene(frames);
    for (u32 i = 0; i < source_count; ++i) {
        const auto clip = register_pcm(scene.engine, voices[i].pcm, 1, 48000);
        scene.add(clip, voices[i].pos, 0.03f);
    }

    double max_ref_err = 0.0;
    double worst_d2_ratio = 0.0;
    double peak = 0.0;
    bool finite = true;
    bool sized = true;
    double prev[2][2] = {{0.0, 0.0}, {0.0, 0.0}};
    for (u32 b = 0; b < buffers; ++b) {
        // dt deliberately differs from the buffer length (1/60 s vs 512/48000 s): playback must
        // follow the sample clock or buffers skip/repeat audio.
        scene.engine.update(scene.registry, 1.f / 60.f);
        const std::vector<float>& out = scene.engine.last_mix_buffer();
        sized = sized && out.size() == static_cast<size_t>(frames) * 2;
        for (u32 f = 0; f < frames && sized; ++f) {
            const u32 n = b * frames + f;
            for (u32 ch = 0; ch < 2; ++ch) {
                const double y = out[f * 2 + ch];
                finite = finite && std::isfinite(y);
                peak = std::max(peak, std::fabs(y));
                double ref = 0.0;
                double bound = 0.0;
                for (const Voice& v : voices) {
                    const double g = ch == 0 ? v.gain_l : v.gain_r;
                    ref += g * v.pcm[n % v.pcm.size()];
                    bound += std::fabs(g) * v.bound;
                }
                max_ref_err = std::max(max_ref_err, std::fabs(y - ref));
                if (n >= 2) {
                    const double d2 = std::fabs(y - 2.0 * prev[ch][1] + prev[ch][0]);
                    worst_d2_ratio = std::max(worst_d2_ratio, d2 / (bound + 1e-6));
                }
                prev[ch][0] = prev[ch][1];
                prev[ch][1] = y;
            }
        }
    }
    std::printf("[b7-audio] 32 sources x %u buffers: max |out-ref| = %.2e, peak = %.3f, "
                "worst second-difference / analytic bound = %.4f\n",
                buffers, max_ref_err, peak, worst_d2_ratio);
    expectTrue(sized, "every update produces a full buffer (no underrun)");
    expectTrue(finite, "32-source stream is finite");
    expectTrue(peak < 1.0, "32-source stream does not clip");
    expectTrue(max_ref_err <= 1e-5, "32-source stream matches sample-continuous reference");
    expectTrue(worst_d2_ratio <= 1.001,
               "no discontinuity: second difference within analytic sine bound (no crackle)");
}

// ---------------------------------------------------------------------------------------------
// Supporting rows: summation, pitch, voice priority, occlusion filtering, gain ramps

void gateMixerSumsLinearly() {
    const u32 frames = 256;
    auto render = [&](bool a, bool b) {
        Scene scene(frames);
        const auto ca = register_pcm(scene.engine, sine(1024, 440.0, 48000, 0.3), 1, 48000);
        const auto cb = register_pcm(scene.engine, sine(1024, 1250.0, 48000, 0.2), 1, 48000);
        if (a) {
            scene.add(ca, {3.f, 0.f, -2.f}, 0.8f);
        }
        if (b) {
            scene.add(cb, {-1.f, 1.f, 4.f}, 0.6f);
        }
        scene.engine.bus_mixer().set_bus_gain(fa::AudioBus::Sfx, 0.5f);
        scene.engine.update(scene.registry, 1.f / 60.f);
        return scene.engine.last_mix_buffer();
    };
    const std::vector<float> both = render(true, true);
    const std::vector<float> only_a = render(true, false);
    const std::vector<float> only_b = render(false, true);
    double err = 0.0;
    for (size_t i = 0; i < both.size(); ++i) {
        err = std::max<double>(err, std::fabs(both[i] - (only_a[i] + only_b[i])));
    }
    expectNear(err, 0.0, 1e-6, "mix of two voices equals the sum of each voice alone");
}

void gatePitchChangesRateNotAmplitude() {
    const u32 frames = 4800; // 0.1 s: whole periods of 1 kHz and 1.5 kHz
    Scene scene(frames);
    const auto clip = register_pcm(scene.engine, sine(48000, 1000.0, 48000, 0.5), 1, 48000);
    fa::AudioSource* src = scene.add(clip, {}, 1.f, false, true);
    src->desc.pitch = 1.5f;
    scene.engine.update(scene.registry, 1.f / 60.f);
    const std::vector<double> l = channel(scene.engine.last_mix_buffer(), 0);
    expectNear(tone_amplitude(l, 1500.0, 48000.0), 0.5, 0.01, "pitch 1.5: tone moves to 1.5 kHz");
    expectNear(tone_amplitude(l, 1000.0, 48000.0), 0.0, 0.01, "pitch 1.5: no energy left at 1 kHz");
    expectNear(src->play_head, 1.5 * frames / 48000.0, 1e-9,
               "play head advances by pitch x rendered duration");
}

void gateVoiceLimitKeepsLoudest() {
    const u32 frames = 128;
    const fa::Vec3 positions[] = {{0.f, 0.f, -40.f}, {0.f, 0.f, -2.f},  {0.f, 0.f, -30.f},
                                  {0.f, 0.f, -1.f},  {0.f, 0.f, -45.f}, {0.f, 0.f, -5.f},
                                  {0.f, 0.f, -20.f}, {0.f, 0.f, -10.f}};
    auto render = [&](u32 max_sources, const std::vector<u32>& subset) {
        Scene scene(frames, max_sources);
        const auto clip = register_pcm(scene.engine, std::vector<float>(1024, 0.1f), 1, 48000);
        for (u32 i : subset) {
            scene.add(clip, positions[i]);
        }
        scene.engine.update(scene.registry, 1.f / 60.f);
        return std::make_pair(scene.engine.last_mix_buffer(),
                              scene.engine.spatial_mixer().last_culled_voice_count());
    };
    const auto limited = render(4, {0, 1, 2, 3, 4, 5, 6, 7});
    const auto nearest = render(256, {1, 3, 5, 7}); // the four closest (loudest) voices
    double err = 0.0;
    for (size_t i = 0; i < limited.first.size(); ++i) {
        err = std::max<double>(err, std::fabs(limited.first[i] - nearest.first[i]));
    }
    expectTrue(limited.second == 4, "voice limit culls the excess voices");
    expectNear(err, 0.0, 1e-7, "voice limit keeps the most audible voices");
}

void gateOcclusionIsHighShelf() {
    const u32 frames = 9600; // 0.2 s; whole periods of both tones
    const double fs = 48000.0;
    const double lo = 100.0;
    const double hi = 8000.0;
    const fa::OcclusionAttenuation occ = fa::evaluate_occlusion_attenuation(0.f);
    const double a = 1.0 - std::exp(-2.0 * kPi * fa::SpatialMixer::kOcclusionCrossoverHz / fs);
    auto shelf = [&](double f) {
        // H(z) = hf + (1 - hf) * a / (1 - (1 - a) z^-1)
        const double w = 2.0 * kPi * f / fs;
        const double dr = 1.0 - (1.0 - a) * std::cos(w);
        const double di = (1.0 - a) * std::sin(w);
        const double den = dr * dr + di * di;
        const double re = occ.hf_gain + (1.0 - occ.hf_gain) * a * dr / den;
        const double im = -(1.0 - occ.hf_gain) * a * di / den;
        return std::sqrt(re * re + im * im);
    };

    for (double f : {lo, hi}) {
        Scene scene(frames);
        const auto clip = register_pcm(scene.engine, sine(48000, f, 48000), 1, 48000);
        fa::AudioSource* src = scene.add(clip, {0.f, 0.f, -1.f});
        src->desc.occlusion = 0.f;
        scene.engine.update(scene.registry, 1.f / 60.f); // settle filter + gain ramp
        scene.engine.update(scene.registry, 1.f / 60.f);
        const std::vector<double> l = channel(scene.engine.last_mix_buffer(), 0);
        const double expected = std::sqrt(0.5) * occ.gain * shelf(f);
        expectNear(tone_amplitude(l, f, fs), expected, expected * 0.01 + 1e-6,
                   "occluded tone amplitude matches analytic high-shelf response");
    }
    expectTrue(shelf(hi) < shelf(lo) * 0.75, "occlusion attenuates highs more than lows");
}

void gateGainRampAvoidsZipper() {
    const u32 frames = 256;
    Scene scene(frames);
    const auto clip = register_pcm(scene.engine, std::vector<float>(48000, 0.5f), 1, 48000);
    fa::AudioSource* src = scene.add(clip, {-1.f, 0.f, 0.f});
    scene.engine.update(scene.registry, 1.f / 60.f);
    const float last_left = scene.engine.last_mix_buffer()[(frames - 1) * 2];
    scene.registry.set_position(scene.registry.source_entities().front(), {1.f, 0.f, 0.f});
    (void)src;
    scene.engine.update(scene.registry, 1.f / 60.f);
    const std::vector<float>& out = scene.engine.last_mix_buffer();
    double max_step = std::fabs(out[0] - last_left);
    for (u32 f = 1; f < frames; ++f) {
        max_step = std::max(max_step, static_cast<double>(std::fabs(out[f * 2] - out[(f - 1) * 2])));
    }
    // Hard left -> hard right swings the left ear 0.5 -> 0; the ramp spreads it over
    // kGainRampFrames samples.
    expectNear(max_step, 0.5 / fa::SpatialMixer::kGainRampFrames, 1e-6,
               "teleporting source ramps gain instead of stepping");
    expectNear(out[(frames - 1) * 2], 0.0, 1e-6, "ramp settles on the new gain");
}

// ---------------------------------------------------------------------------------------------
// CPU budget gate (perf;gate label, enforced under NDEBUG)

int runPerfGate() {
    const u32 frames = 512;
    Scene scene(frames);
    std::vector<fuse::Handle<fa::AudioClip>> clips;
    for (u32 i = 0; i < 32; ++i) {
        clips.push_back(register_pcm(scene.engine, sine(4800 + i * 13, 200.0 + 50.0 * i, 48000),
                                     1, 48000));
        scene.add(clips.back(),
                  {static_cast<float>(i % 8) - 4.f, 0.f, -2.f - static_cast<float>(i / 8)}, 0.03f);
    }
    for (int i = 0; i < 20; ++i) {
        scene.engine.update(scene.registry, 1.f / 60.f);
    }
    const int iterations = 400;
    std::vector<double> ms(iterations);
    for (int i = 0; i < iterations; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        scene.engine.update(scene.registry, 1.f / 60.f);
        const auto t1 = std::chrono::steady_clock::now();
        ms[i] = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
    std::sort(ms.begin(), ms.end());
    const double median = ms[iterations / 2];
    const double p99 = ms[iterations * 99 / 100];
    const double buffer_ms = 1000.0 * frames / 48000.0;
    const double budget_ms = 0.1 * buffer_ms; // 10% of one core per 10.67 ms buffer
    std::printf("[b7-audio] perf: 32 spatial voices x %u frames: median %.3f ms, p99 %.3f ms "
                "(buffer %.2f ms, budget %.3f ms)\n",
                frames, median, p99, buffer_ms, budget_ms);
#if defined(NDEBUG)
    if (fuse::core::timingBudgetsEnforcedNoted() && median > budget_ms) {
        std::fprintf(stderr, "FAIL: 32-voice mix median %.3f ms exceeds %.3f ms budget\n", median,
                     budget_ms);
        return EXIT_FAILURE;
    }
#else
    std::printf("[b7-audio] perf budget not enforced in debug builds\n");
#endif
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    fuse::core::initialize();
    if (argc > 1 && std::strcmp(argv[1], "--perf") == 0) {
        const int rc = runPerfGate();
        fuse::core::shutdown();
        return rc;
    }
    g_requireOpenAL = argc > 1 && std::strcmp(argv[1], "--require-openal") == 0;

    gateEngineInitialises();
    gateWavLoadDecodesExactly();
    gateWavPlaysThroughMixer();
    gateAttenuationCurvesAndReadback();
    gatePlayAtPanningFollowsCamera();
    gateConvolutionReverbAccuracy();
    gateReverbPreservesStereoImage();
    gateThirtyTwoSourcesContinuous();
    gateMixerSumsLinearly();
    gatePitchChangesRateNotAmplitude();
    gateVoiceLimitKeepsLoudest();
    gateOcclusionIsHighShelf();
    gateGainRampAvoidsZipper();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_audio_b7_gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse_audio_b7_gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
