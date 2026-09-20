#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/vk/queue_submit.hpp>
#include <fuse/renderer/vk/swapchain_util.hpp>

#include <string>

#ifndef FUSE_SHADER_FIXTURE_DIR
#define FUSE_SHADER_FIXTURE_DIR "Source/FUSE/Renderer/shaders/fixtures"
#endif

namespace fuse::renderer {

namespace {

std::string fixturePath(const char* name) {
    return std::string(FUSE_SHADER_FIXTURE_DIR) + "/" + name;
}

} // namespace

RhiContext::RhiContext(std::unique_ptr<VulkanBootstrap> bootstrap, const Desc& desc)
    : m_bootstrap(std::move(bootstrap)), m_desc(desc) {}

RhiContext::~RhiContext() {
    if (m_frameSyncInitialized && m_bootstrap && m_bootstrap->device() != nullptr) {
        m_frameSync.destroy(m_bootstrap->device()->nativeHandle());
        m_frameSyncInitialized = false;
    }
}

std::unique_ptr<RhiContext> RhiContext::create(const Desc& desc) {
    auto bootstrap = VulkanBootstrap::create(desc.bootstrap);
    if (!bootstrap) {
        return nullptr;
    }

    return std::unique_ptr<RhiContext>(new RhiContext(std::move(bootstrap), desc));
}

void RhiContext::ensureRasterPath() {
    if (m_rasterPath || !m_desc.enableRasterPath) {
        return;
    }

    VulkanDevice* device = m_bootstrap ? m_bootstrap->device() : nullptr;
    if (device == nullptr) {
        return;
    }

    RasterPathDesc rasterDesc = m_desc.raster;
    if (rasterDesc.vertexSpirvPath == nullptr) {
        static const std::string vertPath = fixturePath("minimal.vert.spv");
        rasterDesc.vertexSpirvPath = vertPath.c_str();
    }
    if (rasterDesc.fragmentSpirvPath == nullptr) {
        static const std::string fragPath = fixturePath("minimal.frag.spv");
        rasterDesc.fragmentSpirvPath = fragPath.c_str();
    }

    m_rasterPath = RasterPath::create(*device, rasterDesc);
}

void RhiContext::ensureCompositePass() {
    if (m_compositePass || !m_desc.enableCompositePass) {
        return;
    }

    m_compositePass = CompositePass::create(m_desc.composite);
}

void RhiContext::ensureCompositeGpuPath() {
    if (m_compositeGpuPath || !m_desc.enableCompositePass) {
        return;
    }

    VulkanDevice* device = m_bootstrap ? m_bootstrap->device() : nullptr;
    if (device == nullptr) {
        return;
    }

    CompositeGpuPathDesc compositeDesc{};
    compositeDesc.width = m_desc.raster.width;
    compositeDesc.height = m_desc.raster.height;
    static const std::string vertPath = fixturePath("composite.vert.spv");
    static const std::string fragPath = fixturePath("composite.frag.spv");
    compositeDesc.vertexSpirvPath = vertPath.c_str();
    compositeDesc.fragmentSpirvPath = fragPath.c_str();

    m_compositeGpuPath = CompositeGpuPath::create(*device, compositeDesc);
}

void RhiContext::ensureFrameSyncPair() {
    if (m_frameSyncInitialized) {
        return;
    }

    VulkanDevice* device = m_bootstrap ? m_bootstrap->device() : nullptr;
    if (device == nullptr || !device->isValid()) {
        return;
    }

    m_frameSync = fuse::renderer::cuda::FrameSyncPair::create(device->nativeHandle(),
                                                              device->nativePhysicalDevice());
    m_frameSyncInitialized = true;
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
    m_renderGraph.beginFrame(frameIndex);
    m_commandRecorder.reset();
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
            m_renderGraph.beginFrame(frameIndex);
            m_commandRecorder.reset();
        }

        ensureCompositePass();
        ensureCompositeGpuPath();
        ensureFrameSyncPair();
        const float compositeBlend =
            m_compositePass ? m_compositePass->blendForFrame(commands) : m_desc.composite.defaultBlend;

