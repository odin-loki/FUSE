#include <fuse/renderer/taa/taa_resolve.hpp>

#include <algorithm>

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
    }
    if (taaResolveRequiresDepth(params) && desc.surfaces.depth_buffer == nullptr) {
        return false;
    }
    return true;
}

bool shouldSkipTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                          TaaResolveSkipReason* reason) {
    const TaaResolveSkipReason skip = classifyTaaResolveSkip(desc, history);
    if (reason != nullptr) {
        *reason = skip;
    }
    return taaResolveSkipReasonIsBlocking(skip);
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

void stampObservedHistoryGeneration(TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    if (taaResolveBypassesHistoryGenerationGuard(desc)) {
        desc.observed_history_generation = history.invalidateGeneration();
    }
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
    if (!taaResolveSurfacesComplete(desc)) {
        return TaaResolveSkipReason::MissingSurfaces;
    }

    if (!taaResolveRejectionSurfacesComplete(desc)) {
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

f32 clampEffectiveBlend(f32 effectiveBlend) {
    return clampF32(effectiveBlend, 0.f, 1.f);
}

bool isTaaBlendFactorInRange(f32 blend_factor) {
    return blend_factor >= 0.f && blend_factor <= 1.f;
}

bool taaBlendWeightReusesHistory(f32 effective_blend) {
    return clampEffectiveBlend(effective_blend) < 1.f;
}

f32 computeHistoryBlendWeight(f32 effectiveBlend) {
    return 1.f - clampEffectiveBlend(effectiveBlend);
}

f32 computeHistoryBlendWeight(bool firstFrame, const TAAParams& params) {
    return computeHistoryBlendWeight(computeEffectiveBlend(firstFrame, params));
}

bool taaUsesWarmupBlend(bool first_frame) {
    return first_frame;
}

void accumulateTaaResolveSkipReason(TaaResolveSkipCounts& counts, TaaResolveSkipReason reason) {
    if (!taaResolveSkipReasonIsBlocking(reason)) {
        return;
    }

    ++counts.total;
    switch (reason) {
    case TaaResolveSkipReason::HistoryNotReady:
        ++counts.historyNotReady;
        break;
    case TaaResolveSkipReason::InvalidDimensions:
        ++counts.invalidDimensions;
        break;
    case TaaResolveSkipReason::DimensionMismatch:
        ++counts.dimensionMismatch;
        break;
    case TaaResolveSkipReason::MissingSurfaces:
        ++counts.missingSurfaces;
        break;
    case TaaResolveSkipReason::MissingVelocityBuffer:
        ++counts.missingVelocityBuffer;
        break;
    case TaaResolveSkipReason::MissingDepthBuffer:
        ++counts.missingDepthBuffer;
        break;
    case TaaResolveSkipReason::StaleHistoryGeneration:
        ++counts.staleHistoryGeneration;
        break;
    default:
        break;
    }
}

TaaResolveSkipCounts taaResolveSkipCountsFromReason(TaaResolveSkipReason reason) {
    TaaResolveSkipCounts counts{};
    accumulateTaaResolveSkipReason(counts, reason);
    return counts;
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
    m_stats.first_frame = !history.hasValidHistory();
    const f32 effectiveBlend = computeEffectiveBlend(m_stats.first_frame, params);
    history.markResolved();
    history.swap();

    m_stats.resolved = true;
    m_stats.width = desc.width;
    m_stats.height = desc.height;
    m_stats.last_blend = params.blend_factor;
    m_stats.effective_blend = effectiveBlend;
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
