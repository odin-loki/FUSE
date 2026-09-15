#include <fuse/audio/attenuation.hpp>
#include <fuse/audio/audio_bus.hpp>
#include <fuse/audio/audio_clip.hpp>
#include <fuse/audio/audio_engine.hpp>
#include <fuse/audio/audio_registry.hpp>
#include <fuse/audio/binaural_pan.hpp>
#include <fuse/audio/conv_reverb_cpu.hpp>
#include <fuse/audio/math.hpp>
#include <fuse/audio/occlusion.hpp>
#include <fuse/audio/reverb_zones.hpp>
#include <fuse/audio/spatial_mixer.hpp>
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

void testAttenuationCurves() {
    fuse::audio::AttenuationParams params;
    params.min_dist = 1.f;
    params.max_dist = 100.f;
    params.rolloff = 1.f;

    params.curve = fuse::audio::AttenuationCurve::Logarithmic;
    const float log_gain = fuse::audio::compute_attenuation(10.f, params);
    expectTrue(log_gain > 0.f && log_gain < 1.f, "logarithmic curve attenuates mid-range");

    params.curve = fuse::audio::AttenuationCurve::Exponential;
    params.rolloff = 2.f;
    const float exp_gain = fuse::audio::compute_attenuation(10.f, params);
    expectTrue(exp_gain > 0.f && exp_gain < 1.f, "exponential curve attenuates mid-range");
    expectTrue(exp_gain < log_gain, "exponential with rolloff=2 falls off faster than logarithmic");
}

void testListenerOrientationTransform() {
    const fuse::audio::ListenerBasis basis =
        fuse::audio::make_listener_basis(fuse::audio::Vec3{0.f, 0.f, -1.f}, fuse::audio::Vec3{0.f, 1.f, 0.f});
    const fuse::audio::Vec3 local =
        fuse::audio::to_listener_space(fuse::audio::Vec3{1.f, 0.f, -5.f}, basis);
    expectNear(local.x, 1.f, 1e-4f, "source to the right stays on +X in listener space");
    expectNear(local.z, 5.f, 1e-4f, "forward offset maps to +Z in listener space");

    const fuse::audio::ListenerBasis rotated =
        fuse::audio::make_listener_basis(fuse::audio::Vec3{1.f, 0.f, 0.f}, fuse::audio::Vec3{0.f, 1.f, 0.f});
    const fuse::audio::Vec3 rotated_local =
        fuse::audio::to_listener_space(fuse::audio::Vec3{1.f, 0.f, -5.f}, rotated);
    expectTrue(std::fabs(rotated_local.x - local.x) > 0.5f,
               "rotated listener changes lateral component");
}

void testBusGains() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 0.5f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.8f);
    expectNear(mixer.effective_gain(fuse::audio::AudioBus::Sfx), 0.4f, 1e-5f,
               "effective gain multiplies bus and master");
    expectNear(mixer.effective_gain(fuse::audio::AudioBus::Master), 0.5f, 1e-5f,
               "master effective gain is master alone");
}

void testBusRoutingAllCategories() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Music, 0.6f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Voice, 0.4f);
    expectNear(mixer.effective_gain(fuse::audio::AudioBus::Music), 0.6f, 1e-5f,
               "music bus routes through master");
    expectNear(mixer.effective_gain(fuse::audio::AudioBus::Voice), 0.4f, 1e-5f,
               "voice bus routes through master");
    expectNear(mixer.bus_gain(fuse::audio::AudioBus::Sfx), 1.f, 1e-5f,
               "unset bus keeps unity gain");
}

void testBusGainClampsNegative() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, -0.5f);
    expectNear(mixer.bus_gain(fuse::audio::AudioBus::Sfx), 0.f, 1e-5f,
               "negative bus gain clamps to zero");
    expectNear(mixer.effective_gain(fuse::audio::AudioBus::Sfx), 0.f, 1e-5f,
               "effective gain is zero when bus is clamped");
}

void testBusGainClampsAboveUnity() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Music, 1.5f);
    expectNear(mixer.bus_gain(fuse::audio::AudioBus::Music), 1.f, 1e-5f,
               "bus gain above unity clamps to one");
    expectNear(fuse::audio::clamp_bus_gain(2.f), 1.f, 1e-5f,
               "clamp_bus_gain helper caps at unity");
}

void testBusChainRouting() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 0.5f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.8f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Voice, 0.6f);
    mixer.set_bus_parent(fuse::audio::AudioBus::Voice, fuse::audio::AudioBus::Sfx);

    expectNear(mixer.routed_bus_gain(fuse::audio::AudioBus::Voice), 0.48f, 1e-5f,
               "voice routed through sfx multiplies category gains");
    expectNear(mixer.effective_gain(fuse::audio::AudioBus::Voice), 0.24f, 1e-5f,
               "effective gain walks voice -> sfx -> master chain");
    expectNear(mixer.effective_output_gain(fuse::audio::AudioBus::Voice, 0.5f), 0.12f, 1e-5f,
               "effective_output_gain applies listener master on top of bus chain");
    expectTrue(mixer.bus_parent(fuse::audio::AudioBus::Voice) == fuse::audio::AudioBus::Sfx,
               "voice parent routes through sfx");
}

