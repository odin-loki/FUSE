#pragma once

#include <fuse/renderer/taa/taa_history.hpp>
#include <fuse/renderer/taa/taa_types.hpp>

#include <string>

namespace fuse::renderer {

/// True when history is ready to accept resolve accumulation (B5.9 deepen).
bool taaHistoryCanAccumulate(const TaaHistoryBuffer& history);
/// True when observed epoch matches current invalidate generation (B5.9 deepen).
bool taaHistoryIsGenerationCurrent(const TaaHistoryBuffer& history, u32 observed_generation);
/// Classify why resolve would skip — same ordering as `TaaResolve::wouldSkip` (B5.9 deepen).
TaaResolveSkipReason classifyTaaResolveSkip(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when required colour surfaces are bound (B5.9 deepen).
bool taaResolveSurfacesSatisfied(const TaaResolveDesc& desc);
/// True when rejection surfaces are present when `enforce_rejection_surfaces` is set (B5.9 deepen).
bool taaResolveRejectionSurfacesSatisfied(const TaaResolveDesc& desc);
/// True when resolve request passes all preflight guards (inverse of blocking skip).
bool canAttemptTaaResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Stamp observed generation and return whether request can proceed (B5.9 deepen).
bool prepareTaaResolveDesc(TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when resolve dimensions match allocated history buffer size.
bool taaResolveDimensionsMatch(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when `observed_history_generation` guard is disabled for this desc.
bool taaResolveBypassesHistoryGenerationGuard(const TaaResolveDesc& desc);
/// Fill `observed_history_generation` from history when still at the no-guard sentinel.
void stampObservedHistoryGeneration(TaaResolveDesc& desc, const TaaHistoryBuffer& history);

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
