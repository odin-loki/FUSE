#include <fuse/audio/audio_backend.hpp>

#if defined(FUSE_AUDIO_OPENAL)
#include <AL/al.h>
#include <AL/alc.h>
#endif

namespace fuse::audio {

bool AudioBackend::init(const AudioDesc& desc) {
    shutdown();

#if defined(FUSE_AUDIO_OPENAL)
    ALCdevice* device = alcOpenDevice(nullptr);
    if (device != nullptr) {
        ALCcontext* context = alcCreateContext(device, nullptr);
        if (context != nullptr && alcMakeContextCurrent(context)) {
            alDistanceModel(AL_LINEAR_DISTANCE_CLAMPED);
            m_kind = AudioBackendKind::OpenAL;
            m_initialized = true;
            return true;
        }
        if (context != nullptr) {
            alcDestroyContext(context);
        }
        alcCloseDevice(device);
    }
#endif

    (void)desc;
    m_kind = AudioBackendKind::Null;
    m_initialized = true;
    return true;
}

void AudioBackend::shutdown() {
#if defined(FUSE_AUDIO_OPENAL)
    if (m_kind == AudioBackendKind::OpenAL) {
        for (auto& entry : m_sources) {
            if (entry.second.al_source != 0) {
                ALuint al_source = entry.second.al_source;
                alDeleteSources(1, &al_source);
            }
        }
        ALCcontext* context = alcGetCurrentContext();
        if (context != nullptr) {
            ALCdevice* device = alcGetContextsDevice(context);
            alcMakeContextCurrent(nullptr);
            alcDestroyContext(context);
            if (device != nullptr) {
                alcCloseDevice(device);
            }
        }
    }
#endif

    m_sources.clear();
    m_nextSource = 1;
    m_initialized = false;
    m_kind = AudioBackendKind::Null;
}

u32 AudioBackend::create_source() {
    const u32 id = m_nextSource++;
    SourceState state{};
#if defined(FUSE_AUDIO_OPENAL)
    if (m_kind == AudioBackendKind::OpenAL) {
        ALuint al_source = 0;
        alGenSources(1, &al_source);
        state.al_source = al_source;
    }
#endif
    m_sources.emplace(id, state);
    return id;
}

void AudioBackend::destroy_source(u32 source) {
#if defined(FUSE_AUDIO_OPENAL)
    auto it = m_sources.find(source);
    if (it != m_sources.end() && m_kind == AudioBackendKind::OpenAL && it->second.al_source != 0) {
        ALuint al_source = it->second.al_source;
        alDeleteSources(1, &al_source);
    }
#endif
    m_sources.erase(source);
}

void AudioBackend::set_source_gain(u32 source, float gain) {
    auto it = m_sources.find(source);
    if (it == m_sources.end()) {
        return;
    }
    it->second.gain = gain;
#if defined(FUSE_AUDIO_OPENAL)
    if (m_kind == AudioBackendKind::OpenAL) {
        alSourcef(static_cast<ALuint>(it->second.al_source), AL_GAIN, gain);
    }
#endif
}

void AudioBackend::set_source_position(u32 source, float x, float y, float z) {
    auto it = m_sources.find(source);
    if (it == m_sources.end()) {
        return;
    }
    it->second.x = x;
    it->second.y = y;
    it->second.z = z;
#if defined(FUSE_AUDIO_OPENAL)
    if (m_kind == AudioBackendKind::OpenAL) {
        alSource3f(static_cast<ALuint>(it->second.al_source), AL_POSITION, x, y, z);
    }
#endif
}

void AudioBackend::set_listener_position(float x, float y, float z) {
    m_listenerX = x;
    m_listenerY = y;
    m_listenerZ = z;
#if defined(FUSE_AUDIO_OPENAL)
    if (m_kind == AudioBackendKind::OpenAL) {
        alListener3f(AL_POSITION, x, y, z);
    }
#endif
}

void AudioBackend::set_listener_orientation(float fx, float fy, float fz, float ux, float uy,
                                            float uz) {
#if defined(FUSE_AUDIO_OPENAL)
    if (m_kind == AudioBackendKind::OpenAL) {
        const ALfloat orientation[] = {fx, fy, fz, ux, uy, uz};
        alListenerfv(AL_ORIENTATION, orientation);
        return;
    }
#endif
    (void)fx;
    (void)fy;
    (void)fz;
    (void)ux;
    (void)uy;
    (void)uz;
}

void AudioBackend::play_source(u32 source) {
    auto it = m_sources.find(source);
    if (it == m_sources.end()) {
        return;
    }
    it->second.playing = true;
    it->second.paused = false;
#if defined(FUSE_AUDIO_OPENAL)
    if (m_kind == AudioBackendKind::OpenAL) {
        alSourcePlay(static_cast<ALuint>(it->second.al_source));
    }
#endif
}

void AudioBackend::stop_source(u32 source) {
    auto it = m_sources.find(source);
    if (it == m_sources.end()) {
        return;
    }
    it->second.playing = false;
    it->second.paused = false;
#if defined(FUSE_AUDIO_OPENAL)
    if (m_kind == AudioBackendKind::OpenAL) {
        alSourceStop(static_cast<ALuint>(it->second.al_source));
    }
#endif
}

void AudioBackend::pause_source(u32 source, bool paused) {
    auto it = m_sources.find(source);
    if (it == m_sources.end()) {
        return;
    }
    it->second.paused = paused;
#if defined(FUSE_AUDIO_OPENAL)
    if (m_kind == AudioBackendKind::OpenAL) {
        if (paused) {
            alSourcePause(static_cast<ALuint>(it->second.al_source));
        } else {
            alSourcePlay(static_cast<ALuint>(it->second.al_source));
        }
    }
#endif
}

float AudioBackend::read_source_gain(u32 source) const {
    const auto it = m_sources.find(source);
    return it == m_sources.end() ? 0.f : it->second.gain;
}

} // namespace fuse::audio
