#pragma once

#include <fuse/renderer/taa/taa_history.hpp>
#include <fuse/renderer/taa/taa_types.hpp>

#include <string>

namespace fuse::renderer {

/// Classify why resolve would skip — same ordering as `TaaResolve::wouldSkip` (B5.9 deepen).
TaaResolveSkipReason classifyTaaResolveSkip(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when resolve dimensions match allocated history buffer size.
bool taaResolveDimensionsMatch(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when `observed_history_generation` guard is disabled for this desc.
bool taaResolveBypassesHistoryGenerationGuard(const TaaResolveDesc& desc);
/// True when the history-generation guard is enabled and the observed epoch matches history.
bool taaResolveHistoryGenerationGuardPasses(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when rejection surfaces must be present for this resolve request.
bool taaResolveRejectionSurfacesRequired(const TaaResolveDesc& desc);
/// True when all required rejection surfaces are bound (or rejection is disabled).
bool taaResolveRejectionSurfacesSatisfied(const TaaResolveDesc& desc);
/// Fill `observed_history_generation` from history when still at the no-guard sentinel.
void stampObservedHistoryGeneration(TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Clamp params and stamp observed generation — pre-resolve sanitization for callers.
void sanitizeTaaResolveDesc(TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Preflight resolve without mutating history — returns true when resolve would proceed.
bool preflightTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                         TaaResolveSkipReason* reason = nullptr);
/// Resolve preflight with mandatory skip-reason output (B5.9 deepen).
bool tryPreflightTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                            TaaResolveSkipReason& reason);
/// Early-out when resolve preflight would skip (B5.9 deepen).
bool shouldSkipTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);

/// CPU/CUDA resolve facade — records resolve intent; kernel deferred (B5.9 stub).
class TaaResolve {
public:
    bool resolve(const TaaResolveDesc& desc, TaaHistoryBuffer& history, void* cudaStream = nullptr);
    /// Predict whether resolve would bail before history update (does not mutate history).
    bool wouldSkip(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                   TaaResolveSkipReason* reason = nullptr) const;
    void resetBookkeeping();

    const TaaResolveStats& lastStats() const { return m_stats; }
    const std::string& lastMessage() const { return m_message; }

private:
    TaaResolveStats m_stats{};
    std::string m_message;
};

} // namespace fuse::renderer

// --- deepen additive from deepen-b59-taa-history-resolve-skip-b406 ---
TaaResolveSkipReason preflightTaaResolve(TaaResolveDesc& desc, const TaaHistoryBuffer& history);

// --- deepen additive from deepen-b59-taa-guards-94f6 ---
bool preflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,

// --- deepen additive from deepen-b59-taa-guards-2b1e ---
bool preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
bool taaResolveBlendPreflightPasses(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);

// --- deepen additive from deepen-b59-taa-guards-94db ---
TaaBlendWeights computeTaaResolveBlendPreflight(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);

// --- deepen additive from deepen-b59-taa-guards-108b ---
bool tryComputeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
bool taaResolveHistoryBlendPreflightPasses(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);

// --- deepen additive from deepen-b59-taa-guards-27d7 ---
bool preflightTaaResolveWithBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                 TaaResolveBlendRejectReason* blendReason = nullptr);
