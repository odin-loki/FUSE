#include <fuse/core/init.hpp>
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
    expectTrue(emitter.alive_count() == 3u, "burst emits requested particles");

    const fuse::f32 yBefore = emitter.particles().positions[0].y;
    emitter.simulate(0.1f);
    const fuse::f32 yAfter = emitter.particles().positions[0].y;
    expectTrue(yAfter < yBefore, "gravity integrates particle downward");

    emitter.simulate(1.f);
    expectTrue(emitter.alive_count() == 0u, "particles expire after lifetime");
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
    expectTrue(system.effect_count() == 1u, "spawn registers one effect");
    expectTrue(system.emitter_count() == 1u, "spawn creates backing emitter");
    expectTrue(system.alive_particle_count() >= 1u, "spawned effect emits particles");

    system.update(0.1f);
    expectTrue(system.alive_particle_count() >= 1u, "update keeps particles alive");

    system.update(0.2f);
    expectTrue(system.effect_count() == 0u, "finished effect is cleaned up");
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
    expectTrue(system.alive_particle_count() == 2u, "system aggregates alive particles");

    system.destroy_emitter(handle);
    expectTrue(system.emitter_count() == 0u, "destroy_emitter removes emitter");
    expectTrue(system.get_emitter(handle) == nullptr, "destroyed handle no longer resolves");
}

} // namespace

int main() {
    fuse::core::initialize();

    testParticleEmitterBurstAndSimulate();
    testParticleEmitterEmitRate();
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
