#include <fuse/renderer/taa/taa_history.hpp>

#include <fuse/renderer/taa/taa_jitter.hpp>

namespace fuse::renderer {

bool taaHistoryBufferDescValid(const TaaHistoryBufferDesc& desc) {
    return desc.width > 0u && desc.height > 0u;
}

bool taaHistoryResizeNeeded(u32 currentWidth, u32 currentHeight, u32 newWidth, u32 newHeight) {
    return currentWidth != newWidth || currentHeight != newHeight;
}

bool taaHistoryCanReuse(const TaaHistoryBuffer& history) {
    return history.isReady() && history.hasValidHistory();
}

bool taaHistoryNeedsWarmup(const TaaHistoryBuffer& history) {
bool taaHistoryWarmupRequired(const TaaHistoryBuffer& history) {
    return history.isReady() && history.needsWarmup();
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return history.isReady() && !history.needsWarmup();

bool taaHistoryReusePreflightPasses(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return taaHistoryReuseAllowed(history, observedGeneration);
    return taaHistoryCanReuse(history);

bool taaHistoryReuseBlocked(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return !taaHistoryReuseAllowed(history, observedGeneration);
}

bool taaHistoryReuseAllowed(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return taaHistoryCanReuse(history) && !history.isHistoryStale(observedGeneration);
}

bool taaHistoryNeedsWarmup(const TaaHistoryBuffer& history) {
    return !history.hasValidHistory();
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return history.isReady() && !taaHistoryNeedsWarmup(history);
    return !taaHistoryNeedsWarmup(history);
const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    if (taaHistoryNeedsWarmup(history)) {
        return TaaHistoryWarmupBlockReason::NeedsWarmup;
    return TaaHistoryWarmupBlockReason::None;

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    return block == TaaHistoryWarmupBlockReason::None;

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryWarmupBlockReason::None;

bool taaHistoryReadyForResolve(const TaaHistoryBuffer& history) {
    return history.isReady();
}


bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return taaHistoryNeedsWarmup(history);

bool taaHistoryIsWarmed(const TaaHistoryBuffer& history) {
    return history.hasValidHistory();

bool taaHistoryIsWarm(const TaaHistoryBuffer& history) {
    return history.isReady() && history.hasValidHistory();
const char* taaHistoryWarmupPhaseLabel(TaaHistoryWarmupPhase phase) {
    switch (phase) {
    case TaaHistoryWarmupPhase::NotReady:
        return "not_ready";
    case TaaHistoryWarmupPhase::Cold:
        return "cold";
    case TaaHistoryWarmupPhase::Warm:
        return "warm";
    return "unknown";

TaaHistoryWarmupPhase classifyTaaHistoryWarmupPhase(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupPhase::NotReady;
    if (!history.hasValidHistory()) {
        return TaaHistoryWarmupPhase::Cold;
    return TaaHistoryWarmupPhase::Warm;

bool taaHistoryWarmupPhaseAllowsReuse(TaaHistoryWarmupPhase phase) {
    return phase == TaaHistoryWarmupPhase::Warm;

bool taaHistoryReadyForResolve(const TaaHistoryBuffer& history) {
    return history.isReady();

u32 taaHistoryWarmupFramesRemaining(const TaaHistoryBuffer& history) {
    return taaHistoryNeedsWarmup(history) ? 1u : 0u;

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return taaHistoryNeedsWarmup(history);

TaaHistoryReuseBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryReuseBlockReason::NotReady;
    }
    if (!history.hasValidHistory()) {
        return TaaHistoryReuseBlockReason::NotWarm;
    }
    return TaaHistoryReuseBlockReason::None;
}

bool preflightTaaHistoryWarmupSatisfied(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason* reason) {
    const TaaHistoryReuseBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryReuseBlockReason::None;
}

bool tryPreflightTaaHistoryWarmupSatisfied(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryReuseBlockReason::None;
}

bool taaHistoryWarmupSatisfied(const TaaHistoryBuffer& history) {
    return preflightTaaHistoryWarmupSatisfied(history);
}

TaaHistoryReuseBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryReuseBlockReason::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryReuseBlockReason::NotWarm;
    }
    return TaaHistoryReuseBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason* reason) {
    const TaaHistoryReuseBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryReuseBlockReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryReuseBlockReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return preflightTaaHistoryWarmup(history);
}

bool shouldSkipTaaHistoryResolve(const TaaHistoryBuffer& history) {
    return !taaHistoryReadyForResolve(history);

bool taaHistoryReuseBlockReasonIsBlocking(TaaHistoryReuseBlockReason reason) {
    return reason != TaaHistoryReuseBlockReason::None;
}

TaaHistoryReuseBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryReuseBlockReason::NotReady;
    }
    if (!history.hasValidHistory()) {
        return TaaHistoryReuseBlockReason::NotWarm;
    }
    return TaaHistoryReuseBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason* reason) {
    const TaaHistoryReuseBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryReuseBlockReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryReuseBlockReason::None;
}

bool tryPreflightTaaHistoryReadyForResolve(const TaaHistoryBuffer& history,
                                           TaaHistoryReuseBlockReason& reason) {
    if (!history.isReady()) {
        reason = TaaHistoryReuseBlockReason::NotReady;
        return false;
    reason = TaaHistoryReuseBlockReason::None;
    return true;

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    if (!history.hasValidHistory()) {
        return TaaHistoryWarmupBlockReason::NeedsWarmup;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return classifyTaaHistoryWarmupBlock(history) == TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryWarmupBlockReason::None;
}

TaaHistoryReuseBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryReuseBlockReason::NotReady;
    }
    if (!history.hasValidHistory()) {
        return TaaHistoryReuseBlockReason::NotWarm;
    }
    return TaaHistoryReuseBlockReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return classifyTaaHistoryWarmupBlock(history) == TaaHistoryReuseBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason* reason) {
    const TaaHistoryReuseBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryReuseBlockReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryReuseBlockReason::None;
}

const char* taaHistoryWarmupRejectReasonLabel(TaaHistoryWarmupRejectReason reason) {
    switch (reason) {
    case TaaHistoryWarmupRejectReason::None:
        return "none";
    case TaaHistoryWarmupRejectReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupRejectReason::Incomplete:
        return "incomplete";
    }
    return "unknown";
}

TaaHistoryWarmupRejectReason classifyTaaHistoryWarmupReject(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupRejectReason::NotReady;
    }
    if (!history.hasValidHistory()) {
        return TaaHistoryWarmupRejectReason::Incomplete;
    }
    return TaaHistoryWarmupRejectReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return classifyTaaHistoryWarmupReject(history) == TaaHistoryWarmupRejectReason::None;
}

bool preflightTaaHistoryWarmupComplete(const TaaHistoryBuffer& history,
                                         TaaHistoryWarmupRejectReason* reason) {
    const TaaHistoryWarmupRejectReason reject = classifyTaaHistoryWarmupReject(history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaHistoryWarmupRejectReason::None;
}

bool tryPreflightTaaHistoryWarmupComplete(const TaaHistoryBuffer& history,
                                          TaaHistoryWarmupRejectReason& reason) {
    reason = classifyTaaHistoryWarmupReject(history);
    return reason == TaaHistoryWarmupRejectReason::None;
}

bool shouldSkipTaaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmupComplete(history);
}

const char* taaHistoryWarmupStateLabel(TaaHistoryWarmupState state) {
    switch (state) {
    case TaaHistoryWarmupState::NotReady:
        return "not_ready";
    case TaaHistoryWarmupState::NeedsWarmup:
        return "needs_warmup";
    case TaaHistoryWarmupState::Complete:
        return "complete";
    }
    return "unknown";
}

TaaHistoryWarmupState classifyTaaHistoryWarmupState(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupState::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryWarmupState::NeedsWarmup;
    }
    return TaaHistoryWarmupState::Complete;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return classifyTaaHistoryWarmupState(history) == TaaHistoryWarmupState::Complete;
}

bool preflightTaaHistoryWarmupComplete(const TaaHistoryBuffer& history, TaaHistoryWarmupState* state) {
    const TaaHistoryWarmupState warmupState = classifyTaaHistoryWarmupState(history);
    if (state != nullptr) {
        *state = warmupState;
    }
    return warmupState == TaaHistoryWarmupState::Complete;
}

bool tryPreflightTaaHistoryWarmupComplete(const TaaHistoryBuffer& history, TaaHistoryWarmupState& state) {
    state = classifyTaaHistoryWarmupState(history);
    return state == TaaHistoryWarmupState::Complete;
}

bool shouldSkipTaaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmupComplete(history);
}

bool preflightTaaHistoryForTemporalBlend(const TaaHistoryBuffer& history, u32 observedGeneration,
                                         TaaHistoryReuseBlockReason* reason) {
    return preflightTaaHistoryReuse(history, observedGeneration, reason);
}

bool tryPreflightTaaHistoryForTemporalBlend(const TaaHistoryBuffer& history, u32 observedGeneration,
                                            TaaHistoryReuseBlockReason& reason) {
    return tryPreflightTaaHistoryReuse(history, observedGeneration, reason);
}

bool shouldSkipTaaHistoryForTemporalBlend(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return shouldSkipTaaHistoryReuse(history, observedGeneration);
}

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryWarmupBlockReason::NeedsWarmup;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return classifyTaaHistoryWarmupBlock(history) == TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryTemporalSample(const TaaHistoryBuffer& history, u32 observedGeneration,
                                       TaaHistoryReuseBlockReason* reason) {
    return preflightTaaHistoryReuse(history, observedGeneration, reason);
}

bool tryPreflightTaaHistoryTemporalSample(const TaaHistoryBuffer& history, u32 observedGeneration,
                                          TaaHistoryReuseBlockReason& reason) {
    return tryPreflightTaaHistoryReuse(history, observedGeneration, reason);
}

bool shouldSkipTaaHistoryTemporalSample(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return shouldSkipTaaHistoryReuse(history, observedGeneration);
}

bool TaaHistoryBuffer::readyForResolve() const {
    return taaHistoryReadyForResolve(*this);

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return taaHistoryReadyForResolve(history) && !taaHistoryNeedsWarmup(history);
}

f32 taaHistoryWarmupProgress(const TaaHistoryBuffer& history) {
    return taaHistoryWarmupComplete(history) ? 1.f : 0.f;
}

bool taaHistoryReuseReady(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return taaHistoryWarmupComplete(history) && preflightTaaHistoryReuse(history, observedGeneration);
}

const char* taaJitterSyncBlockReasonLabel(TaaJitterSyncBlockReason reason) {
    switch (reason) {
    case TaaJitterSyncBlockReason::None:
        return "none";
    case TaaJitterSyncBlockReason::InvalidSequence:
        return "invalid_sequence";
    case TaaJitterSyncBlockReason::InvalidViewport:
        return "invalid_viewport";
    }
    return "unknown";
}

TaaJitterSyncBlockReason classifyTaaJitterSyncBlock(u32 width, u32 height, u32 sequenceLength) {
    if (!TaaJitterLayout::validateSequenceLength(sequenceLength)) {
        return TaaJitterSyncBlockReason::InvalidSequence;
    }
    if (!TaaJitterLayout::validateViewportDimensions(width, height)) {
        return TaaJitterSyncBlockReason::InvalidViewport;
    }
    return TaaJitterSyncBlockReason::None;
}

bool preflightTaaJitterSync(u32 /*frameIndex*/, u32 width, u32 height, u32 sequenceLength,
                            TaaJitterSyncBlockReason* reason) {
    const TaaJitterSyncBlockReason block = classifyTaaJitterSyncBlock(width, height, sequenceLength);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaJitterSyncBlockReason::None;
}

bool taaHistoryIsWarmed(const TaaHistoryBuffer& history) {
    return history.isReady() && history.hasValidHistory();
}

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return history.isReady() && !taaHistoryNeedsWarmup(history);
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason* reason) {
    if (!history.isReady()) {
        if (reason != nullptr) {
            *reason = TaaHistoryReuseBlockReason::NotReady;
        }
        return false;
    }
    if (taaHistoryNeedsWarmup(history)) {
        if (reason != nullptr) {
            *reason = TaaHistoryReuseBlockReason::NotWarm;
        }
        return false;
    }
    if (reason != nullptr) {
        *reason = TaaHistoryReuseBlockReason::None;
    }
    return true;
}

bool taaHistoryReuseBlockReasonIsBlocking(TaaHistoryReuseBlockReason reason) {
    return reason != TaaHistoryReuseBlockReason::None;
}

bool taaHistoryWarmupSatisfied(const TaaHistoryBuffer& history) {
    return history.isReady() && history.hasValidHistory();
}

const char* taaHistoryWarmupPhaseLabel(TaaHistoryWarmupPhase phase) {
    switch (phase) {
    case TaaHistoryWarmupPhase::NotAllocated:
        return "not_allocated";
    case TaaHistoryWarmupPhase::NeedsWarmup:
        return "needs_warmup";
    case TaaHistoryWarmupPhase::Warm:
        return "warm";
    }
    return "unknown";
}

TaaHistoryWarmupPhase classifyTaaHistoryWarmupPhase(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupPhase::NotAllocated;
    }
    if (taaHistoryNeedsWarmup(history)) {
        return TaaHistoryWarmupPhase::NeedsWarmup;
    }
    return TaaHistoryWarmupPhase::Warm;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupPhase* phase) {
    const TaaHistoryWarmupPhase currentPhase = classifyTaaHistoryWarmupPhase(history);
    if (phase != nullptr) {
        *phase = currentPhase;
    }
    return currentPhase == TaaHistoryWarmupPhase::Warm;
}

bool taaHistoryTemporalReuseReady(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return preflightTaaHistoryWarmup(history) && preflightTaaHistoryReuse(history, observedGeneration);
}

const char* taaHistoryWarmupStateLabel(TaaHistoryWarmupState state) {
    switch (state) {
    case TaaHistoryWarmupState::NotReady:
        return "not_ready";
    case TaaHistoryWarmupState::AwaitingFirstResolve:
        return "awaiting_first_resolve";
    case TaaHistoryWarmupState::Complete:
        return "complete";
    }
    return "unknown";
}

TaaHistoryWarmupState classifyTaaHistoryWarmupState(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupState::NotReady;
    }
    if (history.hasValidHistory()) {
        return TaaHistoryWarmupState::Complete;
    }
    return TaaHistoryWarmupState::AwaitingFirstResolve;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return classifyTaaHistoryWarmupState(history) == TaaHistoryWarmupState::Complete;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupState* state) {
    const TaaHistoryWarmupState warmupState = classifyTaaHistoryWarmupState(history);
    if (state != nullptr) {
        *state = warmupState;
    }
    return warmupState == TaaHistoryWarmupState::Complete;
}

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsResolve:
        return "needs_resolve";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryWarmupBlockReason::NeedsResolve;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return classifyTaaHistoryWarmupBlock(history) == TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return !taaHistoryNeedsWarmup(history);
}

bool tryCanBeginTemporalReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
                              TaaHistoryReuseBlockReason* reason) {
    return preflightTaaHistoryReuse(history, observedGeneration, reason);
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return history.isReady() && !taaHistoryNeedsWarmup(history);
}

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    if (taaHistoryNeedsWarmup(history)) {
        return TaaHistoryWarmupBlockReason::NeedsWarmup;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

const char* taaHistoryWarmupRejectReasonLabel(TaaHistoryWarmupRejectReason reason) {
    switch (reason) {
    case TaaHistoryWarmupRejectReason::None:
        return "none";
    case TaaHistoryWarmupRejectReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupRejectReason::AlreadyWarm:
        return "already_warm";
    }
    return "unknown";
}

TaaHistoryWarmupRejectReason classifyTaaHistoryWarmupReject(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupRejectReason::NotReady;
    }
    if (history.hasValidHistory()) {
        return TaaHistoryWarmupRejectReason::AlreadyWarm;
    }
    return TaaHistoryWarmupRejectReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return history.isReady() && history.hasValidHistory();
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupRejectReason* reason) {
    const TaaHistoryWarmupRejectReason reject = classifyTaaHistoryWarmupReject(history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaHistoryWarmupRejectReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupRejectReason& reason) {
    reason = classifyTaaHistoryWarmupReject(history);
    return reason == TaaHistoryWarmupRejectReason::None;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmup(history);
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return !taaHistoryNeedsWarmup(history);
}

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryWarmupBlockReason::NeedsWarmup;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return preflightTaaHistoryWarmup(history);
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryWarmupBlockReason::None;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmup(history);
}

const char* taaHistoryWarmupRejectReasonLabel(TaaHistoryWarmupRejectReason reason) {
    switch (reason) {
    case TaaHistoryWarmupRejectReason::None:
        return "none";
    case TaaHistoryWarmupRejectReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupRejectReason::AlreadyWarm:
        return "already_warm";
    }
    return "unknown";
}

TaaHistoryWarmupRejectReason classifyTaaHistoryWarmupReject(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupRejectReason::NotReady;
    }
    if (!history.needsWarmup()) {
        return TaaHistoryWarmupRejectReason::AlreadyWarm;
    }
    return TaaHistoryWarmupRejectReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupRejectReason* reason) {
    const TaaHistoryWarmupRejectReason reject = classifyTaaHistoryWarmupReject(history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaHistoryWarmupRejectReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupRejectReason& reason) {
    reason = classifyTaaHistoryWarmupReject(history);
    return reason == TaaHistoryWarmupRejectReason::None;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmup(history);
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return history.isReady() && !history.needsWarmup();
}

const char* taaHistoryWarmupStateLabel(TaaHistoryWarmupState state) {
    switch (state) {
    case TaaHistoryWarmupState::NotReady:
        return "not_ready";
    case TaaHistoryWarmupState::NeedsWarmup:
        return "needs_warmup";
    case TaaHistoryWarmupState::Ready:
        return "ready";
    }
    return "unknown";
}

TaaHistoryWarmupState classifyTaaHistoryWarmupState(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupState::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryWarmupState::NeedsWarmup;
    }
    return TaaHistoryWarmupState::Ready;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return classifyTaaHistoryWarmupState(history) == TaaHistoryWarmupState::Ready;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupState* state) {
    const TaaHistoryWarmupState warmupState = classifyTaaHistoryWarmupState(history);
    if (state != nullptr) {
        *state = warmupState;
    }
    return warmupState == TaaHistoryWarmupState::Ready;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupState& state) {
    state = classifyTaaHistoryWarmupState(history);
    return state == TaaHistoryWarmupState::Ready;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmup(history);
}

TaaHistoryReuseBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryReuseBlockReason::NotReady;
    }
    if (!history.hasValidHistory()) {
        return TaaHistoryReuseBlockReason::NotWarm;
    }
    return TaaHistoryReuseBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason* reason) {
    const TaaHistoryReuseBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryReuseBlockReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryReuseBlockReason::None;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmup(history);
}

