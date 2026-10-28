#pragma once

namespace fuse::audio {

/// Runtime envelope for one source. `gain` is a linear multiplier in [0, 1].
/// `holdRemaining` counts down the post-sidechain hold before release (seconds).
struct SidechainEnvelopeState {
    float gain = 1.f;
    float holdRemaining = 0.f;
};

/// Per-source sidechain duck settings. Not applied by the spatial mixer.
///
/// Target gain (`R = max(ratio, 1)`, `s = |sidechainPeak|`):
///   s <= threshold -> 1
///   s >  threshold -> 1 / R
/// One-pole step, with tau = attackSec while gain is falling and releaseSec otherwise
/// (alpha = 1 when tau <= 0):
///   alpha = 1 - exp(-dt / tau)
///   gain  = clamp(gain + (target - gain) * alpha, 0, 1)
/// When the sidechain falls back to the threshold, gain stays frozen for `hold` seconds
/// before that release step. dt <= 0 leaves the envelope unchanged. Any non-finite
/// input (settings, peak, dt, or envelope) sets gain to 1.
struct SidechainDuck {
    /// Linear amplitude. Peaks above this duck the source.
    float threshold = 0.1f;
    /// Duck depth. Values below 1 are treated as 1. Above threshold the target is 1/ratio.
    float ratio = 4.f;
    /// One-pole time constant in seconds while the gain is falling.
    float attackSec = 0.01f;
    /// One-pole time constant in seconds while the gain is rising.
    float releaseSec = 0.15f;
    /// Seconds to freeze the ducked gain after the sidechain drops to the threshold.
    float hold = 0.f;

    /// Advance the envelope and return the updated gain in [0, 1].
    float process(SidechainEnvelopeState& envelopeState, float sidechainPeak, float dt) const;
};

/// Linear multiply: `sample * gain`.
float apply_duck(float sample, float gain);

} // namespace fuse::audio
