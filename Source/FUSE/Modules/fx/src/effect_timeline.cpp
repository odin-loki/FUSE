#include <fuse/fx/effect_timeline.hpp>

namespace fuse::fx {

bool EffectTimeline::start(const EffectDescriptor& descriptor, const FxSocket& socket) {
    if (descriptor.id.empty() || descriptor.duration <= 0.f) {
        return false;
    }

    EffectPlayback playback;
    playback.descriptor = &descriptor;
    playback.socket = socket;
    playback.state = EffectPlaybackState::Active;
    playback.elapsed = 0.f;
    playback.loopIndex = 0;
    playback.finished = false;
    m_instances.push_back(playback);
    return true;
}

void EffectTimeline::tick(float dt) {
    for (EffectPlayback& instance : m_instances) {
        if (!instance.finished) {
            advanceInstance(instance, dt);
        }
    }
}

u32 EffectTimeline::activeCount() const {
    u32 count = 0;
    for (const EffectPlayback& instance : m_instances) {
        if (!instance.finished) {
            ++count;
        }
    }
    return count;
}

void EffectTimeline::advanceInstance(EffectPlayback& instance, float dt) {
    if (!instance.descriptor) {
        finishInstance(instance);
        return;
    }

    instance.elapsed += dt;
    const float duration = instance.descriptor->duration;
    if (instance.elapsed < duration) {
        return;
    }

    ++instance.loopIndex;
    if (instance.loopIndex >= instance.descriptor->loopCount) {
        finishInstance(instance);
        return;
    }

    instance.elapsed = 0.f;
}

void EffectTimeline::finishInstance(EffectPlayback& instance) {
    instance.state = EffectPlaybackState::Done;
    instance.finished = true;
    ++m_completedCount;
}

} // namespace fuse::fx
