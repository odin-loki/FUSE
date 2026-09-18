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
    return !history.isReady() || history.needsWarmup();
}

bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history) {
    return history.warmupComplete();
}

bool taaResolveWouldBeFirstFrame(const TaaHistoryBuffer& history) {
    return !history.hasValidHistory();
}

bool TaaHistoryBuffer::warmupComplete() const {
    return m_ready && m_validity.hasValidHistory;
}

bool TaaHistoryBuffer::canReuseHistory() const {
    return taaHistoryCanReuse(*this);
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
