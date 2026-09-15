#pragma once

#include <fuse/audio/audio_desc.hpp>
#include <fuse/audio/math.hpp>
#include <fuse/types.hpp>

namespace fuse::audio {

using EntityId = u32;
constexpr EntityId kInvalidEntity = UINT32_MAX;

/// ECS component — one per audio-emitting entity.
struct AudioSource {
    static constexpr const char* component_name = "AudioSource";

    AudioSourceDesc desc;
    Vec3 position{};
    bool playing = false;
    bool paused = false;
    float play_head = 0.f;
    u32 backend_source = 0;
};

/// Listener — one per scene (typically attached to the camera entity).
struct AudioListener {
    static constexpr const char* component_name = "AudioListener";

    Vec3 position{};
    Vec3 forward{0.f, 0.f, -1.f};
    Vec3 up{0.f, 1.f, 0.f};
    float master_volume = 1.f;
};

} // namespace fuse::audio