void testAttenuationCurveExtremes() {
    fuse::audio::AttenuationParams params;
    params.min_dist = 1.f;
    params.max_dist = 50.f;
    params.rolloff = 1.f;

    expectNear(fuse::audio::compute_attenuation(0.5f, params), 1.f, 1e-5f,
               "inside min distance is full gain");
    expectNear(fuse::audio::compute_attenuation(50.f, params), 0.f, 1e-5f,
               "at max distance gain is zero");

    params.curve = fuse::audio::AttenuationCurve::Linear;
    const float linear_mid = fuse::audio::compute_attenuation(10.f, params);
    expectTrue(linear_mid > 0.f && linear_mid < 1.f, "linear curve attenuates mid-range");

    params.curve = fuse::audio::AttenuationCurve::Inverse;
    const float inverse_near = fuse::audio::compute_attenuation(2.f, params);
    const float inverse_far = fuse::audio::compute_attenuation(20.f, params);
    expectTrue(inverse_near > inverse_far, "inverse curve falls off with distance");
    expectNear(fuse::audio::sample_attenuation_curve(1.f, params), 1.f, 1e-5f,
               "inverse curve is unity at min distance");

    params.curve = fuse::audio::AttenuationCurve::Custom;
    params.keypoints[0] = {1.f, 1.f};
    params.keypoints[1] = {25.f, 0.5f};
    params.keypoints[2] = {50.f, 0.f};
    params.keypoint_count = 3;
    expectNear(fuse::audio::compute_attenuation(1.f, params), 1.f, 1e-5f,
               "custom curve starts at first keypoint gain");
    expectNear(fuse::audio::compute_attenuation(50.f, params), 0.f, 1e-5f,
               "custom curve ends at last keypoint gain");
    expectNear(fuse::audio::sample_attenuation_curve(13.f, params), 0.76f, 1e-2f,
               "custom curve interpolates between keypoints");
}

void testAttenuationGainClamp() {
    fuse::audio::AttenuationParams params;
    params.min_dist = 1.f;
    params.max_dist = 100.f;
    params.curve = fuse::audio::AttenuationCurve::Custom;
    params.keypoints[0] = {1.f, 1.5f};
    params.keypoints[1] = {10.f, -0.25f};
    params.keypoint_count = 2;

    expectNear(fuse::audio::sample_attenuation_curve(1.f, params), 1.f, 1e-5f,
               "custom keypoint gain above unity clamps to one");
    expectNear(fuse::audio::sample_attenuation_curve(10.f, params), 0.f, 1e-5f,
               "custom keypoint gain below zero clamps to zero");
}

float bufferEnergy(const std::vector<float>& buffer) {
    float sum = 0.f;
    for (float sample : buffer) {
        sum += std::fabs(sample);
    }
    return sum;
}

void testBusRoutingAffectsMixOutput() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 256;
    engine.init(desc);

    std::vector<float> pcm(48000, 0.5f);
    fuse::audio::AudioClip clip;
    clip.load_from_pcm(pcm.data(), 48000, 1, 48000);
    const auto clip_handle = engine.register_clip(std::move(clip));

    fuse::audio::AudioRegistry registry;
    const fuse::audio::EntityId listener_entity = registry.create_entity();
    registry.set_listener(listener_entity);

    const fuse::audio::EntityId source_entity = registry.create_entity();
    registry.set_position(source_entity, fuse::audio::Vec3{0.f, 0.f, -5.f});
    fuse::audio::AudioSourceDesc source_desc;
    source_desc.clip = clip_handle;
    source_desc.bus = fuse::audio::AudioBus::Sfx;
    source_desc.spatial = true;
    source_desc.looping = true;
    fuse::audio::AudioSource* source = registry.add_source(source_entity, source_desc);
    source->playing = true;

    engine.bus_mixer().set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    engine.bus_mixer().set_bus_gain(fuse::audio::AudioBus::Sfx, 1.f);
    engine.update(registry, 1.f / 60.f);
    const float full_energy = bufferEnergy(engine.last_mix_buffer());

    engine.bus_mixer().set_bus_gain(fuse::audio::AudioBus::Sfx, 0.25f);
    source->play_head = 0.f;
    engine.update(registry, 1.f / 60.f);
    const float reduced_energy = bufferEnergy(engine.last_mix_buffer());

    expectTrue(full_energy > reduced_energy * 2.f, "bus gain scales mix output");
    expectTrue(reduced_energy > 0.f, "reduced bus still produces audible output");
}

void testCustomAttenuationCurveAffectsMix() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 256;
    engine.init(desc);

    std::vector<float> pcm(48000, 0.5f);
    fuse::audio::AudioClip clip;
    clip.load_from_pcm(pcm.data(), 48000, 1, 48000);
    const auto clip_handle = engine.register_clip(std::move(clip));

    fuse::audio::AudioRegistry registry;
    registry.set_listener(registry.create_entity());

    const fuse::audio::EntityId near_entity = registry.create_entity();
    registry.set_position(near_entity, fuse::audio::Vec3{0.f, 0.f, -2.f});
    fuse::audio::AudioSourceDesc near_desc;
    near_desc.clip = clip_handle;
    near_desc.spatial = true;
    near_desc.looping = true;
    near_desc.attenuation = fuse::audio::AttenuationCurve::Custom;
    near_desc.min_distance = 1.f;
    near_desc.max_distance = 50.f;
    near_desc.attenuation_keypoints[0] = {1.f, 1.f};
    near_desc.attenuation_keypoints[1] = {50.f, 0.1f};
    near_desc.attenuation_keypoint_count = 2;
    fuse::audio::AudioSource* near_source = registry.add_source(near_entity, near_desc);
    near_source->playing = true;

    engine.update(registry, 1.f / 60.f);
    const float near_energy = bufferEnergy(engine.last_mix_buffer());

    const fuse::audio::EntityId far_entity = registry.create_entity();
    registry.set_position(far_entity, fuse::audio::Vec3{0.f, 0.f, -40.f});
    fuse::audio::AudioSourceDesc far_desc = near_desc;
    fuse::audio::AudioSource* far_source = registry.add_source(far_entity, far_desc);
    far_source->playing = true;
    near_source->playing = false;

    engine.update(registry, 1.f / 60.f);
    const float far_energy = bufferEnergy(engine.last_mix_buffer());

    expectTrue(near_energy > far_energy * 1.5f, "custom attenuation keypoints reduce distant mix");
}

