// Engine game side for hybrid projects (see project_host_hybrid.hpp).
#include "fuse/app/project_host_hybrid.hpp"

#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <filesystem>

namespace fuse::app {

namespace {
struct HybridSide {
    world3d::World3D world3D; // worlds outlive the composer that points at them
    world2d::World2D world2D;
    hybrid::HybridComposer composer;
    frame::FrameCtx ctx;
};

void* create(const project::ProjectManifest& m, std::string& error) {
    auto* g = new HybridSide();
    g->composer.setProjectFlags(project::toDimensionFlags(m.dimensions));
    const std::filesystem::path root = m.projectRoot.empty() ? std::filesystem::path(".") : std::filesystem::path(m.projectRoot);
    std::error_code ec;
    if (m.dimensions.enable3D) {
        if (!m.defaultWorld3D.empty() && std::filesystem::exists(root / m.defaultWorld3D, ec)) {
            const auto r = g->world3D.loadWorldFromFuselevel((root / m.defaultWorld3D).string());
            if (!r.ok) error = "defaultWorld3D: " + r.note; // reported; the empty world still renders
        }
        g->world3D.setPhysicsEnabled(true);
        g->composer.attachWorld3D(&g->world3D);
    }
    if (m.dimensions.enable2D) {
        if (!m.defaultWorld2D.empty() && std::filesystem::exists(root / m.defaultWorld2D, ec))
            (void)g->world2D.loadWorldFromFuselevel((root / m.defaultWorld2D).string());
        g->composer.attachWorld2D(&g->world2D);
    }
    return g;
}

const unsigned char* frame(void* side, f32 dt, u32& width, u32& height) {
    auto& g = *static_cast<HybridSide*>(side);
    g.ctx.dt = dt;
    g.ctx.time += dt;
    g.composer.tick(g.ctx);
    g.composer.render(g.ctx);
    ++g.ctx.frameIndex;
    const hybrid::PlaceholderRenderer& fb = g.composer.renderer();
    width = fb.width();
    height = fb.height();
    return width && height ? fb.pixels() : nullptr;
}

void destroy(void* side) { delete static_cast<HybridSide*>(side); }
void* handle(void* side) { return &static_cast<HybridSide*>(side)->composer; }
} // namespace

GameSideBackend hybridGameSide() { return {&create, &frame, &destroy, &handle}; }

} // namespace fuse::app
