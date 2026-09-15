#pragma once

#include <fuse/frame/frame_barrier.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/placeholder_renderer.hpp>
#include <fuse/hybrid/project_flags.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>

namespace fuse::hybrid {

/// Hybrid compositor (U4 v1): 3D opaque → 2D scene → UI overlay ordering.
class HybridComposer {
public:
    HybridComposer();

    void setProjectFlags(const DimensionFlags& flags);
    const DimensionFlags& projectFlags() const { return m_flags; }

    void attachWorld2D(world2d::World2D* world);
    void attachWorld3D(world3d::World3D* world);

    world2d::World2D* world2D() { return m_world2D; }
    world3d::World3D* world3D() { return m_world3D; }

    PlaceholderRenderer& renderer() { return m_renderer; }
    const PlaceholderRenderer& renderer() const { return m_renderer; }

    void tick(frame::FrameCtx& ctx);
    void render(frame::FrameCtx& ctx);

    u32 frameCount() const { return m_frameCount; }

private:
    void applyProjectFlags();

    DimensionFlags m_flags;
    world2d::World2D* m_world2D = nullptr;
    world3d::World3D* m_world3D = nullptr;
    frame::FrameBarrier m_barrier;
    PlaceholderRenderer m_renderer;
    u32 m_frameCount = 0;
};

} // namespace fuse::hybrid
