// Multimedia extras: SystemVideo (streaming decode over the backend in media_*.cpp, audio through os::AudioOutput)
// and the audio level meter. See media.hpp.
#include "fuse/ui/extras/media.hpp"

#include "media_internal.hpp"

#include "fuse/ui/os.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace fuse::ui::media {

struct SystemVideo::Impl {
    std::unique_ptr<detail::Decoder> video;
    detail::StreamInfo vi;
    u32 frames = 0;
    std::string path, audioDesc;
    bool fromFile = false;
    // frame cache (LRU over cacheSlots())
    struct Slot {
        u32 index = ~0u;
        u64 used = 0;
        std::vector<u32> px;
    };
    std::vector<Slot> slots;
    u64 tick = 0;
    u32 nextIndex = 0; // index the decoder delivers next without seeking
    u32 decoded = 0, seeks = 0;
    std::vector<u32> scratch;
    std::mutex videoMutex; // frame() may be called from any thread
    widgets::VideoSource src;
    // audio
    detail::StreamInfo ai;
    bool audio = false, outputEnabled = true;
    std::thread worker;
    std::atomic<bool> stop{false}, running{false};
    std::atomic<u64> played{0};
    std::atomic<double> startSec{0.0}, latency{0.0};
    std::string backend = "none";
    std::mutex tapMutex;
    std::function<void(std::span<const f32>, u32, u32)> tap;

