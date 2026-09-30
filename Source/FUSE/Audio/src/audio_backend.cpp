#include <fuse/audio/audio_backend.hpp>

#include <fuse/audio/spsc_ring.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>

#if defined(FUSE_AUDIO_OPENAL)
#include <AL/al.h>
#include <AL/alc.h>
#if __has_include(<AL/alext.h>)
#include <AL/alext.h>
#endif
#endif

namespace fuse::audio {

#if defined(FUSE_AUDIO_OPENAL)

namespace {

// Extension tokens (values from alext.h; defined here so older system headers still build).
#ifndef AL_FORMAT_STEREO_FLOAT32
#define AL_FORMAT_STEREO_FLOAT32 0x10011
#endif
#ifndef AL_DIRECT_CHANNELS_SOFT
#define AL_DIRECT_CHANNELS_SOFT 0x1033
#endif
#ifndef ALC_CONNECTED
#define ALC_CONNECTED 0x313
#endif
#ifndef AL_SEC_OFFSET_LATENCY_SOFT
#define AL_SEC_OFFSET_LATENCY_SOFT 0x1201
#endif
#ifndef ALC_HRTF_SOFT
#define ALC_HRTF_SOFT 0x1992
#endif
#ifndef ALC_OUTPUT_LIMITER_SOFT
#define ALC_OUTPUT_LIMITER_SOFT 0x199A
#endif

using ReopenDeviceFn = ALCboolean(ALC_APIENTRY*)(ALCdevice*, const ALCchar*, const ALCint*);
using GetSourcedvFn = void(AL_APIENTRY*)(ALuint, ALenum, ALdouble*);

constexpr u32 kMaxDeviceBuffers = 8;

} // namespace

struct AudioBackend::OpenALStream {
    ALCdevice* device = nullptr;
    ALCcontext* context = nullptr;
    ALuint source = 0;
    ALuint buffers[kMaxDeviceBuffers] = {};
    u32 buffer_count = 0;

    // Feeder-thread state.
    ALuint free_buffers[kMaxDeviceBuffers] = {};
    u32 free_count = 0;
    u32 queued = 0;
    bool started = false;

    ALenum format = AL_FORMAT_STEREO16;
    bool float32 = false;
    bool has_disconnect = false;
    ReopenDeviceFn reopen = nullptr;
    GetSourcedvFn get_sourcedv = nullptr;
    ALCint attrs[9] = {};

    u32 block_frames = 512;  ///< Engine block (what submit() writes).
    u32 buffer_frames = 512; ///< Frames per AL buffer: a multiple of block_frames sized so the queue
                             ///< covers at least two device periods (see size_buffers()).
    u32 rate = 48000;
    u32 device_period = 0;   ///< Device update size in frames (rate / ALC_REFRESH), 0 if unknown.

    SpscRing<float> ring;
    std::vector<float> scratch;
    std::vector<std::int16_t> scratch16;

    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> simulate_loss{false};

    std::atomic<u64> frames_submitted{0};
    std::atomic<u64> frames_dropped{0};
    std::atomic<u64> frames_played{0};
    std::atomic<u64> underruns{0};
    std::atomic<u64> silence_frames{0};
    std::atomic<u64> device_lost{0};
    std::atomic<u64> reconnects{0};
    std::atomic<u32> device_queued_frames{0};
    std::atomic<double> device_latency{0.0};
    std::atomic<bool> connected{false};

