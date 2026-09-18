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

void TaaPass::advanceJitter() {
    m_jitter.advance();
    m_stats.lastJitterNdc = currentJitterNdc();
}

void TaaPass::syncJitterToFrameIndex(u32 frameIndex) {
    m_jitter.syncToFrameIndex(frameIndex);
    m_stats.lastJitterNdc = currentJitterNdc();
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

bool TaaPass::canReuseHistory() const {
    return m_history.canReuseHistory();
}

bool TaaPass::historyBlendAllowed() const {
    return taaHistoryBlendAllowed(!m_history.hasValidHistory(), m_history);
}

bool TaaPass::canProduceJitterNdc() const {
    return m_jitter.canProduceNdcOffset(m_desc.width, m_desc.height);
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
