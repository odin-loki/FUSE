#include <fuse/hybrid/hybrid_composer.hpp>

#include <fuse/log/logger.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/platform/thread.hpp>

namespace fuse::hybrid {

HybridComposer::HybridComposer() = default;

HybridComposer::~HybridComposer() {
#if defined(FUSE_HAS_VULKAN_RHI)
    releaseGpuScene();
#endif
}

bool HybridComposer::gpuSceneActive() const {
#if defined(FUSE_HAS_VULKAN_RHI)
    return m_gpuScene != nullptr && m_gpuScene->valid();
#else
    return false;
#endif
}

void HybridComposer::setProjectFlags(const DimensionFlags& flags) {
    m_flags = flags;
    applyProjectFlags();
}

void HybridComposer::attachWorld2D(world2d::World2D* world) {
    m_world2D = world;
    applyProjectFlags();
}

void HybridComposer::attachWorld3D(world3d::World3D* world) {
#if defined(FUSE_HAS_VULKAN_RHI)
    if (m_world3D != nullptr && m_world3D != world && m_gpuScene != nullptr &&
        m_world3D->renderer() == m_gpuScene.get()) {
        m_world3D->setRenderer(nullptr);
    }
    if (world != nullptr && gpuSceneActive()) {
        world->setRenderer(m_gpuScene.get());
    }
#endif
    m_world3D = world;
    applyProjectFlags();
}

void HybridComposer::applyProjectFlags() {
    if (m_world2D) {
        m_world2D->setEnabled(m_flags.enable2D);
    }
    if (m_world3D) {
        m_world3D->setEnabled(m_flags.enable3D);
    }
}

#if defined(FUSE_HAS_VULKAN_RHI)
void HybridComposer::setSharedRhiContext(renderer::RhiContext* context) {
    if (context != m_sharedRhiContext) {
        // The GPU scene lives on the old context's device: drop it (render() re-creates it when requested).
        releaseGpuScene();
        m_gpuSceneFailed = false;
    }
    m_sharedRhiContext = context;
}

bool HybridComposer::enableGpuScene(const HybridSceneRendererDesc& desc) {
    m_gpuSceneDesc = desc;
    m_gpuSceneRequested = true;
    if (m_gpuSceneStatus == nullptr || !m_gpuSceneFailed) {
        m_gpuSceneStatus = "requested (created on the first render)";
    }
    return !m_gpuSceneFailed;
}

void HybridComposer::disableGpuScene() {
    releaseGpuScene();
    m_gpuSceneRequested = false;
    m_gpuSceneFailed = false;
    m_gpuSceneStatus = "not requested";
}

void HybridComposer::releaseGpuScene() {
    if (m_gpuScene == nullptr) {
        return;
    }
    renderer::RhiContext* rhi = m_sharedRhiContext != nullptr ? m_sharedRhiContext : m_ownedRhiContext.get();
    if (rhi != nullptr && rhi->sceneFrameSource().user == m_gpuScene.get()) {
        rhi->setSceneFrameSource({});
    }
    if (m_world3D != nullptr && m_world3D->renderer() == m_gpuScene.get()) {
        m_world3D->setRenderer(nullptr);
    }
    m_gpuScene->shutdown();
    m_gpuScene.reset();
    if (!m_softwarePlaceholderExplicit) {
        m_softwarePlaceholderEnabled = true;
    }
}

void HybridComposer::ensureGpuScene() {
    if (!m_gpuSceneRequested || m_gpuSceneFailed) {
        return;
    }
    renderer::RhiContext* rhi = rhiContext();
    if (m_gpuScene != nullptr) {
        // Follow a real swapchain's extent (editor viewport resize / external surface wiring).
        const renderer::VulkanSwapchain* swapchain = rhi != nullptr ? rhi->bootstrap().swapchain() : nullptr;
        if (swapchain != nullptr && !swapchain->isHeadless() && swapchain->hasImages() &&
            m_gpuSceneDesc.width == 0u && swapchain->info().width != 0u && swapchain->info().height != 0u &&
            (swapchain->info().width != m_gpuScene->width() || swapchain->info().height != m_gpuScene->height())) {
            (void)m_gpuScene->resize(swapchain->info().width, swapchain->info().height);
        }
        return;
    }
    renderer::VulkanDevice* device = rhi != nullptr ? rhi->bootstrap().device() : nullptr;
    if (device == nullptr || !device->isValid() || !rhi->bootstrap().status().deviceReady) {
        m_gpuSceneFailed = true;
        m_gpuSceneStatus = "no Vulkan device (placeholder fallback)";
        return;
    }
    HybridSceneRendererDesc desc = m_gpuSceneDesc;
    const renderer::VulkanSwapchain* swapchain = rhi->bootstrap().swapchain();
    if (desc.width == 0u && swapchain != nullptr && !swapchain->isHeadless() && swapchain->hasImages()) {
        desc.width = swapchain->info().width;
        desc.height = swapchain->info().height;
    }
    auto scene = std::make_unique<HybridSceneRenderer>();
    if (!scene->initialize(*device, desc)) {
        m_gpuSceneFailed = true;
        m_gpuSceneStatus = scene->reason();
        log::warn("HybridComposer: GPU scene unavailable (%s) — PlaceholderRenderer fallback", scene->reason());
        return;
    }
    m_gpuScene = std::move(scene);
    m_gpuSceneStatus = "active";
    rhi->setSceneFrameSource(m_gpuScene->frameSource());
    if (m_world3D != nullptr) {
        m_world3D->setRenderer(m_gpuScene.get());
    }
    if (!m_softwarePlaceholderExplicit) {
        m_softwarePlaceholderEnabled = false;
    }
}

void HybridComposer::ensureRhiContext() {
    if (m_sharedRhiContext != nullptr) {
        return;
    }

    if (m_ownedRhiContext) {
        return;
    }

    renderer::RhiContext::Desc desc{};
    desc.bootstrap.instance.enableValidation = false;
    m_ownedRhiContext = renderer::RhiContext::create(desc);
}

renderer::RhiContext* HybridComposer::rhiContext() {
    if (m_sharedRhiContext != nullptr) {
        return m_sharedRhiContext;
    }

    ensureRhiContext();
    return m_ownedRhiContext.get();
}

const renderer::RhiContext* HybridComposer::rhiContext() const {
    if (m_sharedRhiContext != nullptr) {
        return m_sharedRhiContext;
    }

    return m_ownedRhiContext.get();
}

void HybridComposer::recordClear3D(float r, float g, float b) {
    m_commandList.clear3D(r, g, b);
}

void HybridComposer::recordSprite2D(float x, float y, float rotation, u8 r, u8 g, u8 b) {
    m_commandList.drawSprite2D(x, y, rotation, r, g, b);
}
#endif

void HybridComposer::tick(frame::FrameCtx& ctx) {
    m_barrier.beginTick(ctx.frameIndex);

    // Game-thread snapshot build — scene mutation and snapshot publish stay serial.
    if (m_world3D && m_flags.enable3D) {
        m_world3D->tickGameThread(ctx);
    }
    if (m_world2D && m_flags.enable2D) {
        m_world2D->tickGameThread(ctx);
    }

    // Game-thread cull dispatch — each world parallelizes internally via parallel_for.
    // Dimension culls run serially on the game thread so nested parallel_for does not
    // block worker threads waiting on inner JobCounters (see fuse_hybrid_tests cull edges).
    if (m_world3D && m_flags.enable3D) {
        m_world3D->runParallelCull();
    }
    if (m_world2D && m_flags.enable2D) {
        m_world2D->runParallelCull();
    }

    m_barrier.signalTickJobsComplete();
    m_barrier.waitForTickComplete();
    ++m_frameCount;
}

void HybridComposer::render(frame::FrameCtx& ctx) {
    if (!platform::isRenderThread()) {
        log::warn("HybridComposer::render must run on render thread");
        return;
    }

#if defined(FUSE_HAS_VULKAN_RHI)
    m_commandList.reset();
    ensureRhiContext();
    ensureGpuScene();
    HybridSceneRenderer* gpu = gpuSceneActive() ? m_gpuScene.get() : nullptr;
    if (gpu != nullptr) {
        // Overlay first: the scene renderer's UI stage (run inside World3D::render) composites these quads.
        gpu->beginOverlay();
        if (m_world2D && m_flags.enable2D) {
            for (const world2d::SpriteDrawCmd& cmd : m_world2D->readSnapshot().sprites()) {
                if (cmd.visible) {
                    (void)gpu->addSprite(spriteQuadFromPlaceholder(cmd.x, cmd.y, cmd.rotation, 255, 200, 64,
                                                                   gpu->width(), gpu->height()));
                }
            }
        }
        if (m_flags.enableUI) {
            (void)gpu->addUi(spriteQuadFromPlaceholder(0.f, -90.f, 0.f, 255, 255, 255, gpu->width(), gpu->height()));
        }
        if (m_world3D != nullptr && m_world3D->renderer() != gpu) {
            m_world3D->setRenderer(gpu);
        }
    }
#endif

    if (m_softwarePlaceholderEnabled) {
        m_renderer.beginFrame(320, 240);
    }

    if (m_world3D && m_flags.enable3D) {
        m_world3D->render(ctx);
        m_cookedAssets.refreshTints();
        float clearR = m_world3D->clearColorR() + m_cookedAssets.materialTintR() +
                       m_cookedAssets.shaderTintR();
        float clearG = m_world3D->clearColorG() + m_cookedAssets.materialTintG() +
                       m_cookedAssets.shaderTintG();
        float clearB = m_world3D->clearColorB() + m_cookedAssets.materialTintB() +
                       m_cookedAssets.shaderTintB();
        clearR = clearR > 1.f ? 1.f : clearR;
        clearG = clearG > 1.f ? 1.f : clearG;
        clearB = clearB > 1.f ? 1.f : clearB;
        if (m_softwarePlaceholderEnabled) {
            m_renderer.clear3D(clearR, clearG, clearB);
        }
#if defined(FUSE_HAS_VULKAN_RHI)
        recordClear3D(clearR, clearG, clearB);
#endif

        m_meshPreviewDraws = 0;
        m_sdfPreviewDraws = 0;
        for (const MeshPreviewHint& meshHint : m_previewCatalog.meshes()) {
            if (!meshHint.visible) {
                continue;
            }
            const float tintBoost = m_cookedAssets.materialTintBoost(meshHint.materialId);
            const u8 r = static_cast<u8>(64u + (meshHint.materialId % 7u) * 24u +
                                         static_cast<u32>(tintBoost * 255.f));
            const u8 g = static_cast<u8>(96u + (meshHint.materialId % 5u) * 16u +
                                         (meshHint.cookedMeshResolved ? 24u : 0u));
            const u8 b = static_cast<u8>(128u + (meshHint.materialId % 3u) * 20u);
            // Software fallback only: on the GPU path the meshes themselves render (ecs::Mesh through the
            // SceneRenderer); the hints are no longer drawn as sprite quads.
            if (m_softwarePlaceholderEnabled) {
                m_renderer.drawMeshPreviewStub(meshHint.x, meshHint.y, meshHint.z, r, g, b);
            }
            ++m_meshPreviewDraws;
        }

        for (const SdfPreviewHint& sdfHint : m_previewCatalog.sdfs()) {
            if (!sdfHint.visible) {
                continue;
            }
            const float tintBoost = m_cookedAssets.materialTintBoost(sdfHint.materialId);
            const u8 r = static_cast<u8>(180u + (sdfHint.materialId % 4u) * 12u +
                                         static_cast<u32>(tintBoost * 255.f));
            const u8 g = static_cast<u8>(96u + (sdfHint.materialId % 6u) * 10u);
            const u8 b = static_cast<u8>(220u);
            if (m_softwarePlaceholderEnabled) {
                m_renderer.drawSdfPreviewStub(sdfHint.x, sdfHint.y, sdfHint.z, sdfHint.primitive,
                                              sdfHint.param0, sdfHint.param1, sdfHint.param2, r, g, b);
            }
            ++m_sdfPreviewDraws;
        }
    } else {
        if (m_softwarePlaceholderEnabled) {
            m_renderer.clear3D(0.f, 0.f, 0.f);
        }
#if defined(FUSE_HAS_VULKAN_RHI)
        recordClear3D(0.f, 0.f, 0.f);
#endif
    }

    if (m_world2D && m_flags.enable2D) {
        m_world2D->render(ctx);

        const world2d::SceneSnapshot2D& snapshot = m_world2D->readSnapshot();
        for (const world2d::SpriteDrawCmd& cmd : snapshot.sprites()) {
            if (!cmd.visible) {
                continue;
            }
            if (m_softwarePlaceholderEnabled) {
                m_renderer.drawSprite2D(cmd.x, cmd.y, cmd.rotation, 255, 200, 64);
            }
#if defined(FUSE_HAS_VULKAN_RHI)
            recordSprite2D(cmd.x, cmd.y, cmd.rotation, 255, 200, 64);
#endif
        }
    }

    if (m_flags.enableUI) {
        if (m_softwarePlaceholderEnabled) {
            m_renderer.drawSprite2D(0.f, -90.f, 0.f, 255, 255, 255);
        }
#if defined(FUSE_HAS_VULKAN_RHI)
        recordSprite2D(0.f, -90.f, 0.f, 255, 255, 255);
#endif
    }

#if defined(FUSE_HAS_VULKAN_RHI)
    if (gpu != nullptr && !gpu->frameRecorded()) {
        // No 3D world this frame (2D-only project, 3D disabled): the same GPU frame over an empty scene.
        (void)gpu->renderEmpty(ctx);
    }
    renderer::RhiContext* rhi = rhiContext();
    if (rhi != nullptr && platform::requireGpuContextThread()) {
        rhi->beginFrame(ctx.frameIndex);
        const u32 sceneFramesBefore = rhi->sceneFramesSubmitted();
        rhi->submitFrame(m_commandList, ctx.frameIndex);
        if (rhi->sceneFramesSubmitted() > sceneFramesBefore) {
            ++m_gpuSceneFrames;
        }
    }
#endif

    if (!m_softwarePlaceholderEnabled) {
        ++m_softwarePlaceholderSkippedFrames;
    }

    (void)ctx;
}

} // namespace fuse::hybrid
