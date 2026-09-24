// WP-4.5 exposure histogram + adaptation. Slang twin of pp_exposure.comp (see there).
#include "pp_common.slang"

groupshared uint s_bins[256];

uint ppBin(float luminance, PpFrame F) {
    const float logLum = log2(max(luminance, 1e-8));
    const float range = F.histMaxLog - F.histMinLog;
    if (range <= 0.0 || F.histBins == 0u) {
        return 0u;
    }
    const float normalized = (logLum - F.histMinLog) / range;
    const float scaled = ppClamp(normalized, 0.0, 1.0) * float(F.histBins - 1u);
    return min(ppRoundPos(scaled), F.histBins - 1u);
}

float ppBinLog(uint bin, PpFrame F) {
    if (F.histBins <= 1u) {
        return F.histMinLog;
    }
    const float normalized = float(bin) / float(F.histBins - 1u);
    return F.histMinLog + normalized * (F.histMaxLog - F.histMinLog);
}

void ppAdaptLinear(PpFrame F, float luminance, inout float current, inout float measured) {
    measured = luminance;
    if (F.adaptValid == 0u) {
        return;
    }
    const float ratio = max(luminance, 1e-8) / max(F.targetLuminance, 1e-8);
    const float target = ppClamp(log2(ratio) + F.meteringBias, F.minEv, F.maxEv);
    const float delta = target - current;
    const float speed = delta > 0.0 ? F.speedUp : F.speedDown;
    const float maxStep = speed * F.deltaSeconds;
    if (abs(delta) <= maxStep) {
        current = target;
    } else {
        current += delta > 0.0 ? maxStep : -maxStep;
    }
    current = ppClamp(current, F.minEv, F.maxEv);
}

[numthreads(256, 1, 1)]
void main(uint t: SV_GroupIndex, uint3 group: SV_GroupID) {
    const PpFrame F = ppFrame();
    s_bins[t] = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (pc.mode == 0u) {
        const uint gx = group.x % F.histGroupsX;
        const uint gy = group.x / F.histGroupsX;
        float4* src = ppTexels(pc.src);
        for (uint k = 0u; k < 16u; ++k) {
            const uint x = gx * kPpHistTile + (t % 16u) + (k % 4u) * 16u;
            const uint y = gy * kPpHistTile + (t / 16u) + (k / 4u) * 16u;
            if (x < F.width && y < F.height) {
                InterlockedAdd(s_bins[ppBin(ppLuminance(src[y * F.width + x].xyz), F)], 1u);
            }
        }
        GroupMemoryBarrierWithGroupSync();
        ppWords(F.histPartials)[group.x * kPpHistMaxBins + t] = s_bins[t];
        return;
    }
    uint* partials = ppWords(F.histPartials);
    uint total = 0u;
    for (uint g = 0u; g < F.histGroups; ++g) {
        total += partials[g * kPpHistMaxBins + t];
    }
    s_bins[t] = total;
    uint* state = ppWords(F.exposureState);
    state[kPpStateBins + t] = total;
    GroupMemoryBarrierWithGroupSync();
    if (t != 0u) {
        return;
    }
    float* stateF = ppFloats(F.exposureState);
    float current = stateF[kPpStateCurrentEv];
    float measured = stateF[kPpStateMeasured];
    float smoothed = stateF[kPpStateSmoothed];
    uint frames = state[kPpStateFrames];
    if ((F.flags & kPpFlagExposureReset) != 0u) {
        current = F.resetEv;
        measured = 0.0;
        smoothed = 0.0;
        frames = 0u;
    }
    const uint count = F.width * F.height;
    float metered = 0.0;
    if (F.histValid != 0u && count > 0u) {
        const uint target = uint(ceil(ppClamp(F.percentile, 0.0, 1.0) * float(count)));
        const uint desired = max(target, 1u);
        uint running = 0u;
        uint bin = F.histBins - 1u;
        for (uint b = 0u; b < F.histBins; ++b) {
            running += s_bins[b];
            if (running >= desired) {
                bin = b;
                break;
            }
        }
        metered = exp2(ppBinLog(bin, F));
        if (metered >= 1e-8) {
            if ((F.flags & kPpFlagAutoEma) != 0u) {
                const bool brightening = metered > smoothed;
                const float alpha = ppClamp(brightening ? F.emaUp : F.emaDown, 0.0, 1.0);
                if (smoothed <= 0.0) {
                    smoothed = metered;
                } else {
                    smoothed = alpha * metered + (1.0 - alpha) * smoothed;
                }
                ppAdaptLinear(F, smoothed, current, measured);
            } else {
                ppAdaptLinear(F, metered, current, measured);
            }
        }
    }
    stateF[kPpStateCurrentEv] = current;
    stateF[kPpStateMeasured] = measured;
    stateF[kPpStateSmoothed] = smoothed;
    stateF[kPpStateMetered] = metered;
    state[kPpStateSamples] = count;
    state[kPpStateFrames] = frames + 1u;
}
