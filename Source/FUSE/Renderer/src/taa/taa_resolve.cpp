#include <fuse/renderer/taa/taa_resolve.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {
namespace {

f32 clampF32(f32 value, f32 minValue, f32 maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

} // namespace

bool taaResolveDimensionsValid(u32 width, u32 height) {
    return width > 0u && height > 0u;
}

bool taaResolveSkipReasonIsBlocking(TaaResolveSkipReason reason) {
    return reason != TaaResolveSkipReason::None;
}

bool taaResolveDimensionsMatch(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return history.matchesDimensions(desc.width, desc.height);
}

bool taaResolveHasDimensionMismatch(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !taaResolveDimensionsMatch(desc, history);
}

bool taaResolveHistoryGenerationIsStale(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !taaResolveBypassesHistoryGenerationGuard(desc) &&
           history.isHistoryStale(desc.observed_history_generation);
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

namespace {

bool velocitySurfaceBound(const TaaResolveDesc& desc) {
    return taaResolveUsesCpuSurfaces(desc) ? desc.cpu.velocity_buffer != nullptr
                                           : desc.surfaces.velocity_buffer != nullptr;
}

bool depthSurfaceBound(const TaaResolveDesc& desc) {
    return taaResolveUsesCpuSurfaces(desc) ? desc.cpu.depth_buffer != nullptr
                                           : desc.surfaces.depth_buffer != nullptr;
}

} // namespace

bool taaResolveUsesCpuSurfaces(const TaaResolveDesc& desc) {
    return desc.cpu.isBound();
}

bool taaResolveRejectionSurfacesSatisfied(const TaaResolveDesc& desc) {
    if (!taaResolveRejectionSurfacesRequired(desc)) {
        return true;
    }
    const TAAParams params = clampTaaParams(desc.params);
    if (taaResolveRequiresVelocity(params) && !velocitySurfaceBound(desc)) {
        return false;
    }
    if (taaResolveRequiresDepth(params) && !depthSurfaceBound(desc)) {
        return false;
    }
    return true;
}

void stampObservedHistoryGeneration(TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (taaResolveBypassesHistoryGenerationGuard(desc)) {
        desc.observed_history_generation = history.invalidateGeneration();
    }
}

void sanitizeTaaResolveDesc(TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    normalizeTaaParams(desc.params);
    stampObservedHistoryGeneration(desc, history);
}

bool preflightTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                         TaaResolveSkipReason* reason) {
    const TaaResolveSkipReason skip = classifyTaaResolveSkip(desc, history);
    if (reason != nullptr) {
        *reason = skip;
    }
    return !taaResolveSkipReasonIsBlocking(skip);
}

bool tryPreflightTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                            TaaResolveSkipReason& reason) {
    reason = classifyTaaResolveSkip(desc, history);
    return !taaResolveSkipReasonIsBlocking(reason);
}

bool shouldSkipTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolve(desc, history);
}

TaaResolveSkipReason classifyTaaResolveSkip(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaResolveSkipReason::HistoryNotReady;
    }
    if (!taaResolveDimensionsValid(desc.width, desc.height)) {
        return TaaResolveSkipReason::InvalidDimensions;
    }
    if (taaResolveHasDimensionMismatch(desc, history)) {
        return TaaResolveSkipReason::DimensionMismatch;
    }
    const bool deviceSurfaces = desc.surfaces.current_frame != nullptr && desc.surfaces.output != nullptr;
    if (!deviceSurfaces && !taaResolveUsesCpuSurfaces(desc)) {
        return TaaResolveSkipReason::MissingSurfaces;
    }

    if (desc.enforce_rejection_surfaces) {
        const TAAParams params = clampTaaParams(desc.params);
        if (taaResolveRequiresVelocity(params) && !velocitySurfaceBound(desc)) {
            return TaaResolveSkipReason::MissingVelocityBuffer;
        }
        if (taaResolveRequiresDepth(params) && !depthSurfaceBound(desc)) {
            return TaaResolveSkipReason::MissingDepthBuffer;
        }
    }
    if (taaResolveHistoryGenerationIsStale(desc, history)) {
        return TaaResolveSkipReason::StaleHistoryGeneration;
    }
    return TaaResolveSkipReason::None;
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
}

f32 computeHistoryBlend(f32 effectiveBlend) {
    return clampF32(1.f - effectiveBlend, 0.f, 1.f);
}

TaaBlendWeights computeTaaBlendWeights(bool firstFrame, const TAAParams& params) {
    const f32 effectiveBlend = computeEffectiveBlend(firstFrame, params);
    TaaBlendWeights weights{};
    weights.current = effectiveBlend;
    weights.history = computeHistoryBlend(effectiveBlend);
    return weights;
}

bool taaBlendWeightsValid(const TaaBlendWeights& weights) {
    if (weights.current < 0.f || weights.current > 1.f) {
        return false;
    }
    if (weights.history < 0.f || weights.history > 1.f) {
        return false;
    }
    return std::fabs(weights.current + weights.history - 1.f) <= 1e-5f;
}

