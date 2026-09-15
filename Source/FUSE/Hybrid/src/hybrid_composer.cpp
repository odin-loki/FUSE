#include <fuse/hybrid/hybrid_composer.hpp>

#include <fuse/log/logger.hpp>
#include <fuse/platform/gl_context.hpp>
#include <fuse/platform/thread.hpp>

namespace fuse::hybrid {

HybridComposer::HybridComposer() = default;

void HybridComposer::setProjectFlags(const DimensionFlags& flags) {
    m_flags = flags;
    applyProjectFlags();
}

void HybridComposer::attachWorld2D(world2d::World2D* world) {
    m_world2D = world;
    applyProjectFlags();
}

void HybridComposer::attachWorld3D(world3d::World3D* world) {
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
    m_sharedRhiContext = context;
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

    if (m_world3D && m_flags.enable3D) {
        m_world3D->tick(ctx);
    }
    if (m_world2D && m_flags.enable2D) {
        m_world2D->tick(ctx);
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
#endif

    m_renderer.beginFrame(320, 240);

    if (m_world3D && m_flags.enable3D) {
        m_world3D->render(ctx);
        const float clearR = m_world3D->clearColorR();
        const float clearG = m_world3D->clearColorG();
        const float clearB = m_world3D->clearColorB();
        m_renderer.clear3D(clearR, clearG, clearB);
#if defined(FUSE_HAS_VULKAN_RHI)
        recordClear3D(clearR, clearG, clearB);
#endif
    } else {
        m_renderer.clear3D(0.f, 0.f, 0.f);
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
            m_renderer.drawSprite2D(cmd.x, cmd.y, cmd.rotation, 255, 200, 64);
#if defined(FUSE_HAS_VULKAN_RHI)
            recordSprite2D(cmd.x, cmd.y, cmd.rotation, 255, 200, 64);
#endif
        }
    }

    if (m_flags.enableUI) {
        m_renderer.drawSprite2D(0.f, -90.f, 0.f, 255, 255, 255);
#if defined(FUSE_HAS_VULKAN_RHI)
        recordSprite2D(0.f, -90.f, 0.f, 255, 255, 255);
#endif
    }

#if defined(FUSE_HAS_VULKAN_RHI)
    renderer::RhiContext* rhi = rhiContext();
    if (rhi != nullptr && platform::requireGpuContextThread()) {
        rhi->beginFrame(ctx.frameIndex);
        rhi->submitFrame(m_commandList, ctx.frameIndex);
    }
#endif

    (void)ctx;
}

} // namespace fuse::hybrid
