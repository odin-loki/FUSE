#pragma once

#include <fuse/fx/effect_descriptor.hpp>
#include <fuse/fx/fx_defs.hpp>
#include <fuse/fx/fx_socket.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::fx {

/// One socket-bound effect playback — ore analogue: `afxEffectron` runtime instance.
struct EffectPlayback {
    const EffectDescriptor* descriptor = nullptr;
    FxSocket socket;
    EffectPlaybackState state = EffectPlaybackState::Inactive;
    float elapsed = 0.f;
    s32 loopIndex = 0;
    bool finished = false;
};

/// Timed effect phrase driver — ore analogue: `afxPhrase` in `afxPhrase.h`.
class EffectTimeline {
public:
    bool start(const EffectDescriptor& descriptor, const FxSocket& socket);
    void tick(float dt);

    u32 activeCount() const;
    u32 completedCount() const { return m_completedCount; }
    const std::vector<EffectPlayback>& instances() const { return m_instances; }

private:
    void advanceInstance(EffectPlayback& instance, float dt);
    void finishInstance(EffectPlayback& instance);

    std::vector<EffectPlayback> m_instances;
    u32 m_completedCount = 0;
};

} // namespace fuse::fx
