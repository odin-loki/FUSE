#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/vfx/effect_instance.hpp>
#include <fuse/vfx/particle_emitter.hpp>
#include <fuse/vfx/particle_gpu.hpp>
#include <fuse/vfx/particle_soa_ops.hpp>
#include <fuse/vfx/particle_system.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

    expectTrue(mirror.writeToCpuSoA(copy), "mirror write succeeds on matching capacity");
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

void testParticleGpuLayoutSize() {
    using fuse::vfx::ParticleGpuBufferLayout;

    const fuse::u32 capacities[] = {1u, 17u, 64u, 257u};
    fuse::usize previous_packed = 0u;
    for (const fuse::u32 capacity : capacities) {
        const fuse::usize raw = ParticleGpuBufferLayout::dataColumnBytes(capacity);
        const fuse::usize packed = ParticleGpuBufferLayout::packedDeviceBytes(capacity);
        const fuse::usize overhead = ParticleGpuBufferLayout::packingOverheadBytes(capacity);

        expectTrue(raw > 0u, "raw column bytes are non-zero at capacity");
        expectTrue(packed >= raw, "packed SSBO size includes raw columns");
        expectEq(static_cast<fuse::u32>(overhead), static_cast<fuse::u32>(packed - raw),
                 "packing overhead matches packed minus raw");
        expectTrue(packed % ParticleGpuBufferLayout::columnAlignment() == 0u,
                   "packed size stays alignment-rounded");
        expectTrue(previous_packed < packed || capacity == 1u, "packed size grows with capacity");
        previous_packed = packed;
    }

    const fuse::u64 base = 0x2000u;
    expectEq(fuse::vfx::ParticleGpuBufferLayout::columnDeviceAddress(fuse::vfx::ParticleGpuColumn::Positions, base, 64u),
             base,
             "columnDeviceAddress for positions equals base");
    expectTrue(fuse::vfx::ParticleGpuBufferLayout::columnDeviceAddress(fuse::vfx::ParticleGpuColumn::Velocities, base, 64u) >
                   fuse::vfx::ParticleGpuBufferLayout::columnDeviceAddress(fuse::vfx::ParticleGpuColumn::Positions, base, 64u),
               "columnDeviceAddress advances for later columns");
    expectEq(fuse::vfx::ParticleGpuBufferLayout::columnDeviceAddress(fuse::vfx::ParticleGpuColumn::Positions, 0u, 64u),
             0u,
             "columnDeviceAddress returns zero for null base");
}

void testParticleGpuEmptyDispatch() {
    using fuse::vfx::ParticleGpuBufferLayout;
    using fuse::vfx::ParticleGpuDispatch;

    const fuse::vfx::ParticleGpuDispatch zero_sim = ParticleGpuDispatch::forSimulate(0u);
    expectEq(zero_sim.simBlockCount, 0u, "zero capacity simulate skips blocks");
    expectEq(zero_sim.simThreadCount, ParticleGpuBufferLayout::kSimBlockSize,
             "zero capacity simulate keeps block width");
    expectTrue(zero_sim.isEmpty(), "zero capacity simulate dispatch is empty");
    expectTrue(!zero_sim.hasSimLaunch(), "zero capacity simulate has no launch");
    expectTrue(zero_sim.simCovers(0u), "empty simulate covers zero slots");

    const fuse::vfx::ParticleGpuDispatch zero_emit = ParticleGpuDispatch::forEmit(0u);
    expectEq(zero_emit.emitBlockCount, 0u, "zero emit skips blocks");
    expectTrue(zero_emit.isEmpty(), "zero emit dispatch is empty");
    expectTrue(!zero_emit.hasEmitLaunch(), "zero emit has no launch");
    expectTrue(zero_emit.emitCovers(0u), "empty emit covers zero count");

    const fuse::vfx::ParticleGpuDispatch sim_only = ParticleGpuDispatch::forFrame(256u, 0u);
    expectTrue(sim_only.hasSimLaunch(), "forFrame with zero emit still simulates");
    expectTrue(!sim_only.hasEmitLaunch(), "forFrame with zero emit skips emit launch");
    expectTrue(sim_only.emitCovers(0u), "forFrame zero emit covers zero count");

    const fuse::vfx::ParticleGpuDispatch idle = ParticleGpuDispatch::forFrame(0u, 0u);
    expectTrue(idle.isEmpty(), "forFrame with zero capacity and emit is empty");
    expectTrue(idle.simCovers(0u) && idle.emitCovers(0u), "idle frame covers zero work");
}

void testParticleGpuMirrorParity() {
    const fuse::u32 capacity = 12u;
    fuse::vfx::ParticleGpuMirror mirror{};
    mirror.reserve(capacity);

    mirror.positions[0] = {1.f, 2.f, 3.f};
    mirror.positions[5] = {4.f, 5.f, 6.f};
    mirror.velocities[0] = {0.1f, 0.2f, 0.3f};
    mirror.ages[0] = 0.25f;
    mirror.lifetimes[0] = 2.f;
    mirror.sizes[0] = 0.5f;
    mirror.colors[0] = {0.9f, 0.8f, 0.7f};
    mirror.alphas[0] = 0.6f;
    mirror.alive_flags[0] = 1u;
    mirror.alive_flags[5] = 1u;
    mirror.alive_count = 2u;

    mirror.positions[3] = {9.f, 8.f, 7.f};
    mirror.alive_flags[3] = 0u;

    const std::vector<fuse::u8> packed = mirror.packToDeviceLayout();
    expectTrue(mirror.matchesPackedLayout(packed), "mirror matches its own packed layout");

    const fuse::vfx::ParticleGpuMirror restored =
        fuse::vfx::ParticleGpuMirror::unpackFromDeviceLayout(packed, capacity);
    expectTrue(restored.matchesPackedLayout(packed), "restored mirror matches packed layout");
    expectEq(restored.alive_count, 2u, "round trip preserves alive count");

    const std::vector<fuse::u8> repacked = restored.packToDeviceLayout();
    expectTrue(restored.matchesPackedLayout(repacked), "double pack preserves layout parity");
    expectNear(restored.positions[5].x, 4.f, 1e-5f, "round trip preserves sparse live slot");
    expectNear(restored.positions[3].x, 9.f, 1e-5f, "round trip preserves dead slot payload");

    fuse::vfx::ParticleGpuMirror mutable_restored = restored;
    mutable_restored.alive_flags[5] = 0u;
    mutable_restored.syncAliveCountFromFlags();
    expectEq(mutable_restored.alive_count, 1u, "syncAliveCountFromFlags recounts live slots");

    fuse::vfx::ParticleSoA cpu{};
    cpu.capacity = capacity;
    cpu.positions.assign(capacity, {});
    cpu.velocities.assign(capacity, {});
    cpu.ages.assign(capacity, 0.f);
    cpu.lifetimes.assign(capacity, 0.f);
    cpu.sizes.assign(capacity, 0.f);
    cpu.colors.assign(capacity, {});
    cpu.alphas.assign(capacity, 0.f);
    cpu.alive_flags.assign(capacity, 0u);
    expectTrue(mirror.writeToCpuSoA(cpu), "writeToCpuSoA succeeds on matching capacity");
    expectTrue(mirror.matchesCpuSoA(cpu), "writeToCpuSoA round trip matches mirror");
}

void testParticleGpuColumnAlignment() {
    using fuse::vfx::ParticleGpuBufferLayout;
    using fuse::vfx::ParticleGpuColumn;

    expectEq(static_cast<fuse::u32>(ParticleGpuBufferLayout::columnAlignment()), 16u, "column alignment is 16 bytes");
    expectTrue(!ParticleGpuBufferLayout::validatePackedLayout(0u), "zero capacity layout is invalid");

    const fuse::u32 capacities[] = {1u, 65u, 256u, 4096u};
    for (const fuse::u32 capacity : capacities) {
        expectTrue(ParticleGpuBufferLayout::validatePackedLayout(capacity),
                   "packed layout valid at representative capacity");

        for (fuse::u32 index = 0; index < ParticleGpuBufferLayout::columnCount(); ++index) {
            const ParticleGpuColumn column = static_cast<ParticleGpuColumn>(index);
            expectTrue(ParticleGpuBufferLayout::isColumnOffsetAligned(column, capacity),
                       "each column offset is 16-byte aligned");
            expectTrue(ParticleGpuBufferLayout::columnName(column) != nullptr, "column has debug name");
            expectTrue(ParticleGpuBufferLayout::columnDeviceOffset(column, capacity) % 16u == 0u,
                       "column offset divisible by alignment");
        }

        const fuse::usize total = ParticleGpuBufferLayout::packedDeviceBytes(capacity);
        expectTrue(total % 16u == 0u, "packed SSBO size is alignment-rounded");

        const fuse::usize alive_end =
            ParticleGpuBufferLayout::columnDeviceOffset(ParticleGpuColumn::AliveFlags, capacity) +
            ParticleGpuBufferLayout::columnByteSize(ParticleGpuColumn::AliveFlags, capacity);
        expectEq(static_cast<fuse::u32>(ParticleGpuBufferLayout::paddingAfterColumn(ParticleGpuColumn::AliveFlags, capacity)),
                 static_cast<fuse::u32>(total - alive_end),
                 "final column tail padding matches SSBO alignment");
    }
}

