#include <fuse/audio/sidechain_duck.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::audio {

namespace {

float reset_unity(SidechainEnvelopeState& state) {
    state.gain = 1.f;
    state.holdRemaining = 0.f;
    return 1.f;
}

bool envelope_inputs_finite(const SidechainDuck& duck, const SidechainEnvelopeState& state,
                            float sidechainPeak, float dt) {
    return std::isfinite(duck.threshold) && std::isfinite(duck.ratio) && std::isfinite(duck.attackSec)
        && std::isfinite(duck.releaseSec) && std::isfinite(duck.hold) && std::isfinite(state.gain)
        && std::isfinite(state.holdRemaining) && std::isfinite(sidechainPeak) && std::isfinite(dt);
}

/// One-pole toward `target`. tau <= 0 snaps. Result is clamped to [0, 1].
float one_pole_gain(float current, float target, float dt, float tau) {
    float next = target;
    if (tau > 0.f) {
        const float alpha = 1.f - std::exp(-dt / tau);
        next = current + (target - current) * alpha;
    }
    return std::clamp(next, 0.f, 1.f);
}

} // namespace

float SidechainDuck::process(SidechainEnvelopeState& envelopeState, float sidechainPeak,
                             float dt) const {
    // NaN compares false, so a non-finite dt falls through to the reset below.
    // -inf counts as dt <= 0 and must not touch the envelope.
    if (dt <= 0.f) {
        return envelopeState.gain;
    }
    if (!envelope_inputs_finite(*this, envelopeState, sidechainPeak, dt)) {
        return reset_unity(envelopeState);
    }

    envelopeState.gain = std::clamp(envelopeState.gain, 0.f, 1.f);

    const float clampedRatio = this->ratio < 1.f ? 1.f : this->ratio;
    const float ducked = 1.f / clampedRatio;
    const float peak = std::fabs(sidechainPeak);
    const bool above = peak > threshold;

    if (above) {
        envelopeState.holdRemaining = hold > 0.f ? hold : 0.f;
        const float tau = ducked < envelopeState.gain ? attackSec : releaseSec;
        envelopeState.gain = one_pole_gain(envelopeState.gain, ducked, dt, tau);
        return envelopeState.gain;
    }

    // At or below threshold: finish the hold, then release toward unity.
    // A step that straddles the end of the hold only releases for the leftover dt.
    float releaseDt = dt;
    if (envelopeState.holdRemaining > 0.f) {
        if (envelopeState.holdRemaining >= dt) {
            envelopeState.holdRemaining -= dt;
            return envelopeState.gain;
        }
        releaseDt = dt - envelopeState.holdRemaining;
        envelopeState.holdRemaining = 0.f;
    }

    const float tau = 1.f < envelopeState.gain ? attackSec : releaseSec;
    envelopeState.gain = one_pole_gain(envelopeState.gain, 1.f, releaseDt, tau);
    return envelopeState.gain;
}

float apply_duck(float sample, float gain) {
    return sample * gain;
}

} // namespace fuse::audio
