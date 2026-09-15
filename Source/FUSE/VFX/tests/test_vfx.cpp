#include <fuse/core/init.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/vfx/effect_instance.hpp>
#include <fuse/vfx/particle_emitter.hpp>
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
    testParticleEmitterBurstCapacity();
    testParticleEmitterEmitRate();
    testParticleEmitterEmitRateSteadyState();
    testParticleAttributeInterpolation();
    testParticleDragIntegration();
    testParallelSimulationParity();
    testEffectInstanceLifecycle();
    testParticleSystemSpawnAndUpdate();
    testParticleSystemEmitterHandles();

    fuse::core::shutdown();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_vfx_tests: all passed\n");
    return EXIT_SUCCESS;
}
