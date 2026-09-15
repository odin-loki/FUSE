#include <fuse/hybrid/hybrid_composer.hpp>

#include <fuse/log/logger.hpp>
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

    m_renderer.beginFrame(320, 240);

    if (m_world3D && m_flags.enable3D) {
        m_world3D->render(ctx);
        m_renderer.clear3D(m_world3D->clearColorR(), m_world3D->clearColorG(), m_world3D->clearColorB());
    } else {
        m_renderer.clear3D(0.f, 0.f, 0.f);
    }

    if (m_world2D && m_flags.enable2D) {
        m_world2D->render(ctx);

        const world2d::SceneSnapshot2D& snapshot = m_world2D->readSnapshot();
        for (const world2d::SpriteDrawCmd& cmd : snapshot.sprites()) {
            if (!cmd.visible) {
                continue;
            }
            m_renderer.drawSprite2D(cmd.x, cmd.y, cmd.rotation, 255, 200, 64);
        }
    }

    if (m_flags.enableUI) {
        m_renderer.drawSprite2D(0.f, -90.f, 0.f, 255, 255, 255);
    }
}

} // namespace fuse::hybrid
