#include <fuse/renderer/taa/taa_pass.hpp>

#include <fuse/renderer/command_buffer.hpp>

namespace fuse::renderer {
namespace {

struct TaaPassUserData {};

TaaPassUserData g_taaPassUserData{};
RGTextureAccess g_taaPassAccesses[2]{};
bool g_taaPassRegistered = false;

void executeTaaPass(void* commandBuffer, void* /*userData*/) {
    auto* recorder = static_cast<CommandBufferRecorder*>(commandBuffer);
    if (recorder == nullptr) {
        return;
    }
    recorder->beginPass("taa_resolve");
    recorder->endPass();
}

} // namespace

std::unique_ptr<TaaPass> TaaPass::create(const TaaPassDesc& desc) {
    return std::unique_ptr<TaaPass>(new TaaPass(desc));
}

TaaPass::TaaPass(const TaaPassDesc& desc) : m_desc(desc), m_jitter(desc.jitter) {}

bool TaaPass::init(ResourceManager& resources) {
    destroy();

    TaaHistoryBufferDesc historyDesc{};
    historyDesc.width = m_desc.width;
    historyDesc.height = m_desc.height;
    if (!m_history.init(resources, historyDesc)) {
        m_stats.message = "TAA pass failed to allocate history buffers";
        m_stats.ready = false;
        return false;
    }

    m_stats.ready = true;
    m_stats.message = "TAA pass scaffold ready";
    return true;
}

void TaaPass::destroy() {
    m_history.destroy();
    m_jitter.reset();
    m_resolve.resetBookkeeping();
    m_stats.framesResolved = 0;
    m_stats.lastJitterNdc = {};
    m_stats.ready = false;
    m_stats.message.clear();
}

fuse::math::Vec2 TaaPass::currentJitterNdc() const {
    return m_jitter.currentNdcOffset(m_desc.width, m_desc.height);
}

bool TaaPass::currentJitterNdcIfReady(fuse::math::Vec2& out) const {
    return m_jitter.currentNdcOffsetIfReady(m_desc.width, m_desc.height, out);
}

bool TaaPass::tryCurrentJitterNdcIfReady(fuse::math::Vec2& out, TaaJitterGuardRejectReason& reason) const {
    return m_jitter.tryCurrentNdcOffsetIfReady(m_desc.width, m_desc.height, out, reason);
bool TaaPass::currentJitterPixelOffsetIfReady(fuse::math::Vec2& out) const {
    return m_jitter.currentPixelOffsetIfReady(out);
}

void TaaPass::advanceJitter() {
    m_jitter.advance();
    m_stats.lastJitterNdc = currentJitterNdc();
}

void TaaPass::syncJitterToFrameIndex(u32 frameIndex) {
    m_jitter.syncToFrameIndex(frameIndex);
    m_stats.lastJitterNdc = currentJitterNdc();
}

bool TaaPass::syncJitterToFrameIndexIfReady(u32 frameIndex) {
    if (!m_jitter.syncToFrameIndexIfReady(frameIndex)) {
        return false;
    }
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;
}

bool TaaPass::canSyncJitterToFrameIndex(u32 frameIndex) const {
    return m_jitter.preflightSyncToFrameIndex(frameIndex);
}

TaaJitterSyncRejectReason TaaPass::classifyJitterSyncReject(u32 frameIndex) const {
    return m_jitter.classifySyncReject(frameIndex);

bool TaaPass::preflightJitterSync(u32 frameIndex, TaaJitterSyncRejectReason* reason) const {
    return m_jitter.preflightSyncToFrameIndex(frameIndex, reason);
bool TaaPass::preflightJitterSync(u32 frameIndex, TaaJitterSyncBlockReason* reason) const {
    return m_jitter.preflightSync(frameIndex, reason);

bool TaaPass::advanceJitterIfAlignedToFrameIndex(u32 frameIndex) {
    if (!m_jitter.advanceIfAlignedToFrameIndex(frameIndex)) {
        return false;
bool TaaPass::syncJitterToFrameIndexIfViewportReady(u32 frameIndex) {
    if (!m_jitter.syncToFrameIndexIfViewportReady(frameIndex, m_desc.width, m_desc.height)) {
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;

bool TaaPass::trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncBlockReason& outReason) {
    if (!m_jitter.trySyncToFrameIndexIfViewportReady(frameIndex, m_desc.width, m_desc.height, outReason)) {
bool TaaPass::trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncRejectReason* reason) {
    if (!m_jitter.syncToFrameIndexIfReady(frameIndex, reason)) {

bool TaaPass::needsJitterSyncToFrameIndex(u32 frameIndex) const {
    return m_jitter.needsSyncToFrameIndex(frameIndex);

bool TaaPass::syncJitterToFrameIndexIfMisaligned(u32 frameIndex) {
    if (!m_jitter.syncToFrameIndexIfMisaligned(frameIndex)) {
bool TaaPass::trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterGuardRejectReason& reason) {
    if (!m_jitter.trySyncToFrameIndexIfReady(frameIndex, reason)) {

bool TaaPass::syncJitterToFrameIndexAndProduceNdcIfReady(u32 frameIndex, fuse::math::Vec2& out) {
    if (!m_jitter.syncToFrameIndexAndProduceNdcIfReady(frameIndex, m_desc.width, m_desc.height, out)) {
    m_stats.lastJitterNdc = out;
bool TaaPass::trySyncJitterToFrameIndex(u32 frameIndex, TaaJitterGuardRejectReason& reason) {
    if (!tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason)) {
bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);

    if (!tryPreflightJitterSync(frameIndex, reason)) {
    return syncJitterToFrameIndexIfReady(frameIndex);
    if (!m_jitter.syncToFrameIndexIfReady(frameIndex)) {
        reason = classifyTaaJitterSyncReject(m_jitter.sequenceLength());
    reason = TaaJitterGuardRejectReason::None;
TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());



}

bool TaaPass::jitterAlignedToFrameIndex(u32 frameIndex) const {
    return m_jitter.isAlignedToFrameIndex(frameIndex);
bool TaaPass::isJitterSyncedTo(u32 frameIndex) const {
bool TaaPass::isJitterSyncedToFrameIndex(u32 frameIndex) const {
    return m_jitter.isSyncedToFrameIndex(frameIndex);
}

TaaJitterSyncRejectReason TaaPass::classifyJitterSyncReject(u32 frameIndex) const {
    return m_jitter.classifySyncReject(frameIndex);
}

bool TaaPass::preflightJitterSync(u32 frameIndex, TaaJitterSyncRejectReason* reason) const {
    const TaaJitterSyncRejectReason reject = m_jitter.classifySyncReject(frameIndex);
    if (reason != nullptr) {
        *reason = reject;
    }
    return reject == TaaJitterSyncRejectReason::None;
}

bool TaaPass::needsJitterResync(u32 frameIndex) const {
    return m_jitter.needsResyncToFrameIndex(frameIndex);
}

bool TaaPass::preflightJitterSync(u32 frameIndex, TaaJitterSyncRejectReason* reason) const {
    return preflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::preflightJitterAlignment(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return m_jitter.preflightAlignmentToFrameIndex(frameIndex, reason);
}

bool TaaPass::canAdvanceJitter() const {
    return m_jitter.canAdvance();
}

bool TaaPass::canSyncJitterToFrameIndex(u32 frameIndex) const {
    return m_jitter.canSyncToFrameIndex(frameIndex);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject(u32 frameIndex) const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::ndcOffsetForFrameIndexIfReady(u32 frameIndex, fuse::math::Vec2& out) const {
    return TaaJitterLayout::ndcOffsetForFrameIndexIfReady(frameIndex, m_desc.width, m_desc.height,
                                                        m_jitter.sequenceLength(), out);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

void TaaPass::invalidateHistory() {
    m_history.invalidateHistory();
    m_resolve.resetBookkeeping();
}

bool TaaPass::invalidateHistoryIfStale(u32 observedGeneration) {
    if (!m_history.invalidateHistoryIfStale(observedGeneration)) {
        return false;
    }
    m_resolve.resetBookkeeping();
    return true;
}

bool TaaPass::isHistoryStale(u32 observedGeneration) const {
    return m_history.isHistoryStale(observedGeneration);
}

void TaaPass::resize(u32 width, u32 height) {
    if (!taaHistoryResizeNeeded(m_desc.width, m_desc.height, width, height)) {
        return;
    }

    m_desc.width = width;
    m_desc.height = height;
    if (m_stats.ready) {
        m_history.resize(width, height);
    }
}

bool TaaPass::matchesDimensions(u32 width, u32 height) const {
    return m_desc.width == width && m_desc.height == height && m_history.matchesDimensions(width, height);
}

bool TaaPass::historyWarmupComplete() const {
    return m_history.warmupComplete();
}

TaaHistoryWarmupState TaaPass::classifyHistoryWarmupState() const {
    return classifyTaaHistoryWarmupState(m_history);
}

bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupState* state) const {
    return preflightTaaHistoryWarmup(m_history, state);
}

bool TaaPass::shouldSkipHistoryWarmup() const {
    return shouldSkipTaaHistoryWarmup(m_history);
}

u32 TaaPass::warmupFramesRemaining() const {
    return m_history.warmupFramesRemaining();
bool TaaPass::isWarmupResolveFrame() const {
    return m_history.isWarmupFrame();
}

bool TaaPass::preflightHistoryReuse(u32 observedGeneration) const {
    return m_history.preflightReuse(observedGeneration);

bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc,
                                    TaaResolveBlendPreflightRejectReason* reason) const {
    return preflightTaaResolveBlend(desc, m_history, reason);
TaaHistoryWarmupPhase TaaPass::historyWarmupPhase() const {
    return classifyTaaHistoryWarmupPhase(m_history);

bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupPhase* phase) const {
    return preflightTaaHistoryWarmup(m_history, phase);
bool TaaPass::historyWarmupComplete() const {
    return taaHistoryWarmupComplete(m_history);

bool TaaPass::tryCanBeginTemporalReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason) const {
    return preflightTaaHistoryReuse(m_history, observedGeneration, reason);
    return taaHistoryWarmupFramesRemaining(m_history);

bool TaaPass::historyReadyForResolve() const {
    return taaHistoryReadyForResolve(m_history);
}

bool TaaPass::historyWarmupComplete() const {
    return m_history.warmupComplete();
}

bool TaaPass::historyWarmupComplete() const {
    return m_history.warmupComplete();
}

bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupRejectReason* reason) const {
    return preflightTaaHistoryWarmup(m_history, reason);
}

bool TaaPass::shouldSkipHistoryWarmup() const {
    return shouldSkipTaaHistoryWarmup(m_history);
}

bool TaaPass::historyWarmupComplete() const {
    return m_history.warmupComplete();
}

bool TaaPass::historyWarmupComplete() const {
    return taaHistoryWarmupComplete(m_history);
}

bool TaaPass::shouldSkipHistoryWarmup() const {
    return shouldSkipTaaHistoryWarmup(m_history);
}

bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason) const {
    return preflightTaaHistoryWarmup(m_history, reason);
}

bool TaaPass::historyWarmupComplete() const {
    return taaHistoryWarmupComplete(m_history);
}

TaaHistoryWarmupBlockReason TaaPass::classifyHistoryWarmupBlock() const {
    return classifyTaaHistoryWarmupBlock(m_history);
}

bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason) const {
    return preflightTaaHistoryWarmup(m_history, reason);
}

bool TaaPass::shouldSkipHistoryWarmup() const {
    return shouldSkipTaaHistoryWarmup(m_history);
}

bool TaaPass::canReuseHistory() const {
    return m_history.canReuseHistory();

bool TaaPass::isHistoryWarm() const {
    return m_history.isWarm();
}

bool TaaPass::historyReuseReady(u32 observedGeneration) const {
    return m_history.reuseReady(observedGeneration);

bool TaaPass::historyWarmupComplete() const {
    return m_history.warmupComplete();
}

bool TaaPass::shouldSkipHistoryReuse(u32 observedGeneration) const {
    return shouldSkipTaaHistoryReuse(m_history, observedGeneration);

bool TaaPass::historyWarmupRequired() const {
    return m_stats.ready && m_history.needsWarmup();
}

bool TaaPass::historyWarmupComplete() const {
    return m_stats.ready && m_history.warmupComplete();
}

bool TaaPass::shouldSkipHistoryWarmup() const {
    return shouldSkipTaaHistoryWarmup(m_history);
}

TaaHistoryWarmupBlockReason TaaPass::classifyHistoryWarmupBlock() const {
    return classifyTaaHistoryWarmupBlock(m_history);
}

bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason) const {
    return preflightTaaHistoryWarmup(m_history, reason);
}

bool TaaPass::tryPreflightHistoryWarmup(TaaHistoryWarmupBlockReason& reason) const {
    return tryPreflightTaaHistoryWarmup(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::wouldSkipHistoryReuse(u32 observedGeneration) const {
    return wouldSkipTaaHistoryReuse(m_history, observedGeneration);
}

bool TaaPass::historyWarmupComplete() const {
    return m_history.warmupComplete();
}

bool TaaPass::shouldSkipHistoryWarmup() const {
    return shouldSkipTaaHistoryWarmup(m_history);
}

bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupRejectReason* reason) const {
    return preflightTaaHistoryWarmup(m_history, reason);
}

bool TaaPass::shouldSkipHistoryWarmup() const {
    return shouldSkipTaaHistoryWarmup(m_history);
}

TaaHistoryWarmupPhase TaaPass::historyWarmupPhase() const {
    return classifyTaaHistoryWarmupPhase(m_history);
}

bool TaaPass::shouldSkipHistoryReuseForResolve(const TaaResolveDesc& desc) const {
    return shouldSkipTaaHistoryReuseForResolve(desc, m_history);
}

bool TaaPass::preflightHistoryReuseForResolve(const TaaResolveDesc& desc,
                                              TaaHistoryReuseBlockReason* reason) const {
    return preflightTaaHistoryReuseForResolve(desc, m_history, reason);
}

bool TaaPass::invalidateHistoryIfStale(u32 observedGeneration) {
    if (!m_history.invalidateHistoryIfStale(observedGeneration)) {
        return false;
    }
    m_resolve.resetBookkeeping();
    return true;
}

bool TaaPass::historyBlendAllowed() const {
    return taaHistoryBlendAllowed(!m_history.hasValidHistory(), m_history);

bool TaaPass::canProduceJitterNdc() const {
    return m_jitter.canProduceNdcOffset(m_desc.width, m_desc.height);

bool TaaPass::advanceJitterIfReady() {
    if (!m_jitter.advanceIfReady()) {
        return false;
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;

TaaBlendWeights TaaPass::expectedResolveBlendWeights(const TaaResolveDesc& desc) const {
    return computeTaaResolveBlendWeights(desc, m_history);

bool TaaPass::resolveWouldReuseHistory(const TaaResolveDesc& desc) const {
    return taaResolveCanReuseHistory(desc, m_history) && taaResolveAppliesHistoryBlend(desc, m_history);

TaaHistoryReuseBlockReason TaaPass::classifyHistoryReuseBlock(u32 observedGeneration) const {
    return classifyTaaHistoryReuseBlock(m_history, observedGeneration);

bool TaaPass::preflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason) const {
    return preflightTaaHistoryReuse(m_history, observedGeneration, reason);

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);

bool TaaPass::preflightResolveBlendWeights(const TaaResolveDesc& desc,
                                           TaaResolveBlendRejectReason* reason) const {
    return preflightTaaResolveBlendWeights(desc, m_history, reason);

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);

bool TaaPass::shouldSkipResolveBlend(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveBlend(desc, m_history);

bool TaaPass::preflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);

bool TaaPass::shouldSkipJitterSync(u32 frameIndex) const {
    return shouldSkipTaaJitterSync(frameIndex, m_jitter.sequenceLength());

bool TaaPass::preflightJitterNdc(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);

bool TaaPass::shouldSkipJitterNdc() const {
    return shouldSkipTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength());

bool TaaPass::shouldSkipHistoryWarmup() const {
    return shouldSkipTaaHistoryWarmup(m_history);

bool TaaPass::historyReadyForResolve() const {
    return m_history.readyForResolve();

bool TaaPass::shouldSkipHistoryResolve() const {
    return shouldSkipTaaHistoryResolve(m_history);

bool TaaPass::shouldSkipResolve(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolve(desc, m_history);

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);








TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
bool TaaPass::viewportMatchesResolve(const TaaResolveDesc& desc) const {
    return taaViewportDimensionsMatchPass(m_desc.width, m_desc.height, desc);
f32 TaaPass::effectiveBlendForNextResolve() const {
    return computeEffectiveBlend(!m_history.hasValidHistory(), m_desc.params);
}

bool TaaPass::canResolveFrame(const TaaResolveDesc& desc) const {
    return canAttemptTaaResolve(desc, m_history);
}

bool TaaPass::prepareAndCanResolve(TaaResolveDesc& desc) const {
    return prepareTaaResolveDesc(desc, m_history);
}

bool TaaPass::resolveWillReuseHistory(const TaaResolveDesc& desc) const {
    return taaResolveWillReuseHistory(desc, m_history);
}

bool TaaPass::isJitterSyncedToFrameIndex(u32 frameIndex) const {
    return m_jitter.isSyncedToFrameIndex(frameIndex);
}

TaaHistoryWarmupPreflight TaaPass::preflightHistoryWarmup() const {
    return preflightTaaHistoryWarmup(m_history);
}

TaaHistoryReusePreflight TaaPass::preflightHistoryReuse(u32 observedGeneration) const {
    return preflightTaaHistoryReuse(m_history, observedGeneration);
}

TaaHistoryReusePreflight TaaPass::preflightHistoryReuseForDesc(const TaaResolveDesc& desc) const {
    return preflightTaaHistoryReuseForDesc(m_history, desc);
}

TaaJitterSyncPreflight TaaPass::preflightJitterSync(u32 frameIndex) const {
    return preflightTaaJitterSync(m_jitter, frameIndex, m_desc.width, m_desc.height);
}

TaaResolveBlendPreflight TaaPass::preflightResolveBlend(const TaaResolveDesc& desc) const {
    return preflightTaaResolveBlendForDesc(desc, m_history);
}

bool TaaPass::canResolveFrame(const TaaResolveDesc& desc) const {
    return canAttemptTaaResolve(desc, m_history);
}

bool TaaPass::prepareAndCanResolve(TaaResolveDesc& desc) const {
    return prepareTaaResolveDesc(desc, m_history);
}

bool TaaPass::resolveWillReuseHistory(const TaaResolveDesc& desc) const {
    return taaResolveWillReuseHistory(desc, m_history);
}

bool TaaPass::isJitterSyncedToFrameIndex(u32 frameIndex) const {
    return m_jitter.isSyncedToFrameIndex(frameIndex) && m_jitter.slotMatchesMonotonicFrame();
}

bool TaaPass::isJitterSyncedToFrameIndex(u32 frameIndex) const {
    return m_jitter.isSyncedToFrameIndex(frameIndex);
}

bool TaaPass::canResolveFrame(const TaaResolveDesc& desc) const {
    return canAttemptTaaResolve(desc, m_history);
}

bool TaaPass::prepareAndCanResolve(TaaResolveDesc& desc) const {
    return prepareTaaResolveDesc(desc, m_history);
}

bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendWeights* out) const {
    return preflightTaaResolveBlend(desc, m_history, out);
}

bool TaaPass::isJitterSyncedToFrameIndex(u32 frameIndex) const {
    return m_jitter.isSyncedToFrameIndex(frameIndex);
}

TaaHistoryReuseRejectReason TaaPass::classifyHistoryReuseReject(u32 observedGeneration) const {
    return classifyTaaHistoryReuseReject(m_history, observedGeneration);
}

bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc, TaaResolveBlendPreflight* out) const {
    return preflightTaaResolveBlend(desc, m_history, out);
}

bool TaaPass::isJitterSyncedToFrameIndex(u32 frameIndex) const {
    return m_jitter.slotMatchesFrameIndex(frameIndex);
}

bool TaaPass::canSyncJitterToFrameIndex() const {
    return m_jitter.canAdvance();
}

bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendWeights* weights) const {
    return preflightTaaResolveBlend(desc, m_history, weights);
}

bool TaaPass::isJitterSyncedToFrameIndex(u32 frameIndex) const {
    return m_jitter.isSyncedToFrameIndex(frameIndex);
}

bool TaaPass::isJitterSyncedToFrameIndex(u32 frameIndex) const {
    return m_jitter.isSyncedToFrameIndex(frameIndex);
}

bool TaaPass::historyWarmupComplete() const {
    return taaHistoryWarmupComplete(m_history);
}

bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendWeights* weights) const {
    return preflightTaaResolveBlend(desc, m_history, weights);
}

bool TaaPass::advanceJitterIfReady() {
    if (!m_jitter.advanceIfReady()) {
        return false;
    }
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;
}

bool TaaPass::advanceJitterIfAligned(u32 expectedFrameIndex) {
    if (!m_jitter.advanceIfAlignedToFrameIndex(expectedFrameIndex)) {
bool TaaPass::advanceJitterIfViewportReady() {
    if (!m_jitter.advanceIfViewportReady(m_desc.width, m_desc.height)) {
bool TaaPass::tryAdvanceJitterIfReady(TaaJitterGuardRejectReason& reason) {
    if (!m_jitter.tryAdvanceIfReady(reason)) {
        return false;
    }
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;

bool TaaPass::preflightJitterAlignment(u32 expectedFrameIndex) const {
    return preflightTaaJitterAlignment(expectedFrameIndex, m_jitter);
bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
bool TaaPass::tryAdvanceJitter(TaaJitterGuardRejectReason& reason) {
    if (!tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason)) {
    return advanceJitterIfReady();

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);

    if (!tryPreflightJitterAdvance(reason)) {
    if (!m_jitter.advanceIfReady()) {
        reason = classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
    reason = TaaJitterGuardRejectReason::None;




TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);

TaaBlendWeights TaaPass::expectedResolveBlendWeights(const TaaResolveDesc& desc) const {
    return computeTaaResolveBlendWeights(desc, m_history);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                             TaaResolveBlendRejectReason& reason) const {
TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);

    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

bool TaaPass::resolveWouldReuseHistory(const TaaResolveDesc& desc) const {
    return taaResolveCanReuseHistory(desc, m_history) && taaResolveAppliesHistoryBlend(desc, m_history);
}

TaaHistoryReuseBlockReason TaaPass::classifyHistoryReuseBlock(u32 observedGeneration) const {
    return classifyTaaHistoryReuseBlock(m_history, observedGeneration);
}

bool TaaPass::isJitterSyncedToFrameIndex(u32 frameIndex) const {
    return m_jitter.isSyncedToFrameIndex(frameIndex);
}

bool TaaPass::canResolveFrame(const TaaResolveDesc& desc) const {
    return canAttemptTaaResolve(desc, m_history);

bool TaaPass::prepareAndCanResolve(TaaResolveDesc& desc) const {
    return prepareTaaResolveDesc(desc, m_history);

bool TaaPass::resolveWillReuseHistory(const TaaResolveDesc& desc) const {
    return taaResolveWillReuseHistory(desc, m_history);

bool TaaPass::historyWarmupComplete() const {
    return taaHistoryWarmupComplete(m_history);

f32 TaaPass::historyWarmupProgress() const {
    return taaHistoryWarmupProgress(m_history);

bool TaaPass::historyReuseReady(u32 observedGeneration) const {
    return taaHistoryReuseReady(m_history, observedGeneration);

TaaJitterSyncBlockReason TaaPass::classifyJitterSyncBlock() const {
    return m_jitter.classifySyncBlock(m_desc.width, m_desc.height);

bool TaaPass::preflightJitterSync(u32 frameIndex, TaaJitterSyncBlockReason* reason) const {
    return m_jitter.preflightSync(frameIndex, m_desc.width, m_desc.height, reason);

bool TaaPass::preflightResolveWithBlend(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
                                        TaaResolveBlendRejectReason* blendReason) const {
    return preflightTaaResolveWithBlend(desc, m_history, skipReason, blendReason);

bool TaaPass::lastResolveStatsBlendConsistent(const TaaResolveDesc& desc) const {
    return taaResolveStatsBlendConsistent(m_resolve.lastStats(), desc, m_history);
bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason) const {
    return preflightTaaHistoryWarmup(m_history, reason);

u32 TaaPass::warmupFramesRemaining() const {
    return taaHistoryWarmupFramesRemaining(m_history);
u32 TaaPass::historyWarmupFramesRemaining() const {

TaaHistoryWarmupBlockReason TaaPass::classifyHistoryWarmupBlock() const {
    return classifyTaaHistoryWarmupBlock(m_history);


bool TaaPass::preflightHistoryReuseForResolve(const TaaResolveDesc& desc,
                                              TaaHistoryReuseBlockReason* reason) const {
    return preflightTaaHistoryReuseForResolve(desc, m_history, reason);

    return preflightTaaJitterSync(m_jitter, frameIndex, reason);
bool TaaPass::canPreflightHistoryReuse(u32 observedGeneration) const {
    return canPreflightTaaHistoryReuse(m_history, observedGeneration);



bool TaaPass::canPreflightHistoryWarmup() const {
    return canPreflightTaaHistoryWarmup(m_history);

bool TaaPass::temporalBlendReady(const TaaResolveDesc& desc) const {
    return taaHistoryTemporalBlendReady(desc, m_history);

bool TaaPass::preflightHistoryReuseForDesc(const TaaResolveDesc& desc,
    return preflightTaaHistoryReuseForDesc(desc, m_history, reason);



    return m_jitter.preflightSyncToFrameIndex(frameIndex, m_desc.width, m_desc.height, reason);

bool TaaPass::syncJitterToFrameIndexIfViewportReady(u32 frameIndex) {
    if (!m_jitter.syncToFrameIndexIfViewportReady(frameIndex, m_desc.width, m_desc.height)) {
        return false;
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;

bool TaaPass::preflightResolveHistoryReuse(const TaaResolveDesc& desc,
    return preflightTaaResolveHistoryReuse(desc, m_history, reason);








bool TaaPass::wouldSkipHistoryReuse(u32 observedGeneration) const {
    return wouldSkipTaaHistoryReuse(m_history, observedGeneration);

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& outReason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, outReason);

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);

bool TaaPass::tryPreflightHistoryWarmup(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryWarmup(m_history, reason);

bool TaaPass::shouldSkipHistoryWarmup() const {
    return shouldSkipTaaHistoryWarmup(m_history);





bool TaaPass::tryPreflightHistoryWarmup(TaaHistoryWarmupBlockReason& reason) const {


bool TaaPass::preflightHistoryWarmup(TaaHistoryReuseBlockReason* reason) const {









bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
















TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);





    if (shouldSkipHistoryWarmup()) {
        reason = TaaHistoryReuseBlockReason::NotWarm;
    reason = TaaHistoryReuseBlockReason::None;

bool TaaPass::preflightHistoryTemporal(u32 observedGeneration, TaaHistoryReuseBlockReason* reason) const {
    TaaHistoryReuseBlockReason localReason = TaaHistoryReuseBlockReason::None;
    if (!tryPreflightHistoryReadyForResolve(localReason)) {
        if (reason != nullptr) {
            *reason = localReason;
    return preflightHistoryReuse(observedGeneration, reason);

bool TaaPass::shouldSkipHistoryTemporal(u32 observedGeneration) const {
    return !preflightHistoryTemporal(observedGeneration);













bool TaaPass::preflightHistoryWarmupAndReuse(u32 observedGeneration,
    if (!m_history.isReady()) {
            *reason = TaaHistoryReuseBlockReason::NotReady;
    if (m_history.needsWarmup()) {
            *reason = TaaHistoryReuseBlockReason::NotWarm;







    if (!m_history.readyForResolve()) {
        *reason = TaaHistoryReuseBlockReason::None;

    return preflightHistoryWarmup(&reason);


bool TaaPass::preflightHistoryWarmupAndReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason) const {
    if (shouldSkipTaaHistoryWarmup(m_history)) {
TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());


TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);





















TaaHistoryReuseBlockReason TaaPass::classifyHistoryWarmupBlock() const {
        return TaaHistoryReuseBlockReason::NotReady;
        return TaaHistoryReuseBlockReason::NotWarm;
    return TaaHistoryReuseBlockReason::None;

TaaHistoryReuseBlockReason TaaPass::classifyHistoryResolveBlock() const {


    if (!m_history.hasValidHistory()) {


bool TaaPass::preflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason) const {
    return preflightTaaHistoryReuse(m_history, observedGeneration, reason);

bool TaaPass::shouldSkipHistoryWarmupAndReuse(u32 observedGeneration) const {
    return !preflightHistoryWarmupAndReuse(observedGeneration);


TaaHistoryReuseBlockReason TaaPass::classifyHistoryWarmupBlock() const {
    if (!historyReadyForResolve()) {
        return TaaHistoryReuseBlockReason::NotReady;
    if (needsHistoryWarmup()) {
        return TaaHistoryReuseBlockReason::NotWarm;
    return TaaHistoryReuseBlockReason::None;

    const TaaHistoryReuseBlockReason block = classifyHistoryWarmupBlock();
        *reason = block;
    return block == TaaHistoryReuseBlockReason::None;

bool TaaPass::preflightHistoryReadyForResolve(TaaHistoryReuseBlockReason* reason) const {
    if (reason == nullptr) {
        return m_history.readyForResolve();
    return tryPreflightTaaHistoryReadyForResolve(m_history, *reason);


    TaaHistoryReuseBlockReason local = TaaHistoryReuseBlockReason::None;
    if (!tryPreflightTaaHistoryReadyForResolve(m_history, local)) {
            *reason = local;

bool TaaPass::tryPreflightHistoryTemporal(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    if (!tryPreflightTaaHistoryReadyForResolve(m_history, reason)) {
    return tryPreflightHistoryReuse(observedGeneration, reason);


bool TaaPass::preflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason) const {

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                             TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryWarmup(TaaHistoryReuseBlockReason& reason) const {
    reason = classifyHistoryWarmupBlock();
    return reason == TaaHistoryReuseBlockReason::None;
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryWarmup(TaaHistoryReuseBlockReason& reason) const {
    reason = classifyHistoryWarmupBlock();
    return reason == TaaHistoryReuseBlockReason::None;
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

bool TaaPass::preflightResolveBlendWeights(const TaaResolveDesc& desc,
                                           TaaResolveBlendRejectReason* reason) const {
    return preflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
                                    TaaResolveBlendRejectReason* blendRejectReason) const {
    return preflightTaaResolveFrame(desc, m_history, skipReason, blendRejectReason);
bool TaaPass::canPreflightResolveBlendWeights(const TaaResolveDesc& desc) const {
    return canPreflightTaaResolveBlendWeights(desc, m_history);
}

bool TaaPass::tryExpectedResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& out,
                                            TaaResolveBlendRejectReason* reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, out, reason);

bool TaaPass::preflightResolveTemporalBlend(const TaaResolveDesc& desc,
                                            TaaResolveTemporalRejectReason* reason) const {
    return preflightTaaResolveTemporalBlend(desc, m_history, reason);

bool TaaPass::canPreflightResolveTemporalBlend(const TaaResolveDesc& desc) const {
    return canPreflightTaaResolveTemporalBlend(desc, m_history);
bool TaaPass::canResolveFrame(const TaaResolveDesc& desc) const {
    return canAttemptTaaResolve(desc, m_history);

bool TaaPass::prepareAndCanResolve(TaaResolveDesc& desc) const {
    return prepareTaaResolveDesc(desc, m_history);

                                    TaaResolveBlendRejectReason* blendReason) const {
    return preflightTaaResolveFrame(desc, m_history, skipReason, blendReason);

bool TaaPass::jitterNeedsResyncToFrameIndex(u32 frameIndex) const {
    return taaJitterNeedsResyncToFrameIndex(m_jitter, frameIndex);

bool TaaPass::preflightJitterSync(u32 frameIndex, TaaJitterSyncBlockReason* reason) const {
    return preflightTaaJitterSync(m_jitter, frameIndex, reason);

u32 TaaPass::historyWarmupFramesRemaining() const {
    return taaHistoryWarmupFramesRemaining(m_history);

bool TaaPass::historyWarmupComplete() const {
    return taaHistoryWarmupComplete(m_history);
bool TaaPass::preflightResolveGuards(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
    return preflightTaaResolveGuards(desc, m_history, skipReason, blendRejectReason);
bool TaaPass::resolveTemporalBlendAllowed(const TaaResolveDesc& desc) const {
    return taaResolveTemporalBlendAllowed(desc, m_history);

                                            TaaResolveBlendRejectReason* blendReason,
                                            TaaHistoryReuseBlockReason* reuseReason) const {
    return preflightTaaResolveTemporalBlend(desc, m_history, blendReason, reuseReason);
TaaHistoryWarmupState TaaPass::classifyHistoryWarmupState() const {
    return classifyTaaHistoryWarmupState(m_history);

bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupState* state) const {
    return preflightTaaHistoryWarmup(m_history, state);

TaaResolveBlendMode TaaPass::classifyResolveBlendMode(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendMode(desc, m_history);

bool TaaPass::jitterNeedsSyncToFrameIndex(u32 frameIndex) const {
    return m_jitter.needsSyncToFrameIndex(frameIndex);

bool TaaPass::preflightResolveDesc(const TaaResolveDesc& desc, TaaResolveDescPreflight* result) const {
    return preflightTaaResolveDesc(desc, m_history, result);
bool TaaPass::preflightResolveTemporalAccumulation(const TaaResolveDesc& desc,
                                                   TaaResolveTemporalPreflight* result) const {
    return preflightTaaResolveTemporalAccumulation(desc, m_history, result);
bool TaaPass::preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveFramePreflight* out) const {
    return preflightTaaResolveFrame(desc, m_history, out);

bool TaaPass::tryExpectedResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
    return fuse::renderer::jitterNeedsResyncToFrameIndex(m_jitter, frameIndex);

bool TaaPass::trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncRejectReason& outReason) {
    if (!fuse::renderer::trySyncJitterToFrameIndexIfReady(m_jitter, frameIndex, outReason)) {
        return false;
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;

bool TaaPass::shouldSkipHistoryReuse(u32 observedGeneration) const {
    return shouldSkipTaaHistoryReuse(m_history, observedGeneration);

bool TaaPass::canSampleHistoryForResolve(const TaaResolveDesc& desc) const {
    return fuse::renderer::canSampleHistoryForResolve(desc, m_history);

bool TaaPass::tryPreflightHistoryReuseForResolve(const TaaResolveDesc& desc,
                                                 TaaHistoryReuseBlockReason& outReason) const {
    return tryPreflightTaaHistoryReuseForResolve(desc, m_history, outReason);

bool TaaPass::shouldSkipHistoryBlendAtResolve(const TaaResolveDesc& desc) const {
    return fuse::renderer::shouldSkipHistoryBlendAtResolve(desc, m_history);

bool TaaPass::canApplyHistoryBlendAtResolve(const TaaResolveDesc& desc) const {
    return fuse::renderer::canApplyHistoryBlendAtResolve(desc, m_history);
bool TaaPass::preflightResolveWithBlend(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
    return preflightTaaResolveWithBlend(desc, m_history, skipReason, blendReason);

bool TaaPass::preflightResolveTemporal(const TaaResolveDesc& desc,
                                       TaaHistoryReuseBlockReason* reuseReason,
    return preflightTaaResolveTemporal(desc, m_history, reuseReason, blendReason);
bool TaaPass::wouldRejectResolveBlendWeights(const TaaResolveDesc& desc) const {
    return wouldRejectTaaResolveBlendWeights(desc, m_history);

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& outReason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, outReason);

bool TaaPass::wouldSkipJitterSync(u32 frameIndex) const {
    return m_jitter.wouldSkipSyncToFrameIndex(frameIndex);

bool TaaPass::trySyncJitterToFrameIndex(u32 frameIndex, TaaJitterSyncRejectReason& outReason) {
    if (!m_jitter.trySyncToFrameIndexIfReady(frameIndex, outReason)) {

bool TaaPass::preflightJitterNdc(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);

bool TaaPass::shouldSkipJitterSync(u32 frameIndex) const {
    return shouldSkipTaaJitterSync(frameIndex, m_jitter.sequenceLength());

bool TaaPass::shouldSkipJitterNdc() const {
    return shouldSkipTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength());

bool TaaPass::jitterSyncReady(u32 frameIndex) const {
    return taaJitterSyncReady(frameIndex, m_jitter.sequenceLength());

bool TaaPass::jitterNdcReady() const {
    return taaJitterNdcReady(m_desc.width, m_desc.height, m_jitter.sequenceLength());

bool TaaPass::resolveBlendReady(const TaaResolveDesc& desc) const {
    return taaResolveBlendReady(desc, m_history);




    return m_history.warmupComplete();

bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupRejectReason* reason) const {
    return preflightTaaHistoryWarmup(m_history, reason);

bool TaaPass::shouldSkipHistoryWarmup() const {
    return shouldSkipTaaHistoryWarmup(m_history);


bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);





bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);

                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,


bool TaaPass::shouldSkipResolve(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolve(desc, m_history);

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);

bool TaaPass::wouldSkipResolveBlend(const TaaResolveDesc& desc) const {
    return wouldSkipTaaResolveBlend(desc, m_history);
bool TaaPass::resolveBlendWeightsReady(const TaaResolveDesc& desc) const {
    return taaResolveBlendWeightsReady(desc, m_history);

bool TaaPass::shouldSkipResolveHistoryBlend(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveHistoryBlend(desc, m_history);

bool TaaPass::computeResolveBlendWeightsIfReady(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
    return computeTaaResolveBlendWeightsIfReady(desc, m_history, outWeights, reason);
bool TaaPass::preflightTemporalBlend(const TaaResolveDesc& desc, TaaHistoryReuseBlockReason* reuseReason,
    return preflightTaaTemporalBlend(desc, m_history, reuseReason, blendReason);

bool TaaPass::shouldSkipTemporalBlend(const TaaResolveDesc& desc) const {
    return shouldSkipTaaTemporalBlend(desc, m_history);

bool TaaPass::isHistoryWarmed() const {
    return m_history.isWarmed();
bool TaaPass::preflightTemporalResolve(const TaaResolveDesc& desc,
                                       TaaTemporalGuardRejectReason* reason) const {
    return preflightTaaTemporalResolve(desc, m_history, reason);

bool TaaPass::shouldSkipTemporalResolve(const TaaResolveDesc& desc) const {
    return shouldSkipTaaTemporalResolve(desc, m_history);


TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);



















    if (wouldSkipResolve(desc, skipReason)) {
    return preflightResolveBlendWeights(desc, blendReason);

bool TaaPass::shouldSkipResolveWithBlend(const TaaResolveDesc& desc) const {
    return !preflightResolveWithBlend(desc);










bool TaaPass::tryComputeExpectedResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,







bool TaaPass::tryComputeExpectedResolveBlendWeights(const TaaResolveDesc& desc,
                                                   TaaBlendWeights& outWeights,









    if (!preflightResolveBlendWeights(desc, blendReason)) {
    if (!taaResolveAppliesHistoryBlend(desc, m_history)) {

    u32 observedGeneration = desc.observed_history_generation;
    if (observedGeneration == kTaaResolveNoHistoryGeneration) {
        observedGeneration = m_history.invalidateGeneration();
    return preflightHistoryReuse(observedGeneration, reuseReason);



















TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);


















































bool TaaPass::shouldSkipResolveBlend(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveBlend(desc, m_history);


bool TaaPass::shouldSkipResolveTemporalBlend(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveTemporalBlend(desc, m_history);

bool TaaPass::preflightTemporalResolveGuards(const TaaResolveDesc& desc, u32 observedGeneration,
                                             TaaHistoryReuseBlockReason* reuseReason,
                                             TaaResolveBlendRejectReason* blendReason) const {
    return preflightTaaTemporalResolveGuards(desc, m_history, observedGeneration, reuseReason, blendReason);
}

bool TaaPass::shouldSkipTemporalResolveGuards(const TaaResolveDesc& desc, u32 observedGeneration) const {
    return shouldSkipTaaTemporalResolveGuards(desc, m_history, observedGeneration);
}

bool TaaPass::resolveBlendReady(const TaaResolveDesc& desc) const {
    return taaResolveBlendReady(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::preflightResolveWithBlend(const TaaResolveDesc& desc,
                                        TaaResolveWithBlendRejectReason* reason) const {
    return preflightTaaResolveWithBlend(desc, m_history, reason);
}

bool TaaPass::shouldSkipResolveWithBlend(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveWithBlend(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeExpectedResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                                    TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

bool TaaPass::preflightResolveTemporal(const TaaResolveDesc& desc, u32 observedGeneration,
                                       TaaResolveTemporalRejectReason* reason) const {
    return preflightTaaResolveTemporal(desc, m_history, observedGeneration, reason);
}

bool TaaPass::shouldSkipResolveTemporal(const TaaResolveDesc& desc, u32 observedGeneration) const {
    return shouldSkipTaaResolveTemporal(desc, m_history, observedGeneration);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::preflightResolveTemporalBlend(const TaaResolveDesc& desc,
                                            TaaHistoryReuseBlockReason* reuseReason,
                                            TaaResolveBlendRejectReason* blendReason) const {
    return preflightTaaResolveTemporalBlend(desc, m_history, reuseReason, blendReason);
}

bool TaaPass::tryPreflightResolveTemporalBlend(const TaaResolveDesc& desc,
                                               TaaHistoryReuseBlockReason& reuseReason,
                                               TaaResolveBlendRejectReason& blendReason) const {
    return tryPreflightTaaResolveTemporalBlend(desc, m_history, reuseReason, blendReason);
}

bool TaaPass::shouldSkipResolveTemporalBlend(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveTemporalBlend(desc, m_history);
}

bool TaaPass::preflightResolveTemporalBlend(const TaaResolveDesc& desc,
                                            TaaResolveTemporalBlendRejectReason* reason) const {
    return preflightTaaResolveTemporalBlend(desc, m_history, reason);
}

bool TaaPass::shouldSkipResolveTemporalBlend(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveTemporalBlend(desc, m_history);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

bool TaaPass::preflightTemporalResolve(const TaaResolveDesc& desc,
                                       TaaHistoryReuseBlockReason* reuseReason,
                                       TaaResolveBlendRejectReason* blendReason) const {
    const u32 observedGeneration = desc.observed_history_generation == kTaaResolveNoHistoryGeneration
                                       ? m_history.invalidateGeneration()
                                       : desc.observed_history_generation;
    if (!preflightHistoryWarmupAndReuse(observedGeneration, reuseReason)) {
        return false;
    }

    TaaResolveBlendRejectReason localBlendReason = TaaResolveBlendRejectReason::None;
    if (!preflightTaaResolveBlendWeights(desc, m_history, &localBlendReason)) {
        if (blendReason != nullptr) {
            *blendReason = localBlendReason;
        }
        return false;
    }

    if (blendReason != nullptr) {
        *blendReason = TaaResolveBlendRejectReason::None;
    }
    return true;
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::preflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject(u32 frameIndex) const {
    (void)frameIndex;
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());










bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterGuardRejectReason& reason) {
    if (!tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason)) {
        return false;
    }
    return syncJitterToFrameIndexIfReady(frameIndex);

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);

bool TaaPass::trySyncJitterToFrameIndex(u32 frameIndex, TaaJitterGuardRejectReason& reason) {
    if (!m_jitter.syncToFrameIndexIfReady(frameIndex)) {
        reason = classifyTaaJitterSyncReject(m_jitter.sequenceLength());
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());


bool TaaPass::shouldSkipJitterSync(u32 frameIndex) const {
    return shouldSkipTaaJitterSync(frameIndex, m_jitter.sequenceLength());
}

bool TaaPass::trySyncJitterToFrameIndex(u32 frameIndex, TaaJitterGuardRejectReason& reason) {
    if (!trySyncTaaJitter(m_jitter, frameIndex, reason)) {
        return false;
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;

bool TaaPass::tryAdvanceJitterIfReady(TaaJitterGuardRejectReason& reason) {
    if (!tryAdvanceTaaJitter(m_jitter, reason)) {

bool TaaPass::jitterNeedsResync(u32 frameIndex) const {
    return taaJitterNeedsResync(frameIndex, m_jitter);

bool TaaPass::shouldResyncJitter(u32 frameIndex) const {
    return shouldResyncTaaJitter(frameIndex, m_jitter);

bool TaaPass::preflightJitterSlot(u32 slot, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterSlot(slot, m_jitter.sequenceLength(), reason);

bool TaaPass::shouldSkipJitterSlot(u32 slot) const {
    return shouldSkipTaaJitterSlot(slot, m_jitter.sequenceLength());
TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
bool TaaPass::ndcOffsetForFrameIndexIfReady(u32 frameIndex, fuse::math::Vec2& out) const {
    return TaaJitterLayout::ndcOffsetForFrameIndexIfReady(frameIndex, m_desc.width, m_desc.height,
                                                            m_jitter.sequenceLength(), out);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());


bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);

bool TaaPass::tryPreflightJitterAlignment(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAlignment(m_jitter, frameIndex, reason);

bool TaaPass::shouldSkipJitterAlignment(u32 frameIndex) const {
    return shouldSkipTaaJitterAlignment(m_jitter, frameIndex);
bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());

bool TaaPass::preflightJitterAligned(u32 frameIndex, TaaJitterAlignmentRejectReason* reason) const {
    return preflightTaaJitterAligned(frameIndex, m_jitter, reason);

    return shouldSkipTaaJitterAlignment(frameIndex, m_jitter);
bool TaaPass::preflightJitterAlignment(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAlignment(m_jitter, frameIndex, reason);



bool TaaPass::preflightJitterSyncAndNdc(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterSyncAndNdc(frameIndex, m_desc.width, m_desc.height, m_jitter.sequenceLength(),
                                      reason);

bool TaaPass::shouldSkipJitterSyncAndNdc(u32 frameIndex) const {
    return shouldSkipTaaJitterSyncAndNdc(frameIndex, m_desc.width, m_desc.height, m_jitter.sequenceLength());
    return preflightTaaJitterAlignment(frameIndex, m_jitter.monotonicFrameIndex(), m_jitter.index(),
                                     m_jitter.sequenceLength(), reason);

    return tryPreflightTaaJitterAlignment(frameIndex, m_jitter.monotonicFrameIndex(), m_jitter.index(),

    return shouldSkipTaaJitterAlignment(frameIndex, m_jitter.monotonicFrameIndex(), m_jitter.index(),
                                        m_jitter.sequenceLength());
bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);





}

bool TaaPass::preflightJitterNdc(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterSync(u32 frameIndex) const {
    return shouldSkipTaaJitterSync(frameIndex, m_jitter.sequenceLength());


bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::preflightJitterSyncAndNdc(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    if (!preflightJitterSync(frameIndex, reason)) {
        return false;
    }
    return preflightJitterNdc(reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::ndcOffsetForFrameIndexIfReady(u32 frameIndex, fuse::math::Vec2& out) const {
    return TaaJitterLayout::ndcOffsetForFrameIndexIfReady(frameIndex, m_desc.width, m_desc.height,
                                                           m_jitter.sequenceLength(), out);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterNdc() const {
    return shouldSkipTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength());

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
    return preflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), &reason);

    return preflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), &reason);


bool TaaPass::jitterNeedsResync(u32 frameIndex) const {
    return m_jitter.needsResyncToFrameIndex(frameIndex);


bool TaaPass::historyWarmupComplete() const {
    return taaHistoryWarmupComplete(m_history);

bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason) const {
    return preflightTaaHistoryWarmup(m_history, reason);

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAlignment(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAlignment(m_jitter, frameIndex, reason);
}

bool TaaPass::shouldSkipJitterAlignment(u32 frameIndex) const {
    return shouldSkipTaaJitterAlignment(m_jitter, frameIndex);
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryWarmup(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryWarmup(m_history, reason);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryAdvanceJitterIfReady(TaaJitterGuardRejectReason& reason) {
    if (!m_jitter.tryAdvanceIfReady(reason)) {
        return false;
    }
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;
}

bool TaaPass::trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterGuardRejectReason& reason) {
    if (!m_jitter.trySyncToFrameIndexIfReady(frameIndex, reason)) {
        return false;
    }
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

void TaaPass::evaluateTemporalGuards(u32 frameIndex, const TaaResolveDesc& desc, u32 observedGeneration,
                                       TaaPassTemporalGuardVerdict& verdict) const {
    verdict.jitterSyncOk =
        preflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), &verdict.jitterReject);
    if (verdict.jitterSyncOk) {
        verdict.jitterNdcOk =
            preflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), &verdict.jitterReject);
    } else {
        verdict.jitterNdcOk = false;
    }

    verdict.historyWarmupComplete = !shouldSkipTaaHistoryWarmup(m_history);
    verdict.historyReuseOk =
        preflightTaaHistoryReuse(m_history, observedGeneration, &verdict.historyReject);
    verdict.resolveBlendOk =
        preflightTaaResolveBlendWeights(desc, m_history, &verdict.blendReject);
}

bool TaaPass::preflightTemporalGuards(u32 frameIndex, const TaaResolveDesc& desc, u32 observedGeneration,
                                        TaaPassTemporalGuardVerdict* verdict) const {
    TaaPassTemporalGuardVerdict local{};
    evaluateTemporalGuards(frameIndex, desc, observedGeneration, local);
    if (verdict != nullptr) {
        *verdict = local;
    }

    const bool inWarmupFrame =
        !local.historyWarmupComplete && !m_history.isHistoryStale(observedGeneration);
    const bool historyTemporalOk =
        local.historyReuseOk || (inWarmupFrame && m_history.readyForResolve());

    return local.jitterSyncOk && local.jitterNdcOk && local.resolveBlendOk && historyTemporalOk;
}

bool TaaPass::shouldSkipTemporalGuards(u32 frameIndex, const TaaResolveDesc& desc,
                                         u32 observedGeneration) const {
    return !preflightTemporalGuards(frameIndex, desc, observedGeneration);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::offsetForFrameIndexIfReady(u32 frameIndex, fuse::math::Vec2& out) const {
    return TaaJitterLayout::offsetForFrameIndexIfReady(frameIndex, m_jitter.sequenceLength(), out);
}

bool TaaPass::ndcOffsetForFrameIndexIfReady(u32 frameIndex, fuse::math::Vec2& out) const {
    return TaaJitterLayout::ndcOffsetForFrameIndexIfReady(frameIndex, m_desc.width, m_desc.height,
                                                          m_jitter.sequenceLength(), out);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterFrame(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    TaaJitterGuardRejectReason localReason = TaaJitterGuardRejectReason::None;
    if (!preflightJitterSync(frameIndex, &localReason)) {
        if (reason != nullptr) {
            *reason = localReason;
        }
        return false;
    }
    return preflightJitterNdc(reason);
}

bool TaaPass::shouldSkipJitterFrame(u32 frameIndex) const {
    return !preflightJitterFrame(frameIndex);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::ndcJitterForFrameIndexIfReady(u32 frameIndex, fuse::math::Vec2& out) const {
    return TaaJitterLayout::ndcOffsetForFrameIndexIfReady(frameIndex, m_desc.width, m_desc.height,
                                                        m_jitter.sequenceLength(), out);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterSyncAndNdc(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    TaaJitterGuardRejectReason local = TaaJitterGuardRejectReason::None;
    if (!preflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), &local)) {
        if (reason != nullptr) {
            *reason = local;
        }
        return false;
    }
    if (!preflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), &local)) {
        if (reason != nullptr) {
            *reason = local;
        }
        return false;
    }
    return true;
}

bool TaaPass::shouldSkipJitterSyncAndNdc(u32 frameIndex) const {
    return !preflightJitterSyncAndNdc(frameIndex);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryAdvanceJitterIfReady(TaaJitterGuardRejectReason& reason) {
    if (!tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason)) {
        return false;
    }
    if (!m_jitter.advanceIfReady()) {
        reason = TaaJitterGuardRejectReason::InvalidSequence;
        return false;
    }
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;
}

bool TaaPass::trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterGuardRejectReason& reason) {
    if (!tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason)) {
        return false;
    }
    if (!m_jitter.syncToFrameIndexIfReady(frameIndex)) {
        reason = TaaJitterGuardRejectReason::InvalidSequence;
        return false;
    }
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterFrame(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    if (!preflightJitterSync(frameIndex, reason)) {
        return false;
    }
    return preflightJitterNdc(reason);
}

bool TaaPass::tryPreflightJitterFrame(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    if (!tryPreflightJitterSync(frameIndex, reason)) {
        return false;
    }
    return tryPreflightJitterNdc(reason);
}

bool TaaPass::shouldSkipJitterFrame(u32 frameIndex) const {
    return shouldSkipJitterSync(frameIndex) || shouldSkipJitterNdc();
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryAdvanceJitter(TaaJitterGuardRejectReason& reason) {
    if (!tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason)) {
        return false;
    }
    return advanceJitterIfReady();
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::canAdvanceJitter() const {
    return m_jitter.canAdvance();
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::canSyncJitterToFrameIndex(u32 frameIndex) const {
    return m_jitter.canSyncToFrameIndex(frameIndex);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryAdvanceJitterIfReady(TaaJitterGuardRejectReason& reason) {
    if (!tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason)) {
        return false;
    }
    return advanceJitterIfReady();
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipHistoryWarmup() const {
    return shouldSkipTaaHistoryWarmup(m_history);

bool TaaPass::preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
                                    TaaResolveBlendRejectReason* blendReason) const {
    return preflightTaaResolveFrame(desc, m_history, skipReason, blendReason);
bool TaaPass::historyWarmupComplete() const {
    return taaHistoryWarmupComplete(m_history);
}

bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason) const {
    return preflightTaaHistoryWarmup(m_history, reason);

bool TaaPass::tryPreflightHistoryWarmup(TaaHistoryWarmupBlockReason& reason) const {
    return tryPreflightTaaHistoryWarmup(m_history, reason);
bool TaaPass::historyWarmupComplete() const {
    return taaHistoryWarmupComplete(m_history);
}

bool TaaPass::preflightHistoryWarmup(TaaHistoryReuseBlockReason* reason) const {

bool TaaPass::tryPreflightHistoryWarmup(TaaHistoryReuseBlockReason& reason) const {
bool TaaPass::preflightHistoryWarmupComplete(TaaHistoryWarmupRejectReason* reason) const {
    return preflightTaaHistoryWarmupComplete(m_history, reason);

bool TaaPass::shouldSkipHistoryWarmupComplete() const {
    return shouldSkipTaaHistoryWarmupComplete(m_history);
TaaHistoryReuseBlockReason TaaPass::classifyHistoryWarmupBlock() const {
    return classifyTaaHistoryWarmupBlock(m_history);

bool TaaPass::preflightHistoryWarmupSatisfied(TaaHistoryReuseBlockReason* reason) const {
    return preflightTaaHistoryWarmupSatisfied(m_history, reason);

bool TaaPass::tryPreflightHistoryWarmupSatisfied(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryWarmupSatisfied(m_history, reason);
bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason) const {
    return preflightTaaHistoryWarmup(m_history, reason);
}

bool TaaPass::preflightHistoryTemporalSample(u32 observedGeneration,
                                             TaaHistoryReuseBlockReason* reason) const {
    return preflightTaaHistoryTemporalSample(m_history, observedGeneration, reason);

bool TaaPass::shouldSkipHistoryTemporalSample(u32 observedGeneration) const {
    return shouldSkipTaaHistoryTemporalSample(m_history, observedGeneration);

bool TaaPass::historyReadyForResolve() const {
    return m_history.readyForResolve();

bool TaaPass::shouldSkipResolveFrame(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveFrame(desc, m_history);

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());

bool TaaPass::jitterNdcReady() const {
    return taaJitterNdcReady(m_desc.width, m_desc.height, m_jitter.sequenceLength());

    return m_history.warmupComplete();

TaaHistoryWarmupState TaaPass::historyWarmupState() const {
    return m_history.warmupState();

bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupState* state) const {
    return preflightTaaHistoryWarmup(m_history, state);

bool TaaPass::preflightResolveTemporal(const TaaResolveDesc& desc,
                                       TaaResolveTemporalRejectReason* reason) const {
    return preflightTaaResolveTemporal(desc, m_history, reason);

bool TaaPass::shouldSkipResolveTemporal(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveTemporal(desc, m_history);

bool TaaPass::preflightHistoryWarmup(TaaHistoryReuseBlockReason* reason) const {

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::shouldSkipHistoryResolve() const {
    return shouldSkipTaaHistoryResolve(m_history);
}

bool TaaPass::preflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason) const {
    return preflightTaaResolve(desc, m_history, reason);
bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::shouldSkipResolve(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolve(desc, m_history);
bool TaaPass::preflightResolveReuseAndBlend(const TaaResolveDesc& desc, u32 observedGeneration,
                                            TaaResolveReuseBlendRejectReason* reason) const {
    return preflightTaaResolveReuseAndBlend(desc, m_history, observedGeneration, reason);

bool TaaPass::shouldSkipResolveReuseAndBlend(const TaaResolveDesc& desc, u32 observedGeneration) const {
    return shouldSkipTaaResolveReuseAndBlend(desc, m_history, observedGeneration);

bool TaaPass::invalidateHistoryIfStale(u32 observedGeneration) {
    return m_history.invalidateHistoryIfStale(observedGeneration);
    return m_jitter.shouldSkipSyncToFrameIndex(frameIndex);

    return m_jitter.shouldSkipNdcOffset(m_desc.width, m_desc.height);

    const bool invalidated = m_history.invalidateHistoryIfStale(observedGeneration);
    if (invalidated) {
        m_resolve.resetBookkeeping();
    return invalidated;


TaaHistoryWarmupPreflight TaaPass::preflightHistoryWarmup() const {
    return preflightTaaHistoryWarmup(m_history);


TaaResolveBlendPreflight TaaPass::preflightResolveBlend(const TaaResolveDesc& desc) const {
    return preflightTaaResolveBlend(desc, m_history);







bool TaaPass::shouldSkipJitterSync() const {
    return shouldSkipTaaJitterSync(m_jitter.sequenceLength());

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);


bool TaaPass::preflightJitterNdc(u32 width, u32 height, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterNdc(width, height, m_jitter.sequenceLength(), reason);

bool TaaPass::preflightJitterNdcIfReady(TaaJitterGuardRejectReason* reason) const {





TaaJitterFramePreflight TaaPass::preflightJitterFrame(u32 frameIndex) const {
    return preflightTaaJitterFrame(frameIndex, m_desc.width, m_desc.height, m_jitter.sequenceLength());

TaaHistoryWarmupPreflight TaaPass::preflightHistoryWarmup(u32 observedGeneration) const {
    return preflightTaaHistoryWarmup(m_history, observedGeneration);

TaaResolveBlendPreflight TaaPass::preflightResolveBlendFrame(const TaaResolveDesc& desc) const {
    return preflightTaaResolveBlendFrame(desc, m_history);

TaaFrameGuardPreflight TaaPass::preflightFrameGuards(const TaaResolveDesc& desc, u32 observedGeneration) const {
    return preflightTaaFrameGuards(desc, m_history, observedGeneration);

bool TaaPass::isHistoryWarmupComplete() const {
    return isTaaHistoryWarmupComplete(m_history);






bool TaaPass::tryPreflightHistoryWarmup(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryWarmup(m_history, reason);


bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);





bool TaaPass::isWarmupComplete() const {

bool TaaPass::shouldSkipWarmup() const {

bool TaaPass::preflightTemporalResolve(const TaaResolveDesc& desc, TaaHistoryReuseBlockReason* reuseReason,
    return preflightTaaResolveTemporal(desc, m_history, reuseReason, blendReason);

bool TaaPass::shouldSkipTemporalResolve(const TaaResolveDesc& desc) const {



bool TaaPass::preflightResolveFrameGuards(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
    return preflightTaaResolveFrameGuards(desc, m_history, skipReason, blendReason);

bool TaaPass::shouldSkipResolveFrameGuards(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveFrameGuards(desc, m_history);







bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);

bool TaaPass::preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
                                    TaaResolveBlendRejectReason* blendReject) const {
    return preflightTaaResolveFrame(desc, m_history, skipReason, blendReject);
}

bool TaaPass::tryPreflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason& skipReason,
                                       TaaResolveBlendRejectReason& blendReject) const {
    return tryPreflightTaaResolveFrame(desc, m_history, skipReason, blendReject);
}

bool TaaPass::shouldSkipResolveFrame(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveFrame(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
                                    TaaResolveBlendRejectReason* blendReason) const {
    return preflightTaaResolveFrame(desc, m_history, skipReason, blendReason);
}

bool TaaPass::tryPreflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason& skipReason,
                                       TaaResolveBlendRejectReason& blendReason) const {
    return tryPreflightTaaResolveFrame(desc, m_history, skipReason, blendReason);
}

bool TaaPass::shouldSkipResolveFrame(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveFrame(desc, m_history);
}

bool TaaPass::preflightJitterAlignment(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAlignment(frameIndex, m_jitter.index(), m_jitter.monotonicFrameIndex(),
                                       m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAlignment(u32 frameIndex) const {
    return shouldSkipTaaJitterAlignment(frameIndex, m_jitter.index(), m_jitter.monotonicFrameIndex(),
                                        m_jitter.sequenceLength());
}

bool TaaPass::jitterNeedsSyncToFrameIndex(u32 frameIndex) const {
    return m_jitter.needsSyncToFrameIndex(frameIndex);
}

TaaHistoryWarmupState TaaPass::classifyHistoryWarmupState() const {
    return classifyTaaHistoryWarmupState(m_history);
}

bool TaaPass::preflightHistoryWarmupComplete(TaaHistoryWarmupState* state) const {
    return preflightTaaHistoryWarmupComplete(m_history, state);
}

bool TaaPass::shouldSkipHistoryWarmupComplete() const {
    return shouldSkipTaaHistoryWarmupComplete(m_history);
}

bool TaaPass::preflightHistoryForTemporalBlend(u32 observedGeneration,
                                               TaaHistoryReuseBlockReason* reason) const {
    return preflightTaaHistoryForTemporalBlend(m_history, observedGeneration, reason);
}

bool TaaPass::shouldSkipHistoryForTemporalBlend(u32 observedGeneration) const {
    return shouldSkipTaaHistoryForTemporalBlend(m_history, observedGeneration);
}

bool TaaPass::preflightResolveWithBlendWeights(const TaaResolveDesc& desc,
                                               TaaResolveSkipReason* skipReason,
                                               TaaResolveBlendRejectReason* blendReason) const {
    return preflightTaaResolveWithBlendWeights(desc, m_history, skipReason, blendReason);
}

bool TaaPass::shouldSkipResolveWithBlendWeights(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveWithBlendWeights(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightHistoryWarmup(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryWarmup(m_history, reason);
}

bool TaaPass::historyWarmupComplete() const {
    return m_history.warmupComplete();
}

bool TaaPass::tryPreflightResolveBlendPolicy(const TaaResolveDesc& desc,
                                             TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendPolicy(desc, m_history, reason);
}

bool TaaPass::shouldSkipResolveBlendPolicy(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveBlendPolicy(desc, m_history);
}

bool TaaPass::tryPreflightResolveCombined(const TaaResolveDesc& desc, TaaResolveSkipReason& skipReason,
                                          TaaResolveBlendRejectReason& blendReason) const {
    return tryPreflightTaaResolveCombined(desc, m_history, skipReason, blendReason);
}

bool TaaPass::shouldSkipResolveCombined(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveCombined(desc, m_history);
}

bool TaaPass::preflightJitterSyncAlignment(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterSyncAlignment(frameIndex, m_jitter.index(), m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterSyncAlignment(u32 frameIndex) const {
    return shouldSkipTaaJitterSyncAlignment(frameIndex, m_jitter.index(), m_jitter.sequenceLength());
}

bool TaaPass::jitterSlotAlignedToFrameIndex(u32 frameIndex) const {
    return m_jitter.slotAlignedToFrameIndex(frameIndex);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
                                    TaaResolveBlendRejectReason* blendReason) const {
    return preflightTaaResolveFrame(desc, m_history, skipReason, blendReason);
}

bool TaaPass::tryPreflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason& skipReason,
                                       TaaResolveBlendRejectReason& blendReason) const {
    return tryPreflightTaaResolveFrame(desc, m_history, skipReason, blendReason);
}

bool TaaPass::shouldSkipResolveFrame(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveFrame(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                              TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
                                    TaaResolveBlendRejectReason* blendReason) const {
    TaaResolveSkipReason localSkip = TaaResolveSkipReason::None;
    if (!tryPreflightTaaResolve(desc, m_history, localSkip)) {
        if (skipReason != nullptr) {
            *skipReason = localSkip;
        }
        return false;
    }

    TaaResolveBlendRejectReason localBlend = TaaResolveBlendRejectReason::None;
    if (!preflightTaaResolveBlendWeights(desc, m_history, &localBlend)) {
        if (blendReason != nullptr) {
            *blendReason = localBlend;
        }
        return false;
    }

    if (skipReason != nullptr) {
        *skipReason = TaaResolveSkipReason::None;
    }
    if (blendReason != nullptr) {
        *blendReason = TaaResolveBlendRejectReason::None;
    }
    return true;
}

bool TaaPass::shouldSkipResolveFrame(const TaaResolveDesc& desc) const {
    return !preflightResolveFrame(desc);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterNdcReject() const {
    return classifyTaaJitterNdcReject(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
}

TaaResolveBlendRejectReason TaaPass::classifyResolveBlendReject(const TaaResolveDesc& desc) const {
    return classifyTaaResolveBlendReject(desc, m_history);
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::preflightJitterAdvance(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::canAdvanceJitter() const {
    return m_jitter.canAdvance();
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                               TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::preflightJitterFrame(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    TaaJitterGuardRejectReason localReason = TaaJitterGuardRejectReason::None;
    if (!preflightJitterSync(frameIndex, &localReason)) {
        if (reason != nullptr) {
            *reason = localReason;
        }
        return false;
    }
    if (!preflightJitterNdc(&localReason)) {
        if (reason != nullptr) {
            *reason = localReason;
        }
        return false;
    }
    if (reason != nullptr) {
        *reason = TaaJitterGuardRejectReason::None;
    }
    return true;
}

bool TaaPass::shouldSkipJitterFrame(u32 frameIndex) const {
    return !preflightJitterFrame(frameIndex);
}

bool TaaPass::preflightHistoryFrame(TaaHistoryReuseBlockReason* reason) const {
    TaaHistoryReuseBlockReason localReason = TaaHistoryReuseBlockReason::None;
    if (!tryPreflightHistoryReadyForResolve(localReason)) {
        if (reason != nullptr) {
            *reason = localReason;
        }
        return false;
    }
    if (reason != nullptr) {
        *reason = TaaHistoryReuseBlockReason::None;
    }
    return true;
}

bool TaaPass::shouldSkipHistoryFrame() const {
    return !preflightHistoryFrame();
}

bool TaaPass::preflightResolveFrameGuards(const TaaResolveDesc& desc, u32 frameIndex,
                                          u32 observedGeneration, TaaResolveSkipReason* reason) const {
    (void)observedGeneration;
    if (!preflightJitterFrame(frameIndex)) {
        return false;
    }
    if (!preflightHistoryFrame()) {
        if (reason != nullptr) {
            *reason = TaaResolveSkipReason::HistoryNotReady;
        }
        return false;
    }
    TaaResolveBlendRejectReason blendReason = TaaResolveBlendRejectReason::None;
    if (!tryPreflightResolveBlendWeights(desc, blendReason)) {
        if (reason != nullptr) {
            *reason = TaaResolveSkipReason::InvalidDimensions;
        }
        return false;
    }
    TaaResolveSkipReason localReason = TaaResolveSkipReason::None;
    if (!tryPreflightResolve(desc, localReason)) {
        if (reason != nullptr) {
            *reason = localReason;
        }
        return false;
    }
    if (reason != nullptr) {
        *reason = TaaResolveSkipReason::None;
    }
    return true;
}

bool TaaPass::shouldSkipResolveFrameGuards(const TaaResolveDesc& desc, u32 frameIndex,
                                             u32 observedGeneration) const {
    return !preflightResolveFrameGuards(desc, frameIndex, observedGeneration);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
                                    TaaResolveBlendRejectReason* blendReason) const {
    if (!preflightTaaResolve(desc, m_history, skipReason)) {
        if (blendReason != nullptr) {
            *blendReason = TaaResolveBlendRejectReason::None;
        }
        return false;
    }
    return preflightTaaResolveBlendWeights(desc, m_history, blendReason);
}

bool TaaPass::tryPreflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason& skipReason,
                                       TaaResolveBlendRejectReason& blendReason) const {
    return preflightResolveFrame(desc, &skipReason, &blendReason);
}

bool TaaPass::shouldSkipResolveFrame(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolve(desc, m_history) || shouldSkipTaaResolveBlend(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                             TaaResolveBlendRejectReason& reason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                            TaaResolveBlendRejectReason& reason) const {
    return tryComputeTaaResolveBlendWeights(desc, m_history, outWeights, reason);
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterAdvance() const {
    return shouldSkipTaaJitterAdvance(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAdvance(m_jitter.sequenceLength(), reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::preflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason) const {
    return preflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::wouldSkipResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason) const {
    return m_resolve.wouldSkip(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
bool TaaPass::preflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason) const {
    return preflightTaaResolve(desc, m_history, reason);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
TaaResolveBlendPreflight TaaPass::preflightResolveBlend(const TaaResolveDesc& desc) const {
    return preflightTaaResolveBlend(desc, m_history);

bool TaaPass::isJitterSyncedToFrameIndex(u32 frameIndex) const {
    return m_jitter.isSyncedToFrameIndex(frameIndex);
bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendWeights* weights) const {
    return preflightTaaResolveBlend(desc, m_history, weights);
bool TaaPass::preflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason) const {
    return preflightTaaHistoryReuse(m_history, observedGeneration, reason);

bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc) const {

}

void TaaPass::stampObservedHistoryGeneration(TaaResolveDesc& desc) const {
    fuse::renderer::stampObservedHistoryGeneration(desc, m_history);
}

void TaaPass::sanitizeResolveDesc(TaaResolveDesc& desc) const {
    fuse::renderer::sanitizeTaaResolveDesc(desc, m_history);
}

bool TaaPass::isObservedHistoryGenerationCurrent(u32 observedGeneration) const {
    return !m_history.isHistoryStale(observedGeneration);
bool TaaPass::canResolveFrame(const TaaResolveDesc& desc) const {
    return canAttemptTaaResolve(desc, m_history);

bool TaaPass::prepareAndCanResolve(TaaResolveDesc& desc) const {
    return prepareTaaResolveDesc(desc, m_history);
}

bool TaaPass::canResolveFrame(const TaaResolveDesc& desc) const {
    return canAttemptTaaResolve(desc, m_history);
}

bool TaaPass::prepareAndCanResolve(TaaResolveDesc& desc) const {
    return prepareTaaResolveDesc(desc, m_history);
}

bool TaaPass::canReuseHistory(const TaaResolveDesc& desc) const {
    return taaHistoryIsReusable(m_history, desc);
}

f32 TaaPass::effectiveBlendForNextResolve(const TaaResolveDesc& desc) const {
    const bool firstFrame = m_history.needsWarmup();
    const bool historyReusable = taaHistoryIsReusable(m_history, desc);
    return computeEffectiveBlend(firstFrame, historyReusable, desc.params);
}

bool TaaPass::isJitterSyncedToFrameIndex(u32 frameIndex) const {
    return m_jitter.isSyncedToFrameIndex(frameIndex);
}

bool TaaPass::preflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseRejectReason* reason) const {
    return taaHistoryReusePreflight(m_history, observedGeneration, reason);
}

bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendPreflightRejectReason* reason) const {
    return taaResolveBlendPreflight(desc, m_history, reason);
}

bool TaaPass::resolveFrame(const TaaResolveDesc& desc, void* cudaStream) {
    if (!m_stats.ready) {
        m_stats.message = "TAA pass not ready";
        return false;
    }

    TaaResolveDesc stampedDesc = desc;
    stampObservedHistoryGeneration(stampedDesc);

    if (!m_resolve.resolve(stampedDesc, m_history, cudaStream)) {
    TaaResolveDesc working = desc;
    stampObservedHistoryGeneration(working);

    TaaResolveSkipReason skipReason = TaaResolveSkipReason::None;
    if (m_resolve.wouldSkip(working, m_history, &skipReason)) {
        m_stats.message = std::string("TAA pass resolve skipped — ") + taaResolveSkipReasonLabel(skipReason);
        return false;
    }

    if (!m_resolve.resolve(working, m_history, cudaStream)) {
        m_stats.message = m_resolve.lastMessage();
        return false;
    }

    ++m_stats.framesResolved;
    m_stats.message = m_resolve.lastMessage();
    return true;
}

void resetTaaPassGraphStorage() {
    g_taaPassRegistered = false;
}

void addTaaPassToGraph(RenderGraph& graph) {
    if (g_taaPassRegistered) {
        return;
    }

    g_taaPassAccesses[0].texture = {RenderGraph::kBackbufferTextureId};
    g_taaPassAccesses[0].access = RGResourceAccess::ShaderRead;
    g_taaPassAccesses[1].texture = {RenderGraph::kBackbufferTextureId};
    g_taaPassAccesses[1].access = RGResourceAccess::ShaderWrite;

    RGPassDesc pass{};
    pass.name = "taa_resolve";
    pass.execute = executeTaaPass;
    pass.userData = &g_taaPassUserData;
    pass.textureAccesses = g_taaPassAccesses;
    pass.textureAccessCount = 2;
    graph.addPass(pass);
    g_taaPassRegistered = true;
}

} // namespace fuse::renderer
