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
    if (desc.surfaces.current_frame == nullptr || desc.surfaces.output == nullptr) {
        return TaaResolveSkipReason::MissingSurfaces;
    }

    if (desc.enforce_rejection_surfaces) {
        const TAAParams params = clampTaaParams(desc.params);
        if (taaResolveRequiresVelocity(params) && desc.surfaces.velocity_buffer == nullptr) {
            return TaaResolveSkipReason::MissingVelocityBuffer;
        }
        if (taaResolveRequiresDepth(params) && desc.surfaces.depth_buffer == nullptr) {
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

const char* taaHistoryReuseRejectReasonLabel(TaaHistoryReuseRejectReason reason) {
    switch (reason) {
    case TaaHistoryReuseRejectReason::None:
        return "none";
    case TaaHistoryReuseRejectReason::HistoryNotReady:
        return "history_not_ready";
    case TaaHistoryReuseRejectReason::HistoryNotWarmed:
        return "history_not_warmed";
    case TaaHistoryReuseRejectReason::StaleGeneration:
        return "stale_generation";
    }
    return "unknown";
}

const char* taaJitterSyncRejectReasonLabel(TaaJitterSyncRejectReason reason) {
    switch (reason) {
    case TaaJitterSyncRejectReason::None:
        return "none";
    case TaaJitterSyncRejectReason::InvalidSequenceLength:
        return "invalid_sequence_length";
    case TaaJitterSyncRejectReason::InvalidViewport:
        return "invalid_viewport";
    case TaaJitterSyncRejectReason::FrameIndexMismatch:
        return "frame_index_mismatch";
    case TaaJitterSyncRejectReason::SlotIndexMismatch:
        return "slot_index_mismatch";
    }
    return "unknown";
}

const char* taaBlendPreflightRejectReasonLabel(TaaBlendPreflightRejectReason reason) {
    switch (reason) {
    case TaaBlendPreflightRejectReason::None:
        return "none";
    case TaaBlendPreflightRejectReason::HistoryNotReady:
        return "history_not_ready";
    case TaaBlendPreflightRejectReason::WarmupRequired:
        return "warmup_required";
    case TaaBlendPreflightRejectReason::StaleGeneration:
        return "stale_generation";
    }
    return "unknown";
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
    const TaaBlendWeights blendWeights = computeTaaBlendWeights(m_stats.first_frame, params);
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
