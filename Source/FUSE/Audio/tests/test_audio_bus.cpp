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

void testEmptyAudioBusPredicate() {
    const auto invalid =
        static_cast<fuse::audio::AudioBus>(static_cast<fuse::u8>(fuse::audio::AudioBus::Count));
    const auto out_of_range = static_cast<fuse::audio::AudioBus>(255);

    expectTrue(fuse::audio::is_empty_audio_bus(invalid), "Count sentinel is empty bus");
    expectTrue(fuse::audio::is_empty_audio_bus(out_of_range), "out-of-range bus is empty");
    expectTrue(!fuse::audio::is_empty_audio_bus(fuse::audio::AudioBus::Sfx),
               "category bus is not empty");
    expectTrue(fuse::audio::is_valid_audio_bus(fuse::audio::AudioBus::Music)
                   != fuse::audio::is_empty_audio_bus(fuse::audio::AudioBus::Music),
               "empty bus is complement of valid bus");
}

void testNearZeroBusGainEpsilon() {
    expectTrue(fuse::audio::is_near_zero_bus_gain(0.f), "zero gain is near-zero");
    expectTrue(fuse::audio::is_near_zero_bus_gain(1e-7f), "sub-epsilon gain is near-zero");
    expectTrue(!fuse::audio::is_near_zero_bus_gain(1e-5f), "above-epsilon gain is audible");
    expectTrue(fuse::audio::is_audible_bus_gain(0.5f), "mid-range gain is audible");
    expectTrue(!fuse::audio::is_audible_bus_gain(-1.f), "negative gain clamps to near-zero");
}

void testBusSoloSilencedPredicate() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.8f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Music, 0.7f);

    expectTrue(!mixer.is_bus_solo_silenced(fuse::audio::AudioBus::Sfx),
               "no solo active — sfx is not silenced");
    mixer.set_bus_solo(fuse::audio::AudioBus::Music, true);
    expectTrue(mixer.is_bus_solo_silenced(fuse::audio::AudioBus::Sfx),
               "sfx is silenced while music is soloed");
    expectTrue(!mixer.is_bus_solo_silenced(fuse::audio::AudioBus::Music),
               "soloed bus is not silenced");
    expectTrue(!mixer.is_bus_solo_silenced(fuse::audio::AudioBus::Master),
               "master is never solo-silenced");
}

void testMultipleSoloBusesMix() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.9f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Music, 0.7f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Voice, 0.5f);

    mixer.set_bus_solo(fuse::audio::AudioBus::Sfx, true);
    mixer.set_bus_solo(fuse::audio::AudioBus::Voice, true);

    expectTrue(mixer.should_mix_bus(fuse::audio::AudioBus::Sfx),
               "first soloed bus mixes");
    expectTrue(mixer.should_mix_bus(fuse::audio::AudioBus::Voice),
               "second soloed bus mixes");
    expectTrue(!mixer.should_mix_bus(fuse::audio::AudioBus::Music),
               "non-solo bus is silenced with multiple solos active");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Sfx, 1.f), 0.9f,
               1e-5f, "soloed sfx keeps output gain");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Music, 1.f), 0.f,
               1e-5f, "non-solo music mix output is zero");
}

void testClearBusSoloRestoresMix() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.8f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Music, 0.6f);

    mixer.set_bus_solo(fuse::audio::AudioBus::Music, true);
    expectTrue(!mixer.should_mix_bus(fuse::audio::AudioBus::Sfx),
               "sfx silenced while music soloed");

    mixer.clear_bus_solo();
    expectTrue(!mixer.any_bus_soloed(), "clear_bus_solo disables solo mode");
    expectTrue(mixer.should_mix_bus(fuse::audio::AudioBus::Sfx),
               "sfx mixes again after solo cleared");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Sfx, 1.f), 0.8f,
               1e-5f, "sfx output gain restored after solo cleared");
}

void testShouldSkipBusMixAlias() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.8f);
    mixer.set_bus_muted(fuse::audio::AudioBus::Sfx, true);

    expectTrue(fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Sfx),
               "muted bus should skip mix");
    expectTrue(!fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Music),
               "unaffected bus should not skip mix");

    const auto invalid =
        static_cast<fuse::audio::AudioBus>(static_cast<fuse::u8>(fuse::audio::AudioBus::Count));
    expectTrue(fuse::audio::should_skip_bus_mix(mixer, invalid),
               "empty bus should skip mix");
}

void testMutedSoloBusDoesNotMix() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.8f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Music, 0.7f);

    mixer.set_bus_solo(fuse::audio::AudioBus::Music, true);
    mixer.set_bus_muted(fuse::audio::AudioBus::Music, true);

    expectTrue(!mixer.should_mix_bus(fuse::audio::AudioBus::Music),
               "muted solo bus does not mix");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Music, 1.f), 0.f,
               1e-5f, "muted solo bus mix output is zero");
}

void testComputeMixOutputGainGuards() {
    const auto invalid =
        static_cast<fuse::audio::AudioBus>(static_cast<fuse::u8>(fuse::audio::AudioBus::Count));

    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 0.8f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Voice, 0.5f);

    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Voice, 1.f),
               0.4f, 1e-5f, "audible bus returns effective output gain");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, invalid, 0.6f), 0.f, 1e-5f,
               "empty bus early-outs to zero mix gain");
    expectNear(fuse::audio::compute_effective_output_gain(mixer, invalid, 0.6f), 0.6f, 1e-5f,
               "empty bus still passes listener master through effective_output_gain");

    mixer.set_bus_gain(fuse::audio::AudioBus::Voice, 1e-7f);
    expectTrue(fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Voice),
               "near-zero effective gain skips mix");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Voice, 1.f), 0.f,
               1e-5f, "near-zero gain mix output is zero");
}

} // namespace

int main() {
    fuse::core::initialize();
    testEmptyAudioBusPredicate();
    testNearZeroBusGainEpsilon();
    testBusSoloSilencedPredicate();
    testMultipleSoloBusesMix();
    testClearBusSoloRestoresMix();
    testShouldSkipBusMixAlias();
    testMutedSoloBusDoesNotMix();
    testComputeMixOutputGainGuards();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
