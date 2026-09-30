#pragma once

#include <fuse/audio/audio_desc.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace fuse::audio {

enum class AudioBackendKind { Null, OpenAL };

/// Output-stream counters (GAP-AUDIO-DEVICE-OUT). Frame counts are stereo frames.
struct AudioOutputStats {
    u64 frames_submitted = 0;   ///< Frames the engine handed to submit() and the output accepted.
    u64 frames_dropped = 0;     ///< Frames submit() rejected because the output queue was full.
    u64 frames_played = 0;      ///< Frames the device finished playing (Null: consumed at submit).
    u64 underruns = 0;          ///< Times the device ran dry and silence was inserted / it restarted.
    u64 silence_frames = 0;     ///< Frames of silence inserted to bridge underruns.
    u64 device_lost = 0;        ///< ALC_EXT_disconnect reported the device gone (or a simulated loss).
    u64 reconnects = 0;         ///< Successful device re-opens after a loss.
    u32 queued_frames = 0;      ///< Frames waiting in the ring + queued on the device right now.
    double latency_seconds = 0; ///< Queued frames / rate (+ device latency when AL_SOFT_source_latency).
    bool float32_output = false;///< OpenAL buffers carry AL_FORMAT_STEREO_FLOAT32 (else 16-bit PCM).
    bool connected = false;     ///< Device currently connected.
};

/// Audio output: the engine's software mix goes to one output stream.
///
/// * OpenAL: one streaming AL source (direct channels, source-relative) with `device_buffers`
///   buffers of `frames_per_buf` stereo frames, refilled by a feeder thread from a lock-free SPSC
///   ring that submit() writes on the game thread. AL_EXT_float32 buffers when available, else
///   16-bit. Underruns insert one block of silence and are counted; ALC_EXT_disconnect is polled
///   and a lost device is re-opened (ALC_SOFT_reopen_device, else a full close / reopen). All AL
///   calls after init happen on the feeder thread.
/// * Null: consumes each submitted block immediately into an optional capture buffer that tests
///   read back (the sample clock is the caller).
///
/// The per-voice "sources" below are CPU-side voice records only (gain / position / play state the
/// engine computed, for diagnostics and headless readback). They own no AL objects: all voices are
/// mixed in software by SpatialMixer and reach the device through the single output stream.
class AudioBackend {
public:
    AudioBackend();
    ~AudioBackend();
    AudioBackend(const AudioBackend&) = delete;
    AudioBackend& operator=(const AudioBackend&) = delete;

    bool init(const AudioDesc& desc);
    void shutdown();

    bool is_initialized() const { return m_initialized; }
    AudioBackendKind kind() const { return m_kind; }

    // ---- output stream ----------------------------------------------------------------------

    /// Blocks of `frames_per_buf` frames the output can accept right now (Null: always 1).
    u32 writable_blocks() const;
    /// Hand one block of interleaved stereo float frames to the output. Never blocks and never
    /// allocates. Returns false (and counts the frames as dropped) when the queue is full.
    bool submit(const float* stereo, u32 frames);
    AudioOutputStats output_stats() const;

    /// Null backend capture (interleaved stereo). Capacity is AudioDesc::capture_frames; frames past
    /// it are not captured (the output still counts them as played).
    const std::vector<float>& capture() const { return m_capture; }
    u32 captured_frames() const { return static_cast<u32>(m_capture.size() / 2); }
    void clear_capture() { m_capture.clear(); }

    /// Test hook: make the feeder thread take the device-lost path on its next poll (as if
    /// ALC_CONNECTED had gone false) and reconnect. No effect on Null.
    void simulate_device_loss();

    // ---- CPU-side voice records (no device objects) ------------------------------------------

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
    bool read_source_position(u32 source, float& x, float& y, float& z) const;
    bool is_source_playing(u32 source) const;
    u32 source_count() const { return static_cast<u32>(m_sources.size()); }

    struct OpenALStream; ///< Defined in audio_backend.cpp (keeps AL headers private).

private:
    bool m_initialized = false;
    AudioBackendKind m_kind = AudioBackendKind::Null;
    u32 m_nextSource = 1;
    u32 m_blockFrames = 512;
    u32 m_sampleRate = 48000;

    struct SourceState {
        float gain = 1.f;
        float x = 0.f;
        float y = 0.f;
        float z = 0.f;
        bool playing = false;
        bool paused = false;
    };

    float m_listenerX = 0.f;
    float m_listenerY = 0.f;
    float m_listenerZ = 0.f;
    float m_listenerOrientation[6] = {0.f, 0.f, -1.f, 0.f, 1.f, 0.f};

    std::unordered_map<u32, SourceState> m_sources;

    // Null output.
    std::vector<float> m_capture;
    u32 m_captureCapacityFrames = 0;
    AudioOutputStats m_nullStats{};

    std::unique_ptr<OpenALStream> m_al;
};

} // namespace fuse::audio
