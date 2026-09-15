#include <fuse/audio/audio_bus.hpp>
#include <fuse/core/init.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

void testEmptyBusMixGuards() {
    const auto invalid =
        static_cast<fuse::audio::AudioBus>(static_cast<fuse::u8>(fuse::audio::AudioBus::Count));
    const auto out_of_range = static_cast<fuse::audio::AudioBus>(255);

    expectTrue(fuse::audio::is_empty_bus_mix(invalid), "Count sentinel is empty-bus mix");
    expectTrue(fuse::audio::is_empty_bus_mix(out_of_range), "out-of-range bus is empty-bus mix");
    expectTrue(!fuse::audio::is_empty_bus_mix(fuse::audio::AudioBus::Music),
               "category bus is not empty-bus mix");

    fuse::audio::AudioBusMixer mixer;
    expectTrue(fuse::audio::should_skip_bus_mix(mixer, invalid),
               "invalid bus skips mix via should_skip_bus_mix");
    expectTrue(!fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Sfx),
               "valid audible bus does not skip mix");
}

void testSilentMixGainGuards() {
    expectTrue(fuse::audio::is_silent_mix_gain(0.f), "zero mix gain is silent");
    expectTrue(fuse::audio::is_silent_mix_gain(-0.5f), "negative mix gain is silent");
    expectTrue(fuse::audio::is_silent_mix_gain(1e-7f), "sub-epsilon mix gain is silent");
    expectTrue(!fuse::audio::is_silent_mix_gain(0.5f), "mid-range mix gain is audible");

    expectNear(fuse::audio::apply_bus_mix_sample(1.f, 0.f), 0.f, 1e-5f,
               "silent mix gain early-out returns zero sample");
    expectNear(fuse::audio::apply_bus_mix_sample(0.8f, 0.5f), 0.4f, 1e-5f,
               "audible mix gain scales sample");
    expectNear(fuse::audio::apply_bus_mix_sample(1.f, 1.5f), 1.f, 1e-5f,
               "mix gain above unity clamps before sample scale");
}

void testMuteEarlyOutPrecedence() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Music, 0.8f);
    mixer.set_bus_solo(fuse::audio::AudioBus::Music, true);
    mixer.set_bus_muted(fuse::audio::AudioBus::Music, true);

    expectTrue(fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Music),
               "explicit mute early-outs even when bus is soloed");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Music, 1.f), 0.f,
               1e-5f, "muted soloed bus mix output is zero");
    expectNear(fuse::audio::compute_bus_mix_sample(1.f, mixer, fuse::audio::AudioBus::Music, 1.f),
               0.f, 1e-5f, "muted soloed bus sample mix is zero");
}

void testSoloSilencingHelpers() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.9f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Voice, 0.6f);

    expectTrue(!fuse::audio::is_bus_solo_silenced(mixer, fuse::audio::AudioBus::Sfx),
               "no solo mode does not solo-silence buses");
    expectTrue(mixer.soloed_category_count() == 0, "no solo buses by default");

    mixer.set_bus_solo(fuse::audio::AudioBus::Voice, true);
    expectTrue(mixer.soloed_category_count() == 1, "one soloed category bus");
    expectTrue(fuse::audio::is_bus_solo_silenced(mixer, fuse::audio::AudioBus::Sfx),
               "non-solo bus is solo-silenced");
    expectTrue(!fuse::audio::is_bus_solo_silenced(mixer, fuse::audio::AudioBus::Voice),
               "soloed bus is not solo-silenced");
    expectTrue(!fuse::audio::is_bus_solo_silenced(mixer, fuse::audio::AudioBus::Master),
               "master is never solo-silenced");

    expectTrue(fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Sfx),
               "solo-silenced bus skips mix");
    expectNear(fuse::audio::compute_bus_mix_sample(1.f, mixer, fuse::audio::AudioBus::Voice, 1.f),
               0.6f, 1e-5f, "soloed bus sample mix keeps effective gain");
}

void testMultipleSoloBusesMix() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.5f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Music, 0.7f);
    mixer.set_bus_solo(fuse::audio::AudioBus::Sfx, true);
    mixer.set_bus_solo(fuse::audio::AudioBus::Music, true);

    expectTrue(mixer.soloed_category_count() == 2, "two soloed category buses");
    expectTrue(!fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Sfx),
               "first soloed bus still mixes");
    expectTrue(!fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Music),
               "second soloed bus still mixes");
    expectTrue(fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Voice),
               "non-solo bus is skipped while multiple solos active");
}

void testClearSoloAndMuteHelpers() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_muted(fuse::audio::AudioBus::Sfx, true);
    mixer.set_bus_muted(fuse::audio::AudioBus::Voice, true);
    mixer.set_bus_solo(fuse::audio::AudioBus::Music, true);

    mixer.clear_all_mute();
    expectTrue(!mixer.bus_muted(fuse::audio::AudioBus::Sfx), "clear_all_mute clears sfx mute");
    expectTrue(!mixer.bus_muted(fuse::audio::AudioBus::Voice), "clear_all_mute clears voice mute");
    expectTrue(mixer.bus_soloed(fuse::audio::AudioBus::Music),
               "clear_all_mute leaves solo flags intact");

    mixer.clear_all_solo();
    expectTrue(mixer.soloed_category_count() == 0, "clear_all_solo clears solo flags");
    expectTrue(!mixer.any_bus_soloed(), "clear_all_solo deactivates solo mode");
    expectTrue(!fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Sfx),
               "cleared solo restores mix for previously silenced buses");
}

void testComputeBusMixSampleGuards() {
    const auto invalid =
        static_cast<fuse::audio::AudioBus>(static_cast<fuse::u8>(fuse::audio::AudioBus::Count));

    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 0.8f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Voice, 0.5f);

    expectNear(fuse::audio::compute_bus_mix_sample(1.f, mixer, fuse::audio::AudioBus::Voice, 1.f),
               0.4f, 1e-5f, "audible bus sample mix applies effective output gain");
    expectNear(fuse::audio::compute_bus_mix_sample(1.f, mixer, invalid, 0.6f), 0.f, 1e-5f,
               "empty-bus sample mix early-outs to zero");

    mixer.set_bus_gain(fuse::audio::AudioBus::Voice, 0.f);
    expectNear(fuse::audio::compute_bus_mix_sample(1.f, mixer, fuse::audio::AudioBus::Voice, 1.f),
               0.f, 1e-5f, "zero-gain bus sample mix early-outs to zero");
}

} // namespace

int main() {
    fuse::core::initialize();
    testEmptyBusMixGuards();
    testSilentMixGainGuards();
    testMuteEarlyOutPrecedence();
    testSoloSilencingHelpers();
    testMultipleSoloBusesMix();
    testClearSoloAndMuteHelpers();
    testComputeBusMixSampleGuards();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_audio_bus_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_audio_bus_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
