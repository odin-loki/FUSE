// GAP-AUDIO-DEVICE-OUT / MP-B7.2-OGG-RUNTIME / UNI-U7-AUDIO-1 / AP-W8.3 gates.
//
// Default run (headless, deterministic):
//   * Null backend capture equals the mixer output block-for-block for N ticks.
//   * A sine clip round-trips through the engine + Null capture within an SNR bound.
//   * Zero heap allocations per steady-state tick (Null output, capture on).
//   * Vendored xiph libs: versions match the pins; a generated sine encoded with vorbisenc decodes
//     with AudioClip::load_ogg within an SNR bound; FLAC 24-bit round-trips within 1 LSB.
//   * Cooked .fuseaudio containers (OGG / PCM_F32 / FLAC) load via load_memory and via the VFS.
//   * A 30 s streaming Ogg voice plays through the Null backend with no decode underrun, zero
//     steady-state allocations, and output identical to the fully decoded clip; looping streaming
//     voices with loop points and a play-head seek match the PCM clip bit for bit.
// `--wave`   (ctest: ALSOFT_CONF selects openal-soft's wave writer): the engine streams a sine mix
//            to a real OpenAL device; the WAV it writes must match the Null-backend reference.
// `--openal-null` (ALSOFT_DRIVERS=null): device output counters, latency report, zero game-thread
//            allocations per tick, and the device-lost -> reconnect path.
// Allocation counting interposes malloc on glibc (non-sanitizer builds) so the xiph C libraries are
// counted too; elsewhere it counts operator new.

#include <fuse/audio/audio_codec.hpp>
#include <fuse/audio/audio_container.hpp>
#include <fuse/audio/audio_engine.hpp>
#include <fuse/audio/spsc_ring.hpp>
#include <fuse/core/sanitizer.hpp>
#include <fuse/io/vfs.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(__GLIBC__) && !FUSE_SANITIZER_BUILD && !FUSE_SANITIZE_THREAD
#define FUSE_TEST_MALLOC_HOOK 1
#include <cerrno>
#else
#define FUSE_TEST_MALLOC_HOOK 0
#endif

namespace {
thread_local bool t_countAllocations = false;
std::atomic<unsigned long long> g_allocations{0};
inline void noteAllocation() {
    if (t_countAllocations) {
        g_allocations.fetch_add(1u, std::memory_order_relaxed);
    }
}
} // namespace

