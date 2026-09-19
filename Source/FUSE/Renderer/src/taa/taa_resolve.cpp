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
    const bool firstFrame = !history.hasValidHistory();
    const TAAParams params = clampTaaParams(desc.params);
    const f32 effectiveBlend =
        computeEffectiveBlend(firstFrame, taaResolveCanReuseHistory(desc, history), params);
    return taaBlendUsesHistory(effectiveBlend);
}

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
    const f32 effectiveBlend = computeEffectiveBlend(firstFrame, params);
    TaaBlendWeights weights{};
    weights.current = effectiveBlend;
    weights.history = computeHistoryBlend(effectiveBlend);
    return weights;

bool taaBlendWeightsValid(const TaaBlendWeights& weights) {
    if (weights.current < 0.f || weights.current > 1.f) {
        return false;
    if (weights.history < 0.f || weights.history > 1.f) {
    return std::fabs(weights.current + weights.history - 1.f) <= 1e-5f;

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

bool taaResolveCanReuseHistory(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (!taaHistoryCanReuse(history)) {
    if (taaResolveBypassesHistoryGenerationGuard(desc)) {
    return !history.isHistoryStale(desc.observed_history_generation);
bool taaParamsRequireClamping(const TAAParams& raw) {
    const TAAParams clamped = clampTaaParams(raw);
    return raw.blend_factor != clamped.blend_factor || raw.velocity_rejection != clamped.velocity_rejection ||
           raw.depth_rejection != clamped.depth_rejection || raw.clamp_gamma != clamped.clamp_gamma;
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
