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

bool TaaPass::jitterAlignedToFrameIndex(u32 frameIndex) const {
    return m_jitter.isAlignedToFrameIndex(frameIndex);
}

void TaaPass::invalidateHistory() {
    m_history.invalidateHistory();
    m_resolve.resetBookkeeping();
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

u32 TaaPass::warmupFramesRemaining() const {
    return m_history.warmupFramesRemaining();
}

bool TaaPass::canReuseHistory() const {
    return m_history.canReuseHistory();
}

bool TaaPass::historyReuseReady(u32 observedGeneration) const {
    return m_history.reuseReady(observedGeneration);
}

bool TaaPass::shouldSkipHistoryReuse(u32 observedGeneration) const {
    return shouldSkipTaaHistoryReuse(m_history, observedGeneration);
}

bool TaaPass::historyBlendAllowed() const {
    return taaHistoryBlendAllowed(!m_history.hasValidHistory(), m_history);
}

bool TaaPass::canProduceJitterNdc() const {
    return m_jitter.canProduceNdcOffset(m_desc.width, m_desc.height);
}

bool TaaPass::advanceJitterIfReady() {
    if (!m_jitter.advanceIfReady()) {
        return false;
    }
    m_stats.lastJitterNdc = currentJitterNdc();
    return true;
}

TaaBlendWeights TaaPass::expectedResolveBlendWeights(const TaaResolveDesc& desc) const {
    return computeTaaResolveBlendWeights(desc, m_history);
}

bool TaaPass::resolveWouldReuseHistory(const TaaResolveDesc& desc) const {
    return taaResolveCanReuseHistory(desc, m_history) && taaResolveAppliesHistoryBlend(desc, m_history);
}

TaaHistoryReuseBlockReason TaaPass::classifyHistoryReuseBlock(u32 observedGeneration) const {
    return classifyTaaHistoryReuseBlock(m_history, observedGeneration);
}

bool TaaPass::preflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason) const {
    return preflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, reason);
}

bool TaaPass::tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryReadyForResolve(m_history, reason);
}

bool TaaPass::preflightResolveBlendWeights(const TaaResolveDesc& desc,
                                           TaaResolveBlendRejectReason* reason) const {
    return preflightTaaResolveBlendWeights(desc, m_history, reason);
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

bool TaaPass::shouldSkipResolveBlend(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveBlend(desc, m_history);
}

bool TaaPass::preflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

TaaJitterGuardRejectReason TaaPass::classifyJitterSyncReject() const {
    return classifyTaaJitterSyncReject(m_jitter.sequenceLength());
}

bool TaaPass::tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterSync(u32 frameIndex) const {
    return shouldSkipTaaJitterSync(frameIndex, m_jitter.sequenceLength());
}

bool TaaPass::preflightJitterNdc(TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), reason);
}

bool TaaPass::shouldSkipJitterNdc() const {
    return shouldSkipTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength());
}

bool TaaPass::shouldSkipHistoryWarmup() const {
    return shouldSkipTaaHistoryWarmup(m_history);
}

bool TaaPass::historyReadyForResolve() const {
    return m_history.readyForResolve();
}

bool TaaPass::shouldSkipHistoryResolve() const {
    return shouldSkipTaaHistoryResolve(m_history);
}

