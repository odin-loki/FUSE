#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/vfx/effect_instance.hpp>
#include <fuse/vfx/particle_emitter.hpp>
#include <fuse/vfx/particle_gpu.hpp>
#include <fuse/vfx/particle_soa_ops.hpp>
#include <fuse/vfx/particle_system.hpp>

#include <cmath>
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

void expectNear(fuse::f32 actual, fuse::f32 expected, fuse::f32 epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (expected %.4f, got %.4f)\n", message, expected, actual);
        ++g_failures;
    }
}

void expectEq(fuse::u32 actual, fuse::u32 expected, const char* message) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s (expected %u, got %u)\n", message, expected, actual);
        ++g_failures;
    }
}

template <typename Body>
void withScheduler(fuse::u32 workers, Body&& body) {
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    scheduler.initialize(workers);
    body();
    scheduler.shutdown();
}

void testParticleEmitterBurstAndSimulate() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 16;
    desc.lifetime_min = 0.5f;
    desc.lifetime_max = 0.5f;
    desc.emit_rate = 0.f;
    desc.gravity = {0.f, -10.f, 0.f};
    desc.drag = 0.f;
    desc.velocity_min = {0.f, 0.f, 0.f};
    desc.velocity_max = {0.f, 0.f, 0.f};

    emitter.init(desc);
    emitter.set_position({0.f, 1.f, 0.f});
    emitter.burst(3);

    expectTrue(emitter.initialized(), "emitter initialized");
    expectEq(emitter.alive_count(), 3u, "burst emits requested particles");

    const fuse::f32 yBefore = emitter.particles().positions[0].y;
    emitter.simulate(0.1f);
    const fuse::f32 yAfter = emitter.particles().positions[0].y;
    expectTrue(yAfter < yBefore, "gravity integrates particle downward");

    emitter.simulate(1.f);
    expectEq(emitter.alive_count(), 0u, "particles expire after lifetime");
}

void testParticleEmitterBurstCapacity() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 4;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 10.f;
    desc.lifetime_max = 10.f;

    emitter.init(desc);
    emitter.burst(4);
    expectEq(emitter.alive_count(), 4u, "burst fills capacity");

    emitter.burst(2);
    expectEq(emitter.alive_count(), 4u, "burst beyond capacity is clamped");

    emitter.simulate(11.f);
    expectEq(emitter.alive_count(), 0u, "expired particles return slots to free list");
    expectEq(static_cast<fuse::u32>(emitter.particles().free_slots.size()), 4u,
             "all slots recycled after expiry");

    emitter.burst(1);
    expectEq(emitter.alive_count(), 1u, "slot reuse allows emission after expiry");
}

void testParticleEmitterEmitRate() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 8;
    desc.emit_rate = 10.f;
    desc.lifetime_min = 5.f;
    desc.lifetime_max = 5.f;

    emitter.init(desc);
    emitter.simulate(0.5f);
    expectTrue(emitter.alive_count() >= 4u, "emit rate produces particles over time");
    expectTrue(emitter.alive_count() <= 8u, "emit rate respects capacity");
}

void testParticleEmitterEmitRateSteadyState() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 64;
    desc.emit_rate = 20.f;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    emitter.init(desc);
    for (fuse::u32 step = 0; step < 200u; ++step) {
        emitter.simulate(0.01f);
    }
    const fuse::u32 afterWarmup = emitter.alive_count();
    expectTrue(afterWarmup >= 18u && afterWarmup <= 22u,
               "steady-state emit rate approximates particles per second");
}

