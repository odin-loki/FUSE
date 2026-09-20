#include "demo_check.hpp"
#include "demo_project_wiring.hpp"

#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_composer.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    fuse::core::initialize();
    fuse::log::info("demo_3d_empty: 3D dimension path + U7 mission convert/load");

    std::string projectPath = "Samples/unification/demo_3d_empty";
    if (argc > 1) {
        projectPath = argv[1];
    }

    const fuse::project::LoadResult project = fuse::project::loadFromDirectory(projectPath);
    fuse::demo::check(project.status == fuse::project::LoadStatus::Ok, "project.json loads");
    fuse::demo::check(project.manifest.dimensions.enable3D, "project enables 3D");

    const fuse::demo::wiring::ProjectRuntimeContext runtime =
        fuse::demo::wiring::prepareProjectRuntime(project);
    fuse::demo::check(runtime.ok, "project VFS mounts");
    fuse::demo::check(runtime.vfs.mountsAdded >= 1u, "at least one project asset root mounted");

    fuse::scene::Scene runtimeScene;
    const fuse::demo::wiring::World3DLoadResult worldLoad =
        fuse::demo::wiring::ensure3DWorldFromProject(project, runtimeScene);
    fuse::demo::check(worldLoad.ok, "bundled ExampleLevel.mis converts and loads");
    fuse::demo::check(worldLoad.entityCount >= 2u, "converted scene has mission entities");
    fuse::demo::check(worldLoad.wiringStubCount >= 1u || worldLoad.datablockBindings >= 1u ||
                          worldLoad.materialBindings >= 1u,
                      "mission wiring stubs or resolved bindings preserved");

    fuse::hybrid::HybridComposer composer;
    fuse::world3d::World3D world3D;
    fuse::SceneObject3D floorProxy("Floor");
    floorProxy.setPosition(0.f, 0.f);
    floorProxy.setZ(0.f);
    world3D.addObject(&floorProxy);

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
    fuse::demo::check(world3D.readSnapshot().objects().size() >= 1u, "3D snapshot includes floor proxy");

    fuse::core::shutdown();
    return fuse::demo::finish("demo_3d_empty");
}