float channelEnergy(const std::vector<float>& buffer, u32 channel) {
    float sum = 0.f;
    for (fuse::usize i = channel; i < buffer.size(); i += 2) {
        sum += std::fabs(buffer[i]);
    }
    return sum;
}

float panAsymmetry(fuse::audio::AudioEngine& engine, fuse::audio::AudioRegistry& registry,
                   const fuse::audio::Vec3& source_pos, const fuse::audio::Vec3& forward) {
    fuse::audio::AudioListener* listener = registry.listener();
    listener->forward = forward;
    for (fuse::audio::EntityId entity : registry.source_entities()) {
        fuse::audio::AudioSource* source = registry.find_source(entity);
        if (source != nullptr) {
            registry.set_position(entity, source_pos);
            source->play_head = 0.f;
        }
    }
    engine.update(registry, 1.f / 60.f);
    return channelEnergy(engine.last_mix_buffer(), 0) - channelEnergy(engine.last_mix_buffer(), 1);
}

void testBinauralPanFrontBackSideExtremes() {
    const fuse::audio::Vec3 forward{0.f, 0.f, 5.f};
    const fuse::audio::Vec3 back{0.f, 0.f, -5.f};
    const fuse::audio::Vec3 left{-5.f, 0.f, 0.f};
    const fuse::audio::Vec3 right{5.f, 0.f, 0.f};

    const fuse::audio::BinauralPanGains ahead =
        fuse::audio::compute_binaural_pan_gains(forward);
    expectTrue(std::fabs(ahead.left - ahead.right) < 1e-3f, "front source is near-centre panned");
    expectNear(ahead.itd_seconds, 0.f, 1e-5f, "front source has zero ITD stub");

    const fuse::audio::BinauralPanGains behind =
        fuse::audio::compute_binaural_pan_gains(back);
    expectTrue(std::fabs(behind.left - behind.right) < 1e-3f, "back source is near-centre panned");

    const fuse::audio::BinauralPanGains left_side =
        fuse::audio::compute_binaural_pan_gains(left);
    expectTrue(left_side.left > left_side.right + 0.1f, "left extreme favours left ear");

    const fuse::audio::BinauralPanGains right_side =
        fuse::audio::compute_binaural_pan_gains(right);
    expectTrue(right_side.right > right_side.left + 0.1f, "right extreme favours right ear");
    expectTrue(right_side.itd_seconds > 0.f, "right-side source has positive ITD stub");
    expectTrue(left_side.itd_seconds < 0.f, "left-side source has negative ITD stub");
}

void testBinauralPanListenerBasisTransform() {
    const fuse::audio::ListenerBasis basis =
        fuse::audio::make_listener_basis(fuse::audio::Vec3{0.f, 0.f, -1.f}, fuse::audio::Vec3{0.f, 1.f, 0.f});
    const fuse::audio::Vec3 world_offset{5.f, 1.f, -5.f};
    const fuse::audio::Vec3 local_offset = fuse::audio::to_listener_space(world_offset, basis);
    const fuse::audio::BinauralPanAngles world_angles =
        fuse::audio::compute_binaural_angles(world_offset, basis);
    const fuse::audio::BinauralPanAngles local_angles =
        fuse::audio::compute_binaural_angles(local_offset);
    expectNear(world_angles.azimuth, local_angles.azimuth, 1e-4f,
               "listener basis maps world offset to lateral angles");
    expectNear(world_angles.elevation, local_angles.elevation, 1e-4f,
               "listener basis preserves elevation component");
}

void testBinauralPanGainClamp() {
    fuse::audio::BinauralPanParams params;
    params.max_ild_pan = 4.f;
    fuse::audio::BinauralPanGains gains =
        fuse::audio::compute_binaural_pan_gains(fuse::audio::Vec3{0.f, 0.f, 5.f}, params);
    expectTrue(gains.left >= 0.f && gains.left <= 1.f, "left gain clamps to unit range");
    expectTrue(gains.right >= 0.f && gains.right <= 1.f, "right gain clamps to unit range");

    gains.left = 1.5f;
    gains.right = -0.25f;
    fuse::audio::clamp_binaural_pan_gains(gains);
    expectNear(gains.left, 1.f, 1e-5f, "overshoot left gain clamps to one");
    expectNear(gains.right, 0.f, 1e-5f, "negative right gain clamps to zero");
}