void testParticleGpuGridUtil() {
    using fuse::vfx::particle_gpu_util::coveredThreadCount;
    using fuse::vfx::particle_gpu_util::gridDimX;

    expectEq(gridDimX(0u, 256u), 0u, "gridDimX returns zero for zero elements");
    expectEq(gridDimX(100u, 0u), 0u, "gridDimX returns zero for zero block size");
    expectEq(gridDimX(256u, 256u), 1u, "gridDimX exact block boundary");
    expectEq(gridDimX(257u, 256u), 2u, "gridDimX rounds up past block boundary");
    expectEq(gridDimX(64u, 64u), 1u, "gridDimX emit block boundary");
    expectEq(gridDimX(65u, 64u), 2u, "gridDimX emit block rounds up");

    expectEq(coveredThreadCount(16u, 256u), 4096u, "covered threads for full simulate grid");
    expectEq(coveredThreadCount(1u, 256u), 256u, "covered threads for single simulate block");
    expectEq(coveredThreadCount(4u, 64u), 256u, "covered threads for emit grid");
}

void testParticleGpuDispatchBoundaries() {
    using fuse::vfx::ParticleGpuBufferLayout;
    using fuse::vfx::ParticleGpuDispatch;

    const fuse::vfx::ParticleGpuDispatch exact_sim =
        fuse::vfx::ParticleGpuDispatch::forSimulate(ParticleGpuBufferLayout::kSimBlockSize);
    expectEq(exact_sim.simBlockCount, 1u, "exact simulate block boundary");
    expectTrue(exact_sim.simCovers(ParticleGpuBufferLayout::kSimBlockSize), "exact sim covers capacity");

    const fuse::vfx::ParticleGpuDispatch over_sim =
        fuse::vfx::ParticleGpuDispatch::forSimulate(ParticleGpuBufferLayout::kSimBlockSize + 1u);
    expectEq(over_sim.simBlockCount, 2u, "simulate rounds up past block boundary");
    expectTrue(over_sim.simCovers(ParticleGpuBufferLayout::kSimBlockSize + 1u), "over sim covers capacity");

    const fuse::vfx::ParticleGpuDispatch exact_emit =
        fuse::vfx::ParticleGpuDispatch::forEmit(ParticleGpuBufferLayout::kEmitBlockSize);
    expectEq(exact_emit.emitBlockCount, 1u, "exact emit block boundary");
    expectTrue(exact_emit.emitCovers(ParticleGpuBufferLayout::kEmitBlockSize), "exact emit covers count");

    const fuse::vfx::ParticleGpuDispatch frame =
        fuse::vfx::ParticleGpuDispatch::forFrame(4096u, 200u);
    expectEq(frame.simBlockCount, 16u, "forFrame carries simulate blocks");
    expectEq(frame.emitBlockCount, 4u, "forFrame carries emit blocks");
    expectTrue(frame.simCovers(4096u), "forFrame sim covers capacity");
    expectTrue(frame.emitCovers(200u), "forFrame emit covers count");
}

void testParticleGpuBuffersForCapacity() {
    const fuse::u32 capacity = 128u;
    const fuse::vfx::ParticleGpuBuffers buffers = fuse::vfx::ParticleGpuBuffers::forCapacity(capacity);
    expectEq(buffers.capacity, capacity, "buffer descriptor carries capacity");
    expectEq(static_cast<fuse::u32>(buffers.deviceBytes),
             static_cast<fuse::u32>(fuse::vfx::ParticleGpuBufferLayout::packedDeviceBytes(capacity)),
             "buffer descriptor device bytes match layout");
    expectTrue(buffers.packedSoa == 0u, "unbound packedSoa stays zero in stub");
}

void testParticleGpuMirrorFullCapacityRoundTrip() {
    const fuse::u32 capacity = 16u;
    fuse::vfx::ParticleGpuMirror mirror{};
    mirror.reserve(capacity);

    for (fuse::u32 slot = 0; slot < capacity; ++slot) {
        mirror.positions[slot] = {static_cast<fuse::f32>(slot), 1.f, 2.f};
        mirror.velocities[slot] = {3.f, static_cast<fuse::f32>(slot), 4.f};
        mirror.ages[slot] = static_cast<fuse::f32>(slot) * 0.01f;
        mirror.lifetimes[slot] = 2.f;
        mirror.sizes[slot] = 0.5f + static_cast<fuse::f32>(slot) * 0.1f;
        mirror.colors[slot] = {0.1f, 0.2f, static_cast<fuse::f32>(slot) * 0.05f};
        mirror.alphas[slot] = 1.f - static_cast<fuse::f32>(slot) * 0.05f;
        mirror.alive_flags[slot] = 1u;
    }
    mirror.alive_count = capacity;

    const std::vector<fuse::u8> packed = mirror.packToDeviceLayout();
    const fuse::vfx::ParticleGpuMirror restored =
        fuse::vfx::ParticleGpuMirror::unpackFromDeviceLayout(packed, capacity);
    expectEq(restored.alive_count, capacity, "full-capacity round trip preserves alive count");

    fuse::vfx::ParticleSoA expected{};
    expected.capacity = capacity;
    expected.positions.assign(capacity, {});
    expected.velocities.assign(capacity, {});
    expected.ages.assign(capacity, 0.f);
    expected.lifetimes.assign(capacity, 0.f);
    expected.sizes.assign(capacity, 0.f);
    expected.colors.assign(capacity, {});
    expected.alphas.assign(capacity, 0.f);
    expected.alive_flags.assign(capacity, 0u);
    expectTrue(mirror.writeToCpuSoA(expected), "mirror write succeeds for full capacity");
    expectTrue(restored.matchesCpuSoA(expected), "full-capacity round trip preserves columns");
    expectNear(expected.positions[capacity - 1].x, static_cast<fuse::f32>(capacity - 1), 1e-5f,
               "mirror write copies last slot position");
}

void testParticleGpuMirrorUndersizedUnpack() {
    const fuse::u32 capacity = 8u;
    fuse::vfx::ParticleGpuMirror mirror{};
    mirror.reserve(capacity);
    mirror.alive_flags[0] = 1u;
    mirror.alive_count = 1u;

    const std::vector<fuse::u8> packed = mirror.packToDeviceLayout();
    const std::vector<fuse::u8> truncated(packed.begin(), packed.begin() + packed.size() / 2u);
    const fuse::vfx::ParticleGpuMirror restored =
        fuse::vfx::ParticleGpuMirror::unpackFromDeviceLayout(truncated, capacity);
    expectEq(restored.alive_count, 0u, "undersized unpack yields empty mirror");
    expectTrue(restored.positions[0].x == 0.f && restored.positions[0].y == 0.f && restored.positions[0].z == 0.f,
               "undersized unpack does not populate columns");
}