void testParticleAttributeInterpolation() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 1;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 2.f;
    desc.lifetime_max = 2.f;
    desc.size_start = 1.f;
    desc.size_end = 0.f;
    desc.color_start = {1.f, 0.f, 0.f};
    desc.color_end = {0.f, 1.f, 0.f};
    desc.alpha_start = 1.f;
    desc.alpha_end = 0.f;
    desc.gravity = {};
    desc.drag = 0.f;
    desc.velocity_min = {};
    desc.velocity_max = {};

    emitter.init(desc);
    emitter.burst(1);
    emitter.simulate(1.f);

    const fuse::vfx::ParticleSoA& particles = emitter.particles();
    expectNear(particles.sizes[0], 0.5f, 0.05f, "size interpolates over normalized lifetime");
    expectNear(particles.colors[0].x, 0.5f, 0.05f, "color red channel interpolates");
    expectNear(particles.colors[0].y, 0.5f, 0.05f, "color green channel interpolates");
    expectNear(particles.alphas[0], 0.5f, 0.05f, "alpha interpolates over normalized lifetime");
}

void testParticleDragIntegration() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 1;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 5.f;
    desc.lifetime_max = 5.f;
    desc.gravity = {};
    desc.drag = 1.f;
    desc.velocity_min = {10.f, 0.f, 0.f};
    desc.velocity_max = {10.f, 0.f, 0.f};

    emitter.init(desc);
    emitter.burst(1);
    emitter.simulate(0.5f);

    const fuse::f32 speed = emitter.particles().velocities[0].x;
    expectTrue(speed < 10.f && speed > 0.f, "drag reduces velocity magnitude");
}

void testParallelSimulationParity() {
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 128;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 2.f;
    desc.lifetime_max = 2.f;
    desc.gravity = {0.f, -9.81f, 0.f};
    desc.drag = 0.05f;
    desc.velocity_min = {-1.f, 2.f, -1.f};
    desc.velocity_max = {1.f, 4.f, 1.f};

    fuse::f32 serialY = 0.f;
    fuse::u32 serialAlive = 0;
    withScheduler(0, [&] {
        fuse::vfx::ParticleEmitter emitter{};
        emitter.init(desc);
        emitter.set_position({0.f, 5.f, 0.f});
        emitter.burst(64);
        emitter.simulate(0.25f);
        serialAlive = emitter.alive_count();
        serialY = emitter.particles().positions[0].y;
        emitter.destroy();
    });

    fuse::f32 parallelY = 0.f;
    fuse::u32 parallelAlive = 0;
    withScheduler(4, [&] {
        fuse::vfx::ParticleEmitter emitter{};
        emitter.init(desc);
        emitter.set_position({0.f, 5.f, 0.f});
        emitter.burst(64);
        emitter.simulate(0.25f);
        parallelAlive = emitter.alive_count();
        parallelY = emitter.particles().positions[0].y;
        emitter.destroy();
    });

    expectEq(parallelAlive, serialAlive, "parallel simulation matches serial alive count");
    expectNear(parallelY, serialY, 1e-4f, "parallel simulation matches serial integration");
}

void testParticleEmitterBurstZero() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 8;

    emitter.init(desc);
    emitter.burst(0);
    expectEq(emitter.alive_count(), 0u, "burst(0) emits nothing");
    expectEq(emitter.free_slot_count(), 8u, "burst(0) leaves all slots free");
}

void testParticleEmitterUninitializedBurst() {
    fuse::vfx::ParticleEmitter emitter{};
    emitter.burst(4);
    expectEq(emitter.alive_count(), 0u, "burst on uninitialized emitter is a no-op");
}

void testSimulateNonPositiveDt() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 4;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 5.f;
    desc.lifetime_max = 5.f;

    emitter.init(desc);
    emitter.burst(2);
    const fuse::f32 yBefore = emitter.particles().positions[0].y;

    emitter.simulate(0.f);
    expectEq(emitter.alive_count(), 2u, "simulate(0) does not expire particles");
    expectNear(emitter.particles().positions[0].y, yBefore, 1e-5f, "simulate(0) does not integrate");

    emitter.simulate(-0.1f);
    expectEq(emitter.alive_count(), 2u, "simulate(negative dt) is a no-op");
}

