#include <fuse/audio/attenuation.hpp>
#include <fuse/audio/audio_clip.hpp>
#include <fuse/audio/audio_engine.hpp>
#include <fuse/audio/audio_registry.hpp>
#include <fuse/audio/conv_reverb_cpu.hpp>
#include <fuse/core/init.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

using fuse::u32;

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message, value, expected);
        ++g_failures;
    }
}

std::filesystem::path write_test_wav(const std::filesystem::path& dir, const char* name,
                                     u32 channels, const std::vector<std::int16_t>& pcm,
                                     u32 sample_rate) {
    const std::filesystem::path path = dir / name;
    std::ofstream out(path, std::ios::binary);
    const u32 data_size = static_cast<u32>(pcm.size() * sizeof(std::int16_t));
    const std::uint16_t audio_format = 1;
    const std::uint16_t bits_per_sample = 16;
    const u32 byte_rate = sample_rate * channels * bits_per_sample / 8;
    const std::uint16_t block_align =
        static_cast<std::uint16_t>(channels * bits_per_sample / 8);
    const u32 riff_size = 36 + data_size;

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riff_size), 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    const u32 fmt_size = 16;
    out.write(reinterpret_cast<const char*>(&fmt_size), 4);
    out.write(reinterpret_cast<const char*>(&audio_format), 2);
    out.write(reinterpret_cast<const char*>(&channels), 2);
    out.write(reinterpret_cast<const char*>(&sample_rate), 4);
    out.write(reinterpret_cast<const char*>(&byte_rate), 4);
    out.write(reinterpret_cast<const char*>(&block_align), 2);
    out.write(reinterpret_cast<const char*>(&bits_per_sample), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&data_size), 4);
    out.write(reinterpret_cast<const char*>(pcm.data()),
              static_cast<std::streamsize>(data_size));
    return path;
}

void testEngineInitializes() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.cuda_reverb = false;
    engine.init(desc);
    expectTrue(engine.is_initialized(), "audio engine initializes backend");
}

void testMonoAndStereoWavLoad() {
    const std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / "fuse_audio_b72";
    std::filesystem::create_directories(temp_dir);

    const std::vector<std::int16_t> mono_pcm = {0, 16000, 0, -16000};
    const std::vector<std::int16_t> stereo_pcm = {1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000};

    const auto mono_path = write_test_wav(temp_dir, "mono.wav", 1, mono_pcm, 44100);
    const auto stereo_path = write_test_wav(temp_dir, "stereo.wav", 2, stereo_pcm, 48000);

    fuse::audio::AudioClip mono;
    fuse::audio::AudioClip stereo;
    expectTrue(mono.load_wav(mono_path.string().c_str()), "mono WAV loads");
    expectTrue(stereo.load_wav(stereo_path.string().c_str()), "stereo WAV loads");
    expectTrue(mono.channel_count == 1, "mono channel count");
    expectTrue(stereo.channel_count == 2, "stereo channel count");
    expectNear(mono.duration, 4.f / 44100.f, 1e-4f, "mono duration");
    expectNear(stereo.duration, 4.f / 48000.f, 1e-4f, "stereo duration");
}

void testSpatialAttenuationAtMaxDistance() {
    const float gain = fuse::audio::compute_attenuation(50.f, 1.f, 50.f);
    expectTrue(gain <= 0.001f, "source at max_distance has near-zero volume");
    expectNear(fuse::audio::compute_attenuation(0.5f, 1.f, 50.f), 1.f, 1e-5f,
               "inside min distance is full gain");
}