void testParticleGpuColumnSpan() {
    using fuse::vfx::ParticleGpuBufferLayout;
    using fuse::vfx::ParticleGpuColumn;

    const fuse::u32 capacity = 65u;
    const fuse::vfx::ParticleGpuColumnSpan positions =
        ParticleGpuBufferLayout::columnSpan(ParticleGpuColumn::Positions, capacity);
    expectEq(positions.offset, 0u, "positions span starts at zero");
    expectEq(positions.slot_count, capacity, "positions span covers full capacity");
    expectEq(static_cast<fuse::u32>(positions.element_size), static_cast<fuse::u32>(sizeof(fuse::math::Vec3)),
             "positions span element size");

    const fuse::vfx::ParticleGpuColumnSpan velocities =
        ParticleGpuBufferLayout::columnSpan(ParticleGpuColumn::Velocities, capacity);
    expectTrue(velocities.offset >= positions.endOffset(), "velocities span follows positions");
    expectTrue(velocities.offset % 16u == 0u, "velocities span offset is aligned");

    const fuse::vfx::ParticleGpuColumnSpan alive =
        ParticleGpuBufferLayout::columnSpan(ParticleGpuColumn::AliveFlags, capacity);
    expectEq(static_cast<fuse::u32>(alive.element_size), static_cast<fuse::u32>(sizeof(fuse::u32)),
             "alive_flags span element size");
    expectTrue(alive.endOffset() <= ParticleGpuBufferLayout::packedDeviceBytes(capacity),
               "alive_flags span fits packed SSBO");

    expectEq(ParticleGpuBufferLayout::columnIndex(ParticleGpuColumn::Colors), 5u, "colors column index");
}

void testParticleGpuSoAValidation() {
    const fuse::u32 capacity = 32u;
    const fuse::u64 base = 0x4000u;

    fuse::vfx::ParticleGpuMirror mirror{};
    mirror.reserve(capacity);
    mirror.alive_flags[0] = 1u;
    mirror.alive_count = 1u;

    const fuse::vfx::ParticleSoAGPU gpu = mirror.toGpuPointers(base);
    expectTrue(gpu.hasDeviceBinding(), "GPU bundle reports device binding");
    expectTrue(gpu.allColumnPointersBound(), "GPU bundle binds all columns");
    expectTrue(gpu.validateAgainstLayout(base, capacity), "GPU bundle matches packed layout");

    const fuse::vfx::ParticleSoAGPU empty = mirror.toGpuPointers(0u);
    expectTrue(!empty.hasDeviceBinding(), "null base yields unbound GPU bundle");
    expectTrue(!empty.allColumnPointersBound(), "null base leaves column pointers unset");
    expectTrue(empty.validateAgainstLayout(0u, capacity), "null base validates as unbound");

    fuse::vfx::ParticleSoAGPU mismatched = gpu;
    mismatched.capacity = capacity + 1u;
    expectTrue(!mismatched.validateAgainstLayout(base, capacity), "capacity mismatch fails validation");
}

void testParticleGpuDispatchPaddingAndSkip() {
    using fuse::vfx::ParticleGpuBufferLayout;
    using fuse::vfx::ParticleGpuDispatch;

    const fuse::vfx::ParticleGpuDispatch partial = ParticleGpuDispatch::forSimulate(100u);
    expectEq(partial.simPaddingThreads(100u), 156u, "partial simulate grid pads to block multiple");
    expectTrue(!partial.shouldSkipSimLaunch(100u), "non-zero capacity sim launches");
    expectTrue(partial.shouldSkipSimLaunch(0u), "zero capacity skips sim launch");

    const fuse::vfx::ParticleGpuDispatch emit = ParticleGpuDispatch::forEmit(200u);
    expectEq(emit.emitPaddingThreads(200u), 56u, "partial emit grid pads to block multiple");
    expectTrue(!emit.shouldSkipEmitLaunch(200u), "non-zero emit launches");
    expectTrue(emit.shouldSkipEmitLaunch(0u), "zero emit count skips launch");

    const fuse::vfx::ParticleGpuDispatch exact =
        ParticleGpuDispatch::forSimulate(ParticleGpuBufferLayout::kSimBlockSize);
    expectEq(exact.simPaddingThreads(ParticleGpuBufferLayout::kSimBlockSize), 0u,
             "exact simulate grid has no padding threads");

    expectEq(fuse::vfx::particle_gpu_util::paddingThreads(0u, 4u, 64u), 0u,
             "paddingThreads returns zero for zero elements");
    expectEq(fuse::vfx::particle_gpu_util::paddingThreads(200u, 4u, 64u), 56u,
             "paddingThreads matches emit padding");
}

void testParticleGpuFramePlan() {
    const fuse::vfx::ParticleGpuFramePlan active =
        fuse::vfx::ParticleGpuFramePlan::forStub(256u, 32u, 48u);
    expectEq(active.capacity, 256u, "frame plan carries capacity");
    expectEq(active.emit_count, 32u, "frame plan carries emit count");
    expectEq(active.alive_count, 48u, "frame plan carries alive count");
    expectTrue(!active.skipSimLaunch(), "active frame simulates");
    expectTrue(!active.skipEmitLaunch(), "active frame emits");
    expectTrue(!active.isIdle(), "active frame is not idle");
    expectEq(static_cast<fuse::u32>(active.buffers.deviceBytes),
             static_cast<fuse::u32>(fuse::vfx::ParticleGpuBufferLayout::packedDeviceBytes(256u)),
             "frame plan sizes packed SSBO");

    const fuse::vfx::ParticleGpuFramePlan sim_only =
        fuse::vfx::ParticleGpuFramePlan::forStub(128u, 0u, 10u);
    expectTrue(!sim_only.skipSimLaunch(), "sim-only frame still simulates");
    expectTrue(sim_only.skipEmitLaunch(), "sim-only frame skips emit launch");
    expectTrue(!sim_only.isIdle(), "sim-only frame is not fully idle");

    const fuse::vfx::ParticleGpuFramePlan idle =
        fuse::vfx::ParticleGpuFramePlan::forStub(0u, 0u, 0u);
    expectTrue(idle.skipSimLaunch(), "idle frame skips sim launch");
    expectTrue(idle.skipEmitLaunch(), "idle frame skips emit launch");
    expectTrue(idle.isIdle(), "idle frame is idle");

    const fuse::vfx::ParticleSoAGPU gpu = active.gpuPointers(0x8000u);
    expectEq(gpu.count, 48u, "frame plan gpuPointers carries alive count");
    expectTrue(gpu.validateAgainstLayout(0x8000u, 256u), "frame plan gpuPointers match layout");
}

void testParticleGpuColumnSpanChain() {
    using fuse::vfx::ParticleGpuBufferLayout;
    using fuse::vfx::ParticleGpuColumn;

    const fuse::u32 capacities[] = {1u, 17u, 65u, 256u};
    for (const fuse::u32 capacity : capacities) {
        expectTrue(ParticleGpuBufferLayout::validateColumnSpanChain(capacity),
                   "column span chain is contiguous and aligned");

        const std::array<fuse::vfx::ParticleGpuColumnSpan, 8> spans =
            ParticleGpuBufferLayout::collectColumnSpans(capacity);
        expectEq(static_cast<fuse::u32>(spans.size()), 8u, "collectColumnSpans returns eight columns");

        fuse::usize previous_end = 0u;
        for (fuse::u32 index = 0; index < spans.size(); ++index) {
            const fuse::vfx::ParticleGpuColumnSpan& span = spans[index];
            expectTrue(span.offset >= previous_end, "column spans do not overlap");
            expectEq(span.slot_count, capacity, "column span covers full capacity");
            expectEq(static_cast<fuse::u32>(span.element_size),
                     static_cast<fuse::u32>(ParticleGpuBufferLayout::elementSize(static_cast<ParticleGpuColumn>(index))),
                     "column span element size matches layout");
            previous_end = span.endOffset();
        }

        const fuse::usize alive_end =
            spans.back().endOffset() +
            ParticleGpuBufferLayout::paddingAfterColumn(ParticleGpuColumn::AliveFlags, capacity);
        expectEq(static_cast<fuse::u32>(alive_end),
                 static_cast<fuse::u32>(ParticleGpuBufferLayout::packedDeviceBytes(capacity)),
                 "final column span plus tail padding matches packed SSBO size");
    }

    expectTrue(!ParticleGpuBufferLayout::validateColumnSpanChain(0u), "zero capacity span chain is invalid");
}

