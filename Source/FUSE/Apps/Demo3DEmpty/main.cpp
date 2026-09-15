#include "demo_check.hpp"

#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_3d_empty: 3D dimension path (software placeholder renderer)");

    std::string projectPath = "Samples/unification/demo_3d_empty";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.dimensions.enable3D, "project enables 3D");

    fuse::hybrid::HybridComposer composer;
    fuse::world3d::World3D world3D;
    composer.setProjectFlags(fuse::project::toDimensionFlags(project.manifest.dimensions));
    composer.attachWorld3D(&world3D);
    world3D.setClearColor(0.12f, 0.18f, 0.28f);
    world3D.loadWorld(fuse::dimension::WorldHandle(1u, 1u));

    fuse::frame::FrameCtx ctx;
    for (int frame = 0; frame < 30; ++frame) {
        ctx.dt = 1.f / 60.f;
        ctx.frameIndex = static_cast<fuse::u32>(frame);
        composer.tick(ctx);
        composer.render(ctx);
    }

    fuse::demo::check(composer.frameCount() == 30u, "30 frames ticked");
    fuse::demo::check(composer.renderer().sample(160, 120) > 0, "3D clear colour visible");

    fuse::core::shutdown();
    return fuse::demo::finish("demo_3d_empty");
}