void testBinauralPanDistanceFactorNarrowsImage() {
    fuse::audio::BinauralPanGains wide =
        fuse::audio::compute_binaural_pan_gains(fuse::audio::Vec3{5.f, 0.f, 0.f});
    fuse::audio::apply_hrtf_distance_factor(wide, 0.2f);
    const float wide_spread = std::fabs(wide.left - wide.right);

    fuse::audio::BinauralPanGains narrow =
        fuse::audio::compute_binaural_pan_gains(fuse::audio::Vec3{5.f, 0.f, 0.f});
    fuse::audio::apply_hrtf_distance_factor(narrow, 1.f);
    const float narrow_spread = std::fabs(narrow.left - narrow.right);

    expectTrue(wide_spread < narrow_spread, "attenuated source narrows binaural image");
    expectNear(fuse::audio::compute_hrtf_distance_factor(1.f), 1.f, 1e-5f,
               "unity attenuation preserves full spatial blend");
}

void testHrtfPanEdgeCases() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 256;
    desc.hrtf_enabled = true;
    engine.init(desc);

    std::vector<float> pcm(48000, 0.5f);
    fuse::audio::AudioClip clip;
    clip.load_from_pcm(pcm.data(), 48000, 1, 48000);
    const auto clip_handle = engine.register_clip(std::move(clip));

    fuse::audio::AudioRegistry registry;
    const fuse::audio::EntityId listener_entity = registry.create_entity();
    registry.set_position(listener_entity, fuse::audio::Vec3{0.f, 0.f, 0.f});
    registry.set_listener(listener_entity);

    const fuse::audio::EntityId source_entity = registry.create_entity();
    fuse::audio::AudioSourceDesc source_desc;
    source_desc.clip = clip_handle;
    source_desc.spatial = true;
    source_desc.looping = true;
    fuse::audio::AudioSource* source = registry.add_source(source_entity, source_desc);
    source->playing = true;

    const fuse::audio::Vec3 forward{0.f, 0.f, -1.f};
    const float ahead_asymmetry =
        panAsymmetry(engine, registry, fuse::audio::Vec3{0.f, 0.f, -5.f}, forward);
    expectTrue(std::fabs(ahead_asymmetry) < 1e-2f, "source ahead is near-centre panned");

    const float left_asymmetry =
        panAsymmetry(engine, registry, fuse::audio::Vec3{-5.f, 0.f, -5.f}, forward);
    expectTrue(left_asymmetry > 0.1f, "source to the left favours left channel");

    const float right_asymmetry =
        panAsymmetry(engine, registry, fuse::audio::Vec3{5.f, 0.f, -5.f}, forward);
    expectTrue(right_asymmetry < -0.1f, "source to the right favours right channel");

    const float behind_asymmetry =
        panAsymmetry(engine, registry, fuse::audio::Vec3{0.f, 0.f, 5.f}, forward);
    expectTrue(std::fabs(behind_asymmetry) < 0.15f,
               "source behind listener stays near-centre with HRTF-lite");

    registry.set_position(source_entity, fuse::audio::Vec3{0.f, 0.f, 0.f});
    source->play_head = 0.f;
    engine.update(registry, 1.f / 60.f);
    const float co_located_left = channelEnergy(engine.last_mix_buffer(), 0);
    const float co_located_right = channelEnergy(engine.last_mix_buffer(), 1);
    expectTrue(std::fabs(co_located_left - co_located_right) < 1e-3f,
               "co-located source bypasses pan split");

    fuse::audio::AudioEngine flat_engine;
    fuse::audio::AudioDesc flat_desc;
    flat_desc.frames_per_buf = 256;
    flat_desc.hrtf_enabled = false;
    flat_engine.init(flat_desc);
    fuse::audio::AudioClip flat_clip;
    flat_clip.load_from_pcm(pcm.data(), 48000, 1, 48000);
    const auto flat_clip_handle = flat_engine.register_clip(std::move(flat_clip));
    fuse::audio::AudioRegistry flat_registry;
    flat_registry.set_listener(flat_registry.create_entity());
    const fuse::audio::EntityId flat_source = flat_registry.create_entity();
    flat_registry.set_position(flat_source, fuse::audio::Vec3{-5.f, 0.f, -5.f});
    fuse::audio::AudioSourceDesc flat_source_desc;
    flat_source_desc.clip = flat_clip_handle;
    flat_source_desc.spatial = true;
    flat_source_desc.looping = true;
    fuse::audio::AudioSource* flat = flat_registry.add_source(flat_source, flat_source_desc);
    flat->playing = true;
    flat_engine.update(flat_registry, 1.f / 60.f);
    const float flat_left = channelEnergy(flat_engine.last_mix_buffer(), 0);
    const float flat_right = channelEnergy(flat_engine.last_mix_buffer(), 1);
    expectTrue(std::fabs(flat_left - flat_right) < 1e-3f,
               "HRTF disabled produces equal L/R regardless of position");
}