const char* taaHistoryWarmupRejectReasonLabel(TaaHistoryWarmupRejectReason reason) {
    switch (reason) {
    case TaaHistoryWarmupRejectReason::None:
        return "none";
    case TaaHistoryWarmupRejectReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupRejectReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";
}

TaaHistoryWarmupRejectReason classifyTaaHistoryWarmupReject(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupRejectReason::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryWarmupRejectReason::NeedsWarmup;
    }
    return TaaHistoryWarmupRejectReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return classifyTaaHistoryWarmupReject(history) == TaaHistoryWarmupRejectReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupRejectReason* reason) {
    const TaaHistoryWarmupRejectReason reject = classifyTaaHistoryWarmupReject(history);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaHistoryWarmupRejectReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupRejectReason& reason) {
    reason = classifyTaaHistoryWarmupReject(history);
    return reason == TaaHistoryWarmupRejectReason::None;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmup(history);
}

bool taaHistoryReuseBlockedByWarmup(const TaaHistoryBuffer& history) {
    return history.isReady() && history.needsWarmup();
}

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    if (taaHistoryNeedsWarmup(history)) {
        return TaaHistoryWarmupBlockReason::NeedsWarmup;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return classifyTaaHistoryWarmupBlock(history) == TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryWarmupBlockReason::None;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmup(history);
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return taaHistoryReadyForResolve(history) && !taaHistoryNeedsWarmup(history);
}

