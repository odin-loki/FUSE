// GAP-GAME-LOOP-ECS: PIE runs the World3D runtime schedule. A play session over an editor scene with
// a rigid body, an animated entity and a particle emitter steps all of them through
// PlaySession::runtimeSchedule() (physics through the session's PhysicsManager), and Stop restores
// the edit-time registry.

#include <fuse/animation/animation_system.hpp>
#include <fuse/animation/blend_tree.hpp>
#include <fuse/animation/clip.hpp>
#include <fuse/core/init.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/editor_state.hpp>
#include <fuse/editor/play_session.hpp>
#include <fuse/scene/scene.hpp>
#include <fuse/vfx/vfx_ecs_system.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

void testPieRunsRuntimeSchedule() {
    fuse::editor::EditorScene editorScene;
    editorScene.init();
    fuse::ecs::Registry& reg = editorScene.registry();

    const fuse::ecs::EntityID ball = reg.create();
    fuse::ecs::Transform ballT{};
    ballT.position = {0.f, 3.f, 0.f, 1.f};
    reg.add<fuse::ecs::Transform>(ball, ballT);
    reg.add<fuse::ecs::RigidBody>(ball, fuse::ecs::RigidBody{});
    reg.add<fuse::ecs::Collider>(ball, fuse::ecs::Collider{});

    const fuse::ecs::EntityID emitter = reg.create();
    reg.add<fuse::ecs::Transform>(emitter, fuse::ecs::Transform{});
    fuse::vfx::VfxEmitter emitterC{};
    emitterC.burst_on_start = 8;
    reg.add<fuse::vfx::VfxEmitter>(emitter, emitterC);

    fuse::animation::Skeleton skeleton;
    skeleton.bones.resize(2);
    skeleton.bone_count = 2;
    skeleton.bones[1].parent_index = 0;
    fuse::animation::AnimationClip clip;
    clip.duration = 1.f;

    fuse::scene::Scene scene("PieSchedule");
    fuse::editor::EditorState state;
    fuse::editor::PlaySession session;
    fuse::editor::PlayModePhysicsState physics;
    session.start(editorScene, scene, state, physics);
    fuse::world3d::RuntimeSchedule& schedule = session.runtimeSchedule();
    expectTrue(schedule.initialized() && schedule.registry() == &reg, "PIE schedule bound to the play registry");
    expectTrue(!schedule.stageEnabled(fuse::world3d::RuntimeStage::Transform),
               "PIE leaves TransformSystem to the editor (dirty flags are its change tracking)");

    const fuse::ecs::EntityID animated = reg.create();
    fuse::animation::Animator animator;
    animator.state_machine = std::make_unique<fuse::animation::AnimStateMachine>();
    auto node = std::make_unique<fuse::animation::ClipNode>();
    node->clip = &clip;
    animator.state_machine->add_state("idle", std::move(node));
    const fuse::animation::AnimatorRef ref = schedule.animation().attach(reg, animated, std::move(animator), skeleton);

    const fuse::u32 steps = session.tickFixedStep(0.51f, 1.f / 60.f, editorScene, physics);
    expectTrue(steps == 30u, "PIE drained 30 fixed steps");
    expectTrue(schedule.stepCount() == steps, "one schedule pass per simulated PIE step");
    expectTrue(session.physicsWorld().stepCount() == steps, "physics stage stepped the session's PhysicsManager");
    const fuse::ecs::Transform* ballNow = reg.get<fuse::ecs::Transform>(ball);
    expectTrue(ballNow != nullptr && ballNow->position.y < 3.f, "rigid body fell during PIE");
    const fuse::animation::AnimatorInstance* inst = schedule.animation().get(ref);
    expectTrue(inst != nullptr && inst->animator.tick_count == steps, "animator ticked every PIE step");
    const fuse::vfx::VfxEmitter* em = reg.get<fuse::vfx::VfxEmitter>(emitter);
    expectTrue(em != nullptr && em->alive_particles > 0u, "emitter simulated during PIE");

    session.stop(editorScene, scene, state, physics);
    expectTrue(!schedule.initialized(), "stop shuts the PIE schedule down");
    const fuse::ecs::Transform* ballAfter = reg.get<fuse::ecs::Transform>(ball);
    expectTrue(ballAfter != nullptr && ballAfter->position.y == 3.f, "stop restores the edit-time body pose");
    expectTrue(!reg.alive(animated) || !reg.has<fuse::animation::AnimatorRef>(animated),
               "entity spawned during play is gone after stop");
}

} // namespace

int main() {
    fuse::core::initialize();
    testPieRunsRuntimeSchedule();
    fuse::core::shutdown();
    if (g_failures == 0) {
        std::printf("fuse editor PIE schedule tests: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse editor PIE schedule tests: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
