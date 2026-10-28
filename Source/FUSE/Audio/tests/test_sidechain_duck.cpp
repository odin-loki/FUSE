#include <fuse/audio/duck_db.hpp>
#include <fuse/audio/semitone.hpp>
#include <fuse/audio/sidechain_duck.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void expectNear(float value, float expected, float epsilon, const char* message) {
    if (!std::isfinite(value) || !std::isfinite(expected) || std::fabs(value - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f, expected %f)\n", message,
                     static_cast<double>(value), static_cast<double>(expected));
        ++g_failures;
    }
}

fuse::audio::SidechainDuck makeDuck() {
    fuse::audio::SidechainDuck duck;
    duck.threshold = 0.2f;
    duck.ratio = 4.f;
    duck.attackSec = 0.005f;
    duck.releaseSec = 0.05f;
    duck.hold = 0.f;
    return duck;
}

void testSilenceSidechainLeavesUnityGain() {
    fuse::audio::SidechainDuck duck = makeDuck();
    fuse::audio::SidechainEnvelopeState state;

    float gain = state.gain;
    for (int i = 0; i < 200; ++i) {
        gain = duck.process(state, 0.f, 0.01f);
    }
    expectNear(gain, 1.f, 1e-6f, "silence sidechain leaves gain at 1");
    expectNear(state.gain, 1.f, 1e-6f, "silence keeps envelope gain at 1");

    gain = duck.process(state, duck.threshold, 0.01f);
    expectNear(gain, 1.f, 1e-6f, "sidechain at threshold does not duck");

    gain = duck.process(state, 0.05f, 0.01f);
    expectNear(gain, 1.f, 1e-6f, "sidechain below threshold stays at unity");
    expectNear(fuse::audio::apply_duck(0.5f, gain), 0.5f, 1e-6f, "unity duck leaves the sample");
}

void testLoudBurstReducesGain() {
    fuse::audio::SidechainDuck duck = makeDuck();
    fuse::audio::SidechainEnvelopeState state;

    const float gain = duck.process(state, 1.f, 0.001f);
    const float alpha = 1.f - std::exp(-0.001f / duck.attackSec);
    const float expected = 1.f + ((1.f / duck.ratio) - 1.f) * alpha;
    expectTrue(gain < 1.f, "loud burst reduces gain");
    expectNear(gain, expected, 1e-5f, "attack step is a one-pole toward 1/ratio");

    for (int i = 0; i < 80; ++i) {
        duck.process(state, 1.f, 0.001f);
    }
    expectNear(state.gain, 1.f / duck.ratio, 1e-4f, "sustained sidechain settles at 1/ratio");
    expectTrue(state.gain >= 0.f && state.gain <= 1.f, "duck gain stays in 0..1");
    expectTrue(fuse::audio::apply_duck(0.5f, state.gain) < 0.5f, "ducked sample is quieter");
    expectNear(fuse::audio::apply_duck(0.5f, state.gain), 0.5f * state.gain, 1e-6f,
               "apply_duck multiplies");
}

void testReleaseReturnsNearUnity() {
    fuse::audio::SidechainDuck duck = makeDuck();
    fuse::audio::SidechainEnvelopeState state;

    for (int i = 0; i < 80; ++i) {
        duck.process(state, 1.f, 0.001f);
    }
    const float ducked = state.gain;
    expectTrue(ducked < 0.5f, "burst ducks before release");

    float gain = ducked;
    for (int i = 0; i < 400; ++i) {
        gain = duck.process(state, 0.f, 0.001f);
    }
    expectTrue(gain > ducked, "release moves gain upward");
    expectNear(gain, 1.f, 1e-3f, "after release the gain returns near 1");
}

void testHoldDelaysRelease() {
    fuse::audio::SidechainDuck duck = makeDuck();
    duck.attackSec = 0.f;
    duck.releaseSec = 0.f;
    duck.hold = 0.05f;

    fuse::audio::SidechainEnvelopeState state;
    expectNear(duck.process(state, 1.f, 0.01f), 0.25f, 1e-6f, "zero attack snaps to 1/ratio");

    expectNear(duck.process(state, 0.f, 0.03f), 0.25f, 1e-6f, "hold keeps the ducked gain");
    expectNear(state.holdRemaining, 0.02f, 1e-6f, "hold counts down while the sidechain is quiet");

    expectNear(duck.process(state, 0.f, 0.03f), 1.f, 1e-6f, "release after hold returns to 1");
    expectNear(state.holdRemaining, 0.f, 1e-6f, "hold is clear once release starts");
}