void testParticleGpuDispatchPaddingThreadGuards() {
    using fuse::vfx::ParticleGpuBufferLayout;
    using fuse::vfx::ParticleGpuDispatch;
    using fuse::vfx::particle_gpu_util::isPaddingThread;
    using fuse::vfx::particle_gpu_util::threadCoversElement;

    expectTrue(!threadCoversElement(0u, 0u), "threadCoversElement rejects zero elements");
    expectTrue(!isPaddingThread(0u, 0u), "isPaddingThread rejects zero elements");
    expectTrue(threadCoversElement(99u, 100u), "threadCoversElement accepts in-range index");
    expectTrue(!threadCoversElement(100u, 100u), "threadCoversElement rejects out-of-range index");
    expectTrue(isPaddingThread(100u, 100u), "isPaddingThread flags out-of-range index");
    expectTrue(!isPaddingThread(99u, 100u), "isPaddingThread accepts in-range index");

    const fuse::vfx::ParticleGpuDispatch partial = ParticleGpuDispatch::forSimulate(100u);
    expectEq(partial.firstSimPaddingThread(100u), 100u, "first sim padding thread starts at slot count");
    expectTrue(!partial.isSimPaddingThread(99u, 100u), "last covered sim thread is not padding");
    expectTrue(partial.isSimPaddingThread(100u, 100u), "first sim padding thread is flagged");
    expectTrue(partial.isSimPaddingThread(255u, 100u), "last sim padding thread is flagged");
    expectTrue(!partial.isSimPaddingThread(0u, 0u), "zero slot sim has no padding threads");

    const fuse::vfx::ParticleGpuDispatch emit = ParticleGpuDispatch::forEmit(200u);
    expectEq(emit.firstEmitPaddingThread(200u), 200u, "first emit padding thread starts at emit count");
    expectTrue(!emit.isEmitPaddingThread(199u, 200u), "last covered emit thread is not padding");
    expectTrue(emit.isEmitPaddingThread(200u, 200u), "first emit padding thread is flagged");
    expectTrue(!emit.isEmitPaddingThread(0u, 0u), "zero emit has no padding threads");

    const fuse::vfx::ParticleGpuDispatch exact =
        ParticleGpuDispatch::forSimulate(ParticleGpuBufferLayout::kSimBlockSize);
    expectEq(exact.firstSimPaddingThread(ParticleGpuBufferLayout::kSimBlockSize),
             ParticleGpuBufferLayout::kSimBlockSize,
             "exact sim grid starts padding at capacity");
    expectTrue(!exact.isSimPaddingThread(ParticleGpuBufferLayout::kSimBlockSize - 1u,
                                          ParticleGpuBufferLayout::kSimBlockSize),
               "exact sim grid has no padding threads");
}

void testParticleGpuMirrorSyncGuards() {
    using fuse::vfx::ParticleGpuBufferLayout;
    using fuse::vfx::ParticleGpuSyncGuard;

    fuse::vfx::ParticleSoA cpu{};
    fuse::vfx::particle_soa::init(cpu, 8u);

    fuse::vfx::ParticleGpuMirror mirror{};
    expectEq(static_cast<fuse::u32>(mirror.syncGuardForCpu(cpu)),
             static_cast<fuse::u32>(ParticleGpuSyncGuard::MirrorUninitialized),
             "uninitialized mirror reports mirror guard");
    expectTrue(mirror.canSyncFromCpuSoA(cpu), "uninitialized mirror can resize-sync from CPU");
    expectTrue(!mirror.canWriteToCpuSoA(cpu), "uninitialized mirror cannot write to CPU");
    expectTrue(mirror.trySyncFromCpuSoA(cpu), "trySyncFromCpuSoA resizes uninitialized mirror");
    expectEq(mirror.capacity, 8u, "trySyncFromCpuSoA sets mirror capacity");
    expectEq(static_cast<fuse::u32>(mirror.syncGuardForCpu(cpu)),
             static_cast<fuse::u32>(ParticleGpuSyncGuard::Ok),
             "synced mirror reports ok guard");

    fuse::vfx::ParticleSoA smaller{};
    fuse::vfx::particle_soa::init(smaller, 4u);
    expectEq(static_cast<fuse::u32>(mirror.syncGuardForCpu(smaller)),
             static_cast<fuse::u32>(ParticleGpuSyncGuard::CapacityMismatch),
             "capacity mismatch reports mismatch guard");
    expectTrue(!mirror.trySyncFromCpuSoA(smaller), "trySyncFromCpuSoA rejects capacity mismatch");
    expectEq(static_cast<fuse::u32>(mirror.writeGuardForCpu(smaller)),
             static_cast<fuse::u32>(ParticleGpuSyncGuard::CapacityMismatch),
             "write guard rejects capacity mismatch");

    fuse::vfx::ParticleSoA empty{};
    expectEq(static_cast<fuse::u32>(mirror.syncGuardForCpu(empty)),
             static_cast<fuse::u32>(ParticleGpuSyncGuard::CpuUninitialized),
             "empty CPU reports cpu guard");
    expectTrue(!mirror.trySyncFromCpuSoA(empty), "trySyncFromCpuSoA rejects empty CPU");

    expectTrue(ParticleGpuBufferLayout::syncGuardName(ParticleGpuSyncGuard::Ok) != nullptr,
               "sync guard name is defined");
    expectTrue(std::strcmp(ParticleGpuBufferLayout::syncGuardName(ParticleGpuSyncGuard::CapacityMismatch),
                           "capacity_mismatch") == 0,
               "sync guard name matches mismatch reason");
}

void testParticleGpuFramePlanPaddingAndBuffers() {
    const fuse::vfx::ParticleGpuFramePlan plan = fuse::vfx::ParticleGpuFramePlan::forStub(100u, 200u, 48u);
    expectTrue(plan.buffersSizedForCapacity(), "frame plan buffers match capacity sizing");
    expectEq(plan.simPaddingThreadCount(), 156u, "frame plan reports sim padding threads");
    expectEq(plan.emitPaddingThreadCount(), 56u, "frame plan reports emit padding threads");

    fuse::vfx::ParticleGpuFramePlan mismatched = plan;
    mismatched.buffers.deviceBytes -= 16u;
    expectTrue(!mismatched.buffersSizedForCapacity(), "undersized buffer fails sizing guard");

    const fuse::vfx::ParticleGpuFramePlan idle = fuse::vfx::ParticleGpuFramePlan::forStub(0u, 0u, 0u);
    expectEq(idle.simPaddingThreadCount(), 0u, "idle frame has zero sim padding");
    expectEq(idle.emitPaddingThreadCount(), 0u, "idle frame has zero emit padding");
    expectTrue(idle.buffersSizedForCapacity(), "idle frame buffers still validate");
}

void testParticleGpuMirrorSyncAndWriteGuard() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 8;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 2.f;
    desc.lifetime_max = 2.f;
    desc.gravity = {0.f, -2.f, 0.f};

    emitter.init(desc);
    emitter.burst(2);
    emitter.simulate(0.05f);

    fuse::vfx::ParticleGpuMirror mirror{};
    mirror.reserve(desc.max_particles);
    mirror.syncFromCpuSoA(emitter.particles());
    expectTrue(mirror.matchesCpuSoA(emitter.particles()), "syncFromCpuSoA matches CPU SoA");

    emitter.simulate(0.05f);
    mirror.syncFromCpuSoA(emitter.particles());
    expectTrue(mirror.matchesCpuSoA(emitter.particles()), "syncFromCpuSoA refreshes after simulate");

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

    expectTrue(mirror.writeToCpuSoA(copy), "writeToCpuSoA succeeds on matching capacity");
    expectTrue(mirror.matchesCpuSoA(copy), "writeToCpuSoA produces CPU parity");

    fuse::vfx::ParticleSoA mismatched{};
    mismatched.capacity = desc.max_particles + 4u;
    mismatched.positions.assign(mismatched.capacity, {});
    mismatched.velocities.assign(mismatched.capacity, {});
    mismatched.ages.assign(mismatched.capacity, 0.f);
    mismatched.lifetimes.assign(mismatched.capacity, 0.f);
    mismatched.sizes.assign(mismatched.capacity, 0.f);
    mismatched.colors.assign(mismatched.capacity, {});
    mismatched.alphas.assign(mismatched.capacity, 0.f);
    mismatched.alive_flags.assign(mismatched.capacity, 0u);
    expectTrue(!mirror.writeToCpuSoA(mismatched), "writeToCpuSoA rejects capacity mismatch");

    const std::vector<fuse::u8> packed = mirror.packToDeviceLayout();
    expectTrue(mirror.packedBytesFit(packed), "packedBytesFit accepts full layout buffer");
    expectTrue(!mirror.packedBytesFit(std::vector<fuse::u8>(packed.size() / 2u, 0u)),
               "packedBytesFit rejects undersized buffer");
    expectTrue(mirror.isEmpty() == (mirror.alive_count == 0u), "isEmpty tracks alive count");
}

