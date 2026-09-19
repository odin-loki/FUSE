#include <fuse/renderer/taa/taa_resolve.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

f32 clampF32(f32 value, f32 minValue, f32 maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

} // namespace

bool taaHistoryCanAccumulate(const TaaHistoryBuffer& history) {
    return history.isReady();
}

bool taaHistoryIsGenerationCurrent(const TaaHistoryBuffer& history, u32 observed_generation) {
    return history.isGenerationCurrent(observed_generation);
bool taaHistoryCanReuse(const TaaHistoryBuffer& history) {
    return history.canReuseHistory();
}

bool taaHistoryReuseReady(const TaaHistoryBuffer& history, u32 observed_generation) {
    return history.canReuseHistory() && history.generationMatches(observed_generation);

    return history.generationMatches(observed_generation);

bool taaResolveSurfacesSatisfied(const TaaResolveDesc& desc) {
    return desc.surfaces.current_frame != nullptr && desc.surfaces.output != nullptr;
}

bool taaResolveDimensionsValid(u32 width, u32 height) {
    return width > 0u && height > 0u;
}

bool taaResolveSkipReasonIsBlocking(TaaResolveSkipReason reason) {
    return reason != TaaResolveSkipReason::None;
}

bool taaResolveSurfacesComplete(const TaaResolveDesc& desc) {
    return desc.surfaces.current_frame != nullptr && desc.surfaces.output != nullptr;
}

bool taaResolveRejectionSurfacesComplete(const TaaResolveDesc& desc) {
    if (!desc.enforce_rejection_surfaces) {
        return true;
    }

    const TAAParams params = clampTaaParams(desc.params);
    if (taaResolveRequiresVelocity(params) && desc.surfaces.velocity_buffer == nullptr) {
        return false;
    if (taaResolveRequiresDepth(params) && desc.surfaces.depth_buffer == nullptr) {

bool shouldSkipTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                          TaaResolveSkipReason* reason) {
    const TaaResolveSkipReason skip = classifyTaaResolveSkip(desc, history);
    if (reason != nullptr) {
        *reason = skip;
    return taaResolveSkipReasonIsBlocking(skip);

bool taaResolveDimensionsMatch(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return history.matchesDimensions(desc.width, desc.height);
}

bool taaResolveHasDimensionMismatch(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !taaResolveDimensionsMatch(desc, history);
}

bool taaResolveSurfacesSatisfied(const TaaResolveDesc& desc) {
    return desc.surfaces.current_frame != nullptr && desc.surfaces.output != nullptr;
}

bool canAttemptTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolve(desc, history);
}

bool prepareTaaResolveDesc(TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    sanitizeTaaResolveDesc(desc, history);
    return canAttemptTaaResolve(desc, history);
}

bool taaResolveHistoryGenerationIsStale(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !taaResolveBypassesHistoryGenerationGuard(desc) &&
           history.isHistoryStale(desc.observed_history_generation);
bool taaViewportDimensionsMatchPass(u32 passWidth, u32 passHeight, const TaaResolveDesc& desc) {
    return taaResolveDimensionsValid(passWidth, passHeight) && passWidth == desc.width && passHeight == desc.height;
}

bool taaResolveBypassesHistoryGenerationGuard(const TaaResolveDesc& desc) {
    return desc.observed_history_generation == kTaaResolveNoHistoryGeneration;
}

bool taaResolveHistoryGenerationGuardPasses(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return taaResolveBypassesHistoryGenerationGuard(desc) ||
           !history.isHistoryStale(desc.observed_history_generation);
}

bool taaResolveRejectionSurfacesRequired(const TaaResolveDesc& desc) {
    if (!desc.enforce_rejection_surfaces) {
        return false;
    }
    const TAAParams params = clampTaaParams(desc.params);
    return taaResolveRequiresVelocity(params) || taaResolveRequiresDepth(params);
}

bool taaResolveRejectionSurfacesSatisfied(const TaaResolveDesc& desc) {
    if (!taaResolveRejectionSurfacesRequired(desc)) {
        return true;
    }
    const TAAParams params = clampTaaParams(desc.params);
    if (taaResolveRequiresVelocity(params) && desc.surfaces.velocity_buffer == nullptr) {
        return false;
    }
    if (taaResolveRequiresDepth(params) && desc.surfaces.depth_buffer == nullptr) {
        return false;
    }
    return true;
}

void stampObservedHistoryGeneration(TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (taaResolveBypassesHistoryGenerationGuard(desc)) {
        desc.observed_history_generation = history.invalidateGeneration();
    }
}

bool taaHistoryIsReusable(const TaaHistoryBuffer& history, const TaaResolveDesc& desc) {
    if (!history.isReady() || !history.hasValidHistory()) {
        return false;
    }
    return !taaResolveHistoryGenerationIsStale(desc, history);
bool taaResolveSurfacesSatisfied(const TaaResolveDesc& desc) {
    return desc.surfaces.current_frame != nullptr && desc.surfaces.output != nullptr;

bool canAttemptTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolve(desc, history);




bool prepareTaaResolveDesc(TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    sanitizeTaaResolveDesc(desc, history);
    return canAttemptTaaResolve(desc, history);

bool taaResolveWillReuseHistory(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (!taaResolveCanReuseHistory(desc, history)) {
}

        return false;
    const bool firstFrame = !history.hasValidHistory();
    const TAAParams params = clampTaaParams(desc.params);
    const f32 effectiveBlend =
        computeEffectiveBlend(firstFrame, taaResolveCanReuseHistory(desc, history), params);

    const f32 effectiveBlend = computeEffectiveBlend(false, true, params);
    return taaBlendUsesHistory(effectiveBlend);


bool preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaBlendWeights* weights) {
    if (!preflightTaaResolve(desc, history)) {
    const bool historyReusable = taaResolveCanReuseHistory(desc, history);
    const TaaBlendWeights blendWeights =
        computeTaaBlendWeightsWithReuseGuard(firstFrame, historyReusable, clampTaaParams(desc.params));
    if (weights != nullptr) {
        *weights = blendWeights;
    return taaBlendWeightsValid(blendWeights);


bool preflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaResolveSkipReason* skipReason,
                              TaaResolveBlendRejectReason* blendReason) {
    if (!preflightTaaResolve(desc, history, skipReason)) {
        if (blendReason != nullptr) {
            *blendReason = TaaResolveBlendRejectReason::None;
    return preflightTaaResolveBlendWeights(desc, history, blendReason);

    return taaResolveCanReuseHistory(desc, history) && taaResolveAppliesHistoryBlend(desc, history);

void sanitizeTaaResolveDesc(TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    normalizeTaaParams(desc.params);
    stampObservedHistoryGeneration(desc, history);
}

bool preflightTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                         TaaResolveSkipReason* reason) {
bool isObservedHistoryGenerationCurrent(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return taaResolveBypassesHistoryGenerationGuard(desc) ||
           history.generationMatches(desc.observed_history_generation);

TaaResolveSkipReason preflightTaaResolve(TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return classifyTaaResolveSkip(desc, history);

bool taaResolveCanProceed(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
    const TaaResolveSkipReason skip = classifyTaaResolveSkip(desc, history);
    if (reason != nullptr) {
        *reason = skip;
    return !taaResolveSkipReasonIsBlocking(skip);

bool tryPreflightTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                            TaaResolveSkipReason& reason) {
    reason = classifyTaaResolveSkip(desc, history);
    return !taaResolveSkipReasonIsBlocking(reason);

bool shouldSkipTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolve(desc, history);

bool taaResolveSurfacesSatisfied(const TaaResolveDesc& desc) {
    return desc.surfaces.current_frame != nullptr && desc.surfaces.output != nullptr;

bool taaResolveRejectionSurfacesSatisfied(const TaaResolveDesc& desc) {
    if (!desc.enforce_rejection_surfaces) {
        return true;

    const TAAParams params = clampTaaParams(desc.params);
    if (taaResolveRequiresVelocity(params) && desc.surfaces.velocity_buffer == nullptr) {
        return false;
    if (taaResolveRequiresDepth(params) && desc.surfaces.depth_buffer == nullptr) {

bool taaHistoryIsReusable(const TaaHistoryBuffer& history, const TaaResolveDesc& desc) {
    if (!history.isReady() || !history.hasValidHistory()) {
    return !taaResolveHistoryGenerationIsStale(desc, history);

bool preflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                     TaaBlendWeights* outWeights) {
    if (!preflightTaaResolve(desc, history)) {
        return false;
    }

    const bool firstFrame = !history.hasValidHistory();
    const TaaBlendWeights weights = computeTaaBlendWeights(firstFrame, desc.params);
    if (outWeights != nullptr) {
        *outWeights = weights;
    }
    return taaBlendWeightsValid(weights);
}

bool preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaResolveBlendPreflight* out) {
    TaaResolveBlendPreflight snapshot{};
    snapshot.resolve_skip_reason = classifyTaaResolveSkip(desc, history);
    snapshot.resolve_would_pass = !taaResolveSkipReasonIsBlocking(snapshot.resolve_skip_reason);

    const bool firstFrame = !history.hasValidHistory();
    snapshot.weights = computeTaaBlendWeights(firstFrame, desc.params);
    snapshot.blend_weights_valid = taaBlendWeightsValid(snapshot.weights);
    snapshot.history_blend_allowed = taaHistoryBlendAllowed(firstFrame, history);

    if (out != nullptr) {
        *out = snapshot;
    }
    return snapshot.passes();
}

TaaBlendWeights computeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return computeTaaBlendWeights(taaResolveWouldBeFirstFrame(history), desc.params);
}

bool preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaBlendWeights* weights) {
    if (!preflightTaaResolve(desc, history)) {
        return false;
    }
    const TaaBlendWeights computed = computeTaaResolveBlendWeights(desc, history);
    if (weights != nullptr) {
        *weights = computed;
    }
    return taaBlendWeightsValid(computed);
}

bool taaResolveBlendPreflightPasses(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveBlend(desc, history);
}

TaaBlendWeights computeTaaResolveBlendPreflight(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    const bool firstFrame = !history.hasValidHistory();
    return computeTaaBlendWeights(firstFrame, clampTaaParams(desc.params));
}

bool taaResolveBlendPreflightPasses(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (!preflightTaaResolve(desc, history)) {
        return false;
    }
    const TaaBlendWeights weights = computeTaaResolveBlendPreflight(desc, history);
    return taaBlendWeightsValid(weights);
}

bool preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaBlendWeights* weights) {
    const TaaBlendWeights computed = computeTaaResolveBlendPreflight(desc, history);
    if (weights != nullptr) {
        *weights = computed;
    }
    return taaResolveBlendPreflightPasses(desc, history);
}

bool tryComputeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        TaaBlendWeights& out) {
    if (!preflightTaaResolve(desc, history)) {
        return false;
    }

    const bool firstFrame = !history.hasValidHistory();
    out = computeTaaBlendWeights(firstFrame, desc.params);
    return taaBlendWeightsValid(out);
}

bool preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaBlendWeights* weights) {
    TaaBlendWeights computed{};
    if (!tryComputeTaaResolveBlendWeights(desc, history, computed)) {
        return false;
    }

    if (weights != nullptr) {
        *weights = computed;
    }
    return true;
}

bool taaResolveHistoryBlendPreflightPasses(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    TaaBlendWeights weights{};
    if (!tryComputeTaaResolveBlendWeights(desc, history, weights)) {
        return false;
    }

    if (weights.history > 0.f && !taaResolveCanReuseHistory(desc, history)) {
        return false;
    }
    return true;
}

bool taaResolveRequiresWarmup(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolve(desc, history) && history.needsWarmup();
}

bool preflightTaaResolveWithBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                  TaaResolveSkipReason* skipReason,
                                  TaaResolveBlendRejectReason* blendReason) {
    if (!preflightTaaResolve(desc, history, skipReason)) {
        if (blendReason != nullptr) {
            *blendReason = TaaResolveBlendRejectReason::None;
        }
        return false;
    }
    return preflightTaaResolveBlendWeights(desc, history, blendReason);
}

bool taaResolveStatsBlendConsistent(const TaaResolveStats& stats, const TaaResolveDesc& desc,
                                    const TaaHistoryBuffer& /*history*/) {
    if (!stats.resolved) {
        return false;
    }
    const TaaBlendWeights weights{stats.effective_blend, stats.history_blend};
    if (!taaBlendWeightsValid(weights)) {
        return false;
    }
    const bool historyBlendAllowed = !stats.first_frame && stats.history_blend > 1e-5f;
    if (!taaBlendWeightsConsistentWithReuse(weights, historyBlendAllowed)) {
        return false;
    }
    if (stats.first_frame) {
        return stats.effective_blend >= 1.f - 1e-5f && stats.history_blend <= 1e-5f;
    }
    const TAAParams params = clampTaaParams(desc.params);
    return std::fabs(stats.effective_blend - params.blend_factor) <= 1e-5f ||
           stats.history_blend <= 1e-5f;
}

bool preflightTaaResolveWithBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                  TaaResolveSkipReason* skipReason,
                                  TaaResolveBlendRejectReason* blendReason) {
    const bool resolvePasses = preflightTaaResolve(desc, history, skipReason);
    const bool blendPasses = preflightTaaResolveBlendWeights(desc, history, blendReason);
    return resolvePasses && blendPasses;
}

bool preflightTaaResolveHistoryReuse(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                     TaaHistoryReuseBlockReason* reason) {
    const u32 observedGeneration = taaResolveBypassesHistoryGenerationGuard(desc)
                                         ? history.invalidateGeneration()
                                         : desc.observed_history_generation;
    return preflightTaaHistoryReuse(history, observedGeneration, reason);
}

bool tryPreflightTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                            TaaResolveSkipReason& reason) {
    reason = classifyTaaResolveSkip(desc, history);
    return !taaResolveSkipReasonIsBlocking(reason);
}

bool shouldSkipTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolve(desc, history);
}

bool taaResolveReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolve(desc, history);
}

bool tryPreflightTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                            TaaResolveSkipReason& reason) {
    reason = classifyTaaResolveSkip(desc, history);
    return !taaResolveSkipReasonIsBlocking(reason);
}

bool shouldSkipTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolve(desc, history);
}

bool preflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaResolveSkipReason* skipReason, TaaResolveBlendRejectReason* blendReason) {
    const bool resolveOk = preflightTaaResolve(desc, history, skipReason);
    const bool blendOk = preflightTaaResolveBlendWeights(desc, history, blendReason);
    return resolveOk && blendOk;
}

bool tryPreflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                 TaaResolveSkipReason& skipReason, TaaResolveBlendRejectReason& blendReason) {
    const bool resolveOk = preflightTaaResolve(desc, history, &skipReason);
    const bool blendOk = preflightTaaResolveBlendWeights(desc, history, &blendReason);
    return resolveOk && blendOk;
}

bool shouldSkipTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolveFrame(desc, history);
}

bool tryPreflightTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                            TaaResolveSkipReason& reason) {
    return preflightTaaResolve(desc, history, &reason);
}

bool shouldSkipTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolve(desc, history);
}

bool tryPreflightTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                            TaaResolveSkipReason& reason) {
    reason = classifyTaaResolveSkip(desc, history);
    return !taaResolveSkipReasonIsBlocking(reason);
}

bool shouldSkipTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolve(desc, history);
}

bool preflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaResolveSkipReason* skipReason, TaaResolveBlendRejectReason* blendReason) {
    if (!preflightTaaResolve(desc, history, skipReason)) {
        if (blendReason != nullptr) {
            *blendReason = TaaResolveBlendRejectReason::None;
        }
        return false;
    }
    return preflightTaaResolveBlendWeights(desc, history, blendReason);
}

bool tryPreflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                 TaaResolveSkipReason& skipReason, TaaResolveBlendRejectReason& blendReason) {
    if (!tryPreflightTaaResolve(desc, history, skipReason)) {
        blendReason = TaaResolveBlendRejectReason::None;
        return false;
    }
    return tryPreflightTaaResolveBlendWeights(desc, history, blendReason);
}

bool shouldSkipTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolveFrame(desc, history);
}

bool tryPreflightTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                            TaaResolveSkipReason& reason) {
    return preflightTaaResolve(desc, history, &reason);
}

