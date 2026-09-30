// GAP-GAME-LOOP-ECS gate: a headless World3D ticks every engine system from its ECS registry in the
// fixed runtime order (input -> scripts -> animation -> physics -> transform -> camera -> audio ->
// VFX -> render extraction), with a falling rigid body (a scene object and an ECS-only body), an
// animated entity, a playing AudioSource and a particle emitter. Checks:
//   * stage order (SystemScheduler order + the per-step trace + the input/scripts hooks),
//   * every system did its job (bodies fell and settled, animator ticked, audio advanced, particles live),
//   * two independent runs are bit-identical frame by frame (animation evaluates on 3 workers),
//   * zero heap allocations in steady state (global operator new counted on every thread).

#include <fuse/animation/animation_system.hpp>
#include <fuse/animation/blend_tree.hpp>
#include <fuse/animation/clip.hpp>
#include <fuse/ecs/components/camera.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/vfx/vfx_ecs_system.hpp>
#include <fuse/world3d/runtime_schedule.hpp>
#include <fuse/world3d/world_3d.hpp>

#if defined(FUSE_WORLD3D_HAS_AUDIO) && FUSE_WORLD3D_HAS_AUDIO
#include <fuse/audio/audio_clip.hpp>
#include <fuse/audio/audio_components.hpp>
#endif
#if defined(FUSE_TEST_HAS_SCRIPT) && FUSE_TEST_HAS_SCRIPT
#include <fuse/world3d/runtime_schedule_script.hpp>
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>
#include <vector>

// ---- counting allocator (every thread; the aligned forms keep the library defaults) --------------------------------------------------------------
namespace {
std::atomic<unsigned long long> g_allocations{0};
std::atomic<bool> g_counting{false};

void* countedAlloc(std::size_t size) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

} // namespace