void testParticleGpuSlotOffsetGuards() {
    using fuse::vfx::ParticleGpuBufferLayout;
    using fuse::vfx::ParticleGpuColumn;
    using fuse::vfx::ParticleGpuSlotOffsetGuard;

    const fuse::u32 capacity = 65u;
    expectTrue(!ParticleGpuBufferLayout::isValidSlotIndex(0u, 0u), "zero capacity rejects slot zero");
    expectTrue(ParticleGpuBufferLayout::isValidSlotIndex(0u, capacity), "slot zero valid at capacity");
    expectTrue(ParticleGpuBufferLayout::isValidSlotIndex(capacity - 1u, capacity),
               "last slot valid at capacity");
    expectTrue(!ParticleGpuBufferLayout::isValidSlotIndex(capacity, capacity),
               "slot equal to capacity is out of range");

    expectEq(static_cast<fuse::u32>(ParticleGpuBufferLayout::slotOffsetGuard(0u, 0u)),
             static_cast<fuse::u32>(ParticleGpuSlotOffsetGuard::UninitializedCapacity),
             "zero capacity reports uninitialized guard");
    expectEq(static_cast<fuse::u32>(ParticleGpuBufferLayout::slotOffsetGuard(capacity, capacity)),
             static_cast<fuse::u32>(ParticleGpuSlotOffsetGuard::SlotOutOfRange),
             "slot at capacity reports out-of-range guard");
    expectEq(static_cast<fuse::u32>(ParticleGpuBufferLayout::slotOffsetGuard(3u, capacity)),
             static_cast<fuse::u32>(ParticleGpuSlotOffsetGuard::Ok),
             "in-range slot reports ok guard");

    const fuse::usize positions_slot3 =
        ParticleGpuBufferLayout::columnSlotByteOffset(ParticleGpuColumn::Positions, capacity, 3u);
    expectEq(positions_slot3, sizeof(fuse::math::Vec3) * 3u, "positions slot byte offset scales by element size");
    expectEq(ParticleGpuBufferLayout::columnSlotByteOffset(ParticleGpuColumn::Positions, capacity, capacity),
             0u,
             "out-of-range slot byte offset is zero");

    const fuse::u64 base = 0x5000u;
    const fuse::u64 slot_addr =
        ParticleGpuBufferLayout::columnSlotDeviceAddress(ParticleGpuColumn::Ages, base, capacity, 4u);
    expectTrue(slot_addr > base, "slot device address follows packed base");
    expectEq(ParticleGpuBufferLayout::columnSlotDeviceAddress(ParticleGpuColumn::Ages, 0u, capacity, 4u),
             0u,
             "null base yields zero slot device address");
    expectTrue(ParticleGpuBufferLayout::canAccessSlotAtOffset(ParticleGpuColumn::AliveFlags, capacity, capacity - 1u),
               "last alive_flags slot is accessible");
    expectTrue(!ParticleGpuBufferLayout::canAccessSlotAtOffset(ParticleGpuColumn::AliveFlags, capacity, capacity),
               "OOB alive_flags slot fails access guard");

    const fuse::vfx::ParticleGpuSlotOffsetPreflight ok_preflight =
        fuse::vfx::particle_gpu_util::preflight_slot_offset(ParticleGpuColumn::Velocities, capacity, 7u);
    expectTrue(ok_preflight.can_access, "preflight_slot_offset allows in-range slot");
    expectEq(ok_preflight.column_byte_offset,
             ParticleGpuBufferLayout::columnSlotByteOffset(ParticleGpuColumn::Velocities, capacity, 7u),
             "preflight_slot_offset reports column byte offset");

    const fuse::vfx::ParticleGpuSlotOffsetPreflight oob_preflight =
        fuse::vfx::particle_gpu_util::preflight_slot_offset(ParticleGpuColumn::Velocities, capacity, capacity);
    expectTrue(!oob_preflight.can_access, "preflight_slot_offset rejects OOB slot");
    expectEq(static_cast<fuse::u32>(oob_preflight.guard),
             static_cast<fuse::u32>(ParticleGpuSlotOffsetGuard::SlotOutOfRange),
             "preflight_slot_offset reports OOB guard");

    expectTrue(std::strcmp(ParticleGpuBufferLayout::slotOffsetGuardName(ParticleGpuSlotOffsetGuard::Ok), "ok") == 0,
               "slot offset guard name is defined");
}

void testParticleGpuDispatchPreflight() {
    using fuse::vfx::ParticleGpuDispatch;
    using fuse::vfx::particle_gpu_util::preflight_dispatch;

    const fuse::vfx::DispatchPreflight active = preflight_dispatch(100u, 200u);
    expectEq(active.capacity, 100u, "dispatch preflight carries capacity");
    expectEq(active.emit_count, 200u, "dispatch preflight carries emit count");
    expectTrue(!active.skip_sim, "active dispatch preflight simulates");
    expectTrue(!active.skip_emit, "active dispatch preflight emits");
    expectTrue(!active.is_idle, "active dispatch preflight is not idle");
    expectEq(active.sim_padding_threads, 156u, "dispatch preflight reports sim padding");
    expectEq(active.emit_padding_threads, 56u, "dispatch preflight reports emit padding");
    expectTrue(active.sim_covers, "dispatch preflight sim covers capacity");
    expectTrue(active.emit_covers, "dispatch preflight emit covers count");

    const fuse::vfx::ParticleGpuDispatch dispatch = ParticleGpuDispatch::forFrame(100u, 200u);
    const fuse::vfx::DispatchPreflight from_dispatch = dispatch.preflight(100u, 200u);
    expectTrue(from_dispatch.skip_sim == active.skip_sim, "dispatch member preflight matches util helper");
    expectTrue(from_dispatch.emit_covers == active.emit_covers, "dispatch member preflight matches emit coverage");

    const fuse::vfx::DispatchPreflight idle = preflight_dispatch(0u, 0u);
    expectTrue(idle.skip_sim, "idle dispatch preflight skips sim");
    expectTrue(idle.skip_emit, "idle dispatch preflight skips emit");
    expectTrue(idle.is_idle, "idle dispatch preflight is idle");
    expectEq(idle.sim_padding_threads, 0u, "idle dispatch preflight has zero sim padding");
}

void testParticleGpuMirrorPreflight() {
    using fuse::vfx::ParticleGpuSyncGuard;

    fuse::vfx::ParticleSoA cpu{};
    fuse::vfx::particle_soa::init(cpu, 8u);

    fuse::vfx::ParticleGpuMirror mirror{};
    const fuse::vfx::MirrorPreflight uninitialized = mirror.preflightSync(cpu);
    expectEq(static_cast<fuse::u32>(uninitialized.sync_guard),
             static_cast<fuse::u32>(ParticleGpuSyncGuard::MirrorUninitialized),
             "mirror preflight reports uninitialized sync guard");
    expectTrue(uninitialized.can_sync, "mirror preflight allows resize sync");
    expectTrue(!uninitialized.can_write, "mirror preflight rejects write before sync");
    expectTrue(uninitialized.would_resize, "mirror preflight marks resize on first sync");

    expectTrue(mirror.trySyncFromCpuSoA(cpu), "mirror preflight resize sync succeeds");
    const fuse::vfx::MirrorPreflight synced = mirror.preflightSync(cpu);
    expectEq(static_cast<fuse::u32>(synced.sync_guard),
             static_cast<fuse::u32>(ParticleGpuSyncGuard::Ok),
             "synced mirror preflight reports ok guard");
    expectTrue(synced.can_write, "synced mirror preflight allows write");
    expectTrue(!synced.would_resize, "synced mirror preflight does not resize");

    fuse::vfx::ParticleSoA smaller{};
    fuse::vfx::particle_soa::init(smaller, 4u);
    const fuse::vfx::MirrorPreflight mismatch = mirror.preflightSync(smaller);
    expectEq(static_cast<fuse::u32>(mismatch.sync_guard),
             static_cast<fuse::u32>(ParticleGpuSyncGuard::CapacityMismatch),
             "mirror preflight reports capacity mismatch");
    expectTrue(!mismatch.can_write, "mirror preflight rejects write on mismatch");
}

