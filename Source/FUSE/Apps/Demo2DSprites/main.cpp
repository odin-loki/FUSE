#include "demo_check.hpp"

#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_2d_sprites: 2D dimension path (SpriteToy-inspired stub)");

    std::string projectPath = "Samples/unification/demo_2d_sprites";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.dimensions.enable2D, "project enables 2D");

    fuse::hybrid::HybridComposer composer;
    fuse::world2d::World2D world2D;
    fuse::SceneObject2D sprite("sprite_a");
    sprite.setPosition(0.f, 0.f);
    sprite.setLayer(1);
    world2D.addSprite(&sprite);

    composer.setProjectFlags(fuse::project::toDimensionFlags(project.manifest.dimensions));
    composer.attachWorld2D(&world2D);
    world2D.loadWorld(fuse::dimension::WorldHandle(2u, 1u));

    fuse::frame::FrameCtx ctx;
    for (int frame = 0; frame < 30; ++frame) {
        ctx.dt = 1.f / 60.f;
        ctx.frameIndex = static_cast<fuse::u32>(frame);
        composer.tick(ctx);
        composer.render(ctx);
    }

    fuse::demo::check(composer.frameCount() == 30u, "30 frames ticked");
    fuse::demo::check(composer.renderer().sample(160, 120) > 0, "2D sprite visible");

    fuse::core::shutdown();
    return fuse::demo::finish("demo_2d_sprites");
}