    bool open(const char* device_name) {
        device = alcOpenDevice(device_name);
        if (device == nullptr) {
            return false;
        }
        context = alcCreateContext(device, attrs);
        if (context == nullptr || alcMakeContextCurrent(context) == ALC_FALSE) {
            if (context != nullptr) {
                alcDestroyContext(context);
                context = nullptr;
            }
            alcCloseDevice(device);
            device = nullptr;
            return false;
        }
        has_disconnect = alcIsExtensionPresent(device, "ALC_EXT_disconnect") == ALC_TRUE;
        reopen = nullptr;
        if (alcIsExtensionPresent(device, "ALC_SOFT_reopen_device") == ALC_TRUE) {
            reopen = reinterpret_cast<ReopenDeviceFn>(alcGetProcAddress(device, "alcReopenDeviceSOFT"));
        }
        get_sourcedv = nullptr;
        if (alIsExtensionPresent("AL_SOFT_source_latency") == AL_TRUE) {
            get_sourcedv = reinterpret_cast<GetSourcedvFn>(alGetProcAddress("alGetSourcedvSOFT"));
        }
        float32 = alIsExtensionPresent("AL_EXT_float32") == AL_TRUE;
        format = float32 ? static_cast<ALenum>(AL_FORMAT_STEREO_FLOAT32) : static_cast<ALenum>(AL_FORMAT_STEREO16);

        alGetError();
        alGenSources(1, &source);
        alGenBuffers(static_cast<ALsizei>(buffer_count), buffers);
        if (alGetError() != AL_NO_ERROR) {
            close();
            return false;
        }
        // The mix is already spatialised and in speaker space: play it 1:1.
        alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(source, AL_POSITION, 0.f, 0.f, 0.f);
        alSourcef(source, AL_ROLLOFF_FACTOR, 0.f);
        alSourcef(source, AL_GAIN, 1.f);
        if (alIsExtensionPresent("AL_SOFT_direct_channels") == AL_TRUE) {
            alSourcei(source, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
        }
        alGetError();
        size_buffers();
        reset_queue();
        return true;
    }

    /// The device mixes one period at a time; if every queued buffer fits in one period the
    /// source starves each time it mixes. Pick the buffer size so the queue spans >= 2 periods.
    void size_buffers() {
        ALCint refresh = 0;
        alcGetIntegerv(device, ALC_REFRESH, 1, &refresh);
        ALCint freq = 0;
        alcGetIntegerv(device, ALC_FREQUENCY, 1, &freq);
        device_period = (refresh > 0 && freq > 0) ? static_cast<u32>(freq / refresh) : 0;
        const u32 half_queue = std::max<u32>(buffer_count / 2, 1) * block_frames;
        const u32 blocks_per_buffer = device_period > half_queue ? (device_period + half_queue - 1) / half_queue : 1;
        u32 wanted = std::max(buffer_frames, block_frames * blocks_per_buffer);
        if (!scratch.empty()) {
            // Re-open after a device loss: stay within the scratch / ring sized at init.
            const usize cap = std::min(scratch.size(), ring.capacity() / 2) / 2;
            wanted = std::min<u32>(wanted, static_cast<u32>(cap / block_frames) * block_frames);
        }
        buffer_frames = std::max(wanted, block_frames);
    }

    void close() {
        if (context != nullptr) {
            if (source != 0) {
                alSourceStop(source);
                alSourcei(source, AL_BUFFER, 0);
                alDeleteSources(1, &source);
                source = 0;
            }
            if (buffers[0] != 0) {
                alDeleteBuffers(static_cast<ALsizei>(buffer_count), buffers);
                std::fill(std::begin(buffers), std::end(buffers), 0u);
            }
            if (alcGetCurrentContext() == context) {
                alcMakeContextCurrent(nullptr);
            }
            alcDestroyContext(context);
            context = nullptr;
        }
        if (device != nullptr) {
            alcCloseDevice(device);
            device = nullptr;
        }
    }

    /// Detach every buffer from the source and mark them all free (source stopped).
    void reset_queue() {
        alSourceStop(source);
        alSourcei(source, AL_BUFFER, 0);
        for (u32 i = 0; i < buffer_count; ++i) {
            free_buffers[i] = buffers[i];
        }
        free_count = buffer_count;
        queued = 0;
    }

    void upload(ALuint buffer, const float* stereo) {
        const u32 samples = buffer_frames * 2;
        if (float32) {
            alBufferData(buffer, format, stereo, static_cast<ALsizei>(samples * sizeof(float)),
                         static_cast<ALsizei>(rate));
            return;
        }
        for (u32 i = 0; i < samples; ++i) {
            const float s = std::clamp(stereo[i], -1.f, 1.f);
            scratch16[i] = static_cast<std::int16_t>(std::lrint(s * 32767.f));
        }
        alBufferData(buffer, format, scratch16.data(), static_cast<ALsizei>(samples * sizeof(std::int16_t)),
                     static_cast<ALsizei>(rate));
    }

    /// Move played buffers back to the free list, refill them from the ring, restart on starvation.
    void pump() {
        ALint processed = 0;
        alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
        while (processed > 0 && queued > 0) {
            ALuint buffer = 0;
            alSourceUnqueueBuffers(source, 1, &buffer);
            free_buffers[free_count++] = buffer;
            --queued;
            --processed;
            frames_played.fetch_add(buffer_frames, std::memory_order_relaxed);
        }

        const u32 buffer_samples = buffer_frames * 2;
        const bool starved = started && queued == 0;
        if (starved) {
            underruns.fetch_add(1, std::memory_order_relaxed);
        }
        while (free_count > 0) {
            if (ring.read(scratch.data(), buffer_samples)) {
                started = true;
            } else if (starved && queued == 0) {
                std::fill(scratch.begin(), scratch.end(), 0.f);
                silence_frames.fetch_add(buffer_frames, std::memory_order_relaxed);
            } else {
                break;
            }
            const ALuint buffer = free_buffers[--free_count];
            upload(buffer, scratch.data());
            alSourceQueueBuffers(source, 1, &buffer);
            ++queued;
        }

        ALint state = AL_STOPPED;
        alGetSourcei(source, AL_SOURCE_STATE, &state);
        if (queued > 0 && state != AL_PLAYING) {
            alSourcePlay(source);
        }

        ALint offset = 0;
        alGetSourcei(source, AL_SAMPLE_OFFSET, &offset);
        const u64 on_device = static_cast<u64>(queued) * buffer_frames;
        const u64 played_in_current = std::min<u64>(static_cast<u64>(std::max(offset, 0)), on_device);
        device_queued_frames.store(static_cast<u32>(on_device - played_in_current), std::memory_order_relaxed);
        if (get_sourcedv != nullptr) {
            ALdouble values[2] = {0.0, 0.0};
            get_sourcedv(source, AL_SEC_OFFSET_LATENCY_SOFT, values);
            device_latency.store(values[1], std::memory_order_relaxed);
        }
    }

    bool reconnect() {
        if (reopen != nullptr && device != nullptr && reopen(device, nullptr, attrs) == ALC_TRUE) {
            reset_queue();
            return true;
        }
        close();
        return open(nullptr);
    }

    void run() {
        const double block_seconds = static_cast<double>(std::min(block_frames, buffer_frames)) / static_cast<double>(rate);
        const auto poll = std::chrono::microseconds(
            std::max<long long>(500, static_cast<long long>(block_seconds * 1e6 / 4.0)));
        while (!stop.load(std::memory_order_acquire)) {
            bool lost = simulate_loss.exchange(false, std::memory_order_acq_rel);
            if (!lost && has_disconnect && device != nullptr) {
                ALCint alive = ALC_TRUE;
                alcGetIntegerv(device, ALC_CONNECTED, 1, &alive);
                lost = alive == ALC_FALSE;
            }
            if (lost || device == nullptr) {
                if (lost) {
                    device_lost.fetch_add(1, std::memory_order_relaxed);
                }
                connected.store(false, std::memory_order_release);
                if (reconnect()) {
                    reconnects.fetch_add(1, std::memory_order_relaxed);
                    connected.store(true, std::memory_order_release);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
            }
            pump();
            std::this_thread::sleep_for(poll);
        }
    }
};

#else

struct AudioBackend::OpenALStream {};

#endif

AudioBackend::AudioBackend() = default;

AudioBackend::~AudioBackend() {
    shutdown();
}

bool AudioBackend::init(const AudioDesc& desc) {
    shutdown();
    m_blockFrames = std::max<u32>(desc.frames_per_buf, 1);
    m_sampleRate = std::max<u32>(desc.sample_rate, 1);

#if defined(FUSE_AUDIO_OPENAL)
    if (desc.output != AudioOutputRequest::Null) {
        auto stream = std::make_unique<OpenALStream>();
        stream->block_frames = m_blockFrames;
        stream->buffer_frames = m_blockFrames;
        stream->rate = m_sampleRate;
        stream->buffer_count = std::clamp<u32>(desc.device_buffers, 2, kMaxDeviceBuffers);
        const ALCint attrs[] = {ALC_FREQUENCY, static_cast<ALCint>(m_sampleRate),
                                ALC_HRTF_SOFT, ALC_FALSE,
                                ALC_OUTPUT_LIMITER_SOFT, ALC_FALSE,
                                0, 0, 0};
        std::copy(std::begin(attrs), std::end(attrs), stream->attrs);
        if (stream->open(nullptr)) {
            // The ring holds at least two AL buffers' worth so the feeder always finds a full one.
            const u32 blocks_per_buffer = stream->buffer_frames / m_blockFrames;
            const u32 ring_blocks = std::max<u32>(std::max<u32>(desc.output_ring_blocks, 1), 2 * blocks_per_buffer);
            stream->ring.reset(static_cast<usize>(ring_blocks) * m_blockFrames * 2);
            // Sized for a larger device period after a reconnect too (size_buffers only grows).
            const usize scratch_frames = static_cast<usize>(stream->buffer_frames) * 4;
            stream->scratch.assign(scratch_frames * 2, 0.f);
            stream->scratch16.assign(scratch_frames * 2, 0);
            stream->connected.store(true, std::memory_order_release);
            OpenALStream* raw = stream.get();
            stream->thread = std::thread([raw] { raw->run(); });
            m_al = std::move(stream);
            m_kind = AudioBackendKind::OpenAL;
            m_initialized = true;
            return true;
        }
        if (desc.output == AudioOutputRequest::OpenAL) {
            m_initialized = false;
            return false;
        }
    }
#else
    if (desc.output == AudioOutputRequest::OpenAL) {
        m_initialized = false;
        return false;
    }
#endif

    m_kind = AudioBackendKind::Null;
    m_captureCapacityFrames = desc.capture_frames;
    m_capture.clear();
    m_capture.reserve(static_cast<usize>(m_captureCapacityFrames) * 2);
    m_nullStats = {};
    m_nullStats.connected = true;
    m_nullStats.float32_output = true;
    m_initialized = true;
    return true;
}

void AudioBackend::shutdown() {
#if defined(FUSE_AUDIO_OPENAL)
    if (m_al) {
        m_al->stop.store(true, std::memory_order_release);
        if (m_al->thread.joinable()) {
            m_al->thread.join();
        }
        m_al->close();
        m_al.reset();
    }
#endif
    m_al.reset();
    m_sources.clear();
    m_nextSource = 1;
    m_capture.clear();
    m_capture.shrink_to_fit();
    m_captureCapacityFrames = 0;
    m_nullStats = {};
    m_initialized = false;
    m_kind = AudioBackendKind::Null;
}

u32 AudioBackend::writable_blocks() const {
    if (!m_initialized) {
        return 0;
    }
#if defined(FUSE_AUDIO_OPENAL)
    if (m_al) {
        return static_cast<u32>(m_al->ring.write_available() / (static_cast<usize>(m_blockFrames) * 2));
    }
#endif
    return 1;
}

bool AudioBackend::submit(const float* stereo, u32 frames) {
    if (!m_initialized || stereo == nullptr || frames == 0) {
        return false;
    }
#if defined(FUSE_AUDIO_OPENAL)
    if (m_al) {
        if (!m_al->ring.write(stereo, static_cast<usize>(frames) * 2)) {
            m_al->frames_dropped.fetch_add(frames, std::memory_order_relaxed);
            return false;
        }
        m_al->frames_submitted.fetch_add(frames, std::memory_order_relaxed);
        return true;
    }
#endif
    m_nullStats.frames_submitted += frames;
    m_nullStats.frames_played += frames;
    const usize room = static_cast<usize>(m_captureCapacityFrames) * 2 - m_capture.size();
    const usize take = std::min(room, static_cast<usize>(frames) * 2);
    m_capture.insert(m_capture.end(), stereo, stereo + take);
    return true;
}

AudioOutputStats AudioBackend::output_stats() const {
#if defined(FUSE_AUDIO_OPENAL)
    if (m_al) {
        AudioOutputStats s;
        s.frames_submitted = m_al->frames_submitted.load(std::memory_order_relaxed);
        s.frames_dropped = m_al->frames_dropped.load(std::memory_order_relaxed);
        s.frames_played = m_al->frames_played.load(std::memory_order_relaxed);
        s.underruns = m_al->underruns.load(std::memory_order_relaxed);
        s.silence_frames = m_al->silence_frames.load(std::memory_order_relaxed);
        s.device_lost = m_al->device_lost.load(std::memory_order_relaxed);
        s.reconnects = m_al->reconnects.load(std::memory_order_relaxed);
        s.queued_frames = static_cast<u32>(m_al->ring.size_approx() / 2)
            + m_al->device_queued_frames.load(std::memory_order_relaxed);
        s.latency_seconds = static_cast<double>(s.queued_frames) / static_cast<double>(m_sampleRate)
            + m_al->device_latency.load(std::memory_order_relaxed);
        s.float32_output = m_al->float32;
        s.connected = m_al->connected.load(std::memory_order_acquire);
        return s;
    }
#endif
    return m_nullStats;
}

void AudioBackend::simulate_device_loss() {
#if defined(FUSE_AUDIO_OPENAL)
    if (m_al) {
        m_al->simulate_loss.store(true, std::memory_order_release);
    }
#endif
}

// ---- CPU-side voice records ------------------------------------------------------------------

u32 AudioBackend::create_source() {
    const u32 id = m_nextSource++;
    m_sources.emplace(id, SourceState{});
    return id;
}

void AudioBackend::destroy_source(u32 source) {
    m_sources.erase(source);
}

void AudioBackend::set_source_gain(u32 source, float gain) {
    auto it = m_sources.find(source);
    if (it != m_sources.end()) {
        it->second.gain = gain;
    }
}

void AudioBackend::set_source_position(u32 source, float x, float y, float z) {
    auto it = m_sources.find(source);
    if (it == m_sources.end()) {
        return;
    }
    it->second.x = x;
    it->second.y = y;
    it->second.z = z;
}

void AudioBackend::set_listener_position(float x, float y, float z) {
    m_listenerX = x;
    m_listenerY = y;
    m_listenerZ = z;
}

void AudioBackend::set_listener_orientation(float fx, float fy, float fz, float ux, float uy, float uz) {
    const float values[6] = {fx, fy, fz, ux, uy, uz};
    std::copy(std::begin(values), std::end(values), m_listenerOrientation);
}

void AudioBackend::play_source(u32 source) {
    auto it = m_sources.find(source);
    if (it == m_sources.end()) {
        return;
    }
    it->second.playing = true;
    it->second.paused = false;
}

void AudioBackend::stop_source(u32 source) {
    auto it = m_sources.find(source);
    if (it == m_sources.end()) {
        return;
    }
    it->second.playing = false;
    it->second.paused = false;
}

void AudioBackend::pause_source(u32 source, bool paused) {
    auto it = m_sources.find(source);
    if (it != m_sources.end()) {
        it->second.paused = paused;
    }
}

float AudioBackend::read_source_gain(u32 source) const {
    const auto it = m_sources.find(source);
    return it == m_sources.end() ? 0.f : it->second.gain;
}

bool AudioBackend::read_source_position(u32 source, float& x, float& y, float& z) const {
    const auto it = m_sources.find(source);
    if (it == m_sources.end()) {
        return false;
    }
    x = it->second.x;
    y = it->second.y;
    z = it->second.z;
    return true;
}

bool AudioBackend::is_source_playing(u32 source) const {
    const auto it = m_sources.find(source);
    return it != m_sources.end() && it->second.playing && !it->second.paused;
}

} // namespace fuse::audio