        ensureRasterPath();
        const VkFrameEncodeContext* encodeContext = nullptr;
        VkFrameEncodeContext encodeContextStorage{};
        if (m_rasterPath && m_rasterPath->isReady()) {
            encodeContextStorage = m_rasterPath->vulkanEncodeContext();
            if (encodeContextStorage.active) {
                encodeContext = &encodeContextStorage;
            }
        }

#if defined(FUSE_VULKAN_BACKEND)
        VulkanSwapchain* swapchain = m_bootstrap->swapchain();
        const bool presentTargetsReady =
            swapchain != nullptr && swapchain->hasPresentTargets() &&
            !isEmptyAcquireResult(m_acquiredSwapchainImage);
        if (encodeContext != nullptr && presentTargetsReady) {
            encodeContextStorage.presentRenderPass = swapchain->presentRenderPass();
            encodeContextStorage.presentFramebuffer =
                swapchain->framebufferForImage(m_acquiredSwapchainImage);
            encodeContextStorage.presentBarrierImage =
                swapchain->imageHandleForIndex(m_acquiredSwapchainImage);
            encodeContextStorage.presentWidth = swapchain->info().width;
            encodeContextStorage.presentHeight = swapchain->info().height;
            encodeContextStorage.presentActive =
                encodeContextStorage.presentFramebuffer != nullptr &&
                encodeContextStorage.presentRenderPass != nullptr;
            encodeContext = &encodeContextStorage;
        } else if (encodeContextStorage.active) {
            encodeContext = &encodeContextStorage;
        }

        if (m_compositeGpuPath && m_compositeGpuPath->isReady() && m_rasterPath != nullptr) {
            VulkanDevice* compositeDevice = m_bootstrap ? m_bootstrap->device() : nullptr;
            (void)m_compositeGpuPath->ensureCudaInteropTexture();
            if (m_frameSyncInitialized) {
                (void)m_frameSync.signalRenderLane(
                    compositeDevice != nullptr ? compositeDevice->nativeHandle() : nullptr, frameIndex);
            }
            (void)m_compositeGpuPath->fillCudaInteropTexture(
                m_frameSyncInitialized ? &m_frameSync : nullptr, frameIndex, true);
            m_compositeGpuPath->registerRasterSource(m_rasterPath->colorViewHandle());
            if (presentTargetsReady && encodeContextStorage.presentRenderPass != nullptr) {
                m_compositeGpuPath->ensurePresentPipeline(encodeContextStorage.presentRenderPass);
            }
            if (encodeContext != nullptr) {
                m_compositeGpuPath->fillEncodeContext(encodeContextStorage, compositeBlend,
                                                      presentTargetsReady);
                encodeContext = &encodeContextStorage;
            }
        }
#endif

        populateRenderGraphFromCommandList(m_renderGraph, commands, compositeBlend);
        m_renderGraph.compile();

        VulkanDevice* device = m_bootstrap->device();
        if (device != nullptr) {
#if defined(FUSE_VULKAN_BACKEND)
            if (device->isValid() && encodeContext != nullptr) {
                (void)resetFrameSlotCommandPool(*device, *frameManager);
            }
#endif
            const RenderGraphExecuteInfo executeInfo =
                m_renderGraph.execute(*device, *frameManager, m_commandRecorder, encodeContext);
            m_lastGraphPassCount = executeInfo.executedPassCount;
            m_lastRecordedCommands = executeInfo.recordedCommands;
        }

        m_lastGraphBarrierCount = m_renderGraph.compileInfo().barrierCount;
    }

    if (m_rasterPath && m_rasterPath->isReady()) {
        m_rasterPath->updateStatsFromCommands(commands);
        m_lastRasterStats = m_rasterPath->lastStats();
    }

    if (m_compositePass && m_compositePass->isReady()) {
        m_compositePass->recordFrame(commands);
        m_lastCompositeStats = m_compositePass->lastStats();
    }

    if (m_compositeGpuPath && m_compositeGpuPath->isReady()) {
        m_lastCompositeGpuStats = m_compositeGpuPath->lastStats();
    }

    if (frameManager != nullptr && frameManager->isReady()) {
        VulkanDevice* device = m_bootstrap->device();
        if (device != nullptr && device->isValid()) {
            GraphicsQueueSubmitDesc submitDesc{};
            submitDesc.device = device;
            submitDesc.frameManager = frameManager;
            submitDesc.swapchain = m_bootstrap->swapchain();
            submitDesc.acquiredImageIndex = m_acquiredSwapchainImage;
            submitDesc.commandsAlreadyRecorded = m_commandRecorder.vulkanRecordingComplete();
            m_lastQueueSubmit = submitGraphicsQueue(submitDesc);
            if (m_lastQueueSubmit.submitted) {
                ++m_queueSubmitCount;
            }
            if (!m_lastQueueSubmit.ok) {
                return false;
            }
        }
        frameManager->endFrame();
    }

    m_lastCommandCount = commands.commandCount();
    ++m_submittedFrames;
    return true;
}