bool shouldSkipTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolve(desc, history);
}

TaaResolveSkipReason classifyTaaResolveSkip(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (!taaHistoryCanAccumulate(history)) {
        return TaaResolveSkipReason::HistoryNotReady;
    }
    if (!taaResolveDimensionsValid(desc.width, desc.height)) {
        return TaaResolveSkipReason::InvalidDimensions;
    }
    if (taaResolveHasDimensionMismatch(desc, history)) {
        return TaaResolveSkipReason::DimensionMismatch;
    }
    if (!taaResolveSurfacesComplete(desc)) {
    if (!taaResolveSurfacesSatisfied(desc)) {
        return TaaResolveSkipReason::MissingSurfaces;
    }

    if (!taaResolveRejectionSurfacesSatisfied(desc)) {

    if (!taaResolveRejectionSurfacesComplete(desc)) {
        const TAAParams params = clampTaaParams(desc.params);
        if (taaResolveRequiresVelocity(params) && desc.surfaces.velocity_buffer == nullptr) {
            return TaaResolveSkipReason::MissingVelocityBuffer;
        }
        return TaaResolveSkipReason::MissingDepthBuffer;
    }
    if (taaResolveHistoryGenerationIsStale(desc, history)) {
    if (!taaResolveBypassesHistoryGenerationGuard(desc) &&
        !taaHistoryIsGenerationCurrent(history, desc.observed_history_generation)) {
        return TaaResolveSkipReason::StaleHistoryGeneration;
    }
    return TaaResolveSkipReason::None;
}

bool canAttemptTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !taaResolveSkipReasonIsBlocking(classifyTaaResolveSkip(desc, history));
}

bool prepareTaaResolveDesc(TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    stampObservedHistoryGeneration(desc, history);
    return canAttemptTaaResolve(desc, history);
}

TAAParams clampTaaParams(const TAAParams& raw) {
    TAAParams clamped = raw;
    clamped.blend_factor = clampF32(raw.blend_factor, 0.f, 1.f);
    clamped.velocity_rejection = std::max(0.f, raw.velocity_rejection);
    clamped.depth_rejection = std::max(0.f, raw.depth_rejection);
    clamped.clamp_gamma = std::max(1.f, raw.clamp_gamma);
    return clamped;
}

bool taaParamsInRange(const TAAParams& raw) {
    return raw.blend_factor >= 0.f && raw.blend_factor <= 1.f && raw.velocity_rejection >= 0.f &&
           raw.depth_rejection >= 0.f && raw.clamp_gamma >= 1.f;
}

void normalizeTaaParams(TAAParams& params) {
    params = clampTaaParams(params);

f32 computeHistoryBlend(f32 effectiveBlend) {
    return clampF32(1.f - effectiveBlend, 0.f, 1.f);

TaaBlendWeights computeTaaBlendWeights(bool firstFrame, const TAAParams& params) {
    return computeTaaBlendWeightsWithReuseGuard(firstFrame, !firstFrame, params);
}

TaaBlendWeights computeTaaBlendWeightsWithReuseGuard(bool firstFrame, bool historyReusable,
                                                     const TAAParams& params) {
    const f32 effectiveBlend = computeEffectiveBlend(firstFrame, historyReusable, params);
    TaaBlendWeights weights{};
    weights.current = effectiveBlend;
    weights.history = computeHistoryBlend(effectiveBlend);
    return weights;

TaaBlendWeights computeTaaBlendWeightsWithReuseGuard(bool firstFrame, bool historyReusable,
                                                     const TAAParams& params) {
    const f32 effectiveBlend = computeEffectiveBlend(firstFrame, historyReusable, params);
    TaaBlendWeights weights{};
    weights.current = effectiveBlend;
    weights.history = computeHistoryBlend(effectiveBlend);
    return weights;
}

TaaBlendWeights preflightTaaBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    const bool firstFrame = !history.hasValidHistory();
    const bool historyReusable = taaResolveCanReuseHistory(desc, history);
    return computeTaaBlendWeightsWithReuseGuard(firstFrame, historyReusable, desc.params);
}

bool taaBlendWeightsValid(const TaaBlendWeights& weights) {
    if (weights.current < 0.f || weights.current > 1.f) {
        return false;
    if (weights.history < 0.f || weights.history > 1.f) {
    return std::fabs(weights.current + weights.history - 1.f) <= 1e-5f;

bool preflightTaaBlendWeights(bool firstFrame, const TAAParams& params, TaaBlendWeights* out) {
    const TaaBlendWeights weights = computeTaaBlendWeights(firstFrame, params);
    if (!taaBlendWeightsValid(weights)) {
        return false;
    }
    if (out != nullptr) {
        *out = weights;
    }
    return true;
}

bool taaResolveBlendPreflightPasses(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (!preflightTaaResolve(desc, history)) {
        return false;
    }
    return preflightTaaBlendWeights(!history.hasValidHistory(), desc.params);
}

bool preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history, TaaBlendWeights* out) {
    if (!preflightTaaResolve(desc, history)) {
        return false;
    }
    return preflightTaaBlendWeights(!history.hasValidHistory(), desc.params, out);
}

bool taaHistoryBlendAllowed(bool firstFrame, const TaaHistoryBuffer& history) {
    return !firstFrame && taaHistoryCanReuse(history);

bool taaBlendWeightsConsistentWithReuse(const TaaBlendWeights& weights, bool historyBlendAllowed) {
    if (!taaBlendWeightsValid(weights)) {
    if (historyBlendAllowed) {
        return true;
    return weights.history <= 1e-5f && weights.current >= 1.f - 1e-5f;

TaaBlendWeights computeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    const bool firstFrame = !history.hasValidHistory();
    const TAAParams params = clampTaaParams(desc.params);
    TaaBlendWeights weights = computeTaaBlendWeights(firstFrame, params);
    if (!taaResolveCanReuseHistory(desc, history)) {
        weights.current = 1.f;
        weights.history = 0.f;

bool taaResolveAppliesHistoryBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    const TaaBlendWeights weights = computeTaaResolveBlendWeights(desc, history);
    return weights.history > 1e-5f;

const char* taaHistoryReuseBlockReasonLabel(TaaHistoryReuseBlockReason reason) {
    switch (reason) {
    case TaaHistoryReuseBlockReason::None:
        return "none";
    case TaaHistoryReuseBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryReuseBlockReason::NotWarm:
        return "not_warm";
    case TaaHistoryReuseBlockReason::StaleGeneration:
        return "stale_generation";
    return "unknown";

TaaHistoryReuseBlockReason classifyTaaHistoryReuseBlock(const TaaHistoryBuffer& history, u32 observedGeneration) {
    if (!history.isReady()) {
        return TaaHistoryReuseBlockReason::NotReady;
    if (history.isHistoryStale(observedGeneration)) {
        return TaaHistoryReuseBlockReason::StaleGeneration;
    if (!history.hasValidHistory()) {
        return TaaHistoryReuseBlockReason::NotWarm;
    return TaaHistoryReuseBlockReason::None;

const char* taaResolveBlendRejectReasonLabel(TaaResolveBlendRejectReason reason) {
    case TaaResolveBlendRejectReason::None:
    case TaaResolveBlendRejectReason::InvalidWeights:
        return "invalid_weights";
    case TaaResolveBlendRejectReason::InconsistentWithReuse:
        return "inconsistent_with_reuse";

TaaResolveBlendRejectReason classifyTaaResolveBlendReject(const TaaResolveDesc& desc,
                                                          const TaaHistoryBuffer& history) {
        return TaaResolveBlendRejectReason::InvalidWeights;
    const bool historyBlendAllowed = taaHistoryBlendAllowed(firstFrame, history) &&
                                     taaResolveCanReuseHistory(desc, history);
    if (!taaBlendWeightsConsistentWithReuse(weights, historyBlendAllowed)) {
        return TaaResolveBlendRejectReason::InconsistentWithReuse;
    return TaaResolveBlendRejectReason::None;

bool preflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                     TaaResolveBlendRejectReason* reason) {
    const TaaResolveBlendRejectReason reject = classifyTaaResolveBlendReject(desc, history);
    if (reason != nullptr) {
        *reason = reject;
    return reject == TaaResolveBlendRejectReason::None;

bool tryPreflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        TaaResolveBlendRejectReason& reason) {
    reason = classifyTaaResolveBlendReject(desc, history);
    return reason == TaaResolveBlendRejectReason::None;

bool shouldSkipTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolveBlendWeights(desc, history);

bool tryComputeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      TaaBlendWeights& outWeights, TaaResolveBlendRejectReason& reason) {
    outWeights = computeTaaResolveBlendWeights(desc, history);

bool TaaResolveBlendPreflight::readyForBlend() const {
    return weights_valid;
}

TaaResolveBlendPreflight preflightTaaResolveBlend(bool firstFrame, const TAAParams& params,
                                                  const TaaHistoryBuffer& history) {
    TaaResolveBlendPreflight preflight{};
    preflight.first_frame = firstFrame;
    preflight.weights = computeTaaBlendWeights(firstFrame, params);
    preflight.weights_valid = taaBlendWeightsValid(preflight.weights);
    preflight.history_blend_allowed = taaHistoryBlendAllowed(firstFrame, history);
    preflight.history_reuse_allowed = taaHistoryCanReuse(history);
    return preflight;
}

TaaResolveBlendPreflight preflightTaaResolveBlendForDesc(const TaaResolveDesc& desc,
                                                         const TaaHistoryBuffer& history) {
    const bool firstFrame = !history.hasValidHistory();
    TaaResolveBlendPreflight preflight = preflightTaaResolveBlend(firstFrame, desc.params, history);
    preflight.history_reuse_allowed = taaResolveCanReuseHistory(desc, history);
    return preflight;
}

bool taaHistoryNeedsWarmup(const TaaHistoryBuffer& history) {
    return history.needsWarmup();
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return history.isReady() && history.hasValidHistory();
}

TaaHistoryReuseBlockReason classifyTaaHistoryReuseBlock(const TaaHistoryBuffer& history, u32 observedGeneration) {
    if (!history.isReady()) {
        return TaaHistoryReuseBlockReason::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryReuseBlockReason::NeedsWarmup;
    }
    if (history.isHistoryStale(observedGeneration)) {
        return TaaHistoryReuseBlockReason::StaleGeneration;
    }
    return TaaHistoryReuseBlockReason::None;
}

bool taaHistoryReuseBlocked(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return classifyTaaHistoryReuseBlock(history, observedGeneration) != TaaHistoryReuseBlockReason::None;
}

const char* taaHistoryReuseBlockReasonLabel(TaaHistoryReuseBlockReason reason) {
    switch (reason) {
    case TaaHistoryReuseBlockReason::None:
        return "none";
    case TaaHistoryReuseBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryReuseBlockReason::NeedsWarmup:
        return "needs_warmup";
    case TaaHistoryReuseBlockReason::StaleGeneration:
        return "stale_generation";
    }
    return "unknown";
}

bool computeTaaResolveFirstFrame(const TaaHistoryBuffer& history) {
    return history.isReady() && !history.hasValidHistory();
}

TaaBlendPreflightRejectReason classifyTaaBlendPreflightReject(const TaaResolveDesc& desc,
                                                                const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaBlendPreflightRejectReason::HistoryNotReady;
    }
    if (computeTaaResolveFirstFrame(history)) {
        return TaaBlendPreflightRejectReason::WarmupRequired;
    }
    if (!taaResolveBypassesHistoryGenerationGuard(desc) &&
        history.isHistoryStale(desc.observed_history_generation)) {
        return TaaBlendPreflightRejectReason::StaleGeneration;
    }
    return TaaBlendPreflightRejectReason::None;
}

bool taaResolveBlendPreflight(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                               TaaBlendPreflightRejectReason* reason) {
    const TaaBlendPreflightRejectReason reject = classifyTaaBlendPreflightReject(desc, history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaBlendPreflightRejectReason::None;
}

bool taaResolveWouldUseHistoryBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (!taaResolveBlendPreflight(desc, history)) {
        return false;
    }
    const bool firstFrame = computeTaaResolveFirstFrame(history);
    return taaHistoryBlendAllowed(firstFrame, history);
}

bool taaBlendWeightsConsistentWithReuse(const TaaBlendWeights& weights, bool historyBlendAllowed) {
    if (!taaBlendWeightsValid(weights)) {
        return false;
    }
    if (historyBlendAllowed) {
        return true;
    }
    return weights.history <= 1e-5f && weights.current >= 1.f - 1e-5f;
}

TaaBlendWeights computeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    const bool firstFrame = !history.hasValidHistory();
    const TAAParams params = clampTaaParams(desc.params);
    TaaBlendWeights weights = computeTaaBlendWeights(firstFrame, params);
    if (!taaResolveCanReuseHistory(desc, history)) {
        weights.current = 1.f;
        weights.history = 0.f;
    }
    return weights;
}

bool taaResolveAppliesHistoryBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    const TaaBlendWeights weights = computeTaaResolveBlendWeights(desc, history);
    return weights.history > 1e-5f;
}

bool taaResolveBlendRejectReasonIsBlocking(TaaResolveBlendRejectReason reason) {
    return reason != TaaResolveBlendRejectReason::None;
}

const char* taaHistoryReuseBlockReasonLabel(TaaHistoryReuseBlockReason reason) {
    switch (reason) {
    case TaaHistoryReuseBlockReason::None:
        return "none";
    case TaaHistoryReuseBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryReuseBlockReason::NotWarm:
        return "not_warm";
    case TaaHistoryReuseBlockReason::StaleGeneration:
        return "stale_generation";
    }
    return "unknown";
}

const char* taaResolveBlendPreflightRejectReasonLabel(TaaResolveBlendPreflightRejectReason reason) {
    switch (reason) {
    case TaaResolveBlendPreflightRejectReason::None:
        return "none";
    case TaaResolveBlendPreflightRejectReason::HistoryNotReady:
        return "history_not_ready";
    case TaaResolveBlendPreflightRejectReason::InvalidWeights:
        return "invalid_weights";
    case TaaResolveBlendPreflightRejectReason::ReusePolicyViolation:
        return "reuse_policy_violation";
    }
    return "unknown";
}

TaaResolveBlendPreflightRejectReason diagnoseTaaResolveBlendPreflight(const TaaResolveDesc& desc,
                                                                      const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaResolveBlendPreflightRejectReason::HistoryNotReady;
    }

    const TaaBlendWeights weights = computeTaaResolveBlendWeights(desc, history);
    if (!taaBlendWeightsValid(weights)) {
        return TaaResolveBlendPreflightRejectReason::InvalidWeights;
    }

    const bool historyBlendAllowed = taaHistoryBlendAllowed(!history.hasValidHistory(), history) &&
                                     taaResolveCanReuseHistory(desc, history);
    if (!taaBlendWeightsConsistentWithReuse(weights, historyBlendAllowed)) {
        return TaaResolveBlendPreflightRejectReason::ReusePolicyViolation;
    }

    return TaaResolveBlendPreflightRejectReason::None;
}

bool preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaResolveBlendPreflightRejectReason* reason) {
    const TaaResolveBlendPreflightRejectReason reject = diagnoseTaaResolveBlendPreflight(desc, history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaResolveBlendPreflightRejectReason::None;
}

TaaHistoryReuseBlockReason classifyTaaHistoryReuseBlock(const TaaHistoryBuffer& history, u32 observedGeneration) {
    if (!history.isReady()) {
        return TaaHistoryReuseBlockReason::NotReady;
    }
    if (history.isHistoryStale(observedGeneration)) {
        return TaaHistoryReuseBlockReason::StaleGeneration;
    }
    if (!history.hasValidHistory()) {
        return TaaHistoryReuseBlockReason::NotWarm;
    }
    return TaaHistoryReuseBlockReason::None;
}

bool preflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
                            TaaHistoryReuseBlockReason* reason) {
    const TaaHistoryReuseBlockReason block = classifyTaaHistoryReuseBlock(history, observedGeneration);
TaaHistoryReuseBlockReason classifyTaaHistoryReuseBlockForResolve(const TaaResolveDesc& desc,
                                                                 const TaaHistoryBuffer& history) {
    if (taaResolveBypassesHistoryGenerationGuard(desc)) {
        if (!history.isReady()) {
            return TaaHistoryReuseBlockReason::NotReady;
        }
        if (!history.hasValidHistory()) {
            return TaaHistoryReuseBlockReason::NotWarm;
        return TaaHistoryReuseBlockReason::None;
    return classifyTaaHistoryReuseBlock(history, desc.observed_history_generation);

bool preflightTaaHistoryReuseForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
    const TaaHistoryReuseBlockReason block = classifyTaaHistoryReuseBlockForResolve(desc, history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryReuseBlockReason::None;
}

bool preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                            TaaBlendWeights* weights) {
    const TaaBlendWeights computed = computeTaaResolveBlendWeights(desc, history);
    if (weights != nullptr) {
        *weights = computed;
    }
    const bool historyBlendAllowed =
        taaHistoryBlendAllowed(!history.hasValidHistory(), history) && taaResolveCanReuseHistory(desc, history);
    return taaBlendWeightsValid(computed) && taaBlendWeightsConsistentWithReuse(computed, historyBlendAllowed);

bool preflightTaaHistoryReuseForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        TaaHistoryReuseBlockReason* reason) {
    const u32 observedGeneration = taaResolveBypassesHistoryGenerationGuard(desc)
                                       ? history.invalidateGeneration()
                                       : desc.observed_history_generation;
    return preflightTaaHistoryReuse(history, observedGeneration, reason);

bool preflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaResolveSkipReason* skipReason, TaaResolveBlendRejectReason* blendRejectReason) {
    if (!preflightTaaResolve(desc, history, skipReason)) {
        if (blendRejectReason != nullptr) {
            *blendRejectReason = TaaResolveBlendRejectReason::None;
        return false;
    return preflightTaaResolveBlendWeights(desc, history, blendRejectReason);

bool canPreflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveBlendWeights(desc, history);

bool tryComputeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      TaaBlendWeights& out, TaaResolveBlendRejectReason* reason) {
    if (!preflightTaaResolveBlendWeights(desc, history, reason)) {
    out = computeTaaResolveBlendWeights(desc, history);
    return true;

const char* taaResolveTemporalRejectReasonLabel(TaaResolveTemporalRejectReason reason) {
bool tryPreflightTaaHistoryReuseForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                           TaaHistoryReuseBlockReason& outReason) {
    outReason = classifyTaaHistoryReuseBlockForResolve(desc, history);
    return outReason == TaaHistoryReuseBlockReason::None;

const char* taaResolveBlendRejectReasonLabel(TaaResolveBlendRejectReason reason) {
    switch (reason) {
    case TaaResolveTemporalRejectReason::None:
        return "none";
    case TaaResolveTemporalRejectReason::HistoryReuseBlocked:
        return "history_reuse_blocked";
    case TaaResolveTemporalRejectReason::BlendWeightsRejected:
        return "blend_weights_rejected";
    }
    return "unknown";
}

TaaResolveTemporalRejectReason classifyTaaResolveTemporalReject(const TaaResolveDesc& desc,
                                                                  const TaaHistoryBuffer& history) {
    const u32 observedGeneration = taaResolveBypassesHistoryGenerationGuard(desc)
                                       ? history.invalidateGeneration()
                                       : desc.observed_history_generation;
    if (!preflightTaaHistoryReuse(history, observedGeneration)) {
        return TaaResolveTemporalRejectReason::HistoryReuseBlocked;
    }
    if (!preflightTaaResolveBlendWeights(desc, history)) {
        return TaaResolveTemporalRejectReason::BlendWeightsRejected;
    }
    return TaaResolveTemporalRejectReason::None;
}

bool preflightTaaResolveTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      TaaResolveTemporalRejectReason* reason) {
    const TaaResolveTemporalRejectReason reject = classifyTaaResolveTemporalReject(desc, history);
bool taaResolveBlendRejectReasonIsBlocking(TaaResolveBlendRejectReason reason) {
    return reason != TaaResolveBlendRejectReason::None;
}

bool preflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                     TaaResolveBlendRejectReason* reason) {
    const TaaResolveBlendRejectReason reject = classifyTaaResolveBlendReject(desc, history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaResolveTemporalRejectReason::None;
}

bool canPreflightTaaResolveTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveTemporalBlend(desc, history);
}

bool preflightTaaResolveGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                               TaaResolveSkipReason* skipReason,
                               TaaResolveBlendRejectReason* blendRejectReason) {
    if (!preflightTaaResolve(desc, history, skipReason)) {
        if (blendRejectReason != nullptr) {
            *blendRejectReason = TaaResolveBlendRejectReason::None;
        }
        return false;
    }
    return preflightTaaResolveBlendWeights(desc, history, blendRejectReason);
}

const char* taaResolveBlendModeLabel(TaaResolveBlendMode mode) {
    switch (mode) {
    case TaaResolveBlendMode::Warmup:
        return "warmup";
    case TaaResolveBlendMode::Steady:
        return "steady";
    case TaaResolveBlendMode::StaleForcedCurrent:
        return "stale_forced_current";
    }
    return "unknown";
}

TaaResolveBlendMode classifyTaaResolveBlendMode(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (!history.hasValidHistory()) {
        return TaaResolveBlendMode::Warmup;
    }
    if (!taaResolveCanReuseHistory(desc, history)) {
        return TaaResolveBlendMode::StaleForcedCurrent;
    }
    return TaaResolveBlendMode::Steady;
}

bool taaBlendWeightsNearEqual(const TaaBlendWeights& a, const TaaBlendWeights& b, f32 epsilon) {
    return std::fabs(a.current - b.current) <= epsilon && std::fabs(a.history - b.history) <= epsilon;
}

bool taaResolveStatsBlendConsistent(const TaaResolveStats& stats) {
    const TaaBlendWeights weights{stats.effective_blend, stats.history_blend};
    return taaBlendWeightsValid(weights);
}

bool preflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaResolveSkipReason* skipReason, TaaResolveBlendRejectReason* blendReason) {
    if (!preflightTaaResolve(desc, history, skipReason)) {
        if (blendReason != nullptr) {
            *blendReason = TaaResolveBlendRejectReason::None;
        }
        return false;
    }
    return preflightTaaResolveBlendWeights(desc, history, blendReason);
}

bool preflightTaaResolveDesc(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                             TaaResolveDescPreflight* result) {
    TaaResolveDescPreflight local{};
    local.skip_reason = classifyTaaResolveSkip(desc, history);
    if (taaResolveSkipReasonIsBlocking(local.skip_reason)) {
        if (result != nullptr) {
            *result = local;
        }
        return false;
    }

    local.blend_reject_reason = classifyTaaResolveBlendReject(desc, history);
    if (local.blend_reject_reason != TaaResolveBlendRejectReason::None) {
        if (result != nullptr) {
            *result = local;
        }
        return false;
    }

    local.passes = true;
    if (result != nullptr) {
        *result = local;
    }
    return true;
}

bool tryPreflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        TaaResolveBlendRejectReason& outReason) {
    outReason = classifyTaaResolveBlendReject(desc, history);
    return outReason == TaaResolveBlendRejectReason::None;
}

bool preflightTaaResolveTemporalAccumulation(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                             TaaResolveTemporalPreflight* result) {
    TaaResolveTemporalPreflight local{};
    local.skip_reason = classifyTaaResolveSkip(desc, history);
    local.reuse_block = classifyTaaHistoryReuseBlockForResolve(desc, history);
    local.blend_reject = classifyTaaResolveBlendReject(desc, history);
    local.passes = !taaResolveSkipReasonIsBlocking(local.skip_reason) &&
                   local.blend_reject == TaaResolveBlendRejectReason::None;
    if (result != nullptr) {
        *result = local;
    }
    return local.passes;
}

bool taaResolveBlendReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveBlendWeights(desc, history);
}

bool wouldSkipTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return shouldSkipTaaResolveBlend(desc, history);
}

bool taaResolveBlendReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveBlendWeights(desc, history);
}

bool tryComputeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      TaaBlendWeights& outWeights, TaaResolveBlendRejectReason* reason) {
    const TaaResolveBlendRejectReason reject = classifyTaaResolveBlendReject(desc, history);
    if (reason != nullptr) {
        *reason = reject;
    }
    if (reject != TaaResolveBlendRejectReason::None) {
        return false;
    }
    outWeights = computeTaaResolveBlendWeights(desc, history);
    return true;
}

bool preflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaResolveFramePreflight* out) {
    TaaResolveFramePreflight result{};
    result.skip_reason = classifyTaaResolveSkip(desc, history);
    if (taaResolveSkipReasonIsBlocking(result.skip_reason)) {
        if (out != nullptr) {
            *out = result;
        }
        return false;
    }

    result.blend_reason = classifyTaaResolveBlendReject(desc, history);
    if (result.blend_reason != TaaResolveBlendRejectReason::None) {
        if (out != nullptr) {
            *out = result;
        }
        return false;
    }

    result.canProceed = true;
    if (out != nullptr) {
        *out = result;
    }
    return true;
}

bool tryPreflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        TaaResolveBlendRejectReason& outReason) {
    outReason = classifyTaaResolveBlendReject(desc, history);
    return outReason == TaaResolveBlendRejectReason::None;
}

bool shouldSkipHistoryBlendAtResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !taaResolveAppliesHistoryBlend(desc, history);
}

bool canApplyHistoryBlendAtResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveBlendWeights(desc, history) && taaResolveAppliesHistoryBlend(desc, history);
}

bool canPreflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveBlendWeights(desc, history);
}

bool tryComputeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      TaaBlendWeights& out, TaaResolveBlendRejectReason* reason) {
    if (!preflightTaaResolveBlendWeights(desc, history, reason)) {
        return false;
    }
    out = computeTaaResolveBlendWeights(desc, history);
    return true;
}

const char* taaResolveTemporalRejectReasonLabel(TaaResolveTemporalRejectReason reason) {
    switch (reason) {
    case TaaResolveTemporalRejectReason::None:
        return "none";
    case TaaResolveTemporalRejectReason::HistoryReuseBlocked:
        return "history_reuse_blocked";
    case TaaResolveTemporalRejectReason::BlendWeightsRejected:
        return "blend_weights_rejected";
    }
    return "unknown";
}

TaaResolveTemporalRejectReason classifyTaaResolveTemporalReject(const TaaResolveDesc& desc,
                                                                  const TaaHistoryBuffer& history) {
    const u32 observedGeneration = taaResolveBypassesHistoryGenerationGuard(desc)
                                       ? history.invalidateGeneration()
                                       : desc.observed_history_generation;
    if (!preflightTaaHistoryReuse(history, observedGeneration)) {
        return TaaResolveTemporalRejectReason::HistoryReuseBlocked;
    }
    if (!preflightTaaResolveBlendWeights(desc, history)) {
        return TaaResolveTemporalRejectReason::BlendWeightsRejected;
    }
    return TaaResolveTemporalRejectReason::None;
}

bool preflightTaaResolveTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      TaaResolveTemporalRejectReason* reason) {
    const TaaResolveTemporalRejectReason reject = classifyTaaResolveTemporalReject(desc, history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaResolveTemporalRejectReason::None;
}

bool canPreflightTaaResolveTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveTemporalBlend(desc, history);
}

u32 effectiveObservedHistoryGeneration(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (taaResolveBypassesHistoryGenerationGuard(desc)) {
        return history.invalidateGeneration();
    }
    return desc.observed_history_generation;
}

bool preflightTaaResolveHistoryReuse(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                     TaaHistoryReuseBlockReason* reason) {
    return preflightTaaHistoryReuse(history, effectiveObservedHistoryGeneration(desc, history), reason);
}

bool taaHistoryReuseAllowedForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveHistoryReuse(desc, history, nullptr);
}

bool preflightTaaResolveTemporal(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                 TaaHistoryReuseBlockReason* reuseReason,
                                 TaaResolveBlendRejectReason* blendReason) {
    if (!preflightTaaResolveBlendWeights(desc, history, blendReason)) {
        return false;
    }
    const bool firstFrame = !history.hasValidHistory();
    if (taaHistoryBlendAllowed(firstFrame, history)) {
        return preflightTaaResolveHistoryReuse(desc, history, reuseReason);
    }
    if (reuseReason != nullptr) {
        *reuseReason = TaaHistoryReuseBlockReason::None;
    }
    return true;
}

bool tryComputeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      TaaBlendWeights* out) {
    if (out == nullptr) {
        return false;
    }
    if (!preflightTaaResolveBlendWeights(desc, history, nullptr)) {
        return false;
    }
    *out = computeTaaResolveBlendWeights(desc, history);
    return true;
}

bool wouldRejectTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolveBlendWeights(desc, history);
}

bool tryPreflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        TaaResolveBlendRejectReason& outReason) {
    outReason = classifyTaaResolveBlendReject(desc, history);
    return outReason == TaaResolveBlendRejectReason::None;
}

bool taaResolveBlendReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !shouldSkipTaaResolveBlend(desc, history);
}

bool taaResolveBlendReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveBlendWeights(desc, history);
}

bool preflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaResolveSkipReason* skipReason, TaaResolveBlendRejectReason* blendReason) {
    const bool resolveOk = preflightTaaResolve(desc, history, skipReason);
    const bool blendOk = preflightTaaResolveBlendWeights(desc, history, blendReason);
    return resolveOk && blendOk;
}

bool shouldSkipTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolveFrame(desc, history);
}

bool taaResolveBlendWeightsReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveBlendWeights(desc, history);
}

bool shouldSkipTaaResolveHistoryBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !taaResolveAppliesHistoryBlend(desc, history);
}

bool tryPreflightTaaResolveHistoryBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        TaaResolveBlendRejectReason& reason) {
    if (!preflightTaaResolveBlendWeights(desc, history, &reason)) {
        return false;
    }
    return taaResolveAppliesHistoryBlend(desc, history);
}

const char* taaResolveTemporalRejectReasonLabel(TaaResolveTemporalRejectReason reason) {
    switch (reason) {
    case TaaResolveTemporalRejectReason::None:
        return "none";
    case TaaResolveTemporalRejectReason::HistoryReuseBlocked:
        return "history_reuse_blocked";
    case TaaResolveTemporalRejectReason::BlendWeightsRejected:
        return "blend_weights_rejected";
    }
    return "unknown";
}

TaaResolveTemporalRejectReason classifyTaaResolveTemporalReject(const TaaResolveDesc& desc,
                                                                const TaaHistoryBuffer& history) {
    if (!preflightTaaResolveBlendWeights(desc, history)) {
        return TaaResolveTemporalRejectReason::BlendWeightsRejected;
    }

    const TaaBlendWeights weights = computeTaaResolveBlendWeights(desc, history);
    if (weights.history <= 1e-5f) {
        return TaaResolveTemporalRejectReason::None;
    }

    const u32 observedGeneration = taaResolveBypassesHistoryGenerationGuard(desc)
                                       ? history.invalidateGeneration()
                                       : desc.observed_history_generation;
    if (!preflightTaaHistoryReuse(history, observedGeneration)) {
        return TaaResolveTemporalRejectReason::HistoryReuseBlocked;
    }
    return TaaResolveTemporalRejectReason::None;
}

