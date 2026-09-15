#pragma once

#include <fuse/handle.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::vfx {

struct ParticleEmitter;

struct EffectInstance {
    Handle<ParticleEmitter> emitter = Handle<ParticleEmitter>::invalid();
    math::Vec3 position{};
    f32 elapsed = 0.f;
    f32 duration = 0.f;
    bool playing = false;
    bool auto_destroy = true;

    void play();
    void stop();
    bool is_alive() const;
    void tick(f32 dt);
};

} // namespace fuse::vfx
