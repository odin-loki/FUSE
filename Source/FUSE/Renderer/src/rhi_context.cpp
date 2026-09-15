#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/rhi_context.hpp>

namespace fuse::renderer {

RhiContext::RhiContext(std::unique_ptr<VulkanBootstrap> bootstrap)
    : m_bootstrap(std::move(bootstrap)) {}

RhiContext::~RhiContext() = default;

std::unique_ptr<RhiContext> RhiContext::create(const Desc& desc) {
    auto bootstrap = VulkanBootstrap::create(desc.bootstrap);
    if (!bootstrap) {
        return nullptr;
    }

    return std::unique_ptr<RhiContext>(new RhiContext(std::move(bootstrap)));
}

bool RhiContext::beginFrame(u32 frameIndex) {
    if (!platform::requireGpuContextThread()) {
        return false;
    }

    if (!m_bootstrap || !m_bootstrap->status().deviceReady) {
        return false;
    }

    FrameManager* frameManager = m_bootstrap->frameManager();
    if (frameManager == nullptr || !frameManager->isReady()) {
        return false;
    }

    frameManager->signalTickComplete();
    frameManager->beginFrame(frameIndex);
    return true;
}

bool RhiContext::submitFrame(const RenderCommandList& commands, u32 frameIndex) {
    if (!platform::requireGpuContextThread()) {
        return false;
    }

    if (!m_bootstrap || !m_bootstrap->status().deviceReady) {
        return false;
    }

    FrameManager* frameManager = m_bootstrap->frameManager();
    if (frameManager != nullptr && frameManager->isReady()) {
        if (!frameManager->tickComplete()) {
            frameManager->signalTickComplete();
            frameManager->beginFrame(frameIndex);
        }
        frameManager->endFrame();
    }

    m_lastCommandCount = commands.commandCount();
    ++m_submittedFrames;
    return true;
}

u32 RhiContext::currentFrameSlot() const {
    const FrameManager* frameManager =
        m_bootstrap ? m_bootstrap->frameManager() : nullptr;
    if (frameManager == nullptr || !frameManager->isReady()) {
        return 0;
    }
    return frameManager->currentIndex();
}

} // namespace fuse::renderer
