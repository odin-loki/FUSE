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

bool TaaPass::preflightResolveBlendWeights(const TaaResolveDesc& desc,
                                           TaaResolveBlendRejectReason* reason) const {
    return preflightTaaResolveBlendWeights(desc, m_history, reason);
}

bool TaaPass::shouldSkipResolveBlend(const TaaResolveDesc& desc) const {
    return shouldSkipTaaResolveBlend(desc, m_history);
}

bool TaaPass::preflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason* reason) const {
    return preflightTaaJitterSync(frameIndex, m_jitter.sequenceLength(), reason);
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

bool TaaPass::wouldSkipResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason) const {
    return m_resolve.wouldSkip(desc, m_history, reason);
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