bool taaHistoryBlendAllowed(bool firstFrame, const TaaHistoryBuffer& history) {
    return !firstFrame && taaHistoryCanReuse(history);
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

const char* taaResolveBlendRejectReasonLabel(TaaResolveBlendRejectReason reason) {
    switch (reason) {
    case TaaResolveBlendRejectReason::None:
        return "none";
    case TaaResolveBlendRejectReason::InvalidWeights:
        return "invalid_weights";
    case TaaResolveBlendRejectReason::InconsistentWithReuse:
        return "inconsistent_with_reuse";
    }
    return "unknown";
}

TaaResolveBlendRejectReason classifyTaaResolveBlendReject(const TaaResolveDesc& desc,
                                                          const TaaHistoryBuffer& history) {
    const TaaBlendWeights weights = computeTaaResolveBlendWeights(desc, history);
    if (!taaBlendWeightsValid(weights)) {
        return TaaResolveBlendRejectReason::InvalidWeights;
    }
    const bool firstFrame = !history.hasValidHistory();
    const bool historyBlendAllowed = taaHistoryBlendAllowed(firstFrame, history) &&
                                     taaResolveCanReuseHistory(desc, history);
    if (!taaBlendWeightsConsistentWithReuse(weights, historyBlendAllowed)) {
        return TaaResolveBlendRejectReason::InconsistentWithReuse;
    }
    return TaaResolveBlendRejectReason::None;
}

bool preflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                     TaaResolveBlendRejectReason* reason) {
    const TaaResolveBlendRejectReason reject = classifyTaaResolveBlendReject(desc, history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaResolveBlendRejectReason::None;
}

bool tryPreflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        TaaResolveBlendRejectReason& reason) {
    reason = classifyTaaResolveBlendReject(desc, history);
    return reason == TaaResolveBlendRejectReason::None;
}

bool shouldSkipTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaResolveBlendWeights(desc, history);
}

bool tryComputeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      TaaBlendWeights& outWeights, TaaResolveBlendRejectReason& reason) {
    outWeights = computeTaaResolveBlendWeights(desc, history);
    reason = classifyTaaResolveBlendReject(desc, history);
    return reason == TaaResolveBlendRejectReason::None;
}

bool taaResolveCanReuseHistory(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (!taaHistoryCanReuse(history)) {
        return false;
    }
    if (taaResolveBypassesHistoryGenerationGuard(desc)) {
        return true;
    }
    return !history.isHistoryStale(desc.observed_history_generation);
}

bool taaResolveRequiresVelocity(const TAAParams& params) {
    return params.velocity_rejection > 0.f;
}

bool taaResolveRequiresDepth(const TAAParams& params) {
    return params.depth_rejection > 0.f;
}

f32 computeEffectiveBlend(bool firstFrame, const TAAParams& params) {
    if (firstFrame) {
        return 1.f;
    }
    return clampTaaParams(params).blend_factor;
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
    const TaaResolveSkipReason skip = classifyTaaResolveSkip(desc, history);
    if (reason != nullptr) {
        *reason = skip;
    }
    return taaResolveSkipReasonIsBlocking(skip);
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
    m_stats.first_frame = !history.hasValidHistory();
    const TaaBlendWeights blendWeights = computeTaaResolveBlendWeights(desc, history);
    const bool cpuPath = taaResolveUsesCpuSurfaces(desc);
    if (cpuPath && !resolveCpu(desc, history, params, blendWeights.history > 1e-5f)) {
        m_stats.skipped = true;
        m_stats.first_frame = false;
        m_message = "TAA resolve skipped — CPU resolver rejected the bound surfaces";
        return false;
    }
    history.markResolved();
    history.swap();

    m_stats.resolved = true;
    m_stats.width = desc.width;
    m_stats.height = desc.height;
    m_stats.last_blend = params.blend_factor;
    m_stats.effective_blend = blendWeights.current;
    m_stats.history_blend = blendWeights.history;
    m_stats.history_swapped = true;
    m_stats.has_valid_history = history.hasValidHistory();
    m_stats.accumulated_frames = history.accumulatedFrames();
    m_stats.history_invalidate_generation = history.invalidateGeneration();

    if (cpuPath) {
        m_message = m_stats.first_frame ? "TAA resolve (CPU reference) — first frame"
                                        : "TAA resolve (CPU reference)";
        return true;
    }
#if defined(FUSE_HAS_CUDA)
    m_message = m_stats.first_frame ? "TAA resolve recorded — first frame (CUDA kernel deferred)"
                                    : "TAA resolve recorded (CUDA kernel deferred)";
#else
    m_message = m_stats.first_frame ? "TAA resolve recorded — first frame (CPU stub — no CUDA toolkit)"
                                    : "TAA resolve recorded (CPU stub — no CUDA toolkit)";
#endif

    return true;
}

bool TaaResolve::resolveCpu(const TaaResolveDesc& desc, const TaaHistoryBuffer& history, const TAAParams& params,
                            bool historyReusable) {
    if (!m_cpu.resize(desc.width, desc.height)) {
        return false;
    }
    // The CPU history follows the shared history buffer's validity: a warm-up frame, an
    // invalidation (camera cut, resize) or a stale epoch restarts accumulation.
    const u32 generation = history.invalidateGeneration();
    if (!historyReusable || generation != m_cpuHistoryGeneration) {
        m_cpu.invalidate();
    }

    TaaCpuFrameInputs inputs{};
    inputs.current = desc.cpu.current_frame;
    inputs.velocity = desc.cpu.velocity_buffer;
    inputs.depth = desc.cpu.depth_buffer;
    inputs.width = desc.width;
    inputs.height = desc.height;
    const bool historyUsed = m_cpu.hasHistory();
    if (!m_cpu.resolve(inputs, params, desc.cpu.output)) {
        return false;
    }
    m_cpuHistoryGeneration = generation;
    m_stats.cpu_resolved = true;
    m_stats.cpu_history_used = historyUsed;
    return true;
}

} // namespace fuse::renderer