bool TaaPass::shouldSkipResolve(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolve(desc, m_history);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
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

TaaJitterGuardRejectReason TaaPass::classifyJitterAdvanceReject() const {
    return classifyTaaJitterAdvanceReject(m_jitter.sequenceLength());
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

bool TaaPass::wouldSkipResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason) const {
    return m_resolve.wouldSkip(desc, m_history, reason);
}

TaaResolveSkipReason TaaPass::classifyResolveSkip(const TaaResolveDesc& desc) const {
    return classifyTaaResolveSkip(desc, m_history);
}

bool TaaPass::tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const {
    return tryPreflightTaaResolve(desc, m_history, reason);
}

void TaaPass::stampObservedHistoryGeneration(TaaResolveDesc& desc) const {
    fuse::renderer::stampObservedHistoryGeneration(desc, m_history);
}

void TaaPass::sanitizeResolveDesc(TaaResolveDesc& desc) const {
    fuse::renderer::sanitizeTaaResolveDesc(desc, m_history);
}

bool TaaPass::isObservedHistoryGenerationCurrent(u32 observedGeneration) const {
    return !m_history.isHistoryStale(observedGeneration);
}

bool TaaPass::resolveFrame(const TaaResolveDesc& desc, void* cudaStream) {
    if (!m_stats.ready) {
        m_stats.message = "TAA pass not ready";
        return false;
    }

    TaaResolveDesc stampedDesc = desc;
    stampObservedHistoryGeneration(stampedDesc);

    if (!m_resolve.resolve(stampedDesc, m_history, cudaStream)) {
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

// --- deepen additive from deepen-b59-taa-history-resolve-skip-b406 ---
    if (m_resolve.wouldSkip(working, m_history, &skipReason)) {

// --- deepen additive from deepen-b59-taa-guards-8293 ---
TaaHistoryWarmupPreflight TaaPass::preflightHistoryWarmup() const {
    return preflightTaaHistoryWarmup(m_history);
TaaHistoryReusePreflight TaaPass::preflightHistoryReuse(u32 observedGeneration) const {
    return preflightTaaHistoryReuse(m_history, observedGeneration);
TaaHistoryReusePreflight TaaPass::preflightHistoryReuseForDesc(const TaaResolveDesc& desc) const {
    return preflightTaaHistoryReuseForDesc(m_history, desc);
TaaJitterSyncPreflight TaaPass::preflightJitterSync(u32 frameIndex) const {
    return preflightTaaJitterSync(m_jitter, frameIndex, m_desc.width, m_desc.height);
TaaResolveBlendPreflight TaaPass::preflightResolveBlend(const TaaResolveDesc& desc) const {
    return preflightTaaResolveBlendForDesc(desc, m_history);

// --- deepen additive from deepen-b59-taa-jitter-history-preflights-ddf1 ---
bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendWeights* out) const {
    return preflightTaaResolveBlend(desc, m_history, out);

// --- deepen additive from deepen-b59-taa-guards-94f6 ---
TaaHistoryReuseRejectReason TaaPass::classifyHistoryReuseReject(u32 observedGeneration) const {
    return classifyTaaHistoryReuseReject(m_history, observedGeneration);
bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc, TaaResolveBlendPreflight* out) const {

// --- deepen additive from deepen-b59-taa-guards-f9b0 ---
    return preflightTaaResolveBlend(desc, m_history);

// --- deepen additive from deepen-b59-taa-guards-2b1e ---
bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendWeights* weights) const {
    return preflightTaaResolveBlend(desc, m_history, weights);

// --- deepen additive from deepen-b59-taa-guards-5b8c ---
bool TaaPass::preflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseRejectReason* reason) const {
    return taaHistoryReusePreflight(m_history, observedGeneration, reason);
bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendPreflightRejectReason* reason) const {
    return taaResolveBlendPreflight(desc, m_history, reason);

// --- deepen additive from deepen-b59-taa-guards-a831 ---
bool TaaPass::preflightHistoryReuse(u32 observedGeneration) const {
    return m_history.preflightReuse(observedGeneration);
                                    TaaResolveBlendPreflightRejectReason* reason) const {
    return preflightTaaResolveBlend(desc, m_history, reason);

// --- deepen additive from deepen-taa-b59-guards-f7b5 ---
bool TaaPass::preflightResolveBlend(const TaaResolveDesc& desc) const {

// --- deepen additive from deepen-b59-taa-guards-27d7 ---
bool TaaPass::preflightJitterSync(u32 frameIndex, TaaJitterSyncBlockReason* reason) const {
    return m_jitter.preflightSync(frameIndex, m_desc.width, m_desc.height, reason);
bool TaaPass::preflightResolveWithBlend(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
                                        TaaResolveBlendRejectReason* blendReason) const {
    return preflightTaaResolveWithBlend(desc, m_history, skipReason, blendReason);

// --- deepen additive from deepen-b59-taa-guards-117f ---
bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason) const {
    return preflightTaaHistoryWarmup(m_history, reason);
bool TaaPass::preflightHistoryReuseForResolve(const TaaResolveDesc& desc,
    return preflightTaaHistoryReuseForResolve(desc, m_history, reason);
    return preflightTaaJitterSync(m_jitter, frameIndex, reason);
bool TaaPass::preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
                                    TaaResolveBlendRejectReason* blendRejectReason) const {
    return preflightTaaResolveFrame(desc, m_history, skipReason, blendRejectReason);

// --- deepen additive from deepen-fuse-b59-taa-cd32 ---
    return m_jitter.preflightSyncToFrameIndex(frameIndex);
TaaJitterSyncRejectReason TaaPass::classifyJitterSyncReject(u32 frameIndex) const {
    return m_jitter.classifySyncReject(frameIndex);
bool TaaPass::preflightJitterSync(u32 frameIndex, TaaJitterSyncRejectReason* reason) const {
    return m_jitter.preflightSyncToFrameIndex(frameIndex, reason);
bool TaaPass::canPreflightHistoryReuse(u32 observedGeneration) const {
    return canPreflightTaaHistoryReuse(m_history, observedGeneration);
bool TaaPass::canPreflightHistoryWarmup() const {
    return canPreflightTaaHistoryWarmup(m_history);
bool TaaPass::canPreflightResolveBlendWeights(const TaaResolveDesc& desc) const {
    return canPreflightTaaResolveBlendWeights(desc, m_history);
bool TaaPass::tryExpectedResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& out,
    return tryComputeTaaResolveBlendWeights(desc, m_history, out, reason);
bool TaaPass::preflightResolveTemporalBlend(const TaaResolveDesc& desc,
                                            TaaResolveTemporalRejectReason* reason) const {
    return preflightTaaResolveTemporalBlend(desc, m_history, reason);
bool TaaPass::canPreflightResolveTemporalBlend(const TaaResolveDesc& desc) const {
    return canPreflightTaaResolveTemporalBlend(desc, m_history);

// --- deepen additive from deepen-b59-taa-guards-298c ---
    return preflightTaaResolveFrame(desc, m_history, skipReason, blendReason);

// --- deepen additive from deepen-b59-taa-guards-1d2e ---
    return m_jitter.preflightSync(frameIndex, reason);
bool TaaPass::preflightHistoryReuseForDesc(const TaaResolveDesc& desc,
    return preflightTaaHistoryReuseForDesc(desc, m_history, reason);
bool TaaPass::preflightResolveGuards(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
    return preflightTaaResolveGuards(desc, m_history, skipReason, blendRejectReason);

// --- deepen additive from deepen-b59-taa-guards-3c58 ---
    const TaaJitterSyncRejectReason reject = m_jitter.classifySyncReject(frameIndex);
    return reject == TaaJitterSyncRejectReason::None;
bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupPhase* phase) const {
    return preflightTaaHistoryWarmup(m_history, phase);
                                            TaaResolveBlendRejectReason* blendReason,
    return preflightTaaResolveTemporalBlend(desc, m_history, blendReason, reuseReason);

// --- deepen additive from deepen-b59-taa-guards-d966 ---
bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupState* state) const {
    return preflightTaaHistoryWarmup(m_history, state);

// --- deepen additive from deepen-b59-taa-guards-53dc ---
bool TaaPass::preflightJitterAlignment(u32 expectedFrameIndex) const {
    return preflightTaaJitterAlignment(expectedFrameIndex, m_jitter);
bool TaaPass::preflightResolveDesc(const TaaResolveDesc& desc, TaaResolveDescPreflight* result) const {
    return preflightTaaResolveDesc(desc, m_history, result);

// --- deepen additive from deepen-b59-taa-guards-efe8 ---
bool TaaPass::trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncBlockReason& outReason) {
    if (!m_jitter.trySyncToFrameIndexIfViewportReady(frameIndex, m_desc.width, m_desc.height, outReason)) {
bool TaaPass::preflightResolveTemporalAccumulation(const TaaResolveDesc& desc,
                                                   TaaResolveTemporalPreflight* result) const {
    return preflightTaaResolveTemporalAccumulation(desc, m_history, result);

// --- deepen additive from deepen-b59-taa-guards-8394 ---
bool TaaPass::trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncRejectReason* reason) {
bool TaaPass::tryCanBeginTemporalReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason) const {
bool TaaPass::preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveFramePreflight* out) const {
    return preflightTaaResolveFrame(desc, m_history, out);
bool TaaPass::tryExpectedResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,

// --- deepen additive from deepen-b59-taa-guards-9737 ---
bool TaaPass::trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncRejectReason& outReason) {
    if (!fuse::renderer::trySyncJitterToFrameIndexIfReady(m_jitter, frameIndex, outReason)) {
bool TaaPass::tryPreflightHistoryReuseForResolve(const TaaResolveDesc& desc,
    return tryPreflightTaaHistoryReuseForResolve(desc, m_history, outReason);

// --- deepen additive from deepen-taa-b59-guards-fd0d ---
    return m_jitter.preflightSyncToFrameIndex(frameIndex, m_desc.width, m_desc.height, reason);
bool TaaPass::preflightResolveHistoryReuse(const TaaResolveDesc& desc,
    return preflightTaaResolveHistoryReuse(desc, m_history, reason);

// --- deepen additive from deepen-b59-taa-guards-0400 ---
bool TaaPass::preflightResolveTemporal(const TaaResolveDesc& desc,
    return preflightTaaResolveTemporal(desc, m_history, reuseReason, blendReason);

// --- deepen additive from deepen-b59-taa-guards-614c ---
bool TaaPass::wouldSkipHistoryReuse(u32 observedGeneration) const {
    return wouldSkipTaaHistoryReuse(m_history, observedGeneration);
bool TaaPass::tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& outReason) const {
    return tryPreflightTaaHistoryReuse(m_history, observedGeneration, outReason);
                                              TaaResolveBlendRejectReason& outReason) const {
    return tryPreflightTaaResolveBlendWeights(desc, m_history, outReason);
bool TaaPass::wouldSkipJitterSync(u32 frameIndex) const {
    return m_jitter.wouldSkipSyncToFrameIndex(frameIndex);
bool TaaPass::trySyncJitterToFrameIndex(u32 frameIndex, TaaJitterSyncRejectReason& outReason) {
    if (!m_jitter.trySyncToFrameIndexIfReady(frameIndex, outReason)) {

// --- deepen additive from deepen-b59-taa-guards-ceb9 ---
bool TaaPass::preflightHistoryWarmup(TaaHistoryWarmupRejectReason* reason) const {

// --- deepen additive from deepen-b59-taa-guards-3780 ---
bool TaaPass::tryPreflightHistoryWarmup(TaaHistoryWarmupBlockReason& reason) const {
    return tryPreflightTaaHistoryWarmup(m_history, reason);

// --- deepen additive from deepen-b59-taa-guards-b05b ---
    return preflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), &reason);
    return preflightTaaJitterNdc(m_desc.width, m_desc.height, m_jitter.sequenceLength(), &reason);

// --- deepen additive from deepen-b59-taa-guards-bd40 ---
bool TaaPass::wouldSkipResolveBlend(const TaaResolveDesc& desc) const {
    return wouldSkipTaaResolveBlend(desc, m_history);

// --- deepen additive from deepen-b59-taa-guards-aa69 ---
    return preflightTaaResolveTemporal(desc, m_history, reason);

// --- deepen additive from deepen-b59-taa-guards-6ba7 ---
bool TaaPass::preflightHistoryWarmup(TaaHistoryReuseBlockReason* reason) const {

// --- deepen additive from deepen-b59-taa-guards-e107 ---
bool TaaPass::tryCurrentJitterNdcIfReady(fuse::math::Vec2& out, TaaJitterGuardRejectReason& reason) const {
    return m_jitter.tryCurrentNdcOffsetIfReady(m_desc.width, m_desc.height, out, reason);
bool TaaPass::trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterGuardRejectReason& reason) {
    if (!m_jitter.trySyncToFrameIndexIfReady(frameIndex, reason)) {

// --- deepen additive from deepen-taa-b59-guards-ea2b ---
bool TaaPass::preflightTemporalBlend(const TaaResolveDesc& desc, TaaHistoryReuseBlockReason* reuseReason,
    return preflightTaaTemporalBlend(desc, m_history, reuseReason, blendReason);

// --- deepen additive from deepen-b59-taa-guards-eb8c ---
bool TaaPass::preflightResolveReuseAndBlend(const TaaResolveDesc& desc, u32 observedGeneration,
                                            TaaResolveReuseBlendRejectReason* reason) const {
    return preflightTaaResolveReuseAndBlend(desc, m_history, observedGeneration, reason);

// --- deepen additive from deepen-b59-taa-guards-6172 ---
bool TaaPass::preflightTemporalResolve(const TaaResolveDesc& desc,
                                       TaaTemporalGuardRejectReason* reason) const {
    return preflightTaaTemporalResolve(desc, m_history, reason);

// --- deepen additive from deepen-b59-taa-guards-61ca ---
bool TaaPass::tryPreflightHistoryWarmup(TaaHistoryReuseBlockReason& reason) const {

// --- deepen additive from deepen-b59-taa-guards-7381 ---
bool TaaPass::preflightTemporalResolveGuards(const TaaResolveDesc& desc, u32 observedGeneration,
    return preflightTaaTemporalResolveGuards(desc, m_history, observedGeneration, reuseReason, blendReason);
bool TaaPass::preflightJitterNdc(u32 width, u32 height, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterNdc(width, height, m_jitter.sequenceLength(), reason);
bool TaaPass::preflightJitterNdcIfReady(TaaJitterGuardRejectReason* reason) const {

// --- deepen additive from b59-taa-deepen-guards-602b ---
TaaJitterFramePreflight TaaPass::preflightJitterFrame(u32 frameIndex) const {
    return preflightTaaJitterFrame(frameIndex, m_desc.width, m_desc.height, m_jitter.sequenceLength());
TaaHistoryWarmupPreflight TaaPass::preflightHistoryWarmup(u32 observedGeneration) const {
    return preflightTaaHistoryWarmup(m_history, observedGeneration);
TaaResolveBlendPreflight TaaPass::preflightResolveBlendFrame(const TaaResolveDesc& desc) const {
    return preflightTaaResolveBlendFrame(desc, m_history);
TaaFrameGuardPreflight TaaPass::preflightFrameGuards(const TaaResolveDesc& desc, u32 observedGeneration) const {
    return preflightTaaFrameGuards(desc, m_history, observedGeneration);

// --- deepen additive from deepen-b59-taa-guards-48f5 ---
bool TaaPass::preflightTemporalResolve(const TaaResolveDesc& desc, TaaHistoryReuseBlockReason* reuseReason,

// --- deepen additive from deepen-taa-b59-guards-d5f5 ---
bool TaaPass::preflightResolveFrameGuards(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason,
    return preflightTaaResolveFrameGuards(desc, m_history, skipReason, blendReason);

// --- deepen additive from deepen-taa-b59-guards-2768 ---
                                        TaaResolveWithBlendRejectReason* reason) const {
    return preflightTaaResolveWithBlend(desc, m_history, reason);
bool TaaPass::preflightJitterAlignment(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterAlignment(m_jitter, frameIndex, reason);

// --- deepen additive from deepen-b59-taa-guards-2031 ---
bool TaaPass::trySyncJitterToFrameIndex(u32 frameIndex, TaaJitterGuardRejectReason& reason) {
    if (!trySyncTaaJitter(m_jitter, frameIndex, reason)) {
bool TaaPass::tryAdvanceJitterIfReady(TaaJitterGuardRejectReason& reason) {
    if (!tryAdvanceTaaJitter(m_jitter, reason)) {
bool TaaPass::preflightJitterSlot(u32 slot, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterSlot(slot, m_jitter.sequenceLength(), reason);
                                    TaaResolveBlendRejectReason* blendReject) const {
    return preflightTaaResolveFrame(desc, m_history, skipReason, blendReject);
bool TaaPass::tryPreflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason& skipReason,
                                       TaaResolveBlendRejectReason& blendReject) const {
    return tryPreflightTaaResolveFrame(desc, m_history, skipReason, blendReject);

// --- deepen additive from deepen-b59-taa-guards-985f ---
bool TaaPass::tryPreflightJitterAlignment(u32 frameIndex, TaaJitterGuardRejectReason& reason) const {
    return tryPreflightTaaJitterAlignment(m_jitter, frameIndex, reason);

// --- deepen additive from deepen-taa-b59-guards-ec2a ---
bool TaaPass::tryComputeExpectedResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
bool TaaPass::preflightResolveTemporal(const TaaResolveDesc& desc, u32 observedGeneration,
    return preflightTaaResolveTemporal(desc, m_history, observedGeneration, reason);
bool TaaPass::preflightJitterAligned(u32 frameIndex, TaaJitterAlignmentRejectReason* reason) const {
    return preflightTaaJitterAligned(frameIndex, m_jitter, reason);
bool TaaPass::preflightHistoryWarmupComplete(TaaHistoryWarmupRejectReason* reason) const {
    return preflightTaaHistoryWarmupComplete(m_history, reason);

// --- deepen additive from deepen-taa-b59-guards-4701 ---
                                       TaaResolveBlendRejectReason& blendReason) const {
    return tryPreflightTaaResolveFrame(desc, m_history, skipReason, blendReason);

// --- deepen additive from deepen-taa-b59-guards-4c9c ---
    return preflightTaaResolveTemporalBlend(desc, m_history, reuseReason, blendReason);
bool TaaPass::tryPreflightResolveTemporalBlend(const TaaResolveDesc& desc,
    return tryPreflightTaaResolveTemporalBlend(desc, m_history, reuseReason, blendReason);
bool TaaPass::preflightHistoryWarmupSatisfied(TaaHistoryReuseBlockReason* reason) const {
    return preflightTaaHistoryWarmupSatisfied(m_history, reason);
bool TaaPass::tryPreflightHistoryWarmupSatisfied(TaaHistoryReuseBlockReason& reason) const {
    return tryPreflightTaaHistoryWarmupSatisfied(m_history, reason);

// --- deepen additive from deepen-b59-taa-guards-de0e ---
    return preflightTaaJitterAlignment(frameIndex, m_jitter.index(), m_jitter.monotonicFrameIndex(),
bool TaaPass::preflightHistoryWarmupComplete(TaaHistoryWarmupState* state) const {
    return preflightTaaHistoryWarmupComplete(m_history, state);
bool TaaPass::preflightHistoryForTemporalBlend(u32 observedGeneration,
    return preflightTaaHistoryForTemporalBlend(m_history, observedGeneration, reason);
bool TaaPass::preflightResolveWithBlendWeights(const TaaResolveDesc& desc,
    return preflightTaaResolveWithBlendWeights(desc, m_history, skipReason, blendReason);

// --- deepen additive from deepen-b59-taa-guards-2589 ---
bool TaaPass::tryPreflightResolveBlendPolicy(const TaaResolveDesc& desc,
    return tryPreflightTaaResolveBlendPolicy(desc, m_history, reason);
bool TaaPass::tryPreflightResolveCombined(const TaaResolveDesc& desc, TaaResolveSkipReason& skipReason,
    return tryPreflightTaaResolveCombined(desc, m_history, skipReason, blendReason);
bool TaaPass::preflightJitterSyncAlignment(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterSyncAlignment(frameIndex, m_jitter.index(), m_jitter.sequenceLength(), reason);

// --- deepen additive from deepen-taa-b59-guards-61db ---
    return m_jitter.preflightAlignmentToFrameIndex(frameIndex, reason);
                                            TaaResolveTemporalBlendRejectReason* reason) const {
bool TaaPass::preflightJitterSyncAndNdc(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterSyncAndNdc(frameIndex, m_desc.width, m_desc.height, m_jitter.sequenceLength(),
bool TaaPass::preflightHistoryTemporalSample(u32 observedGeneration,
    return preflightTaaHistoryTemporalSample(m_history, observedGeneration, reason);
