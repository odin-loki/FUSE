#pragma once

// GAP-GAME-LOOP-ECS: audio driven from the ECS registry.
//
// Games attach fuse::audio::AudioSource / AudioListener as ECS components (they are plain,
// trivially destructible structs with a component_name). Each `update`:
//   1. reconciles the registry with the engine's voice table: a new AudioSource component gets a
//      voice (play_on_awake starts it), a removed component / destroyed entity releases its voice;
//   2. copies the game-side state into the voices: desc, playing / paused, and the position (the
//      entity's Transform world translation when it has one, else AudioSource::position); the
//      first AudioListener (registry order) becomes the listener, oriented by its Transform
//      (forward = -Z column, up = +Y column);
//   3. runs AudioEngine::update (mix, reverb, backend submit);
//   4. writes the engine-side state back into the components: play_head, playing (a finished
//      one-shot clip stops), backend_source.
// The AudioRegistry is now only the engine's internal voice table — gameplay code never touches it.
// Steady state (no component added / removed) makes no heap allocations in this system.

#include <fuse/audio/audio_components.hpp>
#include <fuse/audio/audio_registry.hpp>
#include <fuse/ecs/entity.hpp>
#include <fuse/types.hpp>

#include <unordered_map>
#include <vector>

namespace fuse::ecs {
class Registry;
}

namespace fuse::audio {

class AudioEngine;

struct AudioEcsStats {
    u64 voices_created = 0;
    u64 voices_released = 0;
    u32 active_sources = 0;
    bool has_listener = false;
};

class AudioEcsSystem {
public:
    /// Non-owning: `engine` (initialised) must outlive the system.
    bool init(AudioEngine& engine, usize expected_sources = 64);
    void shutdown();
    [[nodiscard]] bool is_initialized() const { return m_engine != nullptr; }

    void update(ecs::Registry& registry, f32 dt);

    /// The engine-facing voice table (read-only for diagnostics).
    [[nodiscard]] const AudioRegistry& voices() const { return m_voices; }
    /// Voice id backing an entity's AudioSource, or kInvalidEntity.
    [[nodiscard]] EntityId voice_of(ecs::EntityID entity) const;
    [[nodiscard]] const AudioEcsStats& stats() const { return m_stats; }

private:
    struct Tracked {
        ecs::EntityID entity = ecs::EntityID::null();
        EntityId voice = kInvalidEntity;
        u32 stamp = 0;
    };

    static u64 key_of(ecs::EntityID entity) {
        return (static_cast<u64>(entity.generation) << 32) | static_cast<u64>(entity.index);
    }

    AudioEngine* m_engine = nullptr;
    AudioRegistry m_voices;
    std::vector<Tracked> m_tracked;
    std::unordered_map<u64, u32> m_index; ///< key_of(entity) -> m_tracked slot
    EntityId m_listenerVoice = kInvalidEntity;
    u32 m_stamp = 0;
    AudioEcsStats m_stats{};
};

} // namespace fuse::audio
