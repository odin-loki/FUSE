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
        for (u32 guard = 0; guard < 64; ++guard) {
            u32 got = 0;
            if (!video->readFrame(scratch.data(), got)) return nullptr;
            ++decoded;
            nextIndex = got + 1;
            if (got >= i) {
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

namespace fuse::ui::widgets {
namespace {
struct ChanState {
    AudioLevels l;
    f32 holdLeft = 0.f;
};
struct MeterState {
    AudioMeterOptions opt;
    std::vector<ChanState> ch;
};
struct MeterSlot {
    NodeHandle node;
    std::unique_ptr<MeterState> s;
};
std::vector<MeterSlot>& meterSlots() { static std::vector<MeterSlot> v; return v; }
MeterState* meterOf(const UiContext& ui, NodeHandle n) {
    if (!ui.alive(n)) return nullptr;
    for (auto& s : meterSlots()) if (s.node == n) return s.s.get();
    return nullptr;
}
f32 toDb(f32 a) { return a > 1e-6f ? 20.f * std::log10(a) : -120.f; }

void decay(MeterState& m, f32 dt) {
    const f32 fall = m.opt.releaseDbPerSec * dt;
    for (ChanState& c : m.ch) {
        c.l.peakDb = std::max(-120.f, c.l.peakDb - fall);
        c.l.rmsDb = std::max(-120.f, c.l.rmsDb - fall);
        if (c.holdLeft > 0.f) {
            const f32 used = std::min(c.holdLeft, dt);
            c.holdLeft -= used;
            if (dt > used) c.l.holdDb = std::max(-120.f, c.l.holdDb - m.opt.releaseDbPerSec * (dt - used));
        } else {
            c.l.holdDb = std::max(-120.f, c.l.holdDb - fall);
        }
    }
}
void sync(UiContext& ui, NodeHandle n, MeterState& m) {
    f32 top = -120.f;
    for (const ChanState& c : m.ch) top = std::max(top, c.l.peakDb);
    ui.setValue(n, top, true);
    ui.invalidate(n);
}

struct MeterW {
    static constexpr std::string_view kName = "audiolevelmeter";
    static constexpr A11yRole kRole = A11yRole::ProgressBar;
    static constexpr bool kFocusable = false;
    static constexpr bool kInteractive = true;
    static Size measure(UiContext& ui, NodeHandle n, Size) {
        const MeterState* m = meterOf(ui, n);
        const f32 c = m ? f32(m->ch.size()) : 2.f;
        const f32 thick = c * 10.f + (c - 1.f) * 3.f + ((m && m->opt.scale) ? 26.f : 0.f);
        return m && !m->opt.vertical ? Size{220.f, thick} : Size{thick, 180.f};
    }
    static void paint(const PaintContext& pc, DrawList& dl) {
        MeterState* m = meterOf(pc.ui, pc.node);
        if (!m || m->ch.empty()) return;
        const AudioMeterOptions& o = m->opt;
        const Rect c = pc.content;
        const bool v = o.vertical;
        const f32 fs = pc.ui.computed(pc.node).fontSize * 0.7f;
        const f32 scaleW = o.scale ? 26.f : 0.f;
        const f32 n = f32(m->ch.size());
        const f32 lamp = 6.f;                          // clip lamp at the loud end
        const f32 len = (v ? c.h : c.w) - lamp - 2.f;  // meter length in px
        const f32 across = (v ? c.w : c.h) - scaleW;
        const f32 gap = 3.f, bw = std::max(2.f, (across - gap * (n - 1.f)) / n);
        auto frac = [&](f32 db) { return std::clamp((db - o.floorDb) / -o.floorDb, 0.f, 1.f); };
        // a segment from fraction a to b along the meter for bar i
        auto seg = [&](u32 i, f32 a, f32 b) -> Rect {
            const f32 x0 = f32(i) * (bw + gap);
            if (v) return {c.x + x0, c.y + lamp + 2.f + len * (1.f - b), bw, len * (b - a)};
            return {c.x + len * a, c.y + x0, len * (b - a), bw};
        };
        const Color bg = pc.tint(Color::rgb(0x15171C)), green = pc.tint(Color::rgb(0x3BD16F)),
                    yellow = pc.tint(Color::rgb(0xF2C94C)), red = pc.tint(Color::rgb(0xEB5757));
        const f32 fw = frac(o.warnDb), fdg = frac(o.dangerDb);
        for (u32 i = 0; i < m->ch.size(); ++i) {
            const AudioLevels& l = m->ch[i].l;
            dl.rect(seg(i, 0.f, 1.f), bg, 2.f);
            // dim zone colours behind, peak bright, RMS as a narrower inner bar
            const f32 pk = frac(l.peakDb), rm = frac(l.rmsDb);
            auto zones = [&](f32 upto, f32 alpha, f32 inset) {
                const f32 bands[3][2] = {{0.f, fw}, {fw, fdg}, {fdg, 1.f}};
                const Color cols[3] = {green, yellow, red};
                for (int z = 0; z < 3; ++z) {
                    const f32 a = bands[z][0], b = std::min(bands[z][1], upto);
                    if (b <= a) continue;
                    Rect r = seg(i, a, b);
                    if (v) r.x += inset, r.w -= 2.f * inset;
                    else r.y += inset, r.h -= 2.f * inset;
                    Color col = cols[z];
                    col.a *= alpha;
                    dl.rect(r, col);
                }
            };
            zones(pk, 0.45f, 0.f);
            zones(rm, 1.f, std::min(2.f, bw * 0.2f));
            if (l.holdDb > o.floorDb) {
                const f32 h = frac(l.holdDb);
                const Color hc = h >= fdg ? red : h >= fw ? yellow : pc.tint(Color::rgb(0xE8EAEE));
                Rect r = seg(i, h, h);
                if (v) r.y -= 1.f, r.h = 2.f;
                else r.x -= 1.f, r.w = 2.f;
                dl.rect(r, hc);
            }
            const Rect lr = v ? Rect{seg(i, 0, 1).x, c.y, bw, lamp} : Rect{c.x + len + 2.f, seg(i, 0, 1).y, lamp, bw};
            dl.rect(lr, l.clipped ? red : pc.tint(Color::rgb(0x3A1E22)), 1.f);
        }
        if (o.scale) {
            const Color tc = pc.tint(pc.style.color);
            for (f32 db = 0.f; db >= o.floorDb - 0.01f; db -= (db > -12.f ? 3.f : 12.f)) {
                if (db < 0.f && db > -12.f && std::fmod(-db, 6.f) != 0.f) continue; // 0, -6, -12, -24, ...
                const f32 t = frac(db);
                char s[8];
                std::snprintf(s, sizeof s, "%d", int(db));
                if (v) {
                    const f32 y = c.y + lamp + 2.f + len * (1.f - t);
                    const f32 x = c.x + across + 2.f;
                    dl.line({x, y}, {x + 4.f, y}, tc, 1.f);
                    pc.ui.paintString(dl, s, {x + 5.f, y - fs * 0.7f, scaleW - 5.f, fs * 1.4f}, tc, fs, TextAlignment::Start, pc.node.index);
                } else {
                    const f32 x = c.x + len * t, y = c.y + across + 2.f;
                    dl.line({x, y}, {x, y + 4.f}, tc, 1.f);
                    pc.ui.paintString(dl, s, {x - 12.f, y + 5.f, 24.f, fs * 1.4f}, tc, fs, TextAlignment::Center, pc.node.index);
                }
            }
        }
    }
    static bool onEvent(EventContext& ec, const Event& e) {
        if (ec.phase != Phase::Target || e.type != EventType::Click) return false;
        MeterState* m = meterOf(ec.ui, ec.node);
        if (!m) return false;
        for (ChanState& c : m->ch) c.l.clipped = false; // click clears the clip lamps
        ec.ui.invalidate(ec.node);
        return true;
    }
};
} // namespace

NodeHandle audioLevelMeter(UiContext& ui, NodeHandle parent, u32 channels, const AudioMeterOptions& opt, std::string_view id) {
    WidgetRegistry& r = ui.registry();
    if (r.find("audiolevelmeter") == 0xFFFFFFFFu) r.registerWidget<MeterW>();
    auto h = ui.create("audiolevelmeter", parent, id);
    if (!h) return {};
    auto& slots = meterSlots();
    std::erase_if(slots, [&](const MeterSlot& s) { return !ui.alive(s.node); });
    auto st = std::make_unique<MeterState>();
    st->opt = opt;
    st->ch.resize(std::max(1u, channels));
    slots.push_back({*h, std::move(st)});
    ui.setRange(*h, -120.f, 0.f);
    ui.setValue(*h, -120.f, false);
    ui.setA11y(*h, A11yRole::ProgressBar, "audio level");
    return *h;
}
void audioLevelMeterPush(UiContext& ui, NodeHandle w, std::span<const f32> s, u32 channels, f32 sampleRate) {
    MeterState* m = meterOf(ui, w);
    if (!m || channels == 0 || sampleRate <= 0.f) return;
    const std::size_t frames = s.size() / channels;
    if (frames == 0) return;
    decay(*m, f32(frames) / sampleRate);
    for (u32 c = 0; c < std::min<u32>(channels, u32(m->ch.size())); ++c) {
        f32 pk = 0.f;
        double sum = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            const f32 x = std::fabs(s[i * channels + c]);
            pk = std::max(pk, x), sum += double(x) * x;
        }
        ChanState& ch = m->ch[c];
        const f32 pdb = toDb(pk), rdb = toDb(f32(std::sqrt(sum / double(frames))));
        ch.l.peakDb = std::max(ch.l.peakDb, pdb);
        ch.l.rmsDb = std::max(ch.l.rmsDb, rdb);
        if (pdb >= ch.l.holdDb) ch.l.holdDb = pdb, ch.holdLeft = m->opt.holdSec;
        if (pk >= 1.f) ch.l.clipped = true;
    }
    sync(ui, w, *m);
}
void audioLevelMeterAdvance(UiContext& ui, NodeHandle w, f32 seconds) {
    if (MeterState* m = meterOf(ui, w); m && seconds > 0.f) decay(*m, seconds), sync(ui, w, *m);
}
void audioLevelMeterReset(UiContext& ui, NodeHandle w) {
    if (MeterState* m = meterOf(ui, w)) {
        for (ChanState& c : m->ch) c = {};
        sync(ui, w, *m);
    }
}
AudioLevels audioLevelMeterLevels(const UiContext& ui, NodeHandle w, u32 ch) {
    const MeterState* m = meterOf(ui, w);
    return m && ch < m->ch.size() ? m->ch[ch].l : AudioLevels{};
}
u32 audioLevelMeterChannels(const UiContext& ui, NodeHandle w) {
    const MeterState* m = meterOf(ui, w);
    return m ? u32(m->ch.size()) : 0u;
}
} // namespace fuse::ui::widgets
