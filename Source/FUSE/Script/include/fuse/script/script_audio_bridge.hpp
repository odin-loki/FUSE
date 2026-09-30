#pragma once

// MP-B7.3-LUA-API: `Audio.*` script API on fuse::audio::AudioEngine (fuse_script_audio).

#include <fuse/audio/audio_engine.hpp>
#include <fuse/script/script_engine_api.hpp>

namespace fuse::script {

/// Non-owning adapter: `engine` must outlive script execution. Clips loaded through scripts
/// stay registered in the engine (unload them with the engine when the level ends).
class AudioEngineScriptBackend final : public ScriptAudioBackend {
public:
    explicit AudioEngineScriptBackend(audio::AudioEngine& engine) : m_engine(engine) {}

    u64 load(const char* path) override;
    bool play_at(u64 clip, const ecs::vec3& position, f32 volume, f32 pitch) override;
    bool play_2d(u64 clip, f32 volume) override;

    /// Script clip number <-> engine handle: generation * 2^32 + index + 1 (0 = invalid).
    [[nodiscard]] static u64 encode_clip(Handle<audio::AudioClip> handle);
    [[nodiscard]] static Handle<audio::AudioClip> decode_clip(u64 clip);

private:
    audio::AudioEngine& m_engine;
};

} // namespace fuse::script