void testParticleGpuFramePlanPreflight() {
    using fuse::vfx::particle_gpu_util::preflight_frame_plan;

    const fuse::vfx::FramePlanPreflight active = preflight_frame_plan(100u, 200u, 48u);
    expectTrue(active.buffers_sized, "active frame plan preflight sizes buffers");
    expectTrue(!active.is_idle, "active frame plan preflight is not idle");
    expectTrue(!active.dispatch.skip_sim, "frame plan preflight carries active sim dispatch");
    expectEq(active.dispatch.sim_padding_threads, 156u, "frame plan preflight carries sim padding");

    const fuse::vfx::ParticleGpuFramePlan plan = fuse::vfx::ParticleGpuFramePlan::forStub(100u, 200u, 48u);
    const fuse::vfx::FramePlanPreflight from_plan = plan.preflight();
    expectTrue(from_plan.buffers_sized == active.buffers_sized,
               "frame plan member preflight matches util helper");
    expectTrue(from_plan.dispatch.is_idle == active.dispatch.is_idle,
               "frame plan member preflight matches dispatch idle flag");

    const fuse::vfx::FramePlanPreflight idle = preflight_frame_plan(0u, 0u, 0u);
    expectTrue(idle.is_idle, "idle frame plan preflight is idle");
    expectTrue(idle.buffers_sized, "idle frame plan preflight still validates buffer sizing");
}

void testSoaOpsEmptyBurst() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 8;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    const fuse::vfx::particle_soa::BurstEmitResult burst =
        fuse::vfx::particle_soa::burst_emit(soa, desc, {0.f, 1.f, 0.f}, 0u, 42u);

    expectEq(burst.requested, 0u, "soa burst_emit(0) reports zero requested");
    expectEq(burst.emitted, 0u, "soa burst_emit(0) emits nothing");
    expectEq(soa.count, 0u, "soa burst_emit(0) leaves count at zero");
    expectEq(fuse::vfx::particle_soa::free_slot_count(soa), 8u, "soa burst_emit(0) leaves all slots free");
}

void testSoaOpsBurstUninitializedCapacity() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 0;

    const fuse::vfx::particle_soa::BurstEmitResult burst =
        fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 4u, 1u);
    expectEq(burst.requested, 4u, "soa burst_emit on zero capacity reports requested count");
    expectEq(burst.emitted, 0u, "soa burst_emit on zero capacity emits nothing");
    expectEq(fuse::vfx::particle_soa::free_slot_count(soa), 0u, "soa zero capacity has no free slots");
}

void testSoaOpsFreeSlotCount() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 5;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    expectEq(fuse::vfx::particle_soa::free_slot_count(soa), 5u, "init pre-fills free list");

    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 2u, 9u);
    expectEq(fuse::vfx::particle_soa::free_slot_count(soa), 3u, "free_slot_count shrinks after burst");
    expectEq(fuse::vfx::particle_soa::sync_alive_count(soa), 2u, "sync_alive_count matches emitted slots");
}

void testSoaOpsEmitCount() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 6;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);

    const fuse::vfx::particle_soa::BurstEmitResult one =
        fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 1u, 11u);
    expectEq(one.emitted, 1u, "soa burst_emit reports single emit count");
    expectEq(soa.count, 1u, "soa count tracks single emission");

    const fuse::vfx::particle_soa::BurstEmitResult three =
        fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 3u, one.seed_after);
    expectEq(three.emitted, 3u, "soa burst_emit reports multi emit count");
    expectEq(soa.count, 4u, "soa count accumulates emit count");

    const fuse::vfx::particle_soa::BurstEmitResult overflow =
        fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 5u, three.seed_after);
    expectEq(overflow.requested, 5u, "soa burst_emit reports requested count at capacity");
    expectEq(overflow.emitted, 2u, "soa burst_emit reports clamped emit count at capacity");
    expectEq(soa.count, 6u, "soa count reaches capacity after clamped burst");
    expectEq(fuse::vfx::particle_soa::free_slot_count(soa), 0u, "full soa has zero free slots");
}

void testSoaOpsLifetimeCull() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 4;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {0.f, 2.f, 0.f}, 4u, 5u);

    const fuse::f32 yBefore = soa.positions[0].y;
    const fuse::vfx::particle_soa::LifetimeCullResult partial =
        fuse::vfx::particle_soa::lifetime_cull(soa, 0.4f);
    expectEq(partial.aged, 4u, "lifetime_cull ages all live slots");
    expectEq(partial.culled, 0u, "lifetime_cull keeps young particles alive");
    expectEq(partial.alive_after, 4u, "lifetime_cull alive count unchanged before expiry");
    expectNear(soa.positions[0].y, yBefore, 1e-5f, "lifetime_cull does not integrate position");

    const fuse::vfx::particle_soa::LifetimeCullResult expired =
        fuse::vfx::particle_soa::lifetime_cull(soa, 0.7f);
    expectEq(expired.aged, 4u, "lifetime_cull ages remaining live slots");
    expectEq(expired.culled, 4u, "lifetime_cull removes expired particles");
    expectEq(expired.alive_after, 0u, "lifetime_cull reports zero alive after expiry");
    expectEq(soa.count, 0u, "lifetime_cull updates soa count");
    expectEq(fuse::vfx::particle_soa::free_slot_count(soa), 4u, "lifetime_cull recycles dead slots");
}

void testSoaOpsLifetimeCullMixedLifetimes() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 4;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 4u, 77u);
    soa.lifetimes[0] = 0.2f;
    soa.lifetimes[1] = 0.2f;
    soa.lifetimes[2] = 1.0f;
    soa.lifetimes[3] = 1.0f;

    const fuse::vfx::particle_soa::LifetimeCullResult partial =
        fuse::vfx::particle_soa::lifetime_cull(soa, 0.25f);
    expectEq(partial.culled, 2u, "mixed lifetimes cull only short-lived slots");
    expectEq(partial.alive_after, 2u, "mixed lifetimes keep long-lived slots alive");
    expectEq(partial.alive_after + partial.culled, partial.aged,
             "lifetime_cull aged count matches culled plus survivors");
    expectEq(partial.alive_after, soa.count, "lifetime_cull alive_after matches soa count");
    expectEq(fuse::vfx::particle_soa::sync_alive_count(soa), partial.alive_after,
             "sync_alive_count agrees with lifetime_cull survivors");

    const fuse::vfx::particle_soa::LifetimeCullResult final_pass =
        fuse::vfx::particle_soa::lifetime_cull(soa, 2.f);
    expectEq(final_pass.alive_after, 0u, "mixed lifetimes fully culled after long dt");
    expectEq(fuse::vfx::particle_soa::free_slot_count(soa), 4u, "mixed lifetime cull returns all slots");
}

void testSoaOpsLifetimeCullEmpty() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::particle_soa::init(soa, 8u);

    const fuse::vfx::particle_soa::LifetimeCullResult result =
        fuse::vfx::particle_soa::lifetime_cull(soa, 1.f);
    expectEq(result.culled, 0u, "lifetime_cull on empty soa culls nothing");
    expectEq(result.alive_after, 0u, "lifetime_cull on empty soa stays empty");
    expectEq(static_cast<fuse::u32>(result.dead_slots.size()), 0u, "lifetime_cull on empty soa collects no slots");
}

void testSoaOpsLifetimeCullNoDt() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 2;
    desc.lifetime_min = 0.1f;
    desc.lifetime_max = 0.1f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 2u, 3u);

    const fuse::vfx::particle_soa::LifetimeCullResult zero =
        fuse::vfx::particle_soa::lifetime_cull(soa, 0.f);
    expectEq(zero.culled, 0u, "lifetime_cull(0) is a no-op");
    expectEq(zero.alive_after, 2u, "lifetime_cull(0) keeps alive count");

    const fuse::vfx::particle_soa::LifetimeCullResult negative =
        fuse::vfx::particle_soa::lifetime_cull(soa, -0.5f);
    expectEq(negative.culled, 0u, "lifetime_cull(negative dt) is a no-op");
    expectEq(negative.alive_after, 2u, "lifetime_cull(negative dt) keeps alive count");
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