void testPlayAtPositionsSource() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 256;
    engine.init(desc);

    fuse::audio::AudioRegistry registry;
    const fuse::audio::EntityId listener_entity = registry.create_entity();
    registry.set_position(listener_entity, fuse::audio::Vec3{0.f, 0.f, 0.f});
    fuse::audio::AudioListener* listener = registry.set_listener(listener_entity);
    listener->forward = fuse::audio::Vec3{0.f, 0.f, -1.f};

    std::vector<float> pcm(480, 0.25f);
    fuse::audio::AudioClip clip;
    clip.load_from_pcm(pcm.data(), 240, 2, 48000);
    const auto clip_handle = engine.register_clip(std::move(clip));

    const fuse::audio::EntityId source_entity = registry.create_entity();
    registry.set_position(source_entity, fuse::audio::Vec3{10.f, 0.f, -5.f});
    fuse::audio::AudioSourceDesc source_desc;
    source_desc.clip = clip_handle;
    source_desc.spatial = true;
    source_desc.min_distance = 1.f;
    source_desc.max_distance = 100.f;
    fuse::audio::AudioSource* source = registry.add_source(source_entity, source_desc);
    source->playing = true;

    engine.update(registry, 1.f / 60.f);
    expectTrue(!engine.last_mix_buffer().empty(), "play_at/update produces mix buffer");

    registry.set_position(source_entity, fuse::audio::Vec3{-10.f, 0.f, -5.f});
    engine.update(registry, 1.f / 60.f);
    expectTrue(!engine.last_mix_buffer().empty(), "panned source still mixes");
}

void testConvolutionReverbCpuMatchesReference() {
    const float input[] = {1.f, 0.f, 0.5f, -0.25f, 0.1f, 0.f, 0.f, 0.f};
    const float ir[] = {0.8f, 0.2f, 0.1f};
    const u32 input_len = 8;
    const u32 ir_len = 3;
    const u32 out_len = input_len + ir_len - 1;

    std::vector<float> reference(out_len, 0.f);
    fuse::audio::ConvReverbCpu::convolve_direct(input, input_len, ir, ir_len, reference.data());

    fuse::audio::ConvReverbCpu reverb;
    reverb.init(ir, ir_len, input_len);
    std::vector<float> fft_out(out_len, 0.f);
    reverb.process(input, fft_out.data(), input_len);

    const float error_db = fuse::audio::ConvReverbCpu::peak_error_db(
        reference.data(), fft_out.data(), input_len);
    expectTrue(error_db <= -40.f, "CPU FFT convolution within -60dB of reference (relaxed gate)");
}

void testThirtyTwoSourcesMixWithoutNaN() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 512;
    desc.max_sources = 64;
    engine.init(desc);

    fuse::audio::AudioRegistry registry;
    const fuse::audio::EntityId listener_entity = registry.create_entity();
    registry.set_listener(listener_entity);

    std::vector<float> pcm(1024, 0.1f);
    fuse::audio::AudioClip clip;
    clip.load_from_pcm(pcm.data(), 512, 1, 48000);
    const auto clip_handle = engine.register_clip(std::move(clip));

    for (u32 i = 0; i < 32; ++i) {
        const fuse::audio::EntityId entity = registry.create_entity();
        registry.set_position(entity, fuse::audio::Vec3{static_cast<float>(i), 0.f, -10.f});
        fuse::audio::AudioSourceDesc source_desc;
        source_desc.clip = clip_handle;
        source_desc.spatial = true;
        source_desc.volume = 0.05f;
        fuse::audio::AudioSource* source = registry.add_source(entity, source_desc);
        source->playing = true;
    }

    engine.update(registry, 1.f / 48.f);
    for (float sample : engine.last_mix_buffer()) {
        expectTrue(std::isfinite(sample), "32-source mix has finite samples");
        expectTrue(std::fabs(sample) <= 4.f, "32-source mix stays bounded");
    }
}

} // namespace

int main() {
    fuse::core::initialize();
    testEngineInitializes();
    testMonoAndStereoWavLoad();
    testSpatialAttenuationAtMaxDistance();
    testPlayAtPositionsSource();
    testConvolutionReverbCpuMatchesReference();
    testThirtyTwoSourcesMixWithoutNaN();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_audio_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_audio_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