    ~Impl() { stopAudio(); }
    void reset() {
        stopAudio();
        video.reset();
        vi = {}, ai = {}, audio = false, frames = 0;
        slots.clear(), tick = 0, nextIndex = 0, decoded = 0, seeks = 0;
    }
    bool openAudioProbe() { // probe the audio track once so hasAudio / rate are known before playback
        std::string e;
        auto d = fromFile ? detail::openFile(path, detail::StreamKind::Audio, &e)
                          : (audioDesc.empty() ? nullptr : detail::openPipeline(audioDesc, detail::StreamKind::Audio, &e));
        if (!d) return false;
        ai = d->info();
        return ai.rate && ai.channels;
    }
    void finishOpen(u32 maxFrames) {
        vi = video->info();
        frames = std::min(vi.frames, maxFrames);
        slots.assign(SystemVideo::cacheSlots(), {});
        scratch.resize(std::size_t(vi.width) * vi.height);
        nextIndex = vi.atEnd ? ~0u - 64 : 0; // forces a seek on the first read
        audio = openAudioProbe();
        src = {};
        src.user = this;
        src.width = vi.width, src.height = vi.height, src.frames = frames, src.fps = vi.fps;
        src.frame = [](void* u, u32 i, u32* out) {
            auto* m = static_cast<Impl*>(u);
            if (const u32* p = m->get(i)) std::memcpy(out, p, std::size_t(m->vi.width) * m->vi.height * 4);
        };
        if (audio) {
            src.playback = [](void* u, bool playing, u32 f) {
                auto* m = static_cast<Impl*>(u);
                if (playing) m->playAudio(double(f) / double(m->vi.fps), nullptr);
                else m->stopAudio();
            };
            src.clock = [](void* u) { return static_cast<Impl*>(u)->clock(); };
        }
    }
    const u32* get(u32 i) {
        if (!video || i >= frames) return nullptr;
        std::lock_guard lk(videoMutex);
        for (Slot& s : slots)
            if (s.index == i) {
                s.used = ++tick;
                return s.px.data();
            }
        // sequential when the target is a little ahead (playback); otherwise seek
        if (i < nextIndex || i > nextIndex + 15) {
            ++seeks;
            if (!video->seekFrame(i)) return nullptr;
            nextIndex = i; // provisional: corrected by the delivered index below
        }
        bool any = false;
        for (u32 guard = 0; guard < 64; ++guard) {
            u32 got = 0;
            const bool read = video->readFrame(scratch.data(), got);
            // a duration-derived count can exceed the frames actually present by one or two (stream ends, timestamp
            // rounding): past the end, the last decoded frame stands in
            if (!read && !(any && i + 3 >= frames)) return nullptr;
            if (read) ++decoded, nextIndex = got + 1, any = true;
            if (!read || got >= i) {
                Slot* victim = &slots[0];
                for (Slot& s : slots)
                    if (s.used < victim->used) victim = &s;
                victim->index = i, victim->used = ++tick;
                victim->px = scratch;
                return victim->px.data();
            }
        }
        return nullptr;
    }
    double clock() const {
        if (!running) return -1.0;
        const double heard = double(played.load()) / double(std::max(1u, ai.rate)) - latency.load();
        return startSec.load() + std::max(0.0, heard);
    }
    void stopAudio() {
        stop = true;
        if (worker.joinable()) worker.join();
        stop = false;
        running = false;
    }
    bool playAudio(double from, std::string* err) {
        stopAudio();
        if (!audio) {
            if (err) *err = "no audio track";
            return false;
        }
        std::string e;
        std::unique_ptr<detail::Decoder> dec = fromFile ? detail::openFile(path, detail::StreamKind::Audio, &e)
                                                        : detail::openPipeline(audioDesc, detail::StreamKind::Audio, &e);
        if (!dec || (from > 0.0 && !dec->seekSeconds(from))) {
            if (err) *err = dec ? "audio seek failed" : e;
            return false;
        }
        std::unique_ptr<os::AudioOutput> out;
        if (outputEnabled) out = os::openAudioOutput(ai.rate, ai.channels, nullptr);
        backend = out ? out->backend() : "none";
        latency = out ? out->latencySeconds() : 0.0;
        played = 0, startSec = from, running = true;
        worker = std::thread([this, d = std::move(dec), o = std::move(out)]() mutable {
            std::vector<f32> buf;
            const auto t0 = std::chrono::steady_clock::now();
            while (!stop) {
                buf.clear();
                if (!d->readAudio(buf)) break;
                const u32 ch = ai.channels;
                const std::size_t nf = buf.size() / ch;
                if (!nf) continue;
                {
                    std::lock_guard lk(tapMutex);
                    if (tap) tap(std::span<const f32>(buf.data(), nf * ch), ch, ai.rate);
                }
                if (o) { // write in ~20 ms pieces so stop is responsive
                    const std::size_t piece = std::max<std::size_t>(1, ai.rate / 50);
                    for (std::size_t at = 0; at < nf && !stop; at += piece) {
                        const std::size_t n = std::min(piece, nf - at);
                        if (!o->write(buf.data() + at * ch, n)) { o.reset(); break; }
                        played += n;
                    }
                } else { // no device: pace by the wall clock
                    played += nf;
                    const auto due = t0 + std::chrono::duration<double>(double(played.load()) / ai.rate);
                    while (!stop && std::chrono::steady_clock::now() < due) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            if (o && !stop) o->drain();
            running = false;
        });
        return true;
    }
};

SystemVideo::SystemVideo() : m(std::make_unique<Impl>()) {}
SystemVideo::~SystemVideo() = default;
bool SystemVideo::available() { return detail::backendAvailable(); }
std::string SystemVideo::backendName() { return detail::backendName(); }
bool SystemVideo::openFile(const std::string& path, u32 maxFrames, std::string* error) {
    m->reset();
    m->video = detail::openFile(path, detail::StreamKind::Video, error);
    if (!m->video) return false;
    m->fromFile = true, m->path = path, m->audioDesc.clear();
    m->finishOpen(maxFrames);
    return true;
}
bool SystemVideo::openPipeline(const std::string& d, u32 maxFrames, std::string* error, const std::string& audioDesc) {
    m->reset();
    m->video = detail::openPipeline(d, detail::StreamKind::Video, error);
    if (!m->video) return false;
    m->fromFile = false, m->path.clear(), m->audioDesc = audioDesc;
    m->finishOpen(maxFrames);
    return true;
}
bool SystemVideo::valid() const { return m->video && m->frames; }
u32 SystemVideo::width() const { return m->vi.width; }
u32 SystemVideo::height() const { return m->vi.height; }
u32 SystemVideo::frameCount() const { return m->frames; }
f32 SystemVideo::fps() const { return m->vi.fps; }
double SystemVideo::durationSeconds() const { return m->vi.fps > 0 ? double(m->frames) / m->vi.fps : 0.0; }
const u32* SystemVideo::frame(u32 i) const { return m->get(i); }
u32 SystemVideo::decodedFrames() const { return m->decoded; }
u32 SystemVideo::seekCount() const { return m->seeks; }
u32 SystemVideo::cachedFrames() const {
    u32 n = 0;
    for (const auto& s : m->slots) n += s.index != ~0u;
    return n;
}
const widgets::VideoSource& SystemVideo::source() const { return m->src; }
bool SystemVideo::hasAudio() const { return m->audio; }
u32 SystemVideo::audioRate() const { return m->ai.rate; }
u32 SystemVideo::audioChannels() const { return m->ai.channels; }
bool SystemVideo::playAudio(double from, std::string* e) { return m->playAudio(from, e); }
void SystemVideo::stopAudio() { m->stopAudio(); }
bool SystemVideo::audioPlaying() const { return m->running; }
double SystemVideo::audioClock() const { return m->clock(); }
u64 SystemVideo::audioFramesPlayed() const { return m->played; }
void SystemVideo::setAudioTap(std::function<void(std::span<const f32>, u32, u32)> t) {
    std::lock_guard lk(m->tapMutex);
    m->tap = std::move(t);
}
void SystemVideo::setAudioOutputEnabled(bool e) { m->outputEnabled = e; }
std::string SystemVideo::audioBackend() const { return m->backend; }

} // namespace fuse::ui::media

// (the audio level meter moved to audio_meter.cpp)