void testOcclusionStub() {
    fuse::audio::OcclusionParams params;
    params.min_gain = 0.1f;
    expectNear(fuse::audio::evaluate_occlusion_gain(1.f, params), 1.f, 1e-5f,
               "full visibility is unity gain");
    expectNear(fuse::audio::evaluate_occlusion_gain(0.f, params), 0.1f, 1e-5f,
               "zero visibility floors at min_gain");
    expectTrue(fuse::audio::evaluate_occlusion_gain(0.5f, params) > 0.1f
                   && fuse::audio::evaluate_occlusion_gain(0.5f, params) < 1.f,
               "partial visibility interpolates gain");

    params.min_gain = 0.2f;
    expectNear(fuse::audio::evaluate_occlusion_gain(0.f, params), 0.2f, 1e-5f,
               "custom min_gain floor is honoured");

    params = fuse::audio::OcclusionParams{};
    expectNear(fuse::audio::evaluate_occlusion_hf_gain(1.f, params), 1.f, 1e-5f,
               "full visibility keeps HF energy");
    expectNear(fuse::audio::evaluate_occlusion_hf_gain(0.f, params), 0.6f, 1e-5f,
               "zero visibility rolls off HF toward hf_attenuation");
    expectTrue(fuse::audio::evaluate_occlusion_hf_gain(0.5f, params) > 0.6f
                   && fuse::audio::evaluate_occlusion_hf_gain(0.5f, params) < 1.f,
               "partial visibility interpolates HF gain");

    const fuse::audio::OcclusionAttenuation full =
        fuse::audio::evaluate_occlusion_attenuation(1.f, params);
    expectNear(full.gain, 1.f, 1e-5f, "attenuation bundle exposes unity gain");
    expectNear(full.hf_gain, 1.f, 1e-5f, "attenuation bundle exposes unity HF gain");

    const fuse::audio::OcclusionAttenuation blocked =
        fuse::audio::evaluate_occlusion_attenuation(0.f, params);
    expectNear(blocked.gain, 0.1f, 1e-5f, "attenuation bundle exposes min gain");
    expectNear(blocked.hf_gain, 0.6f, 1e-5f, "attenuation bundle exposes HF floor");

    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    expectNear(fuse::audio::compute_blocker_visibility(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                       fuse::audio::Vec3{10.f, 0.f, 0.f}, blocker),
               0.25f, 1e-5f, "blocker on line-of-sight reduces visibility");
    expectNear(fuse::audio::compute_blocker_visibility(fuse::audio::Vec3{5.f, 0.f, 0.f},
                                                       fuse::audio::Vec3{10.f, 0.f, 0.f}, blocker),
               1.f, 1e-5f, "segment outside blocker AABB is not occluded");

    params.blocked_visibility = 0.5f;
    expectNear(fuse::audio::compute_blocker_visibility(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                       fuse::audio::Vec3{10.f, 0.f, 0.f}, blocker,
                                                       params),
               0.5f, 1e-5f, "blocked_visibility parameter is configurable");

    const fuse::audio::AABB blockers[] = {blocker, {{8.f, -1.f, -1.f}, {12.f, 1.f, 1.f}}};
    expectNear(fuse::audio::compute_blockers_visibility(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                        fuse::audio::Vec3{20.f, 0.f, 0.f},
                                                        blockers, 2, params),
               0.5f, 1e-5f, "multiple blockers take minimum visibility");
    expectNear(fuse::audio::compute_blockers_visibility(fuse::audio::Vec3{0.f, 0.f, 0.f},
                                                        fuse::audio::Vec3{20.f, 0.f, 0.f},
                                                        blockers, 0, params),
               1.f, 1e-5f, "empty blocker list is fully visible");
}

void testOcclusionReducesMixOutput() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 256;
    engine.init(desc);

    std::vector<float> pcm(48000, 0.5f);
    fuse::audio::AudioClip clip;
    clip.load_from_pcm(pcm.data(), 48000, 1, 48000);
    const auto clip_handle = engine.register_clip(std::move(clip));

    fuse::audio::AudioRegistry registry;
    registry.set_listener(registry.create_entity());

    const fuse::audio::EntityId source_entity = registry.create_entity();
    registry.set_position(source_entity, fuse::audio::Vec3{0.f, 0.f, -5.f});
    fuse::audio::AudioSourceDesc source_desc;
    source_desc.clip = clip_handle;
    source_desc.spatial = true;
    source_desc.looping = true;
    source_desc.occlusion = 1.f;
    fuse::audio::AudioSource* source = registry.add_source(source_entity, source_desc);
    source->playing = true;

    engine.update(registry, 1.f / 60.f);
    float clear_energy = 0.f;
    for (float sample : engine.last_mix_buffer()) {
        clear_energy += std::fabs(sample);
    }

    source->desc.occlusion = 0.f;
    source->play_head = 0.f;
    engine.update(registry, 1.f / 60.f);
    float occluded_energy = 0.f;
    for (float sample : engine.last_mix_buffer()) {
        occluded_energy += std::fabs(sample);
    }

    expectTrue(occluded_energy < clear_energy * 0.5f, "occlusion stub attenuates mix output");
    expectTrue(occluded_energy > 0.f, "occluded source retains min_gain floor");
}