void testDisabledEmitterSkipsSimulation() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 2;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 0.1f;
    desc.lifetime_max = 0.1f;
    desc.gravity = {0.f, -10.f, 0.f};

    emitter.init(desc);
    emitter.burst(1);
    const fuse::f32 yBefore = emitter.particles().positions[0].y;

    emitter.set_enabled(false);
    emitter.simulate(1.f);
    expectEq(emitter.alive_count(), 1u, "disabled emitter keeps particles alive");
    expectNear(emitter.particles().positions[0].y, yBefore, 1e-5f,
               "disabled emitter does not integrate motion");
}

void testPartialSlotRecycle() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 4;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 0.2f;
    desc.lifetime_max = 0.2f;

    emitter.init(desc);
    emitter.burst(4);
    expectEq(emitter.free_slot_count(), 0u, "full emitter has no free slots");

    emitter.simulate(0.15f);
    expectEq(emitter.alive_count(), 4u, "particles still alive before lifetime ends");
    expectEq(emitter.free_slot_count(), 0u, "no slots recycled before expiry");

    emitter.simulate(0.1f);
    expectEq(emitter.alive_count(), 0u, "all particles expired");
    expectEq(emitter.free_slot_count(), 4u, "expired particles return all slots");

    emitter.burst(2);
    expectEq(emitter.alive_count(), 2u, "partial burst after recycle");
    expectEq(emitter.free_slot_count(), 2u, "two slots remain free after partial burst");
}

void testFreeListReuseAfterMixedExpiry() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 6;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 0.4f;
    desc.lifetime_max = 0.4f;

    emitter.init(desc);
    emitter.burst(3);
    emitter.simulate(0.25f);
    expectEq(emitter.alive_count(), 3u, "first burst remains alive before expiry window");

    emitter.simulate(0.2f);
    expectEq(emitter.alive_count(), 0u, "first burst expires and returns slots");
    expectEq(emitter.free_slot_count(), 6u, "all slots available after first burst expiry");

    emitter.burst(2);
    expectEq(emitter.alive_count(), 2u, "second burst allocates recycled slots");
    expectEq(emitter.free_slot_count(), 4u, "free list shrinks after second burst");

    emitter.simulate(0.5f);
    expectEq(emitter.alive_count(), 0u, "second burst expires cleanly");
    expectEq(emitter.free_slot_count(), 6u, "slots fully recycled after second expiry");
}

void testBurstThenRateFill() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 8;
    desc.emit_rate = 10.f;
    desc.lifetime_min = 10.f;
    desc.lifetime_max = 10.f;

    emitter.init(desc);
    emitter.burst(5);
    expectEq(emitter.alive_count(), 5u, "burst pre-fills half capacity");
    expectEq(emitter.free_slot_count(), 3u, "remaining free slots after burst");

    emitter.simulate(0.5f);
    expectEq(emitter.alive_count(), 8u, "rate emission fills remaining capacity");
    expectEq(emitter.free_slot_count(), 0u, "rate emission exhausts free list at capacity");
}

void testEmitRateAccumulatorAtCapacity() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 2;
    desc.emit_rate = 100.f;
    desc.lifetime_min = 10.f;
    desc.lifetime_max = 10.f;

    emitter.init(desc);
    emitter.simulate(1.f);
    expectEq(emitter.alive_count(), 2u, "rate emission stops at capacity");
    expectEq(emitter.free_slot_count(), 0u, "capacity leaves no free slots");

    emitter.simulate(1.f);
    expectEq(emitter.alive_count(), 2u, "rate emission at capacity does not over-emit");
}

template <typename Body>
void expectParallelParity(fuse::u32 workers, Body&& setupAndSimulate) {
    fuse::u32 serialAlive = 0;
    fuse::f32 serialChecksum = 0.f;
    withScheduler(0, [&] {
        fuse::vfx::ParticleEmitter emitter{};
        setupAndSimulate(emitter);
        serialAlive = emitter.alive_count();
        for (fuse::u32 i = 0; i < emitter.particles().capacity; ++i) {
            if (emitter.particles().alive_flags[i] != 0U) {
                serialChecksum += emitter.particles().positions[i].y;
            }
        }
        emitter.destroy();
    });

    fuse::u32 parallelAlive = 0;
    fuse::f32 parallelChecksum = 0.f;
    withScheduler(workers, [&] {
        fuse::vfx::ParticleEmitter emitter{};
        setupAndSimulate(emitter);
        parallelAlive = emitter.alive_count();
        for (fuse::u32 i = 0; i < emitter.particles().capacity; ++i) {
            if (emitter.particles().alive_flags[i] != 0U) {
                parallelChecksum += emitter.particles().positions[i].y;
            }
        }
        emitter.destroy();
    });

    expectEq(parallelAlive, serialAlive, "parallel alive count matches serial");
    expectNear(parallelChecksum, serialChecksum, 1e-3f, "parallel integration checksum matches serial");
}