void testSoaOpsRateEmitEmpty() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 4;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    const fuse::vfx::particle_soa::RateEmitResult zero_rate =
        fuse::vfx::particle_soa::accumulate_rate_emit(soa, desc, {}, 1.f, 0.5f, 1u);
    expectEq(zero_rate.emitted, 0u, "soa rate emit with zero rate emits nothing");
    expectNear(zero_rate.accum_after, 0.5f, 1e-5f, "soa rate emit with zero rate preserves accumulator");

    desc.emit_rate = 10.f;
    const fuse::vfx::particle_soa::RateEmitResult zero_dt =
        fuse::vfx::particle_soa::accumulate_rate_emit(soa, desc, {}, 0.f, 0.25f, 1u);
    expectEq(zero_dt.emitted, 0u, "soa rate emit with zero dt emits nothing");
    expectNear(zero_dt.accum_after, 0.25f, 1e-5f, "soa rate emit with zero dt preserves accumulator");
}

void testSoaOpsLifetimeCullParallelParity() {
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 96;
    desc.lifetime_min = 0.5f;
    desc.lifetime_max = 1.5f;

    fuse::u32 serialAlive = 0;
    fuse::u32 serialCulled = 0;
    withScheduler(0, [&] {
        fuse::vfx::ParticleSoA soa{};
        fuse::vfx::particle_soa::init(soa, desc.max_particles);
        (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 80u, 4321u);
        const fuse::vfx::particle_soa::LifetimeCullResult cull =
            fuse::vfx::particle_soa::lifetime_cull(soa, 0.35f);
        serialAlive = cull.alive_after;
        serialCulled = cull.culled;
    });

    fuse::u32 parallelAlive = 0;
    fuse::u32 parallelCulled = 0;
    withScheduler(4, [&] {
        fuse::vfx::ParticleSoA soa{};
        fuse::vfx::particle_soa::init(soa, desc.max_particles);
        (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 80u, 4321u);
        const fuse::vfx::particle_soa::LifetimeCullResult cull =
            fuse::vfx::particle_soa::lifetime_cull(soa, 0.35f);
        parallelAlive = cull.alive_after;
        parallelCulled = cull.culled;
    });

    expectEq(parallelAlive, serialAlive, "lifetime_cull parallel alive count matches serial");
    expectEq(parallelCulled, serialCulled, "lifetime_cull parallel culled count matches serial");
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

void testSoaOpsClampBurstCount() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 6;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    expectEq(fuse::vfx::particle_soa::clamp_burst_count(soa, 10u), 6u, "clamp_burst_count on empty soa returns capacity");
    expectTrue(!fuse::vfx::particle_soa::is_at_capacity(soa), "empty soa is not at capacity");
    expectTrue(!fuse::vfx::particle_soa::has_live_particles(soa), "empty soa has no live particles");

    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 4u, 1u);
    expectEq(fuse::vfx::particle_soa::clamp_burst_count(soa, 5u), 2u, "clamp_burst_count respects remaining free slots");
    expectTrue(fuse::vfx::particle_soa::has_live_particles(soa), "burst soa has live particles");
    expectTrue(!fuse::vfx::particle_soa::is_at_capacity(soa), "partial soa is not at capacity");

    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 2u, 2u);
    expectEq(fuse::vfx::particle_soa::clamp_burst_count(soa, 1u), 0u, "clamp_burst_count at capacity returns zero");
    expectTrue(fuse::vfx::particle_soa::is_at_capacity(soa), "full soa is at capacity");
}

void testSoaOpsBurstClampedFlag() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 3;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);

    const fuse::vfx::particle_soa::BurstEmitResult exact =
        fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 2u, 5u);
    expectTrue(!exact.clamped, "exact burst is not clamped");
    expectEq(exact.emitted, 2u, "exact burst emits requested count");

    const fuse::vfx::particle_soa::BurstEmitResult overflow =
        fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 5u, exact.seed_after);
    expectTrue(overflow.clamped, "overflow burst reports clamped");
    expectEq(overflow.requested, 5u, "overflow burst reports requested count");
    expectEq(overflow.emitted, 1u, "overflow burst emits only remaining slot");
}

void testSoaOpsSimulateStepEmptyGuard() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 8;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    const fuse::vfx::particle_soa::SimStepResult empty =
        fuse::vfx::particle_soa::simulate_step(soa, desc, 0.1f);
    expectEq(empty.alive_after, 0u, "simulate_step on empty soa stays empty");
    expectEq(empty.integrated, 0u, "simulate_step on empty soa integrates nothing");
    expectEq(empty.culled, 0u, "simulate_step on empty soa culls nothing");
    expectTrue(empty.skipped, "simulate_step on empty soa reports skipped");
    expectEq(static_cast<fuse::u32>(empty.dead_slots.size()), 0u, "simulate_step on empty soa collects no slots");
}

void testSoaOpsSimulateStepIntegratedCounts() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 4;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;
    desc.gravity = {};
    desc.drag = 0.f;
    desc.velocity_min = {};
    desc.velocity_max = {};

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 4u, 11u);
    soa.lifetimes[0] = 0.1f;
    soa.lifetimes[1] = 0.1f;
    soa.lifetimes[2] = 1.f;
    soa.lifetimes[3] = 1.f;

    const fuse::vfx::particle_soa::SimStepResult step =
        fuse::vfx::particle_soa::simulate_step(soa, desc, 0.2f);
    expectEq(step.culled, 2u, "simulate_step reports culled short-lived slots");
    expectEq(step.integrated, 2u, "simulate_step reports integrated survivors");
    expectEq(step.alive_after, 2u, "simulate_step alive_after matches integrated count");
    expectEq(step.integrated + step.culled, 4u, "simulate_step integrated plus culled equals live input");
}

void testSoaOpsRateEmitAtCapacityFlag() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 2;
    desc.emit_rate = 50.f;
    desc.lifetime_min = 5.f;
    desc.lifetime_max = 5.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    const fuse::vfx::particle_soa::RateEmitResult fill =
        fuse::vfx::particle_soa::accumulate_rate_emit(soa, desc, {}, 1.f, 0.f, 7u);
    expectEq(fill.emitted, 2u, "rate emit fills to capacity");
    expectTrue(fill.at_capacity, "rate emit at capacity clears accumulator");
    expectNear(fill.accum_after, 0.f, 1e-5f, "rate emit at capacity zeroes accumulator");

    fuse::vfx::ParticleSoA zero_cap_soa{};
    fuse::vfx::particle_soa::init(zero_cap_soa, 0u);
    const fuse::vfx::particle_soa::RateEmitResult zero_capacity =
        fuse::vfx::particle_soa::accumulate_rate_emit(zero_cap_soa, desc, {}, 1.f, 0.5f, 1u);
    expectEq(zero_capacity.emitted, 0u, "rate emit on zero-capacity soa emits nothing");
    expectNear(zero_capacity.accum_after, 0.5f, 1e-5f, "rate emit on zero-capacity soa preserves accumulator");
}

void testParticleEmitterBurstReturnsEmitted() {
    fuse::vfx::ParticleEmitter emitter{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 5;
    desc.emit_rate = 0.f;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    expectEq(emitter.burst(3u), 0u, "uninitialized burst returns zero emitted");

    emitter.init(desc);
    expectEq(emitter.burst(3u), 3u, "burst returns emitted count");
    expectEq(emitter.alive_count(), 3u, "burst return matches alive count");

    expectEq(emitter.burst(5u), 2u, "clamped burst returns actual emitted count");
    expectEq(emitter.alive_count(), 5u, "clamped burst reaches capacity");
    expectEq(emitter.burst(1u), 0u, "burst at capacity returns zero");
}

void testSoaOpsBurstPreflight() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 5;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);

    const fuse::vfx::particle_soa::BurstEmitPreflight empty =
        fuse::vfx::particle_soa::preflight_burst_emit(soa, 8u);
    expectEq(empty.requested, 8u, "preflight reports requested burst count");
    expectEq(empty.allowed, 5u, "preflight clamps to free capacity on empty soa");
    expectEq(empty.remaining_free, 5u, "preflight reports remaining free slots");
    expectTrue(empty.would_clamp, "preflight marks overflow burst as clamped");
    expectTrue(empty.can_emit, "preflight allows non-zero burst on empty soa");
    expectTrue(!empty.at_capacity, "preflight empty soa is not at capacity");
    expectTrue(fuse::vfx::particle_soa::can_burst_emit(soa, 3u), "can_burst_emit true with free slots");

    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 4u, 1u);
    const fuse::vfx::particle_soa::BurstEmitPreflight partial =
        fuse::vfx::particle_soa::preflight_burst_emit(soa, 3u);
    expectEq(partial.allowed, 1u, "preflight respects remaining free slots");
    expectTrue(partial.would_clamp, "preflight marks partial overflow as clamped");

    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 1u, 2u);
    const fuse::vfx::particle_soa::BurstEmitPreflight full =
        fuse::vfx::particle_soa::preflight_burst_emit(soa, 1u);
    expectEq(full.allowed, 0u, "preflight at capacity allows zero burst");
    expectTrue(full.at_capacity, "preflight full soa is at capacity");
    expectTrue(!full.can_emit, "preflight at capacity cannot emit");
    expectTrue(!fuse::vfx::particle_soa::can_burst_emit(soa, 1u), "can_burst_emit false at capacity");
    expectTrue(!fuse::vfx::particle_soa::can_burst_emit(soa, 0u), "can_burst_emit false for zero count");
}