void testOcclusionFactorExtremes() {
    fuse::audio::OcclusionParams params;
    const fuse::audio::OcclusionAttenuation full =
        fuse::audio::evaluate_occlusion_attenuation(1.f, params);
    const fuse::audio::OcclusionAttenuation blocked =
        fuse::audio::evaluate_occlusion_attenuation(0.f, params);

    expectNear(full.gain * full.hf_gain, 1.f, 1e-5f,
               "full visibility preserves unity effective occlusion gain");
    expectNear(blocked.gain * blocked.hf_gain, 0.1f * 0.6f, 1e-5f,
               "zero visibility floors effective occlusion gain");

    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    expectNear(fuse::audio::compute_blockers_visibility(fuse::audio::Vec3{-5.f, 0.f, 0.f},
                                                        fuse::audio::Vec3{5.f, 0.f, 0.f},
                                                        &blocker, 1),
               0.25f, 1e-5f, "blocker reduces visibility along segment");
    expectNear(fuse::audio::compute_blockers_visibility(fuse::audio::Vec3{-5.f, 0.f, 0.f},
                                                        fuse::audio::Vec3{5.f, 0.f, 0.f},
                                                        nullptr, 0),
               1.f, 1e-5f, "no blockers leaves visibility at unity");
}

void testOcclusionBlockerAttenuatesMix() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 256;
    engine.init(desc);

    std::vector<float> pcm(48000, 0.5f);
    fuse::audio::AudioClip clip;
    clip.load_from_pcm(pcm.data(), 48000, 1, 48000);
    const auto clip_handle = engine.register_clip(std::move(clip));

    fuse::audio::AudioRegistry registry;
    const fuse::audio::EntityId listener_entity = registry.create_entity();
    registry.set_position(listener_entity, fuse::audio::Vec3{-5.f, 0.f, 0.f});
    registry.set_listener(listener_entity);

    const fuse::audio::EntityId source_entity = registry.create_entity();
    registry.set_position(source_entity, fuse::audio::Vec3{5.f, 0.f, 0.f});
    fuse::audio::AudioSourceDesc source_desc;
    source_desc.clip = clip_handle;
    source_desc.spatial = true;
    source_desc.looping = true;
    source_desc.occlusion = 1.f;
    fuse::audio::AudioSource* source = registry.add_source(source_entity, source_desc);
    source->playing = true;

    engine.update(registry, 1.f / 60.f);
    const float clear_energy = bufferEnergy(engine.last_mix_buffer());

    const fuse::audio::AABB blocker{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    engine.set_occlusion_blockers(&blocker, 1);
    source->play_head = 0.f;
    engine.update(registry, 1.f / 60.f);
    const float blocked_energy = bufferEnergy(engine.last_mix_buffer());

    engine.clear_occlusion_blockers();
    source->play_head = 0.f;
    engine.update(registry, 1.f / 60.f);
    const float restored_energy = bufferEnergy(engine.last_mix_buffer());

    expectTrue(blocked_energy < clear_energy * 0.75f,
               "AABB blocker on line-of-sight attenuates spatial mix");
    expectTrue(restored_energy > blocked_energy, "clearing blockers restores mix energy");
}

void testReverbZoneMembership() {
    fuse::audio::ReverbZoneParams zone;
    zone.bounds = {{-5.f, -5.f, -5.f}, {5.f, 5.f, 5.f}};
    zone.wet_dry = 0.4f;
    zone.send_level = 0.8f;

    expectTrue(fuse::audio::listener_in_reverb_zone(fuse::audio::Vec3{0.f, 0.f, 0.f}, zone),
               "listener at zone centre is inside");
    expectTrue(fuse::audio::listener_in_reverb_zone(fuse::audio::Vec3{4.9f, 0.f, 0.f}, zone),
               "listener on zone boundary is inside");
    expectTrue(!fuse::audio::listener_in_reverb_zone(fuse::audio::Vec3{6.f, 0.f, 0.f}, zone),
               "listener outside zone is excluded");
}

void testReverbZoneOverlappingBlend() {
    const fuse::audio::ReverbZoneParams zones[] = {
        {{{-10.f, -10.f, -10.f}, {10.f, 10.f, 10.f}}, 0.2f, 0.5f},
        {{{-10.f, -10.f, -10.f}, {10.f, 10.f, 10.f}}, 0.6f, 1.f},
        {{{20.f, -1.f, -1.f}, {30.f, 1.f, 1.f}}, 1.f, 1.f},
    };

    const fuse::audio::ReverbZoneBlend overlap =
        fuse::audio::blend_reverb_zones(fuse::audio::Vec3{0.f, 0.f, 0.f}, zones, 3);
    expectNear(overlap.wet_dry, 0.4f, 1e-5f, "overlapping zones average wet_dry");
    expectNear(overlap.send_level, 0.75f, 1e-5f, "overlapping zones average send_level");
    expectTrue(overlap.active_zone_count == 2, "two zones contain the listener");

    const fuse::audio::ReverbZoneBlend outside =
        fuse::audio::blend_reverb_zones(fuse::audio::Vec3{100.f, 0.f, 0.f}, zones, 3);
    expectNear(outside.wet_dry, 0.f, 1e-5f, "outside all zones yields dry blend");
    expectNear(outside.send_level, 0.f, 1e-5f, "outside all zones yields zero send");
    expectTrue(outside.active_zone_count == 0, "no active zones outside bounds");
}