void testParallelGrainBoundary() {
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 65;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 2.f;
    desc.lifetime_max = 2.f;
    desc.gravity = {0.f, -1.f, 0.f};
    desc.drag = 0.01f;

    auto setup = [&](fuse::vfx::ParticleEmitter& emitter) {
        emitter.init(desc);
        emitter.set_position({0.f, 2.f, 0.f});
        emitter.burst(65);
        emitter.simulate(0.1f);
    };

    expectParallelParity(4, setup);
}

void testParallelSingleParticle() {
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 1;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 3.f;
    desc.lifetime_max = 3.f;
    desc.gravity = {0.f, -5.f, 0.f};

    auto setup = [&](fuse::vfx::ParticleEmitter& emitter) {
        emitter.init(desc);
        emitter.burst(1);
        emitter.simulate(0.05f);
    };

    expectParallelParity(4, setup);
}

void testParallelAllDeadNoOp() {
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 32;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 0.05f;
    desc.lifetime_max = 0.05f;

    auto setup = [&](fuse::vfx::ParticleEmitter& emitter) {
        emitter.init(desc);
        emitter.burst(16);
        emitter.simulate(0.1f);
        expectEq(emitter.alive_count(), 0u, "precondition: all particles expired");
        emitter.simulate(0.25f);
    };

    expectParallelParity(4, setup);
}

void testParallelMultiWorkerParity() {
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 128;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 1.5f;
    desc.lifetime_max = 1.5f;
    desc.gravity = {0.f, -9.81f, 0.f};
    desc.drag = 0.1f;
    desc.velocity_min = {-2.f, 1.f, -2.f};
    desc.velocity_max = {2.f, 3.f, 2.f};

    auto setup = [&](fuse::vfx::ParticleEmitter& emitter) {
        emitter.init(desc);
        emitter.set_position({1.f, 4.f, -1.f});
        emitter.burst(96);
        emitter.simulate(0.2f);
    };

#if !FUSE_JOBS_SINGLE_THREAD
    expectParallelParity(1, setup);
    expectParallelParity(2, setup);
#endif
    expectParallelParity(4, setup);
}

void testParticleSystemSpawnBurstCount() {
    fuse::vfx::ParticleSystem system{};
    system.init({});

    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 16;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 5.f;
    desc.lifetime_max = 5.f;

    const fuse::Handle<fuse::vfx::EffectInstance> effect =
        system.spawn_effect(desc, {0.f, 0.f, 0.f}, 10.f, 6u);
    expectTrue(effect.isValid(), "spawn_effect with burst_count returns valid handle");
    expectEq(system.alive_particle_count(), 6u, "spawn_effect burst_count seeds particles");
}

void testEffectInstanceLifecycle() {
    fuse::vfx::EffectInstance effect{};
    effect.duration = 1.f;
    effect.play();
    expectTrue(effect.is_alive(), "effect alive after play");

    effect.tick(0.5f);
    expectTrue(effect.is_alive(), "effect alive before duration elapses");

    effect.tick(0.6f);
    expectTrue(!effect.is_alive(), "effect stops after duration");

    effect.stop();
    expectTrue(!effect.is_alive(), "stopped effect is not alive");
}