bool preflightTaaResolveTemporal(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                 TaaResolveTemporalRejectReason* reason) {
    const TaaResolveTemporalRejectReason reject = classifyTaaResolveTemporalReject(desc, history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaResolveTemporalRejectReason::None;
}

bool tryPreflightTaaResolveTemporal(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                    TaaResolveTemporalRejectReason& reason) {
    reason = classifyTaaResolveTemporalReject(desc, history);
    return reason == TaaResolveTemporalRejectReason::None;
}

bool shouldSkipTaaResolveTemporal(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolveTemporal(desc, history);
}

bool taaResolveBlendWeightsReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveBlendWeights(desc, history);
}

bool computeTaaResolveBlendWeightsIfReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                          TaaBlendWeights& outWeights,
                                          TaaResolveBlendRejectReason* reason) {
    TaaResolveBlendRejectReason rejectReason = TaaResolveBlendRejectReason::None;
    if (!tryComputeTaaResolveBlendWeights(desc, history, outWeights, rejectReason)) {
        if (reason != nullptr) {
            *reason = rejectReason;
        }
        return false;
    }
    if (reason != nullptr) {
        *reason = TaaResolveBlendRejectReason::None;
    }
    return true;
}

bool preflightTaaTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                               TaaHistoryReuseBlockReason* reuseReason,
                               TaaResolveBlendRejectReason* blendReason) {
    const u32 observedGeneration = taaResolveBypassesHistoryGenerationGuard(desc)
                                         ? history.invalidateGeneration()
                                         : desc.observed_history_generation;
    const bool reuseOk = preflightTaaHistoryReuse(history, observedGeneration, reuseReason);
    const bool blendOk = preflightTaaResolveBlendWeights(desc, history, blendReason);
    return reuseOk && blendOk;
}