TaaHistoryWarmupPreflight preflightTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    TaaHistoryWarmupPreflight preflight{};
    preflight.history_ready = taaHistoryReadyForResolve(history);
    preflight.needs_warmup = taaHistoryNeedsWarmup(history);
    preflight.warmup_frames_remaining = taaHistoryWarmupFramesRemaining(history);
    return preflight;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupPreflight& out) {
    out = preflightTaaHistoryWarmup(history);
    return out.isWarmed();
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !taaHistoryWarmupComplete(history);
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return history.isReady() && !taaHistoryNeedsWarmup(history);
}

const char* taaHistoryWarmupStateLabel(TaaHistoryWarmupState state) {
    switch (state) {
    case TaaHistoryWarmupState::NotReady:
        return "not_ready";
    case TaaHistoryWarmupState::NeedsWarmup:
        return "needs_warmup";
    case TaaHistoryWarmupState::Complete:
        return "complete";
    }
    return "unknown";
}

TaaHistoryWarmupState classifyTaaHistoryWarmupState(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupState::NotReady;
    }
    if (taaHistoryNeedsWarmup(history)) {
        return TaaHistoryWarmupState::NeedsWarmup;
    }
    return TaaHistoryWarmupState::Complete;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupState* state) {
    const TaaHistoryWarmupState warmupState = classifyTaaHistoryWarmupState(history);
    if (state != nullptr) {
        *state = warmupState;
    }
    return warmupState == TaaHistoryWarmupState::Complete;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupState& state) {
    state = classifyTaaHistoryWarmupState(history);
    return state == TaaHistoryWarmupState::Complete;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmup(history);
}

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryWarmupBlockReason::NeedsWarmup;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return classifyTaaHistoryWarmupBlock(history) == TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryWarmupBlockReason::None;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmup(history);
}

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryWarmupBlockReason::NeedsWarmup;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return classifyTaaHistoryWarmupBlock(history) == TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryWarmupBlockReason::None;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmup(history);
}