bool RhiContext::submitDrawList(const DrawList& draws, u32 frameIndex) {
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
            m_renderGraph.beginFrame(frameIndex);
            m_commandRecorder.reset();
        }

        ensureCompositePass();
        ensureCompositeGpuPath();
        ensureFrameSyncPair();
        ensureRasterPath();

        const VkFrameEncodeContext* encodeContext = nullptr;
        VkFrameEncodeContext encodeContextStorage{};
        if (m_rasterPath && m_rasterPath->isReady()) {
            encodeContextStorage = m_rasterPath->vulkanEncodeContext();
            if (encodeContextStorage.active) {
                encodeContext = &encodeContextStorage;
            }
        }

#if defined(FUSE_VULKAN_BACKEND)
        VulkanSwapchain* swapchain = m_bootstrap->swapchain();
        const bool presentTargetsReady =
            swapchain != nullptr && swapchain->hasPresentTargets() &&
            !isEmptyAcquireResult(m_acquiredSwapchainImage);
        if (encodeContext != nullptr && presentTargetsReady) {
            encodeContextStorage.presentRenderPass = swapchain->presentRenderPass();
            encodeContextStorage.presentFramebuffer =
                swapchain->framebufferForImage(m_acquiredSwapchainImage);
            encodeContextStorage.presentBarrierImage =
                swapchain->imageHandleForIndex(m_acquiredSwapchainImage);
            encodeContextStorage.presentWidth = swapchain->info().width;
            encodeContextStorage.presentHeight = swapchain->info().height;
            encodeContextStorage.presentActive =
                encodeContextStorage.presentFramebuffer != nullptr &&
                encodeContextStorage.presentRenderPass != nullptr;
            encodeContext = &encodeContextStorage;
        } else if (encodeContextStorage.active) {
            encodeContext = &encodeContextStorage;
        }
#endif

        populateRenderGraphFromDrawList(m_renderGraph, draws);
        m_renderGraph.compile();

        VulkanDevice* device = m_bootstrap->device();
        if (device != nullptr) {
#if defined(FUSE_VULKAN_BACKEND)
            if (device->isValid() && encodeContext != nullptr) {
                (void)resetFrameSlotCommandPool(*device, *frameManager);
            }
#endif
            const RenderGraphExecuteInfo executeInfo =
                m_renderGraph.execute(*device, *frameManager, m_commandRecorder, encodeContext);
            m_lastGraphPassCount = executeInfo.executedPassCount;
            m_lastRecordedCommands = executeInfo.recordedCommands;
        }

        m_lastGraphBarrierCount = m_renderGraph.compileInfo().barrierCount;
    }

    if (m_rasterPath && m_rasterPath->isReady()) {
        m_lastRasterStats = m_rasterPath->lastStats();
    }

    if (m_compositePass && m_compositePass->isReady()) {
        m_lastCompositeStats = m_compositePass->lastStats();
    }

    if (m_compositeGpuPath && m_compositeGpuPath->isReady()) {
        m_lastCompositeGpuStats = m_compositeGpuPath->lastStats();
    }

    if (frameManager != nullptr && frameManager->isReady()) {
        VulkanDevice* device = m_bootstrap->device();
        if (device != nullptr && device->isValid()) {
            GraphicsQueueSubmitDesc submitDesc{};
            submitDesc.device = device;
            submitDesc.frameManager = frameManager;
            submitDesc.swapchain = m_bootstrap->swapchain();
            submitDesc.acquiredImageIndex = m_acquiredSwapchainImage;
            submitDesc.commandsAlreadyRecorded = m_commandRecorder.vulkanRecordingComplete();
            m_lastQueueSubmit = submitGraphicsQueue(submitDesc);
            if (m_lastQueueSubmit.submitted) {
                ++m_queueSubmitCount;
            }
            if (!m_lastQueueSubmit.ok) {
                return false;
            }
        }
        frameManager->endFrame();
    }

    m_lastDrawListCount = draws.count();
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
