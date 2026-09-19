#pragma once

#include <fuse/renderer/taa/taa_history.hpp>
#include <fuse/renderer/taa/taa_types.hpp>

#include <string>

namespace fuse::renderer {

/// True when history is ready to accept resolve accumulation (B5.9 deepen).
bool taaHistoryCanAccumulate(const TaaHistoryBuffer& history);
/// True when history is warmed and ready for temporal reuse (B5.9 deepen).
bool taaHistoryCanReuse(const TaaHistoryBuffer& history);
/// True when history can be reused for the observed invalidate epoch (B5.9 deepen).
bool taaHistoryReuseReady(const TaaHistoryBuffer& history, u32 observed_generation);
/// True when observed epoch matches current invalidate generation (B5.9 deepen).
bool taaHistoryIsGenerationCurrent(const TaaHistoryBuffer& history, u32 observed_generation);
/// True when required colour surfaces are bound (B5.9 deepen).
bool taaResolveSurfacesSatisfied(const TaaResolveDesc& desc);
/// True when resolve request passes all preflight guards (inverse of blocking skip).
bool canAttemptTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Stamp observed generation and return whether request can proceed (B5.9 deepen).
bool prepareTaaResolveDesc(TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when this resolve would sample prior history (B5.9 deepen).
bool taaResolveWillReuseHistory(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Classify why resolve would skip — same ordering as `TaaResolve::wouldSkip` (B5.9 deepen).
TaaResolveSkipReason classifyTaaResolveSkip(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Stamp observed generation then classify — convenience preflight for resolve callers.
TaaResolveSkipReason preflightTaaResolve(TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Boolean preflight — true when resolve would bail before history update.
bool shouldSkipTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                          TaaResolveSkipReason* reason = nullptr);
/// True when required current/output surfaces are present.
bool taaResolveSurfacesComplete(const TaaResolveDesc& desc);
/// True when rejection surfaces are present when enforcement is enabled.
bool taaResolveRejectionSurfacesComplete(const TaaResolveDesc& desc);
/// True when required colour surfaces are bound (B5.9 deepen).
bool taaResolveSurfacesSatisfied(const TaaResolveDesc& desc);
/// True when rejection surfaces are present when `enforce_rejection_surfaces` is set (B5.9 deepen).
bool taaResolveRejectionSurfacesSatisfied(const TaaResolveDesc& desc);
/// True when resolve request passes all preflight guards (inverse of blocking skip).
bool canAttemptTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Stamp observed generation and return whether request can proceed (B5.9 deepen).
bool prepareTaaResolveDesc(TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Boolean preflight — true when resolve would bail before history update (B5.9 deepen).
/// True when required current/output surfaces are present (B5.9 deepen).
/// True when rejection surfaces are present when enforcement is enabled (B5.9 deepen).
/// True when resolve dimensions match allocated history buffer size.
bool taaResolveDimensionsMatch(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when pass viewport dimensions match the resolve request.
bool taaViewportDimensionsMatchPass(u32 passWidth, u32 passHeight, const TaaResolveDesc& desc);
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
/// True when history is ready, valid, and passes the generation guard (B5.9 deepen).
bool taaHistoryIsReusable(const TaaHistoryBuffer& history, const TaaResolveDesc& desc);
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
/// True when generation guard is bypassed or `observed_history_generation` is current.
bool isObservedHistoryGenerationCurrent(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Inverse of `TaaResolve::wouldSkip` — true when resolve would proceed.
bool taaResolveCanProceed(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
/// Blend weight from history warm-up state and clamped params.
f32 computeEffectiveBlendForHistory(const TaaHistoryBuffer& history, const TAAParams& params);
/// True when history is ready, valid, and passes the generation guard (B5.9 deepen).
bool taaHistoryIsReusable(const TaaHistoryBuffer& history, const TaaResolveDesc& desc);
/// Preflight resolve blend weights for the next frame without mutating history (B5.9 deepen).
bool preflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                    TaaBlendWeights* outWeights = nullptr);

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
