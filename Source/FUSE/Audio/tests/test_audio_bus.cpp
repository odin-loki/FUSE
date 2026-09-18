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

void testListenerMasterMutedPredicate() {
    expectTrue(fuse::audio::is_listener_master_muted(0.f), "zero listener master is muted");
    expectTrue(fuse::audio::is_listener_master_muted(1e-7f),
               "sub-epsilon listener master is muted");
    expectTrue(!fuse::audio::is_listener_master_muted(0.5f),
               "audible listener master is not muted");
    expectTrue(!fuse::audio::is_listener_master_audible(0.f),
               "zero listener master is not audible");
    expectTrue(fuse::audio::is_listener_master_audible(0.5f),
               "mid-range listener master is audible");
    expectTrue(fuse::audio::should_skip_listener_master_mix(0.f),
               "zero listener master skips mix");
    expectTrue(fuse::audio::should_skip_listener_master_mix(1e-7f),
               "near-zero listener master skips mix");
    expectTrue(!fuse::audio::should_skip_listener_master_mix(0.5f),
               "audible listener master does not skip mix");
}

void testClearBusMuteRestoresMix() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.8f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Music, 0.6f);

    mixer.set_bus_muted(fuse::audio::AudioBus::Sfx, true);
    mixer.set_bus_muted(fuse::audio::AudioBus::Music, true);
    expectTrue(mixer.any_bus_muted(), "any_bus_muted reflects active mute flags");
    expectTrue(mixer.muted_bus_count() == 2, "muted_bus_count tracks muted buses");

    mixer.clear_bus_mute();
    expectTrue(!mixer.any_bus_muted(), "clear_bus_mute disables mute mode");
    expectTrue(mixer.muted_bus_count() == 0, "clear_bus_mute resets muted count");
    expectTrue(mixer.should_mix_bus(fuse::audio::AudioBus::Sfx),
               "sfx mixes again after mute cleared");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Sfx, 1.f), 0.8f,
               1e-5f, "sfx output gain restored after mute cleared");
}

void testSoloedBusCount() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_solo(fuse::audio::AudioBus::Sfx, true);
    mixer.set_bus_solo(fuse::audio::AudioBus::Voice, true);

    expectTrue(mixer.soloed_bus_count() == 2, "soloed_bus_count tracks active solos");
    expectTrue(mixer.any_bus_soloed(), "any_bus_soloed true with active solos");

    mixer.clear_bus_solo();
    expectTrue(mixer.soloed_bus_count() == 0, "clear_bus_solo resets solo count");
}

void testClearBusMuteSingleBus() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.8f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Music, 0.6f);

    mixer.set_bus_muted(fuse::audio::AudioBus::Sfx, true);
    mixer.set_bus_muted(fuse::audio::AudioBus::Music, true);
    expectTrue(mixer.any_bus_muted(), "both buses muted");

    mixer.clear_bus_mute(fuse::audio::AudioBus::Sfx);
    expectTrue(!mixer.bus_muted(fuse::audio::AudioBus::Sfx),
               "per-bus clear_bus_mute clears sfx only");
    expectTrue(mixer.bus_muted(fuse::audio::AudioBus::Music),
               "per-bus clear_bus_mute leaves other mute flags");
    expectTrue(mixer.should_mix_bus(fuse::audio::AudioBus::Sfx),
               "cleared sfx mixes again");
}

void testBusParentChainMutedHelpers() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.8f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Voice, 0.6f);
    mixer.set_bus_parent(fuse::audio::AudioBus::Voice, fuse::audio::AudioBus::Sfx);

    expectTrue(!mixer.is_any_ancestor_bus_muted(fuse::audio::AudioBus::Voice),
               "no muted ancestor by default");
    expectTrue(!mixer.is_bus_parent_chain_muted(fuse::audio::AudioBus::Voice),
               "parent chain not muted by default");

    mixer.set_bus_muted(fuse::audio::AudioBus::Voice, true);
    expectTrue(mixer.is_bus_parent_chain_muted(fuse::audio::AudioBus::Voice),
               "self mute counts in parent-chain muted");
    expectTrue(fuse::audio::is_bus_parent_chain_muted(mixer, fuse::audio::AudioBus::Voice),
               "free-function parent-chain muted matches member");

    mixer.clear_bus_mute(fuse::audio::AudioBus::Voice);
    mixer.set_bus_muted(fuse::audio::AudioBus::Sfx, true);
    expectTrue(mixer.is_any_ancestor_bus_muted(fuse::audio::AudioBus::Voice),
               "ancestor mute detected");
    expectTrue(mixer.is_bus_parent_chain_muted(fuse::audio::AudioBus::Voice),
               "child inherits ancestor mute");
}

