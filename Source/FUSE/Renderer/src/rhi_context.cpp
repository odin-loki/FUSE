#include <fuse/platform/gl_context.hpp>
#include <fuse/renderer/rhi_context.hpp>

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

RhiContext::~RhiContext() = default;

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
        const float compositeBlend =
            m_compositePass ? m_compositePass->blendForFrame(commands) : m_desc.composite.defaultBlend;

        populateRenderGraphFromCommandList(m_renderGraph, commands, compositeBlend);
        m_renderGraph.compile();

        VulkanDevice* device = m_bootstrap->device();
        if (device != nullptr) {
            const RenderGraphExecuteInfo executeInfo =
                m_renderGraph.execute(*device, *frameManager, m_commandRecorder);
            m_lastGraphPassCount = executeInfo.executedPassCount;
            m_lastRecordedCommands = executeInfo.recordedCommands;
        }

        m_lastGraphBarrierCount = m_renderGraph.compileInfo().barrierCount;
        frameManager->endFrame();
    }

    ensureRasterPath();
    if (m_rasterPath && m_rasterPath->isReady()) {
        m_rasterPath->recordFrame(commands);
        m_lastRasterStats = m_rasterPath->lastStats();
    }

    if (m_compositePass && m_compositePass->isReady()) {
        m_compositePass->recordFrame(commands);
        m_lastCompositeStats = m_compositePass->lastStats();
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