#if FUSE_TEST_MALLOC_HOOK
extern "C" {
void* __libc_malloc(std::size_t);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
void __libc_free(void*);

void* malloc(std::size_t size) noexcept {
    noteAllocation();
    return __libc_malloc(size);
}
void* calloc(std::size_t count, std::size_t size) noexcept {
    noteAllocation();
    return __libc_calloc(count, size);
}
void* realloc(void* ptr, std::size_t size) noexcept {
    noteAllocation();
    return __libc_realloc(ptr, size);
}
void free(void* ptr) noexcept {
    __libc_free(ptr);
}
void* aligned_alloc(std::size_t alignment, std::size_t size) noexcept {
    noteAllocation();
    return __libc_memalign(alignment, size);
}
int posix_memalign(void** out, std::size_t alignment, std::size_t size) noexcept {
    noteAllocation();
    void* p = __libc_memalign(alignment, size);
    if (p == nullptr) {
        return ENOMEM;
    }
    *out = p;
    return 0;
}
}
#else
void* operator new(std::size_t size) {
    noteAllocation();
    if (void* p = std::malloc(size == 0u ? 1u : size)) {
        return p;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) {
    return operator new(size);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
#endif

namespace {

namespace fa = fuse::audio;
using fuse::u32;
using fuse::u64;
using fuse::u8;

int g_failures = 0;
int g_checks = 0;

void expectTrue(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

struct AllocScope {
    unsigned long long start = 0;
    AllocScope() {
        start = g_allocations.load();
        t_countAllocations = true;
    }
    unsigned long long stop() {
        t_countAllocations = false;
        return g_allocations.load() - start;
    }
};

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(u32 frames, u32 channels, double hz, u32 rate, float amplitude) {
    std::vector<float> out(static_cast<size_t>(frames) * channels);
    for (u32 i = 0; i < frames; ++i) {
        const float v = amplitude * static_cast<float>(std::sin(2.0 * kPi * hz * i / rate));
        for (u32 ch = 0; ch < channels; ++ch) {
            // Second channel a fifth up so a channel swap would show.
            out[static_cast<size_t>(i) * channels + ch] =
                ch == 0 ? v : amplitude * static_cast<float>(std::sin(2.0 * kPi * hz * 1.5 * i / rate));
        }
    }
    return out;
}

/// SNR in dB of `test` against `ref` over [begin, end) samples.
double snr_db(const float* ref, const float* test, size_t count) {
    double signal = 0.0;
    double noise = 0.0;
    for (size_t i = 0; i < count; ++i) {
        signal += static_cast<double>(ref[i]) * ref[i];
        const double d = static_cast<double>(test[i]) - ref[i];
        noise += d * d;
    }
    if (noise <= 0.0) {
        return 300.0;
    }
    return 10.0 * std::log10(signal / noise);
}

fa::AudioDesc null_desc(u32 frames, u32 capture_frames) {
    fa::AudioDesc desc;
    desc.frames_per_buf = frames;
    desc.cuda_reverb = false;
    desc.output = fa::AudioOutputRequest::Null;
    desc.capture_frames = capture_frames;
    return desc;
}

struct Scene {
    fa::AudioEngine engine;
    fa::AudioRegistry registry;

    explicit Scene(const fa::AudioDesc& desc) {
        engine.init(desc);
        fa::AudioListener* listener = registry.set_listener(registry.create_entity());
        listener->forward = {0.f, 0.f, -1.f};
        listener->up = {0.f, 1.f, 0.f};
    }

    fa::AudioSource* add(fuse::Handle<fa::AudioClip> clip, bool spatial, bool looping, float volume = 1.f,
                         const fa::Vec3& pos = {}) {
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

fuse::Handle<fa::AudioClip> register_pcm(fa::AudioEngine& engine, const std::vector<float>& pcm, u32 channels,
                                         u32 rate, u32 loop_start = 0, u32 loop_end = 0) {
    fa::AudioClip clip;
    clip.load_from_pcm(pcm.data(), static_cast<u32>(pcm.size() / channels), channels, rate);
    clip.loop_start = loop_start;
    clip.loop_end = loop_end;
    return engine.register_clip(std::move(clip));
}

// ---------------------------------------------------------------------------------------------

void gateSpscRing() {
    fa::SpscRing<float> ring;
    ring.reset(1000); // rounds to 1024
    expectTrue(ring.capacity() == 1024, "spsc: capacity rounds up to a power of two");
    std::vector<float> in(700);
    std::vector<float> out(700);
    for (size_t i = 0; i < in.size(); ++i) {
        in[i] = static_cast<float>(i);
    }
    expectTrue(ring.write(in.data(), 700), "spsc: write fits");
    expectTrue(!ring.write(in.data(), 400), "spsc: all-or-nothing write rejects overflow");
    expectTrue(ring.read(out.data(), 700) && out == in, "spsc: read returns written data");
    expectTrue(ring.write(in.data(), 700) && ring.read(out.data(), 700) && out == in,
               "spsc: wrap-around write/read preserves data");

    // Two threads: 2M floats through a 4096-slot ring in odd-sized chunks, order preserved.
    ring.reset(4096);
    constexpr u32 kTotal = 2'000'000;
    std::atomic<bool> ok{true};
    std::thread consumer([&] {
        float buf[77];
        u32 next = 0;
        while (next < kTotal) {
            const u32 want = std::min<u32>(77, kTotal - next);
            if (!ring.read(buf, want)) {
                std::this_thread::yield();
                continue;
            }
            for (u32 i = 0; i < want; ++i) {
                if (buf[i] != static_cast<float>((next + i) % 16777216u)) {
                    ok = false;
                }
            }
            next += want;
        }
    });
    float chunk[53];
    u32 produced = 0;
    while (produced < kTotal) {
        const u32 n = std::min<u32>(53, kTotal - produced);
        for (u32 i = 0; i < n; ++i) {
            chunk[i] = static_cast<float>((produced + i) % 16777216u);
        }
        if (ring.write(chunk, n)) {
            produced += n;
        } else {
            std::this_thread::yield();
        }
    }
    consumer.join();
    expectTrue(ok.load(), "spsc: producer/consumer threads see every element in order");
}

void gateNullCaptureEqualsMix() {
    const u32 frames = 256;
    const u32 ticks = 40;
    Scene scene(null_desc(frames, frames * ticks));
    expectTrue(scene.engine.backend_kind() == fa::AudioBackendKind::Null, "null: forced Null backend");
    const auto tone = register_pcm(scene.engine, sine(48000, 1, 440.0, 48000, 0.5f), 1, 48000);
    const auto stereo = register_pcm(scene.engine, sine(7000, 2, 330.0, 44100, 0.25f), 2, 44100);
    scene.add(tone, true, true, 0.8f, {2.f, 0.f, -3.f});
    scene.add(stereo, false, true, 0.6f);

    std::vector<float> reference;
    for (u32 t = 0; t < ticks; ++t) {
        if (t == 10) {
            scene.engine.play_2d(tone, 0.3f);
        }
        scene.engine.update(scene.registry, 1.f / 60.f);
        const std::vector<float>& mix = scene.engine.last_mix_buffer();
        reference.insert(reference.end(), mix.begin(), mix.end());
    }
    const std::vector<float>& cap = scene.engine.output_capture();
    expectTrue(cap.size() == reference.size(), "null: capture holds every tick's block");
    expectTrue(cap.size() == reference.size()
                   && std::memcmp(cap.data(), reference.data(), cap.size() * sizeof(float)) == 0,
               "null: capture equals the mixer output bit for bit for N ticks");
    const fa::AudioOutputStats stats = scene.engine.output_stats();
    expectTrue(stats.frames_submitted == static_cast<u64>(frames) * ticks && stats.frames_played == stats.frames_submitted
                   && stats.frames_dropped == 0 && stats.underruns == 0,
               "null: output counters (submitted == played == N * frames, no drops/underruns)");
    expectTrue(scene.engine.blocks_rendered() == ticks, "null: one block per update (caller pacing)");
}

void gateSineRoundTrip() {
    const u32 frames = 480;
    const u32 ticks = 50; // 0.5 s
    Scene scene(null_desc(frames, frames * ticks));
    const std::vector<float> clip = sine(frames * ticks, 1, 1000.0, 48000, 0.5f);
    scene.add(register_pcm(scene.engine, clip, 1, 48000), false, false);
    for (u32 t = 0; t < ticks; ++t) {
        scene.engine.update(scene.registry, 1.f / 60.f);
    }
    const std::vector<float>& cap = scene.engine.output_capture();
    std::vector<float> left(frames * ticks);
    std::vector<float> right(frames * ticks);
    for (u32 i = 0; i < frames * ticks; ++i) {
        left[i] = cap[static_cast<size_t>(i) * 2];
        right[i] = cap[static_cast<size_t>(i) * 2 + 1];
    }
    const double snr_l = snr_db(clip.data(), left.data(), clip.size());
    const double snr_r = snr_db(clip.data(), right.data(), clip.size());
    std::printf("[audio-out] sine round trip through Null capture: SNR L %.1f dB, R %.1f dB\n", snr_l, snr_r);
    expectTrue(snr_l > 120.0 && snr_r > 120.0, "sine clip round-trips through the engine within 120 dB SNR");
}

void gateZeroAllocationsPerTick() {
    const u32 frames = 512;
    Scene scene(null_desc(frames, frames * 400));
    const auto tone = register_pcm(scene.engine, sine(48000, 1, 440.0, 48000, 0.5f), 1, 48000);
    for (int i = 0; i < 8; ++i) {
        scene.add(tone, true, true, 0.5f, {static_cast<float>(i) - 4.f, 0.f, -5.f});
    }
    for (int t = 0; t < 20; ++t) {
        scene.engine.update(scene.registry, 1.f / 60.f); // warm-up
    }
    AllocScope scope;
    for (int t = 0; t < 200; ++t) {
        scene.engine.update(scene.registry, 1.f / 60.f);
    }
    const unsigned long long allocs = scope.stop();
    std::printf("[audio-out] steady-state ticks (Null output + capture): %llu allocations / 200 ticks (%s)\n",
                allocs, FUSE_TEST_MALLOC_HOOK ? "malloc hook" : "operator new");
    expectTrue(allocs == 0, "zero heap allocations per steady-state tick (mix + output submit)");
}

// ---- xiph codecs -----------------------------------------------------------------------------

void gateCodecs() {
    expectTrue(fa::ogg_vorbis_available(), "vendored Ogg Vorbis built (FUSE_HAS_OGG_VORBIS)");
    expectTrue(fa::flac_available(), "vendored libFLAC built (FUSE_HAS_FLAC)");
    expectTrue(std::strcmp(fa::vorbis_library_version(), "Xiph.Org libVorbis 1.3.7") == 0,
               "libvorbis reports the pinned version 1.3.7");
    expectTrue(std::strcmp(fa::flac_library_version(), "1.5.0") == 0, "libFLAC reports the pinned version 1.5.0");

    // Ogg Vorbis: 2 s stereo sine, encoded in-test with vorbisenc, decoded with load_ogg.
    const u32 rate = 48000;
    const u32 frames = rate * 2;
    const std::vector<float> pcm = sine(frames, 2, 440.0, rate, 0.5f);
    std::vector<u8> ogg;
    expectTrue(fa::encode_ogg_vorbis(pcm.data(), frames, 2, rate, 0.6f, ogg), "vorbisenc encodes a generated sine");
    fa::AudioClip clip;
    expectTrue(clip.load_ogg(ogg.data(), ogg.size()), "AudioClip::load_ogg decodes the stream");
    expectTrue(clip.frame_count() == frames && clip.channel_count == 2 && clip.sample_rate == rate,
               "load_ogg: frame count / channels / rate match the source");
    if (clip.frame_count() == frames) {
        const double snr = snr_db(pcm.data(), clip.samples.data(), pcm.size());
        std::printf("[audio-out] Ogg Vorbis q0.6 round trip: %zu bytes (%.1f kbit/s), SNR %.1f dB\n", ogg.size(),
                    ogg.size() * 8.0 / 2.0 / 1000.0, snr);
        expectTrue(snr > 30.0, "Ogg Vorbis sine round trip SNR > 30 dB");
    }
    fa::AudioClip sniffed;
    expectTrue(sniffed.load_memory(ogg.data(), ogg.size()) && sniffed.samples == clip.samples,
               "load_memory sniffs OggS and decodes identically");
    std::vector<u8> corrupt(ogg.begin(), ogg.begin() + 64);
    fa::AudioClip bad;
    expectTrue(!bad.load_ogg(corrupt.data(), corrupt.size()), "truncated Ogg header is rejected");

    // FLAC: 24-bit round trip is exact to 1 LSB.
    std::vector<u8> flac;
    expectTrue(fa::encode_flac(pcm.data(), frames, 2, rate, 24, flac), "libFLAC encodes 24-bit PCM");
    fa::AudioClip flac_clip;
    expectTrue(flac_clip.load_flac(flac.data(), flac.size()), "AudioClip::load_flac decodes the stream");
    expectTrue(flac_clip.frame_count() == frames && flac_clip.channel_count == 2 && flac_clip.sample_rate == rate,
               "load_flac: frame count / channels / rate match");
    if (flac_clip.frame_count() == frames) {
        double max_err = 0.0;
        for (size_t i = 0; i < pcm.size(); ++i) {
            max_err = std::max(max_err, std::fabs(static_cast<double>(flac_clip.samples[i]) - pcm[i]));
        }
        std::printf("[audio-out] FLAC 24-bit round trip: %zu bytes, max |err| = %.3g (1 LSB = %.3g)\n", flac.size(),
                    max_err, 1.0 / 8388608.0);
        expectTrue(max_err <= 1.0 / 8388608.0, "FLAC 24-bit round trip within 1 LSB");
    }
    fa::AudioClip flac_sniffed;
    expectTrue(flac_sniffed.load_memory(flac.data(), flac.size()) && flac_sniffed.samples == flac_clip.samples,
               "load_memory sniffs fLaC");
}

void gateContainers() {
    const u32 rate = 44100;
    const u32 frames = rate; // 1 s
    const std::vector<float> pcm = sine(frames, 1, 523.25, rate, 0.4f);

    // PCM_F32 container: exact.
    const std::vector<u8> pcm_file = fa::write_fuseaudio(fa::FuseAudioPayload::PcmF32, rate, 1, frames,
                                                         reinterpret_cast<const u8*>(pcm.data()),
                                                         pcm.size() * sizeof(float), 100, 40000);
    fa::AudioClip pcm_clip;
    expectTrue(pcm_clip.load_memory(pcm_file.data(), pcm_file.size()) && pcm_clip.samples == pcm
                   && pcm_clip.sample_rate == rate && pcm_clip.loop_start == 100 && pcm_clip.loop_end == 40000,
               "FUSEAUDIO_PCM_F32 container loads exactly, with loop points");
    std::vector<u8> short_file(pcm_file.begin(), pcm_file.end() - 4);
    fa::AudioClip short_clip;
    expectTrue(!short_clip.load_memory(short_file.data(), short_file.size()),
               "PCM_F32 container with a payload shorter than the header says is rejected");

    // OGG container in the exact layout Tools/FUSE/Cook writes (extra informational keys).
    std::vector<u8> ogg;
    fa::encode_ogg_vorbis(pcm.data(), frames, 1, rate, 0.5f, ogg);
    std::string header = "FUSEAUDIO_OGG\nhook=ogg\nrate=44100\nformat=OGG\nchannels=1\nsamples=44100\n"
                         "encoder=vorbisenc_encode_ok\nwav=yes\nDATA\n";
    std::vector<u8> ogg_file(header.begin(), header.end());
    ogg_file.insert(ogg_file.end(), ogg.begin(), ogg.end());
    fa::AudioClip ogg_clip;
    expectTrue(ogg_clip.load_memory(ogg_file.data(), ogg_file.size()) && ogg_clip.frame_count() == frames
                   && !ogg_clip.is_streaming(),
               "cook-format FUSEAUDIO_OGG container decodes (short clip -> PCM)");
    fa::AudioClipLoadOptions stream_opts;
    stream_opts.stream_threshold_seconds = 0.5f;
    fa::AudioClip ogg_stream;
    expectTrue(ogg_stream.load_memory(ogg_file.data(), ogg_file.size(), stream_opts) && ogg_stream.is_streaming()
                   && ogg_stream.frame_count() == frames && ogg_stream.samples.empty(),
               "FUSEAUDIO_OGG longer than the threshold stays encoded (streaming clip)");

    const std::string stub = "FUSEAUDIO_STUB\nhook=ogg\nrate=48000\nformat=OGG\nsamples=0\n";
    fa::AudioClip stub_clip;
    expectTrue(!stub_clip.load_memory(reinterpret_cast<const u8*>(stub.data()), stub.size()),
               "FUSEAUDIO_STUB placeholder is rejected");

    // FLAC container.
    std::vector<u8> flac;
    fa::encode_flac(pcm.data(), frames, 1, rate, 16, flac);
    const std::vector<u8> flac_file =
        fa::write_fuseaudio(fa::FuseAudioPayload::Flac, rate, 1, frames, flac.data(), flac.size());
    fa::AudioClip flac_clip;
    expectTrue(flac_clip.load_memory(flac_file.data(), flac_file.size()) && flac_clip.frame_count() == frames,
               "FUSEAUDIO_FLAC container decodes");

    // AudioEngine::load_clip through a VFS mount (cooked file on disk, virtual path).
    const std::filesystem::path dir = std::filesystem::temp_directory_path()
        / ("fuse_audio_output_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    {
        std::ofstream(dir / "tone.fuseaudio", std::ios::binary)
            .write(reinterpret_cast<const char*>(ogg_file.data()), static_cast<std::streamsize>(ogg_file.size()));
        std::ofstream(dir / "tone_pcm.fuseaudio", std::ios::binary)
            .write(reinterpret_cast<const char*>(pcm_file.data()), static_cast<std::streamsize>(pcm_file.size()));
    }
    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();
    vfs.mount(fuse::io::MountKind::Game, dir.string(), "fuse_audio_test_vfs:/");
    fa::AudioEngine engine;
    fa::AudioDesc desc = null_desc(256, 0);
    desc.stream_threshold_seconds = 0.5f;
    engine.init(desc);
    const auto h_ogg = engine.load_clip("fuse_audio_test_vfs:/tone.fuseaudio");
    const auto h_pcm = engine.load_clip("fuse_audio_test_vfs:/tone_pcm.fuseaudio");
    const fa::AudioClip* c_ogg = engine.find_clip(h_ogg);
    const fa::AudioClip* c_pcm = engine.find_clip(h_pcm);
    expectTrue(c_ogg != nullptr && c_ogg->is_streaming() && c_ogg->frame_count() == frames,
               "AudioEngine::load_clip reads a cooked OGG through the VFS and dispatches by format (streaming)");
    expectTrue(c_pcm != nullptr && c_pcm->samples == pcm, "AudioEngine::load_clip reads a cooked PCM_F32 through the VFS");
    expectTrue(!engine.load_clip("fuse_audio_test_vfs:/missing.fuseaudio").isValid(), "missing VFS file -> invalid handle");
    engine.destroy();
    vfs.unmount("fuse_audio_test_vfs:/");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- streaming voices ------------------------------------------------------------------------

void gateStreaming30s() {
    const u32 rate = 48000;
    const u32 seconds = 30;
    const u32 frames = rate * seconds;
    // Sweep so every block differs (a stuck or repeated window would show).
    std::vector<float> pcm(frames);
    double phase = 0.0;
    for (u32 i = 0; i < frames; ++i) {
        const double hz = 200.0 + 600.0 * static_cast<double>(i) / frames;
        phase += 2.0 * kPi * hz / rate;
        pcm[i] = 0.5f * static_cast<float>(std::sin(phase));
    }
    std::vector<u8> ogg;
    const auto t0 = std::chrono::steady_clock::now();
    expectTrue(fa::encode_ogg_vorbis(pcm.data(), frames, 1, rate, 0.4f, ogg), "30 s sweep encodes");
    const double encode_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    fa::DecodedAudio full;
    expectTrue(fa::decode_ogg_vorbis(ogg.data(), ogg.size(), full) && full.frames == frames, "reference full decode");

    const u32 block = 512;
    const u32 ticks = (frames + block - 1) / block + 4;
    fa::AudioDesc desc = null_desc(block, ticks * block);
    desc.stream_threshold_seconds = 10.f;
    Scene scene(desc);
    const auto handle = scene.engine.load_clip_memory(ogg.data(), ogg.size());
    const fa::AudioClip* clip = scene.engine.find_clip(handle);
    expectTrue(clip != nullptr && clip->is_streaming(), "30 s Ogg clip loads as a streaming clip");
    if (clip == nullptr) {
        return;
    }
    fa::AudioSource* source = scene.add(handle, false, false);

    unsigned long long steady_allocs = 0;
    const auto t1 = std::chrono::steady_clock::now();
    for (u32 t = 0; t < ticks; ++t) {
        if (t == 8) {
            AllocScope scope;
            for (; t < ticks - 8; ++t) {
                scene.engine.update(scene.registry, 1.f / 60.f);
            }
            steady_allocs = scope.stop();
        }
        scene.engine.update(scene.registry, 1.f / 60.f);
    }
    const double mix_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    const u64 underruns = scene.engine.spatial_mixer().stream_underruns();
    std::printf("[audio-out] 30 s streaming voice: encode %.2f s, %u ticks mixed in %.3f s (%.0fx real time), "
                "stream underruns %llu, seeks %llu, steady-state allocations %llu\n",
                encode_s, ticks, mix_s, seconds / std::max(mix_s, 1e-9), static_cast<unsigned long long>(underruns),
                static_cast<unsigned long long>(scene.engine.spatial_mixer().stream_seeks()), steady_allocs);
    expectTrue(underruns == 0, "30 s streaming voice: no decode underrun");
    expectTrue(steady_allocs == 0, "30 s streaming voice: zero steady-state allocations");
    expectTrue(!source->playing, "streaming voice finished at the end of the clip");
    expectTrue(scene.engine.spatial_mixer().active_stream_voices() == 0, "finished voice released its decoder");

    const std::vector<float>& cap = scene.engine.output_capture();
    bool exact = cap.size() >= static_cast<size_t>(frames) * 2;
    double max_err = 0.0;
    for (u32 i = 0; exact && i < frames; ++i) {
        max_err = std::max(max_err, std::fabs(static_cast<double>(cap[static_cast<size_t>(i) * 2]) - full.samples[i]));
        max_err = std::max(max_err,
                           std::fabs(static_cast<double>(cap[static_cast<size_t>(i) * 2 + 1]) - full.samples[i]));
    }
    std::printf("[audio-out] streaming output vs full decode: max |err| = %.3g\n", max_err);
    // The mixer's bus gain product is not exactly 1.0f, so compare within float rounding here; the
    // loop/seek gate below proves streaming == PCM voices bit for bit through the same mixer.
    expectTrue(exact && max_err <= 1e-6, "streaming voice output equals the fully decoded clip (float rounding)");
    const double snr = exact ? snr_db(pcm.data(), full.samples.data(), frames) : 0.0;
    expectTrue(snr > 25.0, "30 s sweep decodes with SNR > 25 dB vs the source");
}

/// Streaming vs PCM engines with the same loop points / pitch / seek: identical mixes.
void gateStreamingLoopAndSeek() {
    const u32 rate = 44100;
    const u32 frames = rate * 12;
    std::vector<float> pcm(static_cast<size_t>(frames) * 2);
    for (u32 i = 0; i < frames; ++i) {
        pcm[static_cast<size_t>(i) * 2] = 0.4f * static_cast<float>(std::sin(2.0 * kPi * 330.0 * i / rate));
        pcm[static_cast<size_t>(i) * 2 + 1] =
            0.3f * static_cast<float>(std::sin(2.0 * kPi * (500.0 + 100.0 * i / frames) * i / rate));
    }
    std::vector<u8> ogg;
    fa::encode_ogg_vorbis(pcm.data(), frames, 2, rate, 0.3f, ogg);
    fa::DecodedAudio full;
    fa::decode_ogg_vorbis(ogg.data(), ogg.size(), full);
    const u32 loop_start = rate * 1 + 123;
    const u32 loop_end = rate * 11 + 77;

    const u32 block = 480;
    fa::AudioDesc desc = null_desc(block, 0);
    desc.stream_threshold_seconds = 5.f;
    Scene streamed(desc);
    Scene reference(desc);

    fa::AudioClip sclip;
    fa::AudioClipLoadOptions opts;
    opts.stream_threshold_seconds = 5.f;
    sclip.load_memory(ogg.data(), ogg.size(), opts);
    sclip.loop_start = loop_start;
    sclip.loop_end = loop_end;
    expectTrue(sclip.is_streaming(), "loop test clip streams");
    const auto hs = streamed.engine.register_clip(std::move(sclip));
    const auto hr = register_pcm(reference.engine, full.samples, 2, rate, loop_start, loop_end);

    // Voice A: looping, non-spatial stereo, pitch 1.25 (44.1 kHz clip on the 48 kHz bus).
    fa::AudioSource* sa = streamed.add(hs, false, true, 0.9f);
    fa::AudioSource* ra = reference.add(hr, false, true, 0.9f);
    sa->desc.pitch = ra->desc.pitch = 1.25f;
    // Voice B: spatial, looping, starts mid-clip.
    fa::AudioSource* sb = streamed.add(hs, true, true, 0.7f, {3.f, 0.f, -2.f});
    fa::AudioSource* rb = reference.add(hr, true, true, 0.7f, {3.f, 0.f, -2.f});
    sb->play_head = rb->play_head = 7.5;

    const u32 ticks = 48000 * 25 / block; // 25 s: voice A wraps its loop ~2.9 times
    double max_err = 0.0;
    u64 alloc_ticks = 0;
    unsigned long long loop_allocs = 0;
    for (u32 t = 0; t < ticks; ++t) {
        if (t == ticks / 2) {
            // Seek: jump both B voices.
            sb->play_head = rb->play_head = 2.25;
        }
        const bool counted = t > 40 && t != ticks / 2 && t != ticks / 2 + 1;
        if (counted) {
            AllocScope scope;
            streamed.engine.update(streamed.registry, 1.f / 60.f);
            loop_allocs += scope.stop();
            ++alloc_ticks;
        } else {
            streamed.engine.update(streamed.registry, 1.f / 60.f);
        }
        reference.engine.update(reference.registry, 1.f / 60.f);
        const std::vector<float>& a = streamed.engine.last_mix_buffer();
        const std::vector<float>& b = reference.engine.last_mix_buffer();
        for (size_t i = 0; i < a.size(); ++i) {
            max_err = std::max(max_err, std::fabs(static_cast<double>(a[i]) - b[i]));
        }
    }
    const u64 underruns = streamed.engine.spatial_mixer().stream_underruns();
    std::printf("[audio-out] streaming loop/seek vs PCM: max |err| = %.3g over %u ticks, underruns %llu, seeks %llu, "
                "allocations %llu over %llu ticks (loop wraps + seek included)\n",
                max_err, ticks, static_cast<unsigned long long>(underruns),
                static_cast<unsigned long long>(streamed.engine.spatial_mixer().stream_seeks()), loop_allocs,
                static_cast<unsigned long long>(alloc_ticks));
    expectTrue(max_err == 0.0, "looping streaming voices (loop points, pitch, seek) match the PCM clip bit for bit");
    expectTrue(underruns == 0, "looping streaming voices: no decode underrun");
    expectTrue(loop_allocs == 0, "looping streaming voices: zero allocations across loop wraps");
    expectTrue(sa->playing && sb->playing, "looping voices keep playing");
}

// ---- OpenAL device output --------------------------------------------------------------------

#if defined(FUSE_AUDIO_OPENAL)
int runWave(const char* wav_path) {
    const u32 block = 512;
    const u32 rate = 48000;
    const u32 clip_frames = rate; // 1 s
    const std::vector<float> clip = sine(clip_frames, 2, 440.0, rate, 0.5f);

    // Reference: the same scene through the Null backend.
    const u32 total_blocks = clip_frames / block + 20;
    std::vector<float> reference;
    {
        Scene ref(null_desc(block, total_blocks * block));
        ref.add(register_pcm(ref.engine, clip, 2, rate), false, false);
        for (u32 t = 0; t < total_blocks; ++t) {
            ref.engine.update(ref.registry, 1.f / 60.f);
        }
        reference = ref.engine.output_capture();
    }

    std::error_code ec;
    std::filesystem::remove(wav_path, ec);
    fa::AudioOutputStats stats{};
    unsigned long long tick_allocs = 0;
    u64 ticks = 0;
    u64 streaming_underruns = 0;
    {
        fa::AudioDesc desc;
        desc.frames_per_buf = block;
        desc.cuda_reverb = false;
        desc.output = fa::AudioOutputRequest::OpenAL;
        desc.pacing = fa::AudioPacing::Device;
        desc.device_buffers = 4;
        desc.output_ring_blocks = 16; // ~170 ms of slack: a loaded CI box must not starve the device
        Scene scene(desc);
        if (scene.engine.backend_kind() != fa::AudioBackendKind::OpenAL) {
            std::printf("FAIL: OpenAL wave device did not open (ALSOFT_CONF / wave driver)\n");
            return 1;
        }
        scene.add(register_pcm(scene.engine, clip, 2, rate), false, false);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (scene.engine.blocks_rendered() < total_blocks && std::chrono::steady_clock::now() < deadline) {
            if (scene.engine.blocks_rendered() >= 16) { // after warm-up (voice state, first blocks)
                AllocScope scope;
                scene.engine.update(scene.registry, 1.f / 60.f);
                tick_allocs += scope.stop();
                ++ticks;
            } else {
                scene.engine.update(scene.registry, 1.f / 60.f);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Everything is submitted; an underrun before this point would be a real one (after it,
        // the device legitimately runs dry at the end of the stream).
        streaming_underruns = scene.engine.output_stats().underruns;
        // Drain: wait until the device played everything that was submitted.
        while (std::chrono::steady_clock::now() < deadline) {
            stats = scene.engine.output_stats();
            if (stats.frames_played >= stats.frames_submitted) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        stats = scene.engine.output_stats();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        scene.engine.destroy(); // closes the device: the wave writer finalises the RIFF sizes
    }
    std::printf("[audio-out] wave device: submitted %llu played %llu dropped %llu underruns %llu silence %llu, "
                "underruns while streaming %llu, float32=%d, %llu steady ticks with %llu game-thread allocations\n",
                static_cast<unsigned long long>(stats.frames_submitted),
                static_cast<unsigned long long>(stats.frames_played),
                static_cast<unsigned long long>(stats.frames_dropped),
                static_cast<unsigned long long>(stats.underruns),
                static_cast<unsigned long long>(stats.silence_frames),
                static_cast<unsigned long long>(streaming_underruns), stats.float32_output ? 1 : 0,
                static_cast<unsigned long long>(ticks), tick_allocs);
    expectTrue(stats.frames_dropped == 0, "wave: device pacing never drops a block");
    expectTrue(streaming_underruns == 0, "wave: no underrun while the engine was streaming");
    expectTrue(stats.frames_played >= stats.frames_submitted, "wave: every submitted frame was played");
    expectTrue(stats.float32_output, "wave: AL_EXT_float32 streaming buffers");
    expectTrue(tick_allocs == 0, "wave: zero game-thread allocations per tick (mix + SPSC submit)");

    fa::AudioClip wav;
    expectTrue(wav.load_wav(wav_path), "wave: openal-soft wrote a readable WAV");
    if (wav.channel_count != 2 || wav.sample_rate != rate) {
        std::printf("FAIL: wave format %u ch %u Hz\n", wav.channel_count, wav.sample_rate);
        return 1;
    }
    // Align on the first non-zero frame (the device plays exact silence before the stream starts).
    auto first_nonzero = [](const std::vector<float>& s) {
        for (size_t i = 0; i < s.size(); ++i) {
            if (std::fabs(s[i]) > 1e-7f) {
                return i / 2;
            }
        }
        return s.size() / 2;
    };
    const size_t ref0 = first_nonzero(reference);
    const size_t wav0 = first_nonzero(wav.samples);
    const size_t compare = clip_frames - 64;
    const bool fits = wav0 + compare <= wav.samples.size() / 2 && ref0 + compare <= reference.size() / 2;
    expectTrue(fits, "wave: WAV holds the whole streamed clip");
    if (fits) {
        const double snr = snr_db(reference.data() + ref0 * 2, wav.samples.data() + wav0 * 2, compare * 2);
        std::printf("[audio-out] wave file vs Null reference: onset frame %zu, SNR %.1f dB over %zu frames\n", wav0,
                    snr, compare);
        expectTrue(snr > 60.0, "wave: device output matches the reference mix (SNR > 60 dB)");
    }
    return 0;
}

int runOpenALNull() {
    fa::AudioDesc desc;
    desc.frames_per_buf = 256;
    desc.cuda_reverb = false;
    desc.output = fa::AudioOutputRequest::OpenAL;
    desc.pacing = fa::AudioPacing::Device;
    Scene scene(desc);
    if (scene.engine.backend_kind() != fa::AudioBackendKind::OpenAL) {
        std::printf("FAIL: OpenAL null device did not open\n");
        return 1;
    }
    scene.add(register_pcm(scene.engine, sine(48000, 1, 440.0, 48000, 0.5f), 1, 48000), true, true, 0.7f,
              {1.f, 0.f, -2.f});
    auto pump_for = [&](int ms, unsigned long long* allocs) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            if (allocs != nullptr) {
                AllocScope scope;
                scene.engine.update(scene.registry, 1.f / 60.f);
                *allocs += scope.stop();
            } else {
                scene.engine.update(scene.registry, 1.f / 60.f);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    pump_for(100, nullptr);
    unsigned long long allocs = 0;
    pump_for(300, &allocs);
    fa::AudioOutputStats before = scene.engine.output_stats();
    std::printf("[audio-out] OpenAL null device: played %llu frames, latency %.1f ms, queued %u frames, "
                "underruns %llu, float32=%d, allocations %llu\n",
                static_cast<unsigned long long>(before.frames_played), before.latency_seconds * 1000.0,
                before.queued_frames, static_cast<unsigned long long>(before.underruns),
                before.float32_output ? 1 : 0, allocs);
    expectTrue(before.frames_played > 0, "openal: the device consumes the streamed mix");
    expectTrue(before.latency_seconds > 0.0 && before.latency_seconds < 1.0, "openal: latency is reported (0, 1) s");
    expectTrue(before.connected, "openal: device connected");
    expectTrue(allocs == 0, "openal: zero game-thread allocations per tick");

    scene.engine.simulate_output_device_loss();
    pump_for(300, nullptr);
    const fa::AudioOutputStats after = scene.engine.output_stats();
    std::printf("[audio-out] after simulated device loss: lost %llu, reconnects %llu, played %llu\n",
                static_cast<unsigned long long>(after.device_lost), static_cast<unsigned long long>(after.reconnects),
                static_cast<unsigned long long>(after.frames_played));
    expectTrue(after.device_lost == 1 && after.reconnects == 1 && after.connected,
               "openal: device loss is detected and the device is re-opened");
    expectTrue(after.frames_played > before.frames_played, "openal: playback continues after reconnect");

    // Starve the device (no updates): silence is inserted and underruns are counted.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const fa::AudioOutputStats starved = scene.engine.output_stats();
    expectTrue(starved.underruns > after.underruns && starved.silence_frames > after.silence_frames,
               "openal: starving the device counts underruns and inserts silence");
    scene.engine.destroy();
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    int rc = 0;
    if (argc > 2 && std::strcmp(argv[1], "--wave") == 0) {
#if defined(FUSE_AUDIO_OPENAL)
        rc = runWave(argv[2]);
#else
        std::printf("fuse_audio_output: no OpenAL in this build\n");
        return 77;
#endif
    } else if (argc > 1 && std::strcmp(argv[1], "--openal-null") == 0) {
#if defined(FUSE_AUDIO_OPENAL)
        rc = runOpenALNull();
#else
        std::printf("fuse_audio_output: no OpenAL in this build\n");
        return 77;
#endif
    } else {
        gateSpscRing();
        gateNullCaptureEqualsMix();
        gateSineRoundTrip();
        gateZeroAllocationsPerTick();
        gateCodecs();
        gateContainers();
        gateStreaming30s();
        gateStreamingLoopAndSeek();
    }
    if (rc != 0 || g_failures != 0) {
        std::printf("fuse_audio_output: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    std::printf("fuse_audio_output: all %d checks passed\n", g_checks);
    return 0;
}