void* operator new(std::size_t size) { return countedAlloc(size); }
void* operator new[](std::size_t size) { return countedAlloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return countedAlloc(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return countedAlloc(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using fuse::f32;
using fuse::u32;
using fuse::u64;
using fuse::usize;
namespace anim = fuse::animation;
namespace ecs = fuse::ecs;
namespace w3 = fuse::world3d;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr f32 kDt = 1.f / 60.f;

struct StageLog {
    u32 inputCalls = 0;
    u32 scriptCalls = 0;
    u32 sequence = 0;
    u32 lastInputSeq = 0;
    u32 lastScriptSeq = 0;
};

void inputHook(void* user, ecs::Registry&, f32) {
    StageLog& log = *static_cast<StageLog*>(user);
    ++log.inputCalls;
    log.lastInputSeq = ++log.sequence;
}

void scriptsHook(void* user, ecs::Registry&, f32) {
    StageLog& log = *static_cast<StageLog*>(user);
    ++log.scriptCalls;
    log.lastScriptSeq = ++log.sequence;
}

struct Scenario {
    w3::World3D world;
    anim::Skeleton skeleton;
    anim::AnimationClip clip;
    std::unique_ptr<fuse::SceneObject3D> ball;
    ecs::EntityID body{};
    ecs::EntityID animated{};
    ecs::EntityID speaker{};
    ecs::EntityID listener{};
    ecs::EntityID emitter{};
    ecs::EntityID camera{};
    anim::AnimatorRef animatorRef{};
    std::vector<anim::AnimatorRef> crowd;
    StageLog log{};
};

void buildSkeleton(anim::Skeleton& skel, anim::AnimationClip& clip) {
    constexpr u32 kBones = 12;
    skel.bones.resize(kBones);
    skel.bone_count = kBones;
    for (u32 i = 0; i < kBones; ++i) {
        anim::Bone& bone = skel.bones[i];
        std::snprintf(bone.name, sizeof(bone.name), "b%u", i);
        bone.parent_index = static_cast<fuse::s32>(i) - 1;
        bone.local_transform = anim::mat4_from_trs({0.f, i == 0 ? 0.f : 0.5f, 0.f, 0.f}, {}, {1.f, 1.f, 1.f, 0.f});
    }
    const anim::Pose bind = anim::Pose::make_bind_pose(skel);
    for (u32 i = 0; i < kBones; ++i) {
        skel.bones[i].inverse_bind = anim::mat4_inverse_affine(bind.bone_world_transforms[i]);
    }
    clip.duration = 1.f;
    clip.looping = true;
    for (u32 i = 0; i < kBones; ++i) {
        anim::AnimationClip::BoneChannels ch{};
        ch.bone_index = i;
        ch.rotation.times = {0.f, 0.5f, 1.f};
        const f32 a = 0.1f + 0.02f * static_cast<f32>(i);
        ch.rotation.values_quat = {anim::quat{0.f, 0.f, 0.f, 1.f}, anim::quat{0.f, 0.f, std::sin(a), std::cos(a)},
                                   anim::quat{0.f, 0.f, 0.f, 1.f}};
        clip.bone_channels.push_back(ch);
    }
}

void setup(Scenario& s) {
    buildSkeleton(s.skeleton, s.clip);
    w3::World3D& world = s.world;
    world.setPhysicsEnabled(true);
    world.addStaticPlane({0.f, 1.f, 0.f}, 0.f);

    // Scene object with a physics body (falls onto the plane).
    s.ball = std::make_unique<fuse::SceneObject3D>("ball");
    s.ball->setPhysicsShape(fuse::PhysicsShape2D::Circle);
    s.ball->setPhysicsRadius(0.5f);
    s.ball->setPosition3D(0.f, 4.f, 0.f);
    world.addObject(s.ball.get());

    w3::RuntimeSchedule& schedule = world.schedule();
    schedule.setHook(w3::RuntimeStage::Input, &inputHook, &s.log);
    schedule.setHook(w3::RuntimeStage::Scripts, &scriptsHook, &s.log);
    ecs::Registry& reg = world.registry();

    // ECS-only rigid body.
    s.body = reg.create();
    ecs::Transform bodyT{};
    bodyT.position = {3.f, 2.f, 0.f, 1.f};
    reg.add<ecs::Transform>(s.body, bodyT);
    reg.add<ecs::RigidBody>(s.body, ecs::RigidBody{});
    ecs::Collider sphere{};
    sphere.shape = ecs::Collider::Sphere;
    sphere.params = {0.5f, 0.f, 0.f, 0.f};
    reg.add<ecs::Collider>(s.body, sphere);

    // Animated entity.
    s.animated = reg.create();
    reg.add<ecs::Transform>(s.animated, ecs::Transform{});
    anim::Animator animator;
    animator.state_machine = std::make_unique<anim::AnimStateMachine>();
    auto node = std::make_unique<anim::ClipNode>();
    node->clip = &s.clip;
    animator.state_machine->add_state("loop", std::move(node));
    s.animatorRef = schedule.animation().attach(reg, s.animated, std::move(animator), s.skeleton);
    // A crowd of extra animated entities so the parallel evaluation splits into several tasks.
    for (u32 i = 0; i < 15u; ++i) {
        const ecs::EntityID extra = reg.create();
        anim::Animator crowd;
        crowd.playback_rate = 0.5f + 0.1f * static_cast<f32>(i);
        crowd.state_machine = std::make_unique<anim::AnimStateMachine>();
        auto crowdNode = std::make_unique<anim::ClipNode>();
        crowdNode->clip = &s.clip;
        crowd.state_machine->add_state("loop", std::move(crowdNode));
        s.crowd.push_back(schedule.animation().attach(reg, extra, std::move(crowd), s.skeleton));
    }

#if defined(FUSE_WORLD3D_HAS_AUDIO) && FUSE_WORLD3D_HAS_AUDIO
    // Playing AudioSource (looping 440 Hz tone) and a listener.
    std::vector<float> tone(4800);
    for (usize i = 0; i < tone.size(); ++i) {
        tone[i] = 0.25f * std::sin(2.f * 3.14159265f * 440.f * static_cast<f32>(i) / 48000.f);
    }
    fuse::audio::AudioClip clip;
    clip.load_from_pcm(tone.data(), static_cast<u32>(tone.size()), 1u, 48000u);
    const fuse::Handle<fuse::audio::AudioClip> clipHandle = schedule.audioEngine().register_clip(std::move(clip));
    s.speaker = reg.create();
    ecs::Transform speakerT{};
    speakerT.position = {1.f, 1.f, 0.f, 1.f};
    reg.add<ecs::Transform>(s.speaker, speakerT);
    fuse::audio::AudioSource source{};
    source.desc.clip = clipHandle;
    source.desc.looping = true;
    source.desc.play_on_awake = true;
    reg.add<fuse::audio::AudioSource>(s.speaker, source);
    s.listener = reg.create();
    reg.add<ecs::Transform>(s.listener, ecs::Transform{});
    reg.add<fuse::audio::AudioListener>(s.listener, fuse::audio::AudioListener{});
#endif

    // Particle emitter.
    s.emitter = reg.create();
    ecs::Transform emitterT{};
    emitterT.position = {-2.f, 1.f, 0.f, 1.f};
    reg.add<ecs::Transform>(s.emitter, emitterT);
    fuse::vfx::VfxEmitter emitterC{};
    emitterC.max_particles = 256;
    emitterC.emit_rate = 120.f;
    emitterC.burst_on_start = 16;
    reg.add<fuse::vfx::VfxEmitter>(s.emitter, emitterC);

    // Active camera.
    s.camera = reg.create();
    ecs::Transform cameraT{};
    cameraT.position = {0.f, 2.f, 8.f, 1.f};
    reg.add<ecs::Transform>(s.camera, cameraT);
    ecs::Camera cam{};
    cam.is_active = true;
    reg.add<ecs::Camera>(s.camera, cam);
}

u64 fnv(u64 h, const void* data, usize size) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (usize i = 0; i < size; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

/// Bitwise digest of everything the systems wrote this frame.
u64 digest(Scenario& s) {
    u64 h = 14695981039346656037ull;
    ecs::Registry& reg = s.world.registry();
    h = fnv(h, s.ball->worldMatrix().data.data(), sizeof(f32) * 16u);
    const ecs::Transform* bodyT = reg.get<ecs::Transform>(s.body);
    h = fnv(h, &bodyT->position, sizeof(bodyT->position));
    h = fnv(h, &bodyT->local_to_world, sizeof(bodyT->local_to_world));
    const anim::AnimatorInstance* inst = s.world.schedule().animation().get(s.animatorRef);
    h = fnv(h, inst->animator.bone_palette.data(), inst->animator.bone_palette.size() * sizeof(anim::mat4));
    for (const anim::AnimatorRef ref : s.crowd) {
        const anim::AnimatorInstance* member = s.world.schedule().animation().get(ref);
        h = fnv(h, member->animator.bone_palette.data(), member->animator.bone_palette.size() * sizeof(anim::mat4));
    }
#if defined(FUSE_WORLD3D_HAS_AUDIO) && FUSE_WORLD3D_HAS_AUDIO
    const fuse::audio::AudioSource* src = reg.get<fuse::audio::AudioSource>(s.speaker);
    h = fnv(h, &src->play_head, sizeof(src->play_head));
    const std::vector<float>& mix = s.world.schedule().audioEngine().last_mix_buffer();
    h = fnv(h, mix.data(), mix.size() * sizeof(float));
#endif
    const fuse::vfx::ParticleEmitter* emitter =
        s.world.schedule().particles().get_emitter(s.world.schedule().vfx().emitter_of(s.emitter));
    if (emitter != nullptr) {
        const fuse::vfx::ParticleSoA& p = emitter->particles();
        h = fnv(h, p.positions.data(), p.positions.size() * sizeof(fuse::math::Vec3));
        h = fnv(h, p.alive_flags.data(), p.alive_flags.size() * sizeof(u32));
    }
    const ecs::Camera* cam = reg.get<ecs::Camera>(s.camera);
    h = fnv(h, &cam->view_projection, sizeof(cam->view_projection));
    return h;
}

void testOrderAndSystems() {
    Scenario s;
    setup(s);
    w3::RuntimeSchedule& schedule = s.world.schedule();

    // The scheduler's compiled order is the documented stage order.
    const char* expected[w3::kRuntimeStageCount] = {"Input", "Scripts", "Animation", "Physics", "Transform",
                                                    "Camera", "Audio", "Vfx", "RenderExtract"};
    fuse::ecs::SystemScheduler& scheduler = const_cast<fuse::ecs::SystemScheduler&>(schedule.scheduler());
    const std::vector<u32>& order = scheduler.execution_order();
    bool orderOk = order.size() == w3::kRuntimeStageCount;
    for (u32 i = 0; orderOk && i < order.size(); ++i) {
        orderOk = scheduler.system_name(order[i]) == std::string_view(expected[i]);
    }
    expectTrue(orderOk, "SystemScheduler order: input, scripts, animation, physics, transform, camera, audio, vfx, extract");

    fuse::frame::FrameCtx ctx{};
    ctx.dt = kDt;
    s.world.tick(ctx);
    expectTrue(schedule.stepCount() == 1u, "one frame of 1/60 s runs one fixed step");
    bool traceOk = schedule.lastStepOrderCount() == w3::kRuntimeStageCount;
    for (u32 i = 0; traceOk && i < w3::kRuntimeStageCount; ++i) {
        traceOk = schedule.lastStepOrder()[i] == static_cast<w3::RuntimeStage>(i);
    }
    expectTrue(traceOk, "per-step trace runs every stage in order");
    expectTrue(s.log.inputCalls == 1u && s.log.scriptCalls == 1u && s.log.lastInputSeq < s.log.lastScriptSeq,
               "input hook runs before the scripts hook");

    // Two frames' worth of time in one tick: two steps, extraction once.
    ctx.dt = 2.f * kDt;
    const u64 stepsBefore = schedule.stepCount();
    s.world.tick(ctx);
    expectTrue(schedule.stepCount() == stepsBefore + 2u, "accumulator runs two fixed steps for a 2/60 s frame");
    ctx.dt = kDt;

    for (int frame = 0; frame < 240; ++frame) {
        s.world.tick(ctx);
    }
    ecs::Registry& reg = s.world.registry();
    const f32 ballY = s.ball->worldTranslation().y;
    const f32 bodyY = reg.get<ecs::Transform>(s.body)->position.y;
    std::printf("[gate] after 4 s: ball y %.3f, ECS body y %.3f\n", static_cast<double>(ballY), static_cast<double>(bodyY));
    expectTrue(ballY < 1.f && ballY > 0.2f, "scene-object body fell and rests on the plane (PhysicsManager)");
    expectTrue(bodyY < 1.f && bodyY > 0.2f, "ECS rigid body fell and rests on the plane");
    expectTrue(reg.get<ecs::Transform>(s.body)->local_to_world.data[13] == bodyY,
               "TransformSystem ran after physics (local_to_world matches the body)");
    const anim::AnimatorInstance* inst = schedule.animation().get(s.animatorRef);
    expectTrue(inst != nullptr && inst->animator.tick_count == schedule.stepCount(), "animator evaluated every step");
    expectTrue(inst != nullptr && inst->animator.bone_palette.size() == s.skeleton.bones.size(), "skinning palette built");
#if defined(FUSE_WORLD3D_HAS_AUDIO) && FUSE_WORLD3D_HAS_AUDIO
    const fuse::audio::AudioSource* src = reg.get<fuse::audio::AudioSource>(s.speaker);
    expectTrue(src->playing && src->play_head > 0.0, "AudioSource component plays through AudioEngine");
    expectTrue(schedule.audio().stats().has_listener && schedule.audio().stats().active_sources == 1u,
               "audio system saw the listener and the source");
    expectTrue(schedule.audioEngine().blocks_rendered() == schedule.stepCount(), "one audio block per step");
#endif
    const fuse::vfx::VfxEmitter* em = reg.get<fuse::vfx::VfxEmitter>(s.emitter);
    expectTrue(em->alive_particles > 0u, "emitter component drives ParticleSystem");
    const ecs::Camera* cam = reg.get<ecs::Camera>(s.camera);
    expectTrue(cam->view_projection.data[15] != 1.f || cam->view_projection.data[0] != 1.f, "camera system ran");
    expectTrue(s.world.readSnapshot().objects().size() == 1u, "render extraction built the scene snapshot");
}

void testDeterminismAndAllocations() {
    constexpr int kWarmup = 120;
    constexpr int kMeasured = 240;
    Scenario a;
    Scenario b;
    setup(a);
    setup(b);
    fuse::frame::FrameCtx ctx{};
    ctx.dt = kDt;

    bool identical = true;
    int firstMismatch = -1;
    g_allocations.store(0);
    for (int frame = 0; frame < kWarmup + kMeasured; ++frame) {
        // Only run A's ticks are counted (on every thread), after the warm-up.
        g_counting.store(frame >= kWarmup);
        a.world.tick(ctx);
        g_counting.store(false);
        b.world.tick(ctx);
        if (digest(a) != digest(b)) {
            if (firstMismatch < 0) {
                firstMismatch = frame;
            }
            identical = false;
        }
    }
    const unsigned long long steadyAllocations = g_allocations.load();
    std::printf("[gate] determinism: %d frames, first mismatch %d; steady-state allocations over %d frames: %llu\n",
                kWarmup + kMeasured, firstMismatch, kMeasured, steadyAllocations);
    expectTrue(identical, "two runs are bit-identical every frame");
    expectTrue(steadyAllocations == 0u, "zero heap allocations per steady-state world tick");
    expectTrue(a.world.schedule().animation().stats().tasks > 1u, "animation evaluated on several tasks");
}

#if defined(FUSE_TEST_HAS_SCRIPT) && FUSE_TEST_HAS_SCRIPT
void testScriptBindingCompiles() {
    // bindScriptSystem installs the Scripts hook (the ScriptSystem itself needs a Lua VM; the E10
    // script gates cover its update path).
    w3::RuntimeSchedule schedule;
    fuse::script::ScriptSystem scripts;
    w3::bindScriptSystem(schedule, scripts);
    expectTrue(true, "bindScriptSystem compiles and links");
}
#endif

} // namespace

int main() {
    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::jobs::JobScheduler::instance().initialize(3);
    testOrderAndSystems();
    testDeterminismAndAllocations();
#if defined(FUSE_TEST_HAS_SCRIPT) && FUSE_TEST_HAS_SCRIPT
    testScriptBindingCompiles();
#endif
    fuse::jobs::JobScheduler::instance().shutdown();

    if (g_failures == 0) {
        std::printf("fuse runtime schedule gates: all checks passed\n");
        return EXIT_SUCCESS;
    }
    std::fprintf(stderr, "fuse runtime schedule gates: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
}