void testParticleSystemSpawnAndUpdate() {
    fuse::vfx::ParticleSystem system{};
    fuse::vfx::VfxDesc desc{};
    system.init(desc);
    expectTrue(system.is_initialized(), "particle system initialized");
    expectTrue(system.backend_kind() == fuse::vfx::VfxBackendKind::CpuReference,
               "default backend is CPU reference stub");

    fuse::vfx::ParticleEmitterDesc emitterDesc{};
    emitterDesc.max_particles = 32;
    emitterDesc.emit_rate = 20.f;
    emitterDesc.lifetime_min = 2.f;
    emitterDesc.lifetime_max = 2.f;

    const fuse::Handle<fuse::vfx::EffectInstance> effect =
        system.spawn_effect(emitterDesc, {1.f, 0.f, 0.f}, 0.25f);
    expectTrue(effect.isValid(), "spawn_effect returns valid handle");
    expectEq(system.effect_count(), 1u, "spawn registers one effect");
    expectEq(system.emitter_count(), 1u, "spawn creates backing emitter");
    expectTrue(system.alive_particle_count() >= 1u, "spawned effect emits particles");

    system.update(0.1f);
    expectTrue(system.alive_particle_count() >= 1u, "update keeps particles alive");

    system.update(0.2f);
    expectEq(system.effect_count(), 0u, "finished effect is cleaned up");
}

void testParticleGpuBufferLayout() {
    using fuse::vfx::ParticleGpuBufferLayout;
    using fuse::vfx::ParticleGpuColumn;

    expectTrue(ParticleGpuBufferLayout::columnCount() == 8u, "eight GPU SoA columns");
    expectTrue(ParticleGpuBufferLayout::elementSize(ParticleGpuColumn::Positions) == sizeof(fuse::math::Vec3),
               "positions column element size");
    expectTrue(ParticleGpuBufferLayout::elementSize(ParticleGpuColumn::AliveFlags) == sizeof(fuse::u32),
               "alive_flags column element size");

    const fuse::u32 capacity = 64u;
    expectTrue(ParticleGpuBufferLayout::validatePackedLayout(capacity), "packed layout is contiguous and aligned");
    expectTrue(ParticleGpuBufferLayout::columnDeviceOffset(ParticleGpuColumn::Positions, capacity) == 0u,
               "positions column starts at offset zero");
    expectTrue(ParticleGpuBufferLayout::columnDeviceOffset(ParticleGpuColumn::Velocities, capacity) >=
                   ParticleGpuBufferLayout::columnByteSize(ParticleGpuColumn::Positions, capacity),
               "velocities column follows positions");

    const fuse::usize total = ParticleGpuBufferLayout::packedDeviceBytes(capacity);
    expectTrue(total > ParticleGpuBufferLayout::columnByteSize(ParticleGpuColumn::Positions, capacity),
               "packed SSBO includes alignment padding");
}

void testParticleGpuDispatchCounts() {
    const fuse::vfx::ParticleGpuDispatch sim = fuse::vfx::ParticleGpuDispatch::forSimulate(4096u);
    expectEq(sim.simThreadCount, fuse::vfx::ParticleGpuBufferLayout::kSimBlockSize, "simulate block size");
    expectEq(sim.simBlockCount, 16u, "4096 particles dispatch 16 blocks of 256");
    expectEq(sim.totalSimThreads(), 4096u, "simulate launch covers full capacity");

    const fuse::vfx::ParticleGpuDispatch partial = fuse::vfx::ParticleGpuDispatch::forSimulate(100u);
    expectEq(partial.simBlockCount, 1u, "partial capacity rounds up to one block");
    expectTrue(partial.totalSimThreads() >= 100u, "simulate launch covers partial capacity");

    const fuse::vfx::ParticleGpuDispatch emit = fuse::vfx::ParticleGpuDispatch::forEmit(200u);
    expectEq(emit.emitThreadCount, fuse::vfx::ParticleGpuBufferLayout::kEmitBlockSize, "emit block size");
    expectEq(emit.emitBlockCount, 4u, "200 emit threads need four 64-wide blocks");
    expectTrue(emit.totalEmitThreads() >= 200u, "emit launch covers requested count");

    const fuse::vfx::ParticleGpuDispatch noEmit = fuse::vfx::ParticleGpuDispatch::forEmit(0u);
    expectEq(noEmit.emitBlockCount, 0u, "zero emit count skips launch");
}

