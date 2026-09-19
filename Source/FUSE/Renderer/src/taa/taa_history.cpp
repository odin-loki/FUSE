#include <fuse/renderer/taa/taa_history.hpp>

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

bool taaHistoryReuseAllowed(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return taaHistoryCanReuse(history) && !history.isHistoryStale(observedGeneration);
}

bool taaHistoryNeedsWarmup(const TaaHistoryBuffer& history) {
    return !history.hasValidHistory();
}

bool taaHistoryReadyForResolve(const TaaHistoryBuffer& history) {
    return history.isReady();
}

u32 taaHistoryWarmupFramesRemaining(const TaaHistoryBuffer& history) {
    return taaHistoryNeedsWarmup(history) ? 1u : 0u;
}

bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return taaHistoryNeedsWarmup(history);
}

bool shouldSkipTaaHistoryResolve(const TaaHistoryBuffer& history) {
    return !taaHistoryReadyForResolve(history);
}

bool tryPreflightTaaHistoryReadyForResolve(const TaaHistoryBuffer& history,
                                           TaaHistoryReuseBlockReason& reason) {
    if (!history.isReady()) {
        reason = TaaHistoryReuseBlockReason::NotReady;
        return false;
    }
    reason = TaaHistoryReuseBlockReason::None;
    return true;
}

bool TaaHistoryBuffer::readyForResolve() const {
    return taaHistoryReadyForResolve(*this);
}

bool preflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
                              TaaHistoryReuseBlockReason* reason) {
    const TaaHistoryReuseBlockReason block = classifyTaaHistoryReuseBlock(history, observedGeneration);
    if (reason != nullptr) {
        *reason = block;
    }
    return block == TaaHistoryReuseBlockReason::None;
}

bool tryPreflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
                                 TaaHistoryReuseBlockReason& reason) {
    reason = classifyTaaHistoryReuseBlock(history, observedGeneration);
    return reason == TaaHistoryReuseBlockReason::None;
}

bool shouldSkipTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return !preflightTaaHistoryReuse(history, observedGeneration);
}

bool taaHistoryReuseReady(const TaaHistoryBuffer& history, u32 observedGeneration) {
    return preflightTaaHistoryReuse(history, observedGeneration);
}

bool TaaHistoryBuffer::canReuseHistory() const {
    return taaHistoryCanReuse(*this);
}

u32 TaaHistoryBuffer::warmupFramesRemaining() const {
    return taaHistoryWarmupFramesRemaining(*this);
}

bool TaaHistoryBuffer::reuseReady(u32 observedGeneration) const {
    return taaHistoryReuseReady(*this, observedGeneration);
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

bool TaaHistoryBuffer::matchesDimensions(u32 width, u32 height) const {
    return m_desc.width == width && m_desc.height == height;
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

// --- deepen additive from deepen-b59-taa-guards-8293 ---
bool TaaHistoryWarmupPreflight::readyForResolve() const {
bool TaaHistoryWarmupPreflight::warmupComplete() const {
bool TaaHistoryReusePreflight::canReuseHistory() const {
TaaHistoryWarmupPreflight preflightTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    TaaHistoryWarmupPreflight preflight{};
TaaHistoryReusePreflight preflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration) {
    TaaHistoryReusePreflight preflight{};
    preflight.warmup = preflightTaaHistoryWarmup(history);
TaaHistoryReusePreflight preflightTaaHistoryReuseForDesc(const TaaHistoryBuffer& history,
        TaaHistoryReusePreflight preflight = preflightTaaHistoryReuse(history, history.invalidateGeneration());
    return preflightTaaHistoryReuse(history, desc.observed_history_generation);

// --- deepen additive from deepen-b59-taa-jitter-history-preflights-ddf1 ---
bool taaHistoryReusePreflightPasses(const TaaHistoryBuffer& history, u32 observedGeneration) {

// --- deepen additive from deepen-b59-taa-guards-94f6 ---
const char* taaHistoryReuseRejectReasonLabel(TaaHistoryReuseRejectReason reason) {
    case TaaHistoryReuseRejectReason::None:
    case TaaHistoryReuseRejectReason::NotReady:
    case TaaHistoryReuseRejectReason::NeedsWarmup:
    case TaaHistoryReuseRejectReason::StaleGeneration:
TaaHistoryReuseRejectReason classifyTaaHistoryReuseReject(const TaaHistoryBuffer& history, u32 observedGeneration) {
        return TaaHistoryReuseRejectReason::NotReady;
        return TaaHistoryReuseRejectReason::NeedsWarmup;
        return TaaHistoryReuseRejectReason::StaleGeneration;
    return TaaHistoryReuseRejectReason::None;
bool tryTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration, TaaHistoryReuseRejectReason* reason) {
    const TaaHistoryReuseRejectReason reject = classifyTaaHistoryReuseReject(history, observedGeneration);
    return reject == TaaHistoryReuseRejectReason::None;

// --- deepen additive from deepen-b59-taa-guards-94db ---
bool preflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration) {

// --- deepen additive from deepen-b59-taa-guards-5b8c ---
        return TaaHistoryReuseRejectReason::HistoryNotReady;
        return TaaHistoryReuseRejectReason::HistoryNotWarmed;
bool taaHistoryReusePreflight(const TaaHistoryBuffer& history, u32 observedGeneration,

// --- deepen additive from deepen-b59-taa-guards-a831 ---
bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history) {
bool TaaHistoryBuffer::preflightReuse(u32 observedGeneration) const {
    return preflightTaaHistoryReuse(*this, observedGeneration);

// --- deepen additive from deepen-b59-taa-guards-27d7 ---
    return taaHistoryWarmupComplete(history) && preflightTaaHistoryReuse(history, observedGeneration);
bool preflightTaaJitterSync(u32 /*frameIndex*/, u32 width, u32 height, u32 sequenceLength,

// --- deepen additive from deepen-b59-taa-guards-117f ---
bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason) {

// --- deepen additive from deepen-fuse-b59-taa-cd32 ---
bool canPreflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration) {
bool canPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history) {
    return preflightTaaHistoryWarmup(history);
