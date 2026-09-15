#include <fuse/vfx/effect_instance.hpp>

namespace fuse::vfx {

void EffectInstance::play() {
    playing = true;
}

void EffectInstance::stop() {
    playing = false;
}

bool EffectInstance::is_alive() const {
    if (!playing) {
        return false;
    }
    if (duration > 0.f && elapsed >= duration) {
        return false;
    }
    return true;
}

void EffectInstance::tick(f32 dt) {
    if (!playing) {
        return;
    }
    elapsed += dt;
}

} // namespace fuse::vfx