void testParticleGpuMirrorRoundTrip() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 8;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 5.f;
    desc.lifetime_max = 5.f;
    desc.gravity = {0.f, -1.f, 0.f};
    desc.drag = 0.f;
    desc.velocity_min = {0.f, 2.f, 0.f};
    desc.velocity_max = {0.f, 2.f, 0.f};

    emitter.init(desc);
    emitter.burst(3);
    emitter.simulate(0.1f);

    const fuse::vfx::ParticleGpuMirror mirror = fuse::vfx::ParticleGpuMirror::fromCpuSoA(emitter.particles());
    expectTrue(mirror.matchesCpuSoA(emitter.particles()), "mirror matches live CPU SoA");

    const std::vector<fuse::u8> packed = mirror.packToDeviceLayout();
    expectEq(static_cast<fuse::u32>(packed.size()),
             static_cast<fuse::u32>(fuse::vfx::ParticleGpuBufferLayout::packedDeviceBytes(desc.max_particles)),
             "packed bytes match layout size");

    const fuse::vfx::ParticleGpuMirror restored =
        fuse::vfx::ParticleGpuMirror::unpackFromDeviceLayout(packed, desc.max_particles);
    expectTrue(restored.matchesCpuSoA(emitter.particles()), "device-layout round trip preserves live slots");

    fuse::vfx::ParticleSoA copy{};
    copy.capacity = desc.max_particles;
    copy.positions.assign(desc.max_particles, {});
    copy.velocities.assign(desc.max_particles, {});
    copy.ages.assign(desc.max_particles, 0.f);
    copy.lifetimes.assign(desc.max_particles, 0.f);
    copy.sizes.assign(desc.max_particles, 0.f);
    copy.colors.assign(desc.max_particles, {});
    copy.alphas.assign(desc.max_particles, 0.f);
    copy.alive_flags.assign(desc.max_particles, 0u);
    copy.free_slots = emitter.particles().free_slots;

    mirror.writeToCpuSoA(copy);
    expectEq(copy.count, emitter.alive_count(), "mirror write restores alive count");
    expectNear(copy.positions[0].y, emitter.particles().positions[0].y, 1e-5f,
               "mirror write copies integrated position");
}

void testParticleGpuPointerBundle() {
    const fuse::u32 capacity = 32u;
    const fuse::u64 base = 0x1000u;

    fuse::vfx::ParticleGpuMirror mirror{};
    mirror.reserve(capacity);
    mirror.alive_flags[0] = 1u;
    mirror.alive_count = 1u;

    const fuse::vfx::ParticleSoAGPU gpu = mirror.toGpuPointers(base);
    expectEq(gpu.capacity, capacity, "GPU bundle carries capacity");
    expectEq(gpu.count, 1u, "GPU bundle carries alive count");
    expectTrue(gpu.positions == base, "positions pointer uses packed base");
    expectTrue(gpu.velocities > gpu.positions, "velocities pointer follows aligned positions column");
    expectTrue(gpu.alive_flags > gpu.alphas, "alive_flags pointer is last column");
}

void testSoaOpsBurstFill() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 8;
    desc.lifetime_min = 2.f;
    desc.lifetime_max = 2.f;
    desc.velocity_min = {1.f, 0.f, 0.f};
    desc.velocity_max = {1.f, 0.f, 0.f};

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    const fuse::vfx::particle_soa::BurstEmitResult burst =
        fuse::vfx::particle_soa::burst_emit(soa, desc, {0.f, 1.f, 0.f}, 5u, 42u);

    expectEq(burst.emitted, 5u, "soa burst_emit fills requested count");
    expectEq(soa.count, 5u, "soa count tracks burst emissions");
    expectEq(static_cast<fuse::u32>(soa.free_slots.size()), 3u, "soa free list shrinks after burst");

    const fuse::vfx::particle_soa::BurstEmitResult clamped =
        fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 10u, burst.seed_after);
    expectEq(clamped.emitted, 3u, "soa burst_emit clamps to remaining free slots");
    expectEq(soa.count, 8u, "soa burst_emit reaches capacity");
}