bool isTaaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return !taaHistoryNeedsWarmup(history);
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return taaHistoryNeedsWarmup(history);
}

TaaHistoryWarmupPreflight preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, u32 observedGeneration) {
    TaaHistoryWarmupPreflight preflight{};
    preflight.framesRemaining = taaHistoryWarmupFramesRemaining(history);
    preflight.reuseBlock = classifyTaaHistoryReuseBlock(history, observedGeneration);
    return preflight;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return !taaHistoryNeedsWarmup(history);
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason* reason) {
    if (!history.isReady()) {
        if (reason != nullptr) {
            *reason = TaaHistoryReuseBlockReason::NotReady;
        }
        return false;
    }
    if (history.needsWarmup()) {
        if (reason != nullptr) {
            *reason = TaaHistoryReuseBlockReason::NotWarm;
        }
        return false;
    }
    if (reason != nullptr) {
        *reason = TaaHistoryReuseBlockReason::None;
    }
    return true;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason& reason) {
    return preflightTaaHistoryWarmup(history, &reason);
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmup(history);
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return !taaHistoryNeedsWarmup(history);
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return taaHistoryNeedsWarmup(history);
}

bool taaHistoryWarmupReady(const TaaHistoryBuffer& history) {
    return history.isReady() && taaHistoryWarmupComplete(history);
}

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryWarmupBlockReason::NeedsWarmup;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason& reason) {
    reason = classifyTaaHistoryWarmupBlock(history);
    return reason == TaaHistoryWarmupBlockReason::None;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryWarmup(history);
}