void testNonPositiveDtDoesNotChangeState() {
    fuse::audio::SidechainDuck duck = makeDuck();
    fuse::audio::SidechainEnvelopeState state;
    state.gain = 0.4f;
    state.holdRemaining = 0.2f;

    expectNear(duck.process(state, 1.f, 0.f), 0.4f, 0.f, "dt 0 does not change gain");
    expectNear(state.holdRemaining, 0.2f, 0.f, "dt 0 does not change hold");

    expectNear(duck.process(state, 1.f, -0.5f), 0.4f, 0.f, "negative dt does not change gain");
    expectNear(state.holdRemaining, 0.2f, 0.f, "negative dt does not change hold");

    const float negInf = -std::numeric_limits<float>::infinity();
    expectNear(duck.process(state, 1.f, negInf), 0.4f, 0.f, "negative infinity dt does not change gain");
    expectNear(state.holdRemaining, 0.2f, 0.f, "negative infinity dt does not change hold");
}

void testNonFiniteInputResetsGain() {
    fuse::audio::SidechainDuck duck = makeDuck();
    fuse::audio::SidechainEnvelopeState state;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float posInf = std::numeric_limits<float>::infinity();

    state.gain = 0.2f;
    state.holdRemaining = 0.3f;
    expectNear(duck.process(state, nan, 0.01f), 1.f, 0.f, "NaN sidechain sets gain to 1");
    expectNear(state.holdRemaining, 0.f, 0.f, "NaN sidechain clears hold");

    state.gain = 0.2f;
    state.holdRemaining = 0.3f;
    expectNear(duck.process(state, posInf, 0.01f), 1.f, 0.f, "infinite sidechain sets gain to 1");

    state.gain = 0.2f;
    duck.threshold = nan;
    expectNear(duck.process(state, 1.f, 0.01f), 1.f, 0.f, "NaN threshold sets gain to 1");

    duck = makeDuck();
    state.gain = 0.2f;
    duck.ratio = nan;
    expectNear(duck.process(state, 1.f, 0.01f), 1.f, 0.f, "NaN ratio sets gain to 1");

    duck = makeDuck();
    state.gain = 0.2f;
    expectNear(duck.process(state, 1.f, nan), 1.f, 0.f, "NaN dt sets gain to 1");

    duck = makeDuck();
    state.gain = nan;
    state.holdRemaining = 0.f;
    expectNear(duck.process(state, 0.f, 0.01f), 1.f, 0.f, "NaN envelope gain sets gain to 1");
}

void testDuckDbOk() {
    expectTrue(fuse::audio::duck_db_ok(0.f), "0 dB is in range");
    expectTrue(fuse::audio::duck_db_ok(-12.f), "-12 dB is in range");
    expectTrue(fuse::audio::duck_db_ok(-24.f), "-24 dB is in range");
    expectTrue(!fuse::audio::duck_db_ok(1.f), "1 dB is above the range");
    expectTrue(!fuse::audio::duck_db_ok(-30.f), "-30 dB is below the range");
}

void testSemitoneRatio() {
    expectNear(fuse::audio::semitone_ratio(0.f), 1.f, 0.f, "zero semitones is unity");
    expectNear(fuse::audio::semitone_ratio(12.f), 2.f, 1e-5f, "twelve semitones doubles the ratio");
    expectNear(fuse::audio::semitone_ratio(-12.f), 0.5f, 1e-5f, "minus twelve semitones halves the ratio");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float posInf = std::numeric_limits<float>::infinity();
    const float negInf = -std::numeric_limits<float>::infinity();
    expectNear(fuse::audio::semitone_ratio(nan), 1.f, 0.f, "NaN semitones returns 1");
    expectNear(fuse::audio::semitone_ratio(posInf), 1.f, 0.f, "positive infinity semitones returns 1");
    expectNear(fuse::audio::semitone_ratio(negInf), 1.f, 0.f, "negative infinity semitones returns 1");

    const float fourOctaves = std::pow(2.f, 4.f);
    expectNear(fuse::audio::semitone_ratio(48.f), fourOctaves, 1e-4f, "48 semitones is four octaves up");
    expectNear(fuse::audio::semitone_ratio(-48.f), 1.f / fourOctaves, 1e-5f, "minus 48 semitones is four octaves down");
    expectNear(fuse::audio::semitone_ratio(96.f), fourOctaves, 1e-4f, "semitones above 48 clamp to 48");
    expectNear(fuse::audio::semitone_ratio(-96.f), 1.f / fourOctaves, 1e-5f, "semitones below -48 clamp to -48");
}

} // namespace

int main() {
    testDuckDbOk();
    testSemitoneRatio();
    testSilenceSidechainLeavesUnityGain();
    testLoudBurstReducesGain();
    testReleaseReturnsNearUnity();
    testHoldDelaysRelease();
    testNonPositiveDtDoesNotChangeState();
    testNonFiniteInputResetsGain();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