void testParentChainSoloAudibleChild() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.9f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Voice, 0.5f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Music, 0.7f);
    mixer.set_bus_parent(fuse::audio::AudioBus::Voice, fuse::audio::AudioBus::Sfx);

    mixer.set_bus_solo(fuse::audio::AudioBus::Sfx, true);

    expectTrue(mixer.is_any_ancestor_bus_soloed(fuse::audio::AudioBus::Voice),
               "soloed sfx is ancestor of voice");
    expectTrue(mixer.is_bus_solo_audible(fuse::audio::AudioBus::Voice),
               "voice is audible through soloed ancestor");
    expectTrue(!mixer.is_bus_solo_silenced(fuse::audio::AudioBus::Voice),
               "voice is not solo-silenced when ancestor is soloed");
    expectTrue(mixer.should_mix_bus(fuse::audio::AudioBus::Voice),
               "child bus mixes when ancestor is soloed");
    expectTrue(!mixer.should_mix_bus(fuse::audio::AudioBus::Music),
               "unrelated bus remains solo-silenced");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Voice, 1.f),
               0.45f, 1e-5f, "soloed ancestor child keeps routed output gain");
}

void testParentChainNearZeroGainSilencesChild() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 1e-7f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Voice, 0.8f);
    mixer.set_bus_parent(fuse::audio::AudioBus::Voice, fuse::audio::AudioBus::Sfx);

    expectNear(fuse::audio::parent_chain_gain(mixer, fuse::audio::AudioBus::Voice), 1e-7f, 1e-8f,
               "parent chain gain multiplies ancestor gains");
    expectTrue(!fuse::audio::is_parent_chain_audible(mixer, fuse::audio::AudioBus::Voice),
               "near-zero parent chain is not audible");
    expectTrue(fuse::audio::should_skip_parent_chain_mix(mixer, fuse::audio::AudioBus::Voice),
               "near-zero parent chain skips child mix");
    expectTrue(!mixer.should_mix_bus(fuse::audio::AudioBus::Voice),
               "child does not mix with near-zero parent gain");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Voice, 1.f), 0.f,
               1e-5f, "near-zero parent chain mix output is zero");
}

void testParentChainMutePropagation() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.8f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Voice, 0.9f);
    mixer.set_bus_parent(fuse::audio::AudioBus::Voice, fuse::audio::AudioBus::Sfx);

    expectTrue(!mixer.is_parent_chain_muted(fuse::audio::AudioBus::Voice),
               "voice parent chain is not muted by default");
    expectTrue(mixer.should_mix_bus(fuse::audio::AudioBus::Voice),
               "voice mixes before parent mute");

    mixer.set_bus_muted(fuse::audio::AudioBus::Sfx, true);
    expectTrue(mixer.is_parent_chain_muted(fuse::audio::AudioBus::Voice),
               "muted parent silences child via parent-chain guard");
    expectTrue(!mixer.should_mix_bus(fuse::audio::AudioBus::Voice),
               "child bus does not mix when parent is muted");
    expectTrue(fuse::audio::is_bus_mix_silenced(mixer, fuse::audio::AudioBus::Voice),
               "parent-muted child is mix-silenced");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Voice, 1.f), 0.f,
               1e-5f, "parent-muted child mix output is zero");
    expectTrue(mixer.should_mix_bus(fuse::audio::AudioBus::Music),
               "unrelated bus still mixes when only sfx parent is muted");
    expectTrue(fuse::audio::is_bus_muted(mixer, fuse::audio::AudioBus::Voice),
               "parent-muted child is reported as muted");
}

void testListenerMasterMixEarlyOut() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Sfx, 0.8f);

    expectTrue(mixer.should_mix_bus(fuse::audio::AudioBus::Sfx),
               "audible bus mixes before listener mute");
    expectTrue(fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Sfx, 0.f),
               "zero listener master skips mix");
    expectTrue(fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Sfx, 1e-7f),
               "near-zero listener master skips mix");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Sfx, 0.f), 0.f,
               1e-5f, "zero listener master mix output is zero");
    expectNear(fuse::audio::compute_mix_output_gain(mixer, fuse::audio::AudioBus::Sfx, 0.5f), 0.4f,
               1e-5f, "audible listener master preserves mix output");
}

void testIsBusMixSilencedAlias() {
    fuse::audio::AudioBusMixer mixer;
    mixer.set_bus_gain(fuse::audio::AudioBus::Master, 1.f);
    mixer.set_bus_gain(fuse::audio::AudioBus::Music, 0.7f);
    mixer.set_bus_muted(fuse::audio::AudioBus::Music, true);

    expectTrue(fuse::audio::is_bus_mix_silenced(mixer, fuse::audio::AudioBus::Music),
               "muted bus is mix-silenced");
    expectTrue(fuse::audio::is_bus_mix_silenced(mixer, fuse::audio::AudioBus::Music)
                   == fuse::audio::should_skip_bus_mix(mixer, fuse::audio::AudioBus::Music),
               "is_bus_mix_silenced matches should_skip_bus_mix");
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
    testListenerMasterMutedPredicate();
    testClearBusMuteRestoresMix();
    testClearBusMuteSingleBus();
    testSoloedBusCount();
    testBusParentChainMutedHelpers();
    testParentChainSoloAudibleChild();
    testParentChainNearZeroGainSilencesChild();
    testParentChainMutePropagation();
    testListenerMasterMixEarlyOut();
    testIsBusMixSilencedAlias();
    testComputeMixOutputGainGuards();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