void testSoaOpsCountLiveFlags() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 4;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    expectEq(fuse::vfx::particle_soa::count_live_flags(soa), 0u, "count_live_flags on empty soa is zero");

    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 2u, 3u);
    expectEq(fuse::vfx::particle_soa::count_live_flags(soa), 2u, "count_live_flags matches emitted slots");

    soa.count = 99u;
    expectEq(fuse::vfx::particle_soa::count_live_flags(soa), 2u, "count_live_flags ignores stale soa.count");
    expectEq(fuse::vfx::particle_soa::sync_alive_count(soa), 2u, "sync_alive_count repairs stale count");
    expectEq(soa.count, 2u, "sync_alive_count writes repaired count");
}

void testSoaOpsSimStepPreflight() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 4;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    const fuse::vfx::particle_soa::SimStepPreflight empty =
        fuse::vfx::particle_soa::preflight_simulate_step(soa, 0.1f);
    expectTrue(empty.skipped, "preflight skips empty soa simulate step");
    expectTrue(fuse::vfx::particle_soa::should_skip_simulate_step(soa, 0.1f),
               "should_skip_simulate_step on empty soa");

    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 2u, 4u);
    const fuse::vfx::particle_soa::SimStepPreflight live =
        fuse::vfx::particle_soa::preflight_simulate_step(soa, 0.05f);
    expectTrue(!live.skipped, "preflight runs simulate step with live particles");
    expectEq(live.live_input, 2u, "preflight reports live input count");
    expectNear(live.dt, 0.05f, 1e-5f, "preflight carries dt");

    const fuse::vfx::particle_soa::SimStepPreflight zero_dt =
        fuse::vfx::particle_soa::preflight_simulate_step(soa, 0.f);
    expectTrue(zero_dt.skipped, "preflight skips non-positive dt");
    expectTrue(fuse::vfx::particle_soa::should_skip_simulate_step(soa, 0.f),
               "should_skip_simulate_step for zero dt");

    const fuse::vfx::particle_soa::SimStepResult skipped =
        fuse::vfx::particle_soa::simulate_step(soa, desc, -0.1f);
    expectTrue(skipped.skipped, "simulate_step reports skipped for negative dt");
    expectEq(skipped.alive_after, 2u, "skipped simulate_step preserves alive count");
    expectEq(skipped.integrated, 0u, "skipped simulate_step integrates nothing");
}

void testSoaOpsRateEmitPreflight() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 3;
    desc.emit_rate = 10.f;
    desc.lifetime_min = 1.f;
    desc.lifetime_max = 1.f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    const fuse::vfx::particle_soa::RateEmitPreflight active =
        fuse::vfx::particle_soa::preflight_rate_emit(soa, desc, 0.1f);
    expectTrue(!active.skipped, "preflight rate emit runs with positive rate and dt");
    expectEq(active.remaining_free, 3u, "preflight rate emit reports free slots");
    expectTrue(!active.at_capacity, "preflight rate emit not at capacity initially");

    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 3u, 1u);
    const fuse::vfx::particle_soa::RateEmitPreflight full =
        fuse::vfx::particle_soa::preflight_rate_emit(soa, desc, 0.1f);
    expectTrue(full.at_capacity, "preflight rate emit at capacity when full");
    expectEq(full.remaining_free, 0u, "preflight rate emit zero free slots at capacity");

    desc.emit_rate = 0.f;
    const fuse::vfx::particle_soa::RateEmitPreflight zero_rate =
        fuse::vfx::particle_soa::preflight_rate_emit(soa, desc, 0.1f);
    expectTrue(zero_rate.skipped, "preflight skips zero emit rate");
    expectTrue(fuse::vfx::particle_soa::should_skip_rate_emit(soa, desc, 0.1f),
               "should_skip_rate_emit for zero rate");

    desc.emit_rate = 10.f;
    expectTrue(fuse::vfx::particle_soa::should_skip_rate_emit(soa, desc, 0.f),
               "should_skip_rate_emit for zero dt");
}

void testSoaOpsLifetimeCullSkipGuard() {
    fuse::vfx::ParticleSoA soa{};
    fuse::vfx::ParticleEmitterDesc desc{};
    desc.max_particles = 2;
    desc.lifetime_min = 0.5f;
    desc.lifetime_max = 0.5f;

    fuse::vfx::particle_soa::init(soa, desc.max_particles);
    expectTrue(fuse::vfx::particle_soa::should_skip_lifetime_cull(soa, 0.2f),
               "should_skip_lifetime_cull on empty soa");

    (void)fuse::vfx::particle_soa::burst_emit(soa, desc, {}, 2u, 8u);
    expectTrue(!fuse::vfx::particle_soa::should_skip_lifetime_cull(soa, 0.2f),
               "should_skip_lifetime_cull false with live particles");
    expectTrue(fuse::vfx::particle_soa::should_skip_lifetime_cull(soa, 0.f),
               "should_skip_lifetime_cull for zero dt");
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
    testSoaOpsEmptyBurst();
    testSoaOpsBurstUninitializedCapacity();
    testSoaOpsFreeSlotCount();
    testSoaOpsEmitCount();
    testSoaOpsLifetimeCull();
    testSoaOpsLifetimeCullMixedLifetimes();
    testSoaOpsLifetimeCullEmpty();
    testSoaOpsLifetimeCullNoDt();
    testSoaOpsLifetimeCullParallelParity();
    testSoaOpsBurstFill();
    testSoaOpsDeterministicSeed();
    testSoaOpsAgeKill();
    testSoaOpsRateEmit();
    testSoaOpsRateEmitEmpty();
    testSoaOpsClampBurstCount();
    testSoaOpsBurstClampedFlag();
    testSoaOpsSimulateStepEmptyGuard();
    testSoaOpsSimulateStepIntegratedCounts();
    testSoaOpsRateEmitAtCapacityFlag();
    testSoaOpsBurstPreflight();
    testSoaOpsCountLiveFlags();
    testSoaOpsSimStepPreflight();
    testSoaOpsRateEmitPreflight();
    testSoaOpsLifetimeCullSkipGuard();
    testParticleEmitterBurstReturnsEmitted();
    testSoaOpsParallelParity();
    testParticleGpuBufferLayout();
    testParticleGpuColumnAlignment();
    testParticleGpuGridUtil();
    testParticleGpuDispatchCounts();
    testParticleGpuDispatchBoundaries();
    testParticleGpuBuffersForCapacity();
    testParticleGpuMirrorRoundTrip();
    testParticleGpuMirrorFullCapacityRoundTrip();
    testParticleGpuMirrorUndersizedUnpack();
    testParticleGpuColumnSpan();
    testParticleGpuSoAValidation();
    testParticleGpuDispatchPaddingAndSkip();
    testParticleGpuColumnSpanChain();
    testParticleGpuDispatchPaddingThreadGuards();
    testParticleGpuMirrorSyncGuards();
    testParticleGpuFramePlanPaddingAndBuffers();
    testParticleGpuFramePlan();
    testParticleGpuMirrorSyncAndWriteGuard();
    testParticleGpuSlotOffsetGuards();
    testParticleGpuDispatchPreflight();
    testParticleGpuMirrorPreflight();
    testParticleGpuFramePlanPreflight();
    testParticleGpuPointerBundle();
    testParticleGpuLayoutSize();
    testParticleGpuEmptyDispatch();
    testParticleGpuMirrorParity();
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