void testSoaOpsDeterministicSeed() {
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 4;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;
    desc.position_spread = {0.25f, 0.f, 0.25f};
    desc.velocity_min = {-1.f, 0.f, -1.f};
    desc.velocity_max = {1.f, 2.f, 1.f};

    fuse::vfx::ParticleSoA left{};
    fuse::vfx::ParticleSoA right{};
    fuse::vfx::particle_soa::init(left, desc.max_particles);
    fuse::vfx::particle_soa::init(right, desc.max_particles);

    const fuse::u64 seed = 0xDEADBEEFu;
    (void)fuse::vfx::particle_soa::burst_emit(left, desc, {1.f, 2.f, 3.f}, 3u, seed);
    (void)fuse::vfx::particle_soa::burst_emit(right, desc, {1.f, 2.f, 3.f}, 3u, seed);

    for (fuse::u32 i = 0; i < desc.max_particles; ++i) {
        if (left.alive_flags[i] == 0U) {
            continue;
        }
        expectNear(left.positions[i].x, right.positions[i].x, 1e-6f, "deterministic burst position.x");
        expectNear(left.positions[i].y, right.positions[i].y, 1e-6f, "deterministic burst position.y");
        expectNear(left.velocities[i].z, right.velocities[i].z, 1e-6f, "deterministic burst velocity.z");
        expectNear(left.lifetimes[i], right.lifetimes[i], 1e-6f, "deterministic burst lifetime");
    }
}

void testSoaOpsAgeKill() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 4;
    desc.lifetime_min = 0.2f;
    desc.lifetime_max = 0.2f;
    desc.gravity = {};
    desc.drag = 0.f;
    desc.velocity_min = {};
    desc.velocity_max = {};

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 4u, 7u);
    expectEq(soa.count, 4u, "precondition: four live particles");

    const fuse::vfx::particle_soa::SimStepResult step =
        fuse::vfx::particle_soa::simulate_step(soa, desc, 0.25f);
    expectEq(step.alive_after, 0u, "soa simulate_step kills expired particles");
    expectEq(static_cast<fuse::u32>(step.dead_slots.size()), 4u, "soa simulate_step collects dead slots");
    expectEq(static_cast<fuse::u32>(soa.free_slots.size()), 4u, "soa simulate_step recycles all slots");
}

void testSoaOpsRateEmit() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 6;
    desc.emit_rate = 10.f;
    desc.lifetime_min = 5.f;
    desc.lifetime_max = 5.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    fuse::f32 accum = 0.f;
    fuse::u64 seed = 99u;
    const fuse::vfx::particle_soa::RateEmitResult first =
        fuse::vfx::particle_soa::accumulate_rate_emit(soa, desc, {}, 0.5f, accum, seed);
    expectTrue(first.emitted >= 4u && first.emitted <= 6u, "soa rate emit produces particles from accumulator");
    expectTrue(first.accum_after >= 0.f && first.accum_after < 1.f, "soa rate emit leaves fractional remainder");

    const fuse::vfx::particle_soa::RateEmitResult second =
        fuse::vfx::particle_soa::accumulate_rate_emit(soa, desc, {}, 0.5f, first.accum_after, first.seed_after);
    expectEq(soa.count, 6u, "soa rate emit respects capacity");
    expectNear(second.accum_after, 0.f, 1e-5f, "soa rate emit clears accumulator at capacity");
}