bool shouldSkipTaaTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaTemporalBlend(desc, history);
}

const char* taaResolveReuseBlendRejectReasonLabel(TaaResolveReuseBlendRejectReason reason) {
    switch (reason) {
    case TaaResolveReuseBlendRejectReason::None:
        return "none";
    case TaaResolveReuseBlendRejectReason::HistoryReuseBlocked:
        return "history_reuse_blocked";
    case TaaResolveReuseBlendRejectReason::BlendWeightsRejected:
        return "blend_weights_rejected";
    }
    return "unknown";
}

TaaResolveReuseBlendRejectReason classifyTaaResolveReuseBlendReject(const TaaResolveDesc& desc,
                                                                    const TaaHistoryBuffer& history,
                                                                    u32 observedGeneration) {
    if (!preflightTaaHistoryReuse(history, observedGeneration)) {
        return TaaResolveReuseBlendRejectReason::HistoryReuseBlocked;
    }
    if (!preflightTaaResolveBlendWeights(desc, history)) {
        return TaaResolveReuseBlendRejectReason::BlendWeightsRejected;
    }
    return TaaResolveReuseBlendRejectReason::None;
}

bool preflightTaaResolveReuseAndBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      u32 observedGeneration, TaaResolveReuseBlendRejectReason* reason) {
    const TaaResolveReuseBlendRejectReason reject =
        classifyTaaResolveReuseBlendReject(desc, history, observedGeneration);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaResolveReuseBlendRejectReason::None;
}

bool tryPreflightTaaResolveReuseAndBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                         u32 observedGeneration, TaaResolveReuseBlendRejectReason& reason) {
    reason = classifyTaaResolveReuseBlendReject(desc, history, observedGeneration);
    return reason == TaaResolveReuseBlendRejectReason::None;
}

bool shouldSkipTaaResolveReuseAndBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                       u32 observedGeneration) {
    return !preflightTaaResolveReuseAndBlend(desc, history, observedGeneration);
}

const char* taaTemporalGuardRejectReasonLabel(TaaTemporalGuardRejectReason reason) {
    switch (reason) {
    case TaaTemporalGuardRejectReason::None:
        return "none";
    case TaaTemporalGuardRejectReason::HistoryReuseBlocked:
        return "history_reuse_blocked";
    case TaaTemporalGuardRejectReason::BlendWeightsRejected:
        return "blend_weights_rejected";
    }
    return "unknown";
}

TaaTemporalGuardRejectReason classifyTaaTemporalGuardReject(const TaaResolveDesc& desc,
                                                            const TaaHistoryBuffer& history) {
    const u32 observedGeneration = taaResolveBypassesHistoryGenerationGuard(desc)
                                       ? history.invalidateGeneration()
                                       : desc.observed_history_generation;
    if (!preflightTaaHistoryReuse(history, observedGeneration)) {
        return TaaTemporalGuardRejectReason::HistoryReuseBlocked;
    }
    if (!preflightTaaResolveBlendWeights(desc, history)) {
        return TaaTemporalGuardRejectReason::BlendWeightsRejected;
    }
    return TaaTemporalGuardRejectReason::None;
}

bool preflightTaaTemporalResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                 TaaTemporalGuardRejectReason* reason) {
    const TaaTemporalGuardRejectReason reject = classifyTaaTemporalGuardReject(desc, history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaTemporalGuardRejectReason::None;
}

bool tryPreflightTaaTemporalResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                    TaaTemporalGuardRejectReason& reason) {
    reason = classifyTaaTemporalGuardReject(desc, history);
    return reason == TaaTemporalGuardRejectReason::None;
}

bool shouldSkipTaaTemporalResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaTemporalResolve(desc, history);
}

TaaResolveBlendPreflight preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    TaaResolveBlendPreflight preflight{};
    preflight.weights = computeTaaResolveBlendWeights(desc, history);
    preflight.reject_reason = classifyTaaResolveBlendReject(desc, history);
    preflight.can_apply = preflight.reject_reason == TaaResolveBlendRejectReason::None;
    return preflight;
}

const char* taaResolveTemporalRejectReasonLabel(TaaResolveTemporalRejectReason reason) {
    switch (reason) {
    case TaaResolveTemporalRejectReason::None:
        return "none";
    case TaaResolveTemporalRejectReason::HistoryReuseBlocked:
        return "history_reuse_blocked";
    case TaaResolveTemporalRejectReason::BlendWeightsRejected:
        return "blend_weights_rejected";
    }
    return "unknown";
}

TaaResolveTemporalRejectReason classifyTaaResolveTemporalBlendReject(const TaaResolveDesc& desc,
                                                                     const TaaHistoryBuffer& history) {
    const u32 observedGeneration = taaResolveBypassesHistoryGenerationGuard(desc)
                                         ? history.invalidateGeneration()
                                         : desc.observed_history_generation;
    if (!preflightTaaHistoryReuse(history, observedGeneration)) {
        return TaaResolveTemporalRejectReason::HistoryReuseBlocked;
    }
    if (!preflightTaaResolveBlendWeights(desc, history)) {
        return TaaResolveTemporalRejectReason::BlendWeightsRejected;
    }
    return TaaResolveTemporalRejectReason::None;
}

bool preflightTaaResolveTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      TaaResolveTemporalRejectReason* reason) {
    const TaaResolveTemporalRejectReason reject = classifyTaaResolveTemporalBlendReject(desc, history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaResolveTemporalRejectReason::None;
}

bool tryPreflightTaaResolveTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                         TaaResolveTemporalRejectReason& reason) {
    reason = classifyTaaResolveTemporalBlendReject(desc, history);
    return reason == TaaResolveTemporalRejectReason::None;
}

bool shouldSkipTaaResolveTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolveTemporalBlend(desc, history);
}

bool preflightTaaTemporalResolveGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                       u32 observedGeneration, TaaHistoryReuseBlockReason* reuseReason,
                                       TaaResolveBlendRejectReason* blendReason) {
    const bool reuseOk = preflightTaaHistoryReuse(history, observedGeneration, reuseReason);
    const bool blendOk = preflightTaaResolveBlendWeights(desc, history, blendReason);
    return reuseOk && blendOk;
}

bool shouldSkipTaaTemporalResolveGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        u32 observedGeneration) {
    return !preflightTaaTemporalResolveGuards(desc, history, observedGeneration);
}

bool isHistoryBlendDegraded(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (!history.hasValidHistory()) {
        return false;
    }
    const bool historyBlendAllowed =
        taaHistoryBlendAllowed(false, history) && taaResolveCanReuseHistory(desc, history);
    return !historyBlendAllowed;
}

bool canApplyTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveBlendWeights(desc, history);
}

TaaResolveBlendPreflight preflightTaaResolveBlendFrame(const TaaResolveDesc& desc,
                                                       const TaaHistoryBuffer& history) {
    TaaResolveBlendPreflight preflight{};
    preflight.weights = computeTaaResolveBlendWeights(desc, history);
    preflight.rejectReason = classifyTaaResolveBlendReject(desc, history);
    const bool firstFrame = !history.hasValidHistory();
    preflight.historyBlendAllowed =
        taaHistoryBlendAllowed(firstFrame, history) && taaResolveCanReuseHistory(desc, history);
    preflight.historyDegraded = !preflight.historyBlendAllowed && history.hasValidHistory();
    return preflight;
}

TaaFrameGuardPreflight preflightTaaFrameGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                               u32 observedGeneration) {
    TaaFrameGuardPreflight preflight{};
    preflight.history = preflightTaaHistoryWarmup(history, observedGeneration);
    preflight.blend = preflightTaaResolveBlendFrame(desc, history);
    return preflight;
}

bool taaResolveBlendReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveBlendWeights(desc, history);
}

