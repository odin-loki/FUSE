#include <fuse/fx/effect_descriptor.hpp>

namespace fuse::fx {

EffectDescriptor EffectDescriptor::makeSparkBurst() {
    EffectDescriptor descriptor;
    descriptor.id = "spark_burst";
    descriptor.duration = 0.35f;
    descriptor.loopCount = 1;

    EffectEntry entry;
    entry.effectTypeId = "particle_emitter";
    entry.timing.lifetime = 0.35f;
    entry.timing.fadeOut = 0.1f;
    descriptor.entries.push_back(entry);
    return descriptor;
}

} // namespace fuse::fx