void testSoaOpsParallelParity() {
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 96;
    desc.lifetime_min = 1.5f;
    desc.lifetime_max = 1.5f;
    desc.gravity = {0.f, -4.f, 0.f};
    desc.drag = 0.05f;
    desc.velocity_min = {-1.f, 1.f, -1.f};
    desc.velocity_max = {1.f, 3.f, 1.f};

    fuse::f32 serialChecksum = 0.f;
    fuse::u32 serialAlive = 0;
    withScheduler(0, [&] {
        fuse::vfx::ParticleSoA soa{};
        fuse::vfx::particle_soa::init(soa, desc.max_particles);
        (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {0.f, 3.f, 0.f}, 80u, 1234u);
        const fuse::vfx::particle_soa::SimStepResult step =
            fuse::vfx::particle_soa::simulate_step(soa, desc, 0.15f);
        serialAlive = step.alive_after;
        for (fuse::u32 i = 0; i < soa.capacity; ++i) {
            if (soa.alive_flags[i] != 0U) {
                serialChecksum += soa.positions[i].y + soa.velocities[i].y;
            }
        }
    });

    fuse::f32 parallelChecksum = 0.f;
    fuse::u32 parallelAlive = 0;
    withScheduler(4, [&] {
        fuse::vfx::ParticleSoA soa{};
        fuse::vfx::particle_soa::init(soa, desc.max_particles);
        (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {0.f, 3.f, 0.f}, 80u, 1234u);
        const fuse::vfx::particle_soa::SimStepResult step =
            fuse::vfx::particle_soa::simulate_step(soa, desc, 0.15f);
        parallelAlive = step.alive_after;
        for (fuse::u32 i = 0; i < soa.capacity; ++i) {
            if (soa.alive_flags[i] != 0U) {
                parallelChecksum += soa.positions[i].y + soa.velocities[i].y;
            }
        }
    });

    expectEq(parallelAlive, serialAlive, "soa simulate_step parallel alive count matches serial");
    expectNear(parallelChecksum, serialChecksum, 1e-3f, "soa simulate_step parallel checksum matches serial");
}

void testParticleSystemEmitterHandles() {
    fuse::vfx::ParticleSystem system{};
    system.init({});

    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 4;
    desc.emit_rate = 0.f;

    const fuse::Handle<fuse::vfx::ParticleEmitter> handle = system.create_emitter(desc);
    expectTrue(handle.isValid(), "create_emitter returns valid handle");

    fuse::vfx::ParticleEmitter* emitter = system.get_emitter(handle);
    expectTrue(emitter != nullptr, "emitter handle resolves");
    emitter->burst(2);
    expectEq(system.alive_particle_count(), 2u, "system aggregates alive particles");

    system.destroy_emitter(handle);
    expectEq(system.emitter_count(), 0u, "destroy_emitter removes emitter");
    expectTrue(system.get_emitter(handle) == nullptr, "destroyed handle no longer resolves");
}

} // namespace

int main() {
    fuse::core::initialize();

    testParticleEmitterBurstAndSimulate();
    testParticleEmitterBurstZero();
    testParticleEmitterUninitializedBurst();
    testParticleEmitterBurstCapacity();
    testSimulateNonPositiveDt();
    testDisabledEmitterSkipsSimulation();
    testPartialSlotRecycle();
    testFreeListReuseAfterMixedExpiry();
    testBurstThenRateFill();
    testEmitRateAccumulatorAtCapacity();
    testParticleEmitterEmitRate();
    testParticleEmitterEmitRateSteadyState();
    testParticleAttributeInterpolation();
    testParticleDragIntegration();
    testParallelSimulationParity();
    testParallelGrainBoundary();
    testParallelSingleParticle();
    testParallelAllDeadNoOp();
    testParallelMultiWorkerParity();
    testSoaOpsBurstFill();
    testSoaOpsDeterministicSeed();
    testSoaOpsAgeKill();
    testSoaOpsRateEmit();
    testSoaOpsParallelParity();
    testParticleGpuBufferLayout();
    testParticleGpuDispatchCounts();
    testParticleGpuMirrorRoundTrip();
    testParticleGpuPointerBundle();
    testEffectInstanceLifecycle();
    testParticleSystemSpawnAndUpdate();
    testParticleSystemSpawnBurstCount();
    testParticleSystemEmitterHandles();

    fuse::core::shutdown();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_vfx_tests: all passed\n");
    return EXIT_SUCCESS;
}
