#include <fuse/script/script_audio_bridge.hpp>

#include <cmath>

namespace fuse::script {

u64 AudioEngineScriptBackend::encode_clip(Handle<audio::AudioClip> handle) {
    if (!handle.isValid() || handle.generation() >= (1u << 20)) {
        return 0; // keep the value exact in a Lua double (< 2^53)
    }
    return (static_cast<u64>(handle.generation()) << 32u) + static_cast<u64>(handle.index()) + 1u;
}

Handle<audio::AudioClip> AudioEngineScriptBackend::decode_clip(u64 clip) {
    if (clip == 0) {
        return Handle<audio::AudioClip>::invalid();
    }
    const u64 packed = clip - 1u;
    return Handle<audio::AudioClip>(static_cast<u32>(packed & 0xFFFFFFFFull), static_cast<u32>(packed >> 32u));
}

u64 AudioEngineScriptBackend::load(const char* path) {
    if (path == nullptr || !m_engine.is_initialized()) {
        return 0;
    }
    return encode_clip(m_engine.load_clip(path));
}

bool AudioEngineScriptBackend::play_at(u64 clip, const ecs::vec3& position, f32 volume, f32 pitch) {
    const Handle<audio::AudioClip> handle = decode_clip(clip);
    if (!m_engine.is_initialized() || m_engine.find_clip(handle) == nullptr || !std::isfinite(volume) ||
        !std::isfinite(pitch) || pitch <= 0.f) {
        return false;
    }
    m_engine.play_at(handle, audio::Vec3{position.x, position.y, position.z}, volume < 0.f ? 0.f : volume, pitch);
    return true;
}

bool AudioEngineScriptBackend::play_2d(u64 clip, f32 volume) {
    const Handle<audio::AudioClip> handle = decode_clip(clip);
    if (!m_engine.is_initialized() || m_engine.find_clip(handle) == nullptr || !std::isfinite(volume)) {
        return false;
    }
    m_engine.play_2d(handle, volume < 0.f ? 0.f : volume);
    return true;
}

} // namespace fuse::script