bool preflightTaaResolveTemporal(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                 TaaHistoryReuseBlockReason* reuseReason,
                                 TaaResolveBlendRejectReason* blendReason) {
    const u32 observedGeneration = taaResolveBypassesHistoryGenerationGuard(desc)
                                         ? history.invalidateGeneration()
                                         : desc.observed_history_generation;
    if (!preflightTaaHistoryReuse(history, observedGeneration, reuseReason)) {
        return false;
    }
    return preflightTaaResolveBlendWeights(desc, history, blendReason);
}

bool tryPreflightTaaResolveTemporal(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                    TaaHistoryReuseBlockReason& reuseReason,
                                    TaaResolveBlendRejectReason& blendReason) {
    return preflightTaaResolveTemporal(desc, history, &reuseReason, &blendReason);
}

bool shouldSkipTaaResolveTemporal(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolveTemporal(desc, history);
}

bool preflightTaaResolveFrameGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                    TaaResolveSkipReason* skipReason,
                                    TaaResolveBlendRejectReason* blendReason) {
    const bool skipOk = preflightTaaResolve(desc, history, skipReason);
    const bool blendOk = preflightTaaResolveBlendWeights(desc, history, blendReason);
    return skipOk && blendOk;
}

bool tryPreflightTaaResolveFrameGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                       TaaResolveSkipReason& skipReason,
                                       TaaResolveBlendRejectReason& blendReason) {
    return preflightTaaResolveFrameGuards(desc, history, &skipReason, &blendReason);
}

bool shouldSkipTaaResolveFrameGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolveFrameGuards(desc, history);
}

bool computeTaaResolveBlendWeightsIfReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                          TaaBlendWeights& outWeights, TaaResolveBlendRejectReason* reason) {
    TaaResolveBlendRejectReason rejectReason = TaaResolveBlendRejectReason::None;
    if (!tryComputeTaaResolveBlendWeights(desc, history, outWeights, rejectReason)) {
        if (reason != nullptr) {
            *reason = rejectReason;
        }
        return false;
    }
    if (reason != nullptr) {
        *reason = TaaResolveBlendRejectReason::None;
    }
    return true;
}

const char* taaResolveWithBlendRejectReasonLabel(TaaResolveWithBlendRejectReason reason) {
    switch (reason) {
    case TaaResolveWithBlendRejectReason::None:
        return "none";
    case TaaResolveWithBlendRejectReason::ResolveBlocked:
        return "resolve_blocked";
    case TaaResolveWithBlendRejectReason::BlendRejected:
        return "blend_rejected";
    }
    return "unknown";
}

TaaResolveWithBlendRejectReason classifyTaaResolveWithBlendReject(const TaaResolveDesc& desc,
                                                                const TaaHistoryBuffer& history) {
    if (!preflightTaaResolve(desc, history)) {
        return TaaResolveWithBlendRejectReason::ResolveBlocked;
    }
    if (!preflightTaaResolveBlendWeights(desc, history)) {
        return TaaResolveWithBlendRejectReason::BlendRejected;
    }
    return TaaResolveWithBlendRejectReason::None;
}

bool preflightTaaResolveWithBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                  TaaResolveWithBlendRejectReason* reason) {
    const TaaResolveWithBlendRejectReason reject = classifyTaaResolveWithBlendReject(desc, history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaResolveWithBlendRejectReason::None;
}

bool tryPreflightTaaResolveWithBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                     TaaResolveWithBlendRejectReason& reason) {
    reason = classifyTaaResolveWithBlendReject(desc, history);
    return reason == TaaResolveWithBlendRejectReason::None;
}

bool shouldSkipTaaResolveWithBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolveWithBlend(desc, history);
}

bool tryComputeTaaResolveBlendWeightsIfResolveReady(const TaaResolveDesc& desc,
                                                    const TaaHistoryBuffer& history,
                                                    TaaBlendWeights& outWeights,
                                                    TaaResolveWithBlendRejectReason& reason) {
    reason = classifyTaaResolveWithBlendReject(desc, history);
    if (reason != TaaResolveWithBlendRejectReason::None) {
        return false;
    }
    outWeights = computeTaaResolveBlendWeights(desc, history);
    return true;
}

bool preflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                               TaaResolveSkipReason* skipReason,
                               TaaResolveBlendRejectReason* blendReject) {
    const TaaResolveSkipReason skip = classifyTaaResolveSkip(desc, history);
    const TaaResolveBlendRejectReason blend = classifyTaaResolveBlendReject(desc, history);
    if (skipReason != nullptr) {
        *skipReason = skip;
    }
    if (blendReject != nullptr) {
        *blendReject = blend;
    }
    return !taaResolveSkipReasonIsBlocking(skip) && blend == TaaResolveBlendRejectReason::None;
}

bool tryPreflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                 TaaResolveSkipReason& skipReason, TaaResolveBlendRejectReason& blendReject) {
    return preflightTaaResolveFrame(desc, history, &skipReason, &blendReject);
}

bool shouldSkipTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return shouldSkipTaaResolve(desc, history) || shouldSkipTaaResolveBlend(desc, history);
}

