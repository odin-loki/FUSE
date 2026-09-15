#pragma once

#include <fuse/audio/audio_desc.hpp>
#include <fuse/types.hpp>

#include <unordered_map>

namespace fuse::audio {

enum class AudioBackendKind { Null, OpenAL };

/// Thin output backend — OpenAL when available, otherwise a null device for headless CI.
class AudioBackend {
public:
    bool init(const AudioDesc& desc);
    void shutdown();

    bool is_initialized() const { return m_initialized; }
    AudioBackendKind kind() const { return m_kind; }

    u32 create_source();
    void destroy_source(u32 source);
    void set_source_gain(u32 source, float gain);
    void set_source_position(u32 source, float x, float y, float z);
    void set_listener_position(float x, float y, float z);
    void set_listener_orientation(float fx, float fy, float fz, float ux, float uy, float uz);
    void play_source(u32 source);
    void stop_source(u32 source);
    void pause_source(u32 source, bool paused);

    float read_source_gain(u32 source) const;

private:
    bool m_initialized = false;
    AudioBackendKind m_kind = AudioBackendKind::Null;
    u32 m_nextSource = 1;

    struct SourceState {
        float gain = 1.f;
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        bool playing = false;
        bool paused = false;
#if defined(FUSE_AUDIO_OPENAL)
        u32 al_source = 0;
#endif
    };

    float m_listenerX = 0.f;
    float m_listenerY = 0.f;
    float m_listenerZ = 0.f;

    std::unordered_map<u32, SourceState> m_sources;
};

} // namespace fuse::audio