bool TaaHistoryBuffer::warmupComplete() const {
    return taaHistoryWarmupComplete(*this);
}

bool preflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
                              TaaHistoryReuseBlockReason* reason) {
    const TaaHistoryReuseBlockReason block = classifyTaaHistoryReuseBlock(history, observedGeneration);
    if (reason != nullptr) {
        *reason = block;
    return block == TaaHistoryReuseBlockReason::None;

bool tryPreflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
    reason = classifyTaaHistoryReuseBlock(history, observedGeneration);
    return reason == TaaHistoryReuseBlockReason::None;

bool shouldSkipTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return !preflightTaaHistoryReuse(history, observedGeneration);

bool wouldSkipTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return shouldSkipTaaHistoryReuse(history, observedGeneration);
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason& reason) {
    if (!history.isReady()) {
        reason = TaaHistoryReuseBlockReason::NotReady;
        return false;
    }
    if (!history.hasValidHistory()) {
        reason = TaaHistoryReuseBlockReason::NotWarm;
        return false;
    }
    reason = TaaHistoryReuseBlockReason::None;
    return true;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    TaaHistoryReuseBlockReason reason = TaaHistoryReuseBlockReason::None;
    return !tryPreflightTaaHistoryWarmup(history, reason);
}

bool taaHistoryReuseReady(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return preflightTaaHistoryReuse(history, observedGeneration);
bool TaaHistoryWarmupPreflight::readyForResolve() const {
    return buffer_ready;

bool TaaHistoryWarmupPreflight::warmupComplete() const {
    return buffer_ready && !needs_warmup;

bool TaaHistoryReusePreflight::canReuseHistory() const {
    return reuse_allowed;

TaaHistoryWarmupPreflight preflightTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    TaaHistoryWarmupPreflight preflight{};
    preflight.buffer_ready = history.isReady();
    preflight.needs_warmup = history.needsWarmup();
    preflight.can_reuse = taaHistoryCanReuse(history);
    preflight.invalidate_generation = history.invalidateGeneration();
    preflight.accumulated_frames = history.accumulatedFrames();
    return preflight;

TaaHistoryReusePreflight preflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration) {
    TaaHistoryReusePreflight preflight{};
    preflight.warmup = preflightTaaHistoryWarmup(history);
    preflight.observed_generation = observedGeneration;
    preflight.generation_matches = history.generationMatches(observedGeneration);
    preflight.reuse_allowed = taaHistoryReuseAllowed(history, observedGeneration);

TaaHistoryReusePreflight preflightTaaHistoryReuseForDesc(const TaaHistoryBuffer& history,
                                                         const TaaResolveDesc& desc) {
    if (desc.observed_history_generation == kTaaResolveNoHistoryGeneration) {
        TaaHistoryReusePreflight preflight = preflightTaaHistoryReuse(history, history.invalidateGeneration());
        preflight.reuse_allowed = taaHistoryCanReuse(history);
    return preflightTaaHistoryReuse(history, desc.observed_history_generation);
    return !history.isReady() || history.needsWarmup();

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return history.warmupComplete();

bool taaResolveWouldBeFirstFrame(const TaaHistoryBuffer& history) {

bool TaaHistoryBuffer::warmupComplete() const {
    return m_ready && m_validity.hasValidHistory;
bool preflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return taaHistoryReuseAllowed(history, observedGeneration);
    return history.isReady() && history.needsWarmup();

    return history.isReady() && history.hasValidHistory();

TaaHistoryReuseRejectReason classifyTaaHistoryReuseReject(const TaaHistoryBuffer& history, u32 observedGeneration) {
        return TaaHistoryReuseRejectReason::HistoryNotReady;
    if (!history.hasValidHistory()) {
        return TaaHistoryReuseRejectReason::HistoryNotWarmed;
    if (history.isHistoryStale(observedGeneration)) {
        return TaaHistoryReuseRejectReason::StaleGeneration;
    return TaaHistoryReuseRejectReason::None;

bool taaHistoryReusePreflight(const TaaHistoryBuffer& history, u32 observedGeneration,
                               TaaHistoryReuseRejectReason* reason) {
    const TaaHistoryReuseRejectReason reject = classifyTaaHistoryReuseReject(history, observedGeneration);
        *reason = reject;
    return reject == TaaHistoryReuseRejectReason::None;
bool taaHistoryIsWarmupFrame(const TaaHistoryBuffer& history) {
    return history.isReady() && !history.hasValidHistory();
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history) {

}

bool canPreflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return preflightTaaHistoryReuse(history, observedGeneration);
}

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryWarmupBlockReason::NeedsWarmup;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

bool canPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return preflightTaaHistoryWarmup(history);
}

bool taaHistoryTemporalBlendReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return taaResolveCanReuseHistory(desc, history) && taaResolveAppliesHistoryBlend(desc, history);
}

bool preflightTaaHistoryReuseForDesc(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                     TaaHistoryReuseBlockReason* reason) {
    if (desc.observed_history_generation == kTaaResolveNoHistoryGeneration) {
        if (!history.isReady()) {
            if (reason != nullptr) {
                *reason = TaaHistoryReuseBlockReason::NotReady;
            }
            return false;
        }
        if (!history.hasValidHistory()) {
            if (reason != nullptr) {
                *reason = TaaHistoryReuseBlockReason::NotWarm;
            }
            return false;
        }
        if (reason != nullptr) {
            *reason = TaaHistoryReuseBlockReason::None;
        }
        return true;
    }
    return preflightTaaHistoryReuse(history, desc.observed_history_generation, reason);
}

bool tryPreflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
                                 TaaHistoryReuseBlockReason& outReason) {
    outReason = classifyTaaHistoryReuseBlock(history, observedGeneration);
    return outReason == TaaHistoryReuseBlockReason::None;
}

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryWarmupBlockReason::NeedsWarmup;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason& outReason) {
    outReason = classifyTaaHistoryWarmupBlock(history);
    return outReason == TaaHistoryWarmupBlockReason::None;
}

bool shouldSkipTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return classifyTaaHistoryReuseBlock(history, observedGeneration) != TaaHistoryReuseBlockReason::None;
}

bool tryPreflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
                                 TaaHistoryReuseBlockReason& outReason) {
    outReason = classifyTaaHistoryReuseBlock(history, observedGeneration);
    return outReason == TaaHistoryReuseBlockReason::None;
}

bool canSampleHistoryForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return taaHistoryReadyForResolve(history) && taaResolveCanReuseHistory(desc, history);
}

bool tryPreflightTaaHistoryReuseForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                           TaaHistoryReuseBlockReason& outReason) {
    if (!taaHistoryReadyForResolve(history)) {
        outReason = TaaHistoryReuseBlockReason::NotReady;
        return false;
    }
    if (taaResolveHistoryGenerationIsStale(desc, history)) {
        outReason = TaaHistoryReuseBlockReason::StaleGeneration;
        return false;
    }
    if (!history.hasValidHistory()) {
        outReason = TaaHistoryReuseBlockReason::NotWarm;
        return false;
    }
    outReason = TaaHistoryReuseBlockReason::None;
    return true;
}

bool wouldInvalidateHistoryIfStale(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return history.isHistoryStale(observedGeneration);
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return !taaHistoryNeedsWarmup(history);
}

bool canPreflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return preflightTaaHistoryReuse(history, observedGeneration);
}

const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason) {
    switch (reason) {
    case TaaHistoryWarmupBlockReason::None:
        return "none";
    case TaaHistoryWarmupBlockReason::NotReady:
        return "not_ready";
    case TaaHistoryWarmupBlockReason::NeedsWarmup:
        return "needs_warmup";
    }
    return "unknown";
}

TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryWarmupBlockReason::NotReady;
    }
    if (history.needsWarmup()) {
        return TaaHistoryWarmupBlockReason::NeedsWarmup;
    }
    return TaaHistoryWarmupBlockReason::None;
}

bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {
    const TaaHistoryWarmupBlockReason block = classifyTaaHistoryWarmupBlock(history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryWarmupBlockReason::None;
}

bool canPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return preflightTaaHistoryWarmup(history);
}

bool taaHistoryTemporalBlendReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return taaResolveCanReuseHistory(desc, history) && taaResolveAppliesHistoryBlend(desc, history);
}

bool wouldSkipTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return !preflightTaaHistoryReuse(history, observedGeneration);
}

bool tryPreflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
                                 TaaHistoryReuseBlockReason& outReason) {
    outReason = classifyTaaHistoryReuseBlock(history, observedGeneration);
    return outReason == TaaHistoryReuseBlockReason::None;
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return history.isReady() && history.hasValidHistory();
}

bool TaaHistoryBuffer::isWarm() const {
    return taaHistoryIsWarm(*this);
}

TaaHistoryReuseBlockReason classifyTaaHistoryReuseBlockForResolve(const TaaResolveDesc& desc,
                                                                 const TaaHistoryBuffer& history) {
    if (!history.isReady()) {
        return TaaHistoryReuseBlockReason::NotReady;
    }
    if (!history.hasValidHistory()) {
        return TaaHistoryReuseBlockReason::NotWarm;
    }
    if (desc.observed_history_generation != kTaaResolveNoHistoryGeneration &&
        history.isHistoryStale(desc.observed_history_generation)) {
        return TaaHistoryReuseBlockReason::StaleGeneration;
    }
    return TaaHistoryReuseBlockReason::None;
}

bool preflightTaaHistoryReuseForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        TaaHistoryReuseBlockReason* reason) {
    const TaaHistoryReuseBlockReason block = classifyTaaHistoryReuseBlockForResolve(desc, history);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryReuseBlockReason::None;
}

bool tryPreflightTaaHistoryReuseForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                           TaaHistoryReuseBlockReason& reason) {
    reason = classifyTaaHistoryReuseBlockForResolve(desc, history);
    return reason == TaaHistoryReuseBlockReason::None;
}

bool shouldSkipTaaHistoryReuseForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history) {
    return !preflightTaaHistoryReuseForResolve(desc, history);
}

bool TaaHistoryBuffer::canReuseHistory() const {
    return taaHistoryCanReuse(*this);
}

bool TaaHistoryBuffer::warmupComplete() const {
    return taaHistoryWarmupComplete(*this);
}

u32 TaaHistoryBuffer::warmupFramesRemaining() const {
    return taaHistoryWarmupFramesRemaining(*this);
}

bool TaaHistoryBuffer::warmupComplete() const {
    return taaHistoryWarmupComplete(*this);
}

bool TaaHistoryBuffer::reuseReady(u32 observedGeneration) const {
    return taaHistoryReuseReady(*this, observedGeneration);
bool TaaHistoryBuffer::hasReadableHistory() const {
    return m_ready && m_validity.hasValidHistory && read().isValid();

const char* taaHistoryReuseRejectReasonLabel(TaaHistoryReuseRejectReason reason) {
    switch (reason) {
    case TaaHistoryReuseRejectReason::None:
        return "none";
    case TaaHistoryReuseRejectReason::NotReady:
        return "not_ready";
    case TaaHistoryReuseRejectReason::NeedsWarmup:
        return "needs_warmup";
    case TaaHistoryReuseRejectReason::StaleGeneration:
        return "stale_generation";
    return "unknown";

TaaHistoryReuseRejectReason classifyTaaHistoryReuseReject(const TaaHistoryBuffer& history, u32 observedGeneration) {
    if (!history.isReady()) {
        return TaaHistoryReuseRejectReason::NotReady;
    if (!history.hasValidHistory()) {
        return TaaHistoryReuseRejectReason::NeedsWarmup;
    if (observedGeneration != kTaaResolveNoHistoryGeneration && history.isHistoryStale(observedGeneration)) {
        return TaaHistoryReuseRejectReason::StaleGeneration;
    return TaaHistoryReuseRejectReason::None;

bool tryTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration, TaaHistoryReuseRejectReason* reason) {
    const TaaHistoryReuseRejectReason reject = classifyTaaHistoryReuseReject(history, observedGeneration);
    if (reason != nullptr) {
        *reason = reject;
    return reject == TaaHistoryReuseRejectReason::None;
bool TaaHistoryBuffer::temporalReuseAllowed(u32 observedGeneration) const {
    return taaHistoryReuseAllowed(*this, observedGeneration);

bool TaaHistoryBuffer::warmupComplete() const {
    return taaHistoryWarmupComplete(*this);
bool TaaHistoryBuffer::isWarmupFrame() const {
    return taaHistoryIsWarmupFrame(*this);

bool TaaHistoryBuffer::preflightReuse(u32 observedGeneration) const {
    return preflightTaaHistoryReuse(*this, observedGeneration);
}

bool TaaHistoryBuffer::warmupComplete() const {
    return taaHistoryWarmupComplete(*this);
}

bool TaaHistoryBuffer::shouldSkipReuse(u32 observedGeneration) const {
    return shouldSkipTaaHistoryReuse(*this, observedGeneration);
}

bool TaaHistoryBuffer::warmupComplete() const {
    return taaHistoryWarmupComplete(*this);
}

bool TaaHistoryBuffer::readyForResolve() const {
    return taaHistoryReadyForResolve(*this);
}

bool TaaHistoryBuffer::warmupComplete() const {
    return taaHistoryWarmupComplete(*this);
}

TaaHistoryWarmupState TaaHistoryBuffer::warmupState() const {
    return classifyTaaHistoryWarmupState(*this);
}

bool TaaHistoryBuffer::warmupComplete() const {
    return taaHistoryWarmupComplete(*this);
}

bool TaaHistoryBuffer::init(ResourceManager& resources, const TaaHistoryBufferDesc& desc) {
    const u32 preservedGeneration = m_validity.invalidateGeneration;
    destroy();
    m_resources = &resources;
    m_desc = desc;
    m_activeIndex = 0u;
    m_validity = {};
    m_validity.invalidateGeneration = preservedGeneration;

    if (!taaHistoryBufferDescValid(m_desc)) {
        return false;
    }

    TextureDesc textureDesc{};
    textureDesc.width = m_desc.width;
    textureDesc.height = m_desc.height;
    textureDesc.format = GpuFormat::R16G16B16A16Sfloat;
    textureDesc.usage = static_cast<ImageUsage>(
        static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::Storage) |
        static_cast<u32>(ImageUsage::TransferDst));
    textureDesc.cudaInterop = true;
    textureDesc.name = "taa_history_a";
    m_buffers[0] = m_resources->createTexture(textureDesc);

    textureDesc.name = "taa_history_b";
    m_buffers[1] = m_resources->createTexture(textureDesc);

    m_ready = m_buffers[0].isValid() && m_buffers[1].isValid();
    return m_ready;
}

void TaaHistoryBuffer::resize(u32 width, u32 height) {
    if (!taaHistoryResizeNeeded(m_desc.width, m_desc.height, width, height)) {
        return;
    }

    invalidateHistory();

    if (m_resources == nullptr) {
        m_desc.width = width;
        m_desc.height = height;
        return;
    }

    TaaHistoryBufferDesc resized{};
    resized.width = width;
    resized.height = height;
    init(*m_resources, resized);
}

void TaaHistoryBuffer::destroy() {
    releaseTargets();
    m_resources = nullptr;
    m_desc = {};
    m_validity = {};
    m_activeIndex = 0u;
    m_ready = false;
}

TextureHandle TaaHistoryBuffer::read() const {
    return m_buffers[m_activeIndex];
}

TextureHandle TaaHistoryBuffer::write() const {
    return m_buffers[(m_activeIndex + 1u) % 2u];
}

void TaaHistoryBuffer::swap() {
    m_activeIndex = (m_activeIndex + 1u) % 2u;
}

bool TaaHistoryBuffer::isHistoryStale(u32 observedGeneration) const {
    return observedGeneration != m_validity.invalidateGeneration;
}

bool TaaHistoryBuffer::generationMatches(u32 observedGeneration) const {
    return !isHistoryStale(observedGeneration);
}

bool TaaHistoryBuffer::canAcceptResolveAt(u32 width, u32 height) const {
    return isReady() && matchesDimensions(width, height);
bool TaaHistoryBuffer::isGenerationCurrent(u32 observedGeneration) const {
    return observedGeneration == m_validity.invalidateGeneration;
}

bool TaaHistoryBuffer::matchesDimensions(u32 width, u32 height) const {
    return m_desc.width == width && m_desc.height == height;
}

bool canReadHistoryForResolve(const TaaHistoryBuffer& history) {
    return history.canReadForResolve();
}

bool historyAwaitingWarmup(const TaaHistoryBuffer& history) {
    return history.isReady() && history.needsWarmup();
}

void TaaHistoryBuffer::invalidateHistory() {
    m_validity.hasValidHistory = false;
    m_validity.accumulatedFrames = 0u;
    ++m_validity.invalidateGeneration;
}

bool TaaHistoryBuffer::invalidateHistoryIfStale(u32 observedGeneration) {
    if (!isHistoryStale(observedGeneration)) {
        return false;
    }
    invalidateHistory();
    return true;
}

void TaaHistoryBuffer::markResolved() {
    m_validity.hasValidHistory = true;
    ++m_validity.accumulatedFrames;
}

void TaaHistoryBuffer::releaseTargets() {
    if (m_resources == nullptr) {
        m_buffers[0] = TextureHandle{};
        m_buffers[1] = TextureHandle{};
        return;
    }

    if (m_buffers[0].isValid()) {
        m_resources->destroyTexture(m_buffers[0]);
    }
    if (m_buffers[1].isValid()) {
        m_resources->destroyTexture(m_buffers[1]);
    }
    m_buffers[0] = TextureHandle{};
    m_buffers[1] = TextureHandle{};
}

} // namespace fuse::renderer
