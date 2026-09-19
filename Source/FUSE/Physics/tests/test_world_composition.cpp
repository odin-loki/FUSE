#include <fuse/core/init.hpp>
#include <fuse/physics/physics_data.hpp>
#include <fuse/world2d/scene_object_2d.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testWorld2DComposesPhysics() {
    fuse::world2d::World2D world;
    world.setPhysicsEnabled(true);

    fuse::SceneObject2D sprite("physics_sprite");
    sprite.setPosition(0.f, 2.f);
    sprite.setPhysicsEnabled(true);
    world.addSprite(&sprite);

    fuse::frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    world.tick(ctx);

    expectTrue(world.physics().bodyCount() >= 1u, "World2D composes physics bodies");
}

void testWorld2DCollisionLayerFiltering() {
    fuse::world2d::World2D world;
    world.setPhysicsEnabled(true);

    fuse::SceneObject2D wall("wall");
    wall.setPosition(0.f, 0.f);
    wall.setPhysicsEnabled(true);
    wall.setPhysicsShape(fuse::PhysicsShape2D::Box);
    wall.setBoxHalfWidth(2.f);
    wall.setBoxHalfHeight(1.f);
    wall.setCollisionLayer(3);
    wall.setCollisionMask(5u);

    fuse::SceneObject2D ball("ball");
    ball.setPosition(0.1f, 0.f);
    ball.setPhysicsEnabled(true);
    ball.setPhysicsShape(fuse::PhysicsShape2D::Circle);
    ball.setPhysicsRadius(0.5f);
    ball.setCollisionLayer(1);
    ball.setCollisionMask(4u);

    world.addSprite(&wall);
    world.addSprite(&ball);

    fuse::frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    world.tick(ctx);

    const fuse::physics::PhysicsPipeline& pipeline = world.physics().pipeline();
    expectTrue(pipeline.bodyCount() == 2u, "two physics bodies with collision layers");
    expectTrue(pipeline.candidatePairs().empty(),
               "mismatched collision masks suppress broadphase pairs");

    ball.setCollisionMask(5u);
    world.setPhysicsEnabled(false);
    world.setPhysicsEnabled(true);
    world.tick(ctx);
    expectTrue(world.physics().pipeline().candidatePairs().size() >= 1u,
               "matching collision masks allow broadphase pairs");
}

void testWorld3DComposesPhysicsWithoutBox2D() {
    fuse::world3d::World3D world;
    world.setPhysicsEnabled(true);

    fuse::SceneObject3D object("physics_object");
    object.setPosition(0.f, 0.5f);
    object.setZ(0.f);
    world.addObject(&object);

    world.physics().addStaticPlane({0.f, 1.f, 0.f}, 0.f);

    fuse::frame::FrameCtx ctx;
    ctx.dt = 1.f / 60.f;
    world.tick(ctx);

    expectTrue(world.physics().bodyCount() >= 1u, "World3D composes FUSE physics pipeline");
    expectTrue(world.physics().contactCount() >= 1u, "World3D physics detects ground contact");
}

} // namespace

int main() {
    fuse::core::initialize();
    testWorld2DComposesPhysics();
    testWorld2DCollisionLayerFiltering();
    testWorld3DComposesPhysicsWithoutBox2D();
    fuse::core::shutdown();

    if (g_failures == 0) {
        std::printf("fuse_physics_world_composition_tests: all checks passed\n");
        return EXIT_SUCCESS;
    }

    std::fprintf(stderr, "fuse_physics_world_composition_tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