bool taaResolveCanReuseHistory(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (!taaHistoryCanReuse(history)) {
    if (taaResolveBypassesHistoryGenerationGuard(desc)) {
    return !history.isHistoryStale(desc.observed_history_generation);
bool taaParamsRequireClamping(const TAAParams& raw) {
    const TAAParams clamped = clampTaaParams(raw);
    return raw.blend_factor != clamped.blend_factor || raw.velocity_rejection != clamped.velocity_rejection ||
           raw.depth_rejection != clamped.depth_rejection || raw.clamp_gamma != clamped.clamp_gamma;
}

TaaResolveBlendPreflight preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    TaaResolveBlendPreflight preflight{};
    if (!history.isReady()) {
        return preflight;
    }

    const bool firstFrame = !history.hasValidHistory();
    preflight.first_frame = firstFrame;
    preflight.history_reuse = taaResolveCanReuseHistory(desc, history);
    preflight.weights = computeTaaBlendWeights(firstFrame, desc.params);
    preflight.valid = taaBlendWeightsValid(preflight.weights);
    return preflight;
}

bool taaResolveBlendPreflightValid(const TaaResolveBlendPreflight& preflight) {
    return preflight.valid;
}

bool taaResolveWouldBlendHistory(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    const TaaResolveBlendPreflight preflight = preflightTaaResolveBlend(desc, history);
    return preflight.valid && !preflight.first_frame && preflight.history_reuse &&
           preflight.weights.history > 0.f;
}

bool taaResolveBlendConsistentWithHistory(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    const TaaResolveBlendPreflight preflight = preflightTaaResolveBlend(desc, history);
    if (!preflight.valid) {
        return false;
    }
    if (preflight.first_frame != !history.hasValidHistory()) {
        return false;
    }
    if (preflight.first_frame) {
        return preflight.weights.current >= 1.f - 1e-5f && preflight.weights.history <= 1e-5f;
    }
    return taaHistoryBlendAllowed(false, history) == preflight.history_reuse;
}

bool taaResolveBlendWeightsMatchExpected(const TaaBlendWeights& weights, const TaaResolveDesc& desc,
                                         const TaaHistoryBuffer& history) {
    const TaaBlendWeights expected = computeTaaResolveBlendWeights(desc, history);
    return std::fabs(weights.current - expected.current) <= 1e-5f &&
           std::fabs(weights.history - expected.history) <= 1e-5f;
}

bool taaResolveTemporalBlendAllowed(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return preflightTaaResolveBlendWeights(desc, history) && taaResolveCanReuseHistory(desc, history);
}

bool preflightTaaResolveTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      TaaResolveBlendRejectReason* blendReason,
                                      TaaHistoryReuseBlockReason* reuseReason) {
    const bool blendPasses = preflightTaaResolveBlendWeights(desc, history, blendReason);
    u32 observedGeneration = desc.observed_history_generation;
    if (taaResolveBypassesHistoryGenerationGuard(desc)) {
        observedGeneration = history.invalidateGeneration();
    }
    const bool reusePasses = preflightTaaHistoryReuse(history, observedGeneration, reuseReason);
    return blendPasses && reusePasses;
}

bool taaResolveRequiresVelocity(const TAAParams& params) {
    return params.velocity_rejection > 0.f;
}

bool taaResolveRequiresDepth(const TAAParams& params) {
    return params.depth_rejection > 0.f;
}

bool taaResolveRequiresRejectionSurfaces(const TAAParams& params) {
    return taaResolveRequiresVelocity(params) || taaResolveRequiresDepth(params);
bool taaResolveRejectionSurfacesComplete(const TaaResolveDesc& desc) {
    if (!desc.enforce_rejection_surfaces) {
        return true;
    }

    const TAAParams params = clampTaaParams(desc.params);
    if (taaResolveRequiresVelocity(params) && desc.surfaces.velocity_buffer == nullptr) {
        return false;
    if (taaResolveRequiresDepth(params) && desc.surfaces.depth_buffer == nullptr) {

bool shouldSkipTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                          TaaResolveSkipReason* reason) {
    const TaaResolveSkipReason skip = classifyTaaResolveSkip(desc, history);
    if (reason != nullptr) {
        *reason = skip;
    return taaResolveSkipReasonIsBlocking(skip);

f32 computeEffectiveBlend(bool firstFrame, const TAAParams& params) {
    if (taaUsesWarmupBlend(firstFrame)) {
f32 computeEffectiveBlend(bool firstFrame, bool historyReusable, const TAAParams& params) {
    if (firstFrame || !historyReusable) {
bool isTaaBlendFactorInRange(f32 blend_factor) {
    return blend_factor >= 0.f && blend_factor <= 1.f;

bool taaBlendWeightReusesHistory(f32 effective_blend) {
    return effective_blend < 1.f;

f32 computeHistoryContributionWeight(f32 effective_blend) {
    return computeHistoryBlend(effective_blend);

bool taaUsesWarmupBlend(bool first_frame) {
    return first_frame;

    if (taaUsesWarmupBlend(firstFrame) || !historyReusable) {
        return 1.f;
    }
    return clampTaaParams(params).blend_factor;
}

f32 computeEffectiveBlendForHistory(const TaaHistoryBuffer& history, const TAAParams& params) {
    return computeEffectiveBlend(!history.hasValidHistory(), params);
f32 clampEffectiveBlend(f32 effectiveBlend) {
    return clampF32(effectiveBlend, 0.f, 1.f);
}

f32 computeHistoryBlendWeight(f32 effectiveBlend) {
    return 1.f - clampEffectiveBlend(effectiveBlend);

f32 computeHistoryBlendWeight(bool firstFrame, const TAAParams& params) {
    return computeHistoryBlendWeight(computeEffectiveBlend(firstFrame, params));
bool isTaaBlendFactorInRange(f32 blend_factor) {
    return blend_factor >= 0.f && blend_factor <= 1.f;

bool taaBlendWeightReusesHistory(f32 effective_blend) {
    return clampEffectiveBlend(effective_blend) < 1.f;



bool taaUsesWarmupBlend(bool first_frame) {
    return first_frame;

void accumulateTaaResolveSkipReason(TaaResolveSkipCounts& counts, TaaResolveSkipReason reason) {
    if (!taaResolveSkipReasonIsBlocking(reason)) {
        return;

    ++counts.total;
    switch (reason) {
    case TaaResolveSkipReason::HistoryNotReady:
        ++counts.historyNotReady;
        break;
    case TaaResolveSkipReason::InvalidDimensions:
        ++counts.invalidDimensions;
    case TaaResolveSkipReason::DimensionMismatch:
        ++counts.dimensionMismatch;
    case TaaResolveSkipReason::MissingSurfaces:
        ++counts.missingSurfaces;
    case TaaResolveSkipReason::MissingVelocityBuffer:
        ++counts.missingVelocityBuffer;
    case TaaResolveSkipReason::MissingDepthBuffer:
        ++counts.missingDepthBuffer;
    case TaaResolveSkipReason::StaleHistoryGeneration:
        ++counts.staleHistoryGeneration;
    default:

TaaResolveSkipCounts taaResolveSkipCountsFromReason(TaaResolveSkipReason reason) {
    TaaResolveSkipCounts counts{};
    accumulateTaaResolveSkipReason(counts, reason);
    return counts;

    return effective_blend < 1.f;

f32 computeHistoryContributionWeight(f32 effective_blend) {
    return 1.f - effective_blend;

f32 computeEffectiveBlend(bool firstFrame, const TAAParams& params) {
    return computeEffectiveBlend(firstFrame, !firstFrame, params);

bool taaBlendUsesHistory(f32 effectiveBlend) {
    return effectiveBlend < 1.f;


bool taaBlendSkipsHistoryReuse(f32 effectiveBlend) {
    return effectiveBlend >= 1.f;



bool taaBlendUsesHistory(f32 effective_blend) {

bool taaBlendSkipsHistoryReuse(f32 effective_blend) {
    return effective_blend >= 1.f;





TaaBlendWeights computeTaaBlendWeightsWithReuseGuard(bool firstFrame, bool historyReusable,
                                                     const TAAParams& params) {
    const f32 effectiveBlend = computeEffectiveBlend(firstFrame, historyReusable, params);
    TaaBlendWeights weights{};
    weights.current = effectiveBlend;
    weights.history = computeHistoryBlend(effectiveBlend);
    return weights;




const char* taaHistoryReuseRejectReasonLabel(TaaHistoryReuseRejectReason reason) {
    case TaaHistoryReuseRejectReason::None:
        return "none";
    case TaaHistoryReuseRejectReason::HistoryNotReady:
        return "history_not_ready";
    case TaaHistoryReuseRejectReason::HistoryNotWarmed:
        return "history_not_warmed";
    case TaaHistoryReuseRejectReason::StaleGeneration:
        return "stale_generation";
    return "unknown";

const char* taaJitterSyncRejectReasonLabel(TaaJitterSyncRejectReason reason) {
    case TaaJitterSyncRejectReason::None:
    case TaaJitterSyncRejectReason::InvalidSequenceLength:
        return "invalid_sequence_length";
    case TaaJitterSyncRejectReason::InvalidViewport:
        return "invalid_viewport";
    case TaaJitterSyncRejectReason::FrameIndexMismatch:
        return "frame_index_mismatch";
    case TaaJitterSyncRejectReason::SlotIndexMismatch:
        return "slot_index_mismatch";

const char* taaBlendPreflightRejectReasonLabel(TaaBlendPreflightRejectReason reason) {
    case TaaBlendPreflightRejectReason::None:
    case TaaBlendPreflightRejectReason::HistoryNotReady:
    case TaaBlendPreflightRejectReason::WarmupRequired:
        return "warmup_required";
    case TaaBlendPreflightRejectReason::StaleGeneration:




}

const char* taaResolveSkipReasonLabel(TaaResolveSkipReason reason) {
    switch (reason) {
    case TaaResolveSkipReason::None:
        return "none";
    case TaaResolveSkipReason::HistoryNotReady:
        return "history_not_ready";
    case TaaResolveSkipReason::InvalidDimensions:
        return "invalid_dimensions";
    case TaaResolveSkipReason::DimensionMismatch:
        return "dimension_mismatch";
    case TaaResolveSkipReason::MissingSurfaces:
        return "missing_surfaces";
    case TaaResolveSkipReason::MissingVelocityBuffer:
        return "missing_velocity_buffer";
    case TaaResolveSkipReason::MissingDepthBuffer:
        return "missing_depth_buffer";
    case TaaResolveSkipReason::StaleHistoryGeneration:
        return "stale_history_generation";
    }
    return "unknown";
}

void TaaResolve::resetBookkeeping() {
    m_stats = {};
    m_message.clear();
}

bool TaaResolve::wouldSkip(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                           TaaResolveSkipReason* reason) const {
    return shouldSkipTaaResolve(desc, history, reason);
}

bool TaaResolve::preflightDesc(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                               TaaResolveDescPreflight* result) const {
    return preflightTaaResolveDesc(desc, history, result);
}

bool TaaResolve::resolve(const TaaResolveDesc& desc, TaaHistoryBuffer& history, void* /*cudaStream*/) {
    m_stats = {};
    m_message.clear();

    const TaaResolveSkipReason skip = classifyTaaResolveSkip(desc, history);
    if (taaResolveSkipReasonIsBlocking(skip)) {
        m_stats.skipped = true;
        m_stats.skip_reason = skip;
        switch (skip) {
        case TaaResolveSkipReason::HistoryNotReady:
            m_message = "TAA resolve skipped — history buffer not ready";
            break;
        case TaaResolveSkipReason::InvalidDimensions:
            m_message = "TAA resolve skipped — invalid dimensions";
            break;
        case TaaResolveSkipReason::DimensionMismatch:
            m_message = "TAA resolve skipped — resolve dimensions do not match history buffer";
            break;
        case TaaResolveSkipReason::MissingSurfaces:
            m_message = "TAA resolve skipped — missing current/output surfaces";
            break;
        case TaaResolveSkipReason::MissingVelocityBuffer:
            m_message = "TAA resolve skipped — velocity rejection enabled but velocity buffer missing";
            break;
        case TaaResolveSkipReason::MissingDepthBuffer:
            m_message = "TAA resolve skipped — depth rejection enabled but depth buffer missing";
            break;
        case TaaResolveSkipReason::StaleHistoryGeneration:
            m_message = "TAA resolve skipped — history invalidate generation is stale";
            break;
        default:
            break;
        }
        return false;
    }

    const TAAParams params = clampTaaParams(desc.params);
    const bool historyReusable = taaHistoryIsReusable(history, desc);
    m_stats.first_frame = !history.hasValidHistory();
    const TaaBlendWeights blendWeights = computeTaaResolveBlendWeights(desc, history);
    const f32 effectiveBlend = computeEffectiveBlend(m_stats.first_frame, historyReusable, params);
    const bool historyReusable = taaResolveCanReuseHistory(desc, history);
    const TaaBlendWeights blendWeights =
        computeTaaBlendWeightsWithReuseGuard(m_stats.first_frame, historyReusable, params);
    history.markResolved();
    history.swap();

    m_stats.resolved = true;
    m_stats.width = desc.width;
    m_stats.height = desc.height;
    m_stats.last_blend = params.blend_factor;
    m_stats.effective_blend = blendWeights.current;
    m_stats.history_blend = blendWeights.history;
    m_stats.effective_blend = effectiveBlend;
    m_stats.history_reused = historyReusable && taaBlendUsesHistory(effectiveBlend);
    m_stats.history_reused = historyReusable && taaBlendUsesHistory(blendWeights.current);
    m_stats.history_swapped = true;
    m_stats.has_valid_history = history.hasValidHistory();
    m_stats.accumulated_frames = history.accumulatedFrames();
    m_stats.history_invalidate_generation = history.invalidateGeneration();

#if defined(FUSE_HAS_CUDA)
    m_message = m_stats.first_frame ? "TAA resolve recorded — first frame (CUDA kernel deferred)"
                                    : "TAA resolve recorded (CUDA kernel deferred)";
#else
    m_message = m_stats.first_frame ? "TAA resolve recorded — first frame (CPU stub — no CUDA toolkit)"
                                    : "TAA resolve recorded (CPU stub — no CUDA toolkit)";
#endif

    return true;
}

} // namespace fuse::renderer
