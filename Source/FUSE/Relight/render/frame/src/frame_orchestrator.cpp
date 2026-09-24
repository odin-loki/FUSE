// FUSE Relight RL-4.1: frame orchestration (see frame_orchestrator.hpp).
#include <fuse/relight/render/frame/frame_orchestrator.hpp>

#include <fuse/relight/render/frame/vk_dispatch.hpp> // VK_* values

#include <algorithm>

namespace fuse::relight::render::frame {

namespace {

// D3D9 values (relight_tap.hpp carries them raw).
constexpr std::uint32_t kD3dUsageRenderTarget = 0x1u;
constexpr std::uint32_t kD3dUsageDepthStencil = 0x2u;
constexpr std::uint32_t kD3dPoolSystemMem = 2u;
constexpr std::uint32_t kD3dPoolScratch = 3u;
constexpr std::uint32_t kD3dRtypeTexture = 3u;
constexpr std::uint32_t kD3dRtypeVolumeTexture = 4u;
constexpr std::uint32_t kD3dRtypeCubeTexture = 5u;

constexpr std::uint32_t kFrameUsage =
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

} // namespace

bool FrameOrchestrator::swappable(const tap::TextureDesc& t) {
    const bool textureType =
        t.type == kD3dRtypeTexture || t.type == kD3dRtypeVolumeTexture || t.type == kD3dRtypeCubeTexture;
    return textureType && t.vkImage != 0 && !t.isBackBuffer && !t.isAttachmentOnly && t.multiSample == 0 &&
           (t.usage & (kD3dUsageRenderTarget | kD3dUsageDepthStencil)) == 0 && t.pool != kD3dPoolSystemMem &&
           t.pool != kD3dPoolScratch;
}

FrameOrchestrator::FrameOrchestrator(FrameConfig config, BindlessImageRegistry* bindless)
    : m_config(std::move(config)), m_bindless(bindless) {}

FrameOrchestrator::~FrameOrchestrator() { detach(); }

bool FrameOrchestrator::attach(tap::IFrameHost* host) {
    if (host == m_host && attached()) {
        return true;
    }
    detach();
    if (!host) {
        m_error = "the device has no frame host";
        return false;
    }
    std::string error;
    if (!m_gpu.init(*host, &error)) {
        m_error = "FUSE frame GPU unavailable: " + error;
        return false;
    }
    m_host = host;
    return true;
}

ExternalImageDesc FrameOrchestrator::registryDesc(tap::ResourceId id, const GpuImage& image) const {
    ExternalImageDesc d;
    d.texture = id;
    d.vkImage = image.image.vkImage;
    d.vkFormat = image.image.info.format;
    const bool cube = (image.image.info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0 &&
                      image.image.info.arrayLayers == 6;
    d.viewType = image.image.info.imageType == VK_IMAGE_TYPE_3D ? 2u : (cube ? 3u : 1u);
    d.width = image.image.info.width;
    d.height = image.image.info.height;
    d.depth = image.image.info.depth;
    d.mipLevels = image.image.info.mipLevels;
    d.arrayLayers = image.image.info.arrayLayers;
    d.owned = true;
    return d;
}

void FrameOrchestrator::retire(GpuImage& image) {
    if (!image.valid()) {
        return;
    }
    if (image.host && m_host) {
        m_host->releaseImage(image.host);
    }
    image.host = 0;
    m_retired.push_back(Retired{image, m_acquire + 1});
    image = GpuImage{};
    m_stats.pendingDestroy = static_cast<std::uint32_t>(m_retired.size());
}

std::uint32_t FrameOrchestrator::collect() {
    if (!attached()) {
        return 0;
    }
    const std::uint64_t completed = m_gpu.acquireCompleted();
    std::uint32_t n = 0;
    auto keep = std::remove_if(m_retired.begin(), m_retired.end(), [&](Retired& r) {
        if (r.acquire > completed) {
            return false;
        }
        m_gpu.destroyImage(r.image);
        ++n;
        return true;
    });
    m_retired.erase(keep, m_retired.end());
    if (m_bindless) {
        m_bindless->collect(completed);
    }
    m_stats.destroyed += n;
    m_stats.pendingDestroy = static_cast<std::uint32_t>(m_retired.size());
    return n;
}

void FrameOrchestrator::detach() {
    if (!m_host) {
        m_gpu.shutdown();
        return;
    }
    // Every host command and every FUSE submission completes; then nothing references FUSE's images.
    m_host->waitIdle();
    m_gpu.waitIdle();
    for (auto& [texture, image] : m_swaps) {
        m_host->setTextureSwap(texture, nullptr);
        if (m_bindless) {
            m_bindless->release(swapTwinId(texture), 0);
        }
        m_gpu.destroyImage(image);
    }
    m_swaps.clear();
    for (GpuImage* image : {&m_input, &m_output}) {
        if (image->host) {
            m_host->releaseImage(image->host);
        }
        m_gpu.destroyImage(*image);
    }
    if (m_bindless) {
        m_bindless->release(kFrameInputId, 0);
        m_bindless->release(kFrameOutputId, 0);
        m_bindless->collect(UINT64_MAX);
    }
    for (Retired& r : m_retired) {
        m_gpu.destroyImage(r.image);
    }
    m_retired.clear();
    m_frameFormat = m_frameWidth = m_frameHeight = 0;
    m_gpu.shutdown();
    m_host = nullptr;
    m_stats.swaps = 0;
    m_stats.frameImages = 0;
    m_stats.pendingDestroy = 0;
}

bool FrameOrchestrator::ensureFrameImages(const tap::HostImageInfo& bb) {
    const bool wantInput = m_config.mode == FrameMode::Passthrough;
    if (m_output.valid() && m_frameFormat == bb.format && m_frameWidth == bb.width && m_frameHeight == bb.height &&
        m_input.valid() == wantInput) {
        return true;
    }
    if (m_output.valid() || m_input.valid()) {
        ++m_stats.imageRebuilds;
    }
    retire(m_input);
    retire(m_output);
    if (m_bindless) {
        m_bindless->release(kFrameInputId, m_acquire + 1);
        m_bindless->release(kFrameOutputId, m_acquire + 1);
    }
    tap::HostImageInfo like;
    like.imageType = VK_IMAGE_TYPE_2D;
    like.format = bb.format;
    like.width = bb.width;
    like.height = bb.height;
    const auto create = [&](GpuImage& image, tap::ResourceId id) {
        if (!m_gpu.createImage(like, kFrameUsage, true, image)) {
            m_error = "cannot create a FUSE frame image: " + m_gpu.lastError();
            return false;
        }
        image.host = m_host->importImage(image.image);
        if (!image.host) {
            m_error = "the host rejected a FUSE frame image";
            m_gpu.destroyImage(image);
            return false;
        }
        if (m_bindless) {
            m_bindless->registerImage(registryDesc(id, image), m_acquire + 1);
        }
        return true;
    };
    if (!create(m_output, kFrameOutputId) || (wantInput && !create(m_input, kFrameInputId))) {
        return false;
    }
    m_frameFormat = bb.format;
    m_frameWidth = bb.width;
    m_frameHeight = bb.height;
    m_stats.frameImages = wantInput ? 2u : 1u;
    return true;
}

InjectResult FrameOrchestrator::inject() {
    InjectResult r;
    if (!attached()) {
        r.error = m_error.empty() ? "not attached" : m_error;
        return r;
    }
    collect();
    if (m_config.mode == FrameMode::Off) {
        // Texture swap only: still advance the acquire timeline while released images wait for it.
        if (!m_retired.empty() && m_host->flushAndSignal(m_acquire + 1)) {
            ++m_acquire;
            m_stats.acquireValue = m_acquire;
        }
        r.error = "relight.frame.mode = off";
        return r;
    }
    auto fail = [&](std::string why) {
        r.error = std::move(why);
        m_error = r.error;
        ++m_stats.failures;
        return r;
    };
    tap::HostImageInfo bb;
    if (!m_host->backBufferInfo(bb)) {
        return fail("no back buffer");
    }
    if (bb.samples != 1 || bb.aspects != VK_IMAGE_ASPECT_COLOR_BIT) {
        return fail("the back buffer is multisampled or not a colour image");
    }
    if (!ensureFrameImages(bb)) {
        return fail(m_error);
    }
    const bool passthrough = m_config.mode == FrameMode::Passthrough;
    const std::uint64_t acquire = m_acquire + 1, release = m_release + 1;
    if (passthrough && !m_host->copyBackBuffer(m_input.host)) {
        return fail("the host could not copy the back buffer");
    }
    if (!m_host->flushAndSignal(acquire)) {
        return fail("the host could not flush and signal");
    }
    m_acquire = acquire;
    r.acquire = acquire;
    r.submit = m_gpu.submitFrame(passthrough ? FramePass::Passthrough : FramePass::Solid, &m_input, m_output,
                                 m_config.solidColor, acquire, release);
    if (!r.submit.ok) {
        return fail("FUSE frame submission failed: " + m_gpu.lastError());
    }
    m_release = release;
    r.release = release;
    if (!m_host->composite(m_output.host, release)) {
        return fail("the host could not composite");
    }
    r.injected = true;
    ++m_stats.injections;
    m_stats.acquireValue = m_acquire;
    m_stats.releaseValue = m_release;
    return r;
}

bool FrameOrchestrator::swapTexture(const tap::TextureDesc& texture) {
    if (!attached() || !swappable(texture) || m_swaps.count(texture.id)) {
        return false;
    }
    tap::HostImageInfo info;
    if (!m_host->textureInfo(texture.id, info) || info.aspects != VK_IMAGE_ASPECT_COLOR_BIT || info.samples != 1) {
        ++m_stats.swapsRejected;
        return false;
    }
    GpuImage twin;
    const std::uint32_t usage =
        info.usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!m_gpu.createImage(info, usage, false, twin)) {
        ++m_stats.swapsRejected;
        return false;
    }
    if (!m_host->setTextureSwap(texture.id, &twin.image)) {
        m_gpu.destroyImage(twin); // the host never saw it
        ++m_stats.swapsRejected;
        return false;
    }
    if (m_bindless) {
        m_bindless->registerImage(registryDesc(swapTwinId(texture.id), twin), m_acquire + 1);
    }
    m_swaps.emplace(texture.id, twin);
    ++m_stats.swapsCreated;
    m_stats.swaps = static_cast<std::uint32_t>(m_swaps.size());
    return true;
}

void FrameOrchestrator::onTextureDestroyed(tap::ResourceId texture) {
    auto it = m_swaps.find(texture);
    if (it == m_swaps.end()) {
        return;
    }
    if (m_bindless) {
        m_bindless->release(swapTwinId(texture), m_acquire + 1);
    }
    retire(it->second);
    m_swaps.erase(it);
    m_stats.swaps = static_cast<std::uint32_t>(m_swaps.size());
}

} // namespace fuse::relight::render::frame