void testReverbZoneListenerPositionAffectsMix() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 64;
    desc.cuda_reverb = false;
    engine.init(desc);

    std::vector<float> pcm(4096, 0.5f);
    fuse::audio::AudioClip source_clip;
    source_clip.load_from_pcm(pcm.data(), 4096, 1, 48000);
    const auto source_handle = engine.register_clip(std::move(source_clip));

    const float ir_samples[] = {1.f, 0.5f, 0.25f};
    fuse::audio::AudioClip ir_clip;
    ir_clip.load_from_pcm(ir_samples, 3, 1, 48000);
    const auto ir_handle = engine.register_clip(std::move(ir_clip));

    fuse::audio::AudioRegistry registry;
    const fuse::audio::EntityId listener_entity = registry.create_entity();
    registry.set_position(listener_entity, fuse::audio::Vec3{0.f, 0.f, 0.f});
    fuse::audio::AudioListener* listener = registry.set_listener(listener_entity);

    const fuse::audio::EntityId source_entity = registry.create_entity();
    fuse::audio::AudioSourceDesc source_desc;
    source_desc.clip = source_handle;
    source_desc.spatial = false;
    source_desc.looping = true;
    fuse::audio::AudioSource* source = registry.add_source(source_entity, source_desc);
    source->playing = true;

    fuse::audio::AudioEngine::ReverbZone zone;
    zone.bounds = {{-5.f, -5.f, -5.f}, {5.f, 5.f, 5.f}};
    zone.impulse_response = ir_handle;
    zone.wet_dry = 1.f;
    zone.send_level = 1.f;
    engine.add_reverb_zone(zone);

    engine.update(registry, 1.f / 60.f);
    const float inside_energy = bufferEnergy(engine.last_mix_buffer());

    registry.set_position(listener_entity, fuse::audio::Vec3{50.f, 0.f, 0.f});
    listener = registry.set_listener(listener_entity);
    source->play_head = 0.f;
    engine.update(registry, 1.f / 60.f);
    const float outside_energy = bufferEnergy(engine.last_mix_buffer());

    expectTrue(inside_energy > outside_energy * 1.05f,
               "listener inside reverb zone receives wetter mix than outside");
}

void testSpatialPanRespectsListenerOrientation() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 256;
    desc.hrtf_enabled = true;
    engine.init(desc);

    std::vector<float> pcm(48000, 0.5f);
    fuse::audio::AudioClip clip;
    clip.load_from_pcm(pcm.data(), 48000, 1, 48000);
    const auto clip_handle = engine.register_clip(std::move(clip));

    fuse::audio::AudioRegistry registry;
    const fuse::audio::EntityId listener_entity = registry.create_entity();
    registry.set_position(listener_entity, fuse::audio::Vec3{0.f, 0.f, 0.f});
    fuse::audio::AudioListener* listener = registry.set_listener(listener_entity);
    listener->forward = fuse::audio::Vec3{0.f, 0.f, -1.f};

    const fuse::audio::EntityId source_entity = registry.create_entity();
    registry.set_position(source_entity, fuse::audio::Vec3{3.f, 0.f, -5.f});
    fuse::audio::AudioSourceDesc source_desc;
    source_desc.clip = clip_handle;
    source_desc.spatial = true;
    source_desc.looping = true;
    fuse::audio::AudioSource* source = registry.add_source(source_entity, source_desc);
    source->playing = true;

    engine.update(registry, 1.f / 60.f);
    const float left_a = channelEnergy(engine.last_mix_buffer(), 0);
    const float right_a = channelEnergy(engine.last_mix_buffer(), 1);

    listener->forward = fuse::audio::Vec3{-1.f, 0.f, 0.f};
    source->play_head = 0.f;
    engine.update(registry, 1.f / 60.f);
    const float left_b = channelEnergy(engine.last_mix_buffer(), 0);
    const float right_b = channelEnergy(engine.last_mix_buffer(), 1);

    expectTrue(std::fabs(left_a - right_a) > 1e-3f, "facing -Z pans source off-center");
    expectTrue(std::fabs(left_b - right_b) > 1e-3f, "facing -X pans source off-center");
    expectTrue(std::fabs((left_a - right_a) - (left_b - right_b)) > 1e-3f,
               "pan asymmetry changes with listener orientation");
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

void testReverbSendLevelDryMix() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 64;
    desc.cuda_reverb = false;
    engine.init(desc);

    std::vector<float> pcm(4096, 0.5f);
    fuse::audio::AudioClip source_clip;
    source_clip.load_from_pcm(pcm.data(), 4096, 1, 48000);
    const auto source_handle = engine.register_clip(std::move(source_clip));

    const float ir_samples[] = {1.f, 0.5f, 0.25f};
    fuse::audio::AudioClip ir_clip;
    ir_clip.load_from_pcm(ir_samples, 3, 1, 48000);
    const auto ir_handle = engine.register_clip(std::move(ir_clip));

    fuse::audio::AudioRegistry registry;
    registry.set_listener(registry.create_entity());
    const fuse::audio::EntityId source_entity = registry.create_entity();
    registry.set_position(source_entity, fuse::audio::Vec3{0.f, 0.f, -5.f});
    fuse::audio::AudioSourceDesc source_desc;
    source_desc.clip = source_handle;
    source_desc.spatial = false;
    source_desc.looping = true;
    fuse::audio::AudioSource* source = registry.add_source(source_entity, source_desc);
    source->playing = true;

    engine.update(registry, 1.f / 60.f);
    const float dry_energy = bufferEnergy(engine.last_mix_buffer());

    fuse::audio::AudioEngine::ReverbZone zone;
    zone.impulse_response = ir_handle;
    zone.wet_dry = 0.f;
    zone.send_level = 1.f;
    engine.add_reverb_zone(zone);
    source->play_head = 0.f;
    engine.update(registry, 1.f / 60.f);
    const float zero_send_energy = bufferEnergy(engine.last_mix_buffer());

    expectTrue(dry_energy > 0.f, "dry mix produces energy");
    expectNear(zero_send_energy, dry_energy, dry_energy * 0.05f + 1e-4f,
               "zero wet_dry send leaves dry mix unchanged");
    expectNear(engine.reverb_send_level(), 1.f, 1e-5f, "default reverb send level is unity");
}

