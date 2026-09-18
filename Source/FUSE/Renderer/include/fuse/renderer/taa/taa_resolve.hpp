#pragma once

#include <fuse/renderer/taa/taa_history.hpp>
#include <fuse/renderer/taa/taa_types.hpp>

#include <string>

namespace fuse::renderer {

/// Classify why resolve would skip — same ordering as `TaaResolve::wouldSkip` (B5.9 deepen).
TaaResolveSkipReason classifyTaaResolveSkip(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Stamp observed generation then classify — convenience preflight for resolve callers.
TaaResolveSkipReason preflightTaaResolve(TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when resolve dimensions match allocated history buffer size.
bool taaResolveDimensionsMatch(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when pass viewport dimensions match the resolve request.
bool taaViewportDimensionsMatchPass(u32 passWidth, u32 passHeight, const TaaResolveDesc& desc);
/// True when `observed_history_generation` guard is disabled for this desc.
bool taaResolveBypassesHistoryGenerationGuard(const TaaResolveDesc& desc);
/// True when generation guard is bypassed or `observed_history_generation` is current.
bool isObservedHistoryGenerationCurrent(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Fill `observed_history_generation` from history when still at the no-guard sentinel.
void stampObservedHistoryGeneration(TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Inverse of `TaaResolve::wouldSkip` — true when resolve would proceed.
bool taaResolveCanProceed(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                          TaaResolveSkipReason* reason = nullptr);
/// Blend weight from history warm-up state and clamped params.
f32 computeEffectiveBlendForHistory(const TaaHistoryBuffer& history, const TAAParams& params);

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
