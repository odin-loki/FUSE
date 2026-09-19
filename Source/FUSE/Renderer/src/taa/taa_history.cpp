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

bool taaHistoryReadyForResolve(const TaaHistoryBuffer& history) {
    return history.isReady();

u32 taaHistoryWarmupFramesRemaining(const TaaHistoryBuffer& history) {
    return taaHistoryNeedsWarmup(history) ? 1u : 0u;

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return taaHistoryNeedsWarmup(history);

bool shouldSkipTaaHistoryResolve(const TaaHistoryBuffer& history) {
    return !taaHistoryReadyForResolve(history);

bool tryPreflightTaaHistoryReadyForResolve(const TaaHistoryBuffer& history,
                                           TaaHistoryReuseBlockReason& reason) {
    if (!history.isReady()) {
        reason = TaaHistoryReuseBlockReason::NotReady;
        return false;
    reason = TaaHistoryReuseBlockReason::None;
    return true;

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

bool TaaHistoryBuffer::canReuseHistory() const {
    return taaHistoryCanReuse(*this);
}

u32 TaaHistoryBuffer::warmupFramesRemaining() const {
    return taaHistoryWarmupFramesRemaining(*this);
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