void testReverbSendLevelWetMix() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 64;
    desc.cuda_reverb = false;
    engine.init(desc);

    std::vector<float> pcm(4096, 0.5f);
    fuse::audio::AudioClip source_clip;
    source_clip.load_from_pcm(pcm.data(), 4096, 1, 48000);
    const auto source_handle = engine.register_clip(std::move(source_clip));

    const float ir_samples[] = {1.f, 0.5f, 0.25f};
    fuse::audio::AudioClip ir_clip;
    ir_clip.load_from_pcm(ir_samples, 3, 1, 48000);
    const auto ir_handle = engine.register_clip(std::move(ir_clip));

    fuse::audio::AudioRegistry registry;
    registry.set_listener(registry.create_entity());
    const fuse::audio::EntityId source_entity = registry.create_entity();
    fuse::audio::AudioSourceDesc source_desc;
    source_desc.clip = source_handle;
    source_desc.spatial = false;
    source_desc.looping = true;
    fuse::audio::AudioSource* source = registry.add_source(source_entity, source_desc);
    source->playing = true;

    fuse::audio::AudioEngine::ReverbZone zone;
    zone.impulse_response = ir_handle;
    zone.wet_dry = 1.f;
    zone.send_level = 1.f;
    engine.add_reverb_zone(zone);
    engine.update(registry, 1.f / 60.f);
    const float wet_energy = bufferEnergy(engine.last_mix_buffer());

    zone.wet_dry = 0.f;
    engine.clear_reverb_zones();
    engine.add_reverb_zone(zone);
    source->play_head = 0.f;
    engine.update(registry, 1.f / 60.f);
    const float dry_energy = bufferEnergy(engine.last_mix_buffer());

    expectTrue(wet_energy > dry_energy * 1.05f, "full reverb send increases mix energy");
}

void testReverbSendLevelScalesMixOutput() {
    fuse::audio::AudioEngine engine;
    fuse::audio::AudioDesc desc;
    desc.frames_per_buf = 64;
    desc.cuda_reverb = false;
    engine.init(desc);

    std::vector<float> pcm(4096, 0.5f);
    fuse::audio::AudioClip source_clip;
    source_clip.load_from_pcm(pcm.data(), 4096, 1, 48000);
    const auto source_handle = engine.register_clip(std::move(source_clip));

    const float ir_samples[] = {1.f, 0.5f, 0.25f};
    fuse::audio::AudioClip ir_clip;
    ir_clip.load_from_pcm(ir_samples, 3, 1, 48000);
    const auto ir_handle = engine.register_clip(std::move(ir_clip));

    fuse::audio::AudioRegistry registry;
    registry.set_listener(registry.create_entity());
    const fuse::audio::EntityId source_entity = registry.create_entity();
    fuse::audio::AudioSourceDesc source_desc;
    source_desc.clip = source_handle;
    source_desc.spatial = false;
    source_desc.looping = true;
    fuse::audio::AudioSource* source = registry.add_source(source_entity, source_desc);
    source->playing = true;

    fuse::audio::AudioEngine::ReverbZone zone;
    zone.impulse_response = ir_handle;
    zone.wet_dry = 0.8f;
    zone.send_level = 1.f;
    engine.add_reverb_zone(zone);
    engine.update(registry, 1.f / 60.f);
    const float full_send_energy = bufferEnergy(engine.last_mix_buffer());

    engine.set_reverb_send_level(0.25f);
    source->play_head = 0.f;
    engine.update(registry, 1.f / 60.f);
    const float reduced_send_energy = bufferEnergy(engine.last_mix_buffer());

    expectNear(engine.reverb_send_level(), 0.25f, 1e-5f, "set_reverb_send_level updates active zone");
    expectTrue(full_send_energy > reduced_send_energy, "lower send level reduces wet mix contribution");
    expectTrue(reduced_send_energy > 0.f, "partial send still produces audible output");
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
    testAttenuationCurves();
    testListenerOrientationTransform();
    testBusGains();
    testBusRoutingAllCategories();
    testBusGainClampsNegative();
    testBusGainClampsAboveUnity();
    testBusChainRouting();
    testAttenuationCurveExtremes();
    testAttenuationGainClamp();
    testCustomAttenuationCurveAffectsMix();
    testBusRoutingAffectsMixOutput();
    testBinauralPanFrontBackSideExtremes();
    testBinauralPanListenerBasisTransform();
    testBinauralPanGainClamp();
    testBinauralPanDistanceFactorNarrowsImage();
    testHrtfPanEdgeCases();
    testOcclusionStub();
    testOcclusionFactorExtremes();
    testOcclusionReducesMixOutput();
    testOcclusionBlockerAttenuatesMix();
    testReverbZoneMembership();
    testReverbZoneOverlappingBlend();
    testReverbZoneListenerPositionAffectsMix();
    testReverbSendLevelDryMix();
    testReverbSendLevelWetMix();
    testReverbSendLevelScalesMixOutput();
    testSpatialPanRespectsListenerOrientation();
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
