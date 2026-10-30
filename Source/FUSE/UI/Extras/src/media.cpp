// Multimedia extras: SystemVideo (runtime-loaded GStreamer decode) and the audio level meter. See media.hpp.
#include "fuse/ui/extras/media.hpp"

#include "fuse/ui/os.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fuse::ui::media {
namespace {
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define FUSE_MEDIA_GST 1
// GStreamer through opaque pointers only; GstMapInfo is the one struct read (stable 1.x layout).
struct GstMapInfo {
    void* memory;
    int flags;
    u8* data;
    std::size_t size, maxsize;
    void* user_data[4];
    void* reserved[4];
};
struct GError {
    u32 domain;
    int code;
    char* message;
};
struct Gst {
    std::unique_ptr<os::Library> core, app;
    int (*gst_init_check)(int*, char***, GError**) = nullptr;
    void* (*gst_parse_launch)(const char*, GError**) = nullptr;
    void* (*gst_bin_get_by_name)(void*, const char*) = nullptr;
    int (*gst_element_set_state)(void*, int) = nullptr;
    int (*gst_element_get_state)(void*, int*, int*, u64) = nullptr;
    void (*gst_object_unref)(void*) = nullptr;
    void* (*gst_sample_get_buffer)(void*) = nullptr;
    void* (*gst_sample_get_caps)(void*) = nullptr;
    void* (*gst_caps_get_structure)(void*, u32) = nullptr;
    int (*gst_structure_get_int)(void*, const char*, int*) = nullptr;
    int (*gst_structure_get_fraction)(void*, const char*, int*, int*) = nullptr;
    int (*gst_buffer_map)(void*, GstMapInfo*, int) = nullptr;
    void (*gst_buffer_unmap)(void*, GstMapInfo*) = nullptr;
    void (*gst_mini_object_unref)(void*) = nullptr;
    void* (*gst_element_factory_find)(const char*) = nullptr;
    void (*gst_version)(u32*, u32*, u32*, u32*) = nullptr;
    void* (*gst_app_sink_try_pull_sample)(void*, u64) = nullptr;
    int (*gst_app_sink_is_eos)(void*) = nullptr;
    void* (*gst_element_get_bus)(void*) = nullptr;
    void* (*gst_bus_timed_pop_filtered)(void*, u64, int) = nullptr;
    void (*g_error_free)(GError*) = nullptr;
    bool ok = false;
};
Gst& gst() {
    static Gst& g = *new Gst{}; // never unloaded: GStreamer keeps threads past static destruction
    static bool once = [] {
        g.core = os::loadLibrary({"libgstreamer-1.0.so.0", "libgstreamer-1.0.so"});
        g.app = os::loadLibrary({"libgstapp-1.0.so.0", "libgstapp-1.0.so"});
        if (!g.core || !g.app) return false;
        bool all = true;
        auto L = [&](os::Library& lib, auto& f, const char* n) {
            f = lib.get<std::remove_reference_t<decltype(f)>>(n);
            all = all && f;
        };
#define C(n) L(*g.core, g.n, #n)
        C(gst_init_check); C(gst_parse_launch); C(gst_bin_get_by_name); C(gst_element_set_state); C(gst_element_get_state);
        C(gst_object_unref); C(gst_sample_get_buffer); C(gst_sample_get_caps); C(gst_caps_get_structure);
        C(gst_structure_get_int); C(gst_structure_get_fraction); C(gst_buffer_map); C(gst_buffer_unmap);
        C(gst_mini_object_unref); C(gst_element_factory_find); C(gst_version);
        C(gst_element_get_bus); C(gst_bus_timed_pop_filtered);
#undef C
        L(*g.app, g.gst_app_sink_try_pull_sample, "gst_app_sink_try_pull_sample");
        L(*g.app, g.gst_app_sink_is_eos, "gst_app_sink_is_eos");
        g.g_error_free = g.core->get<void (*)(GError*)>("g_error_free");
        if (!g.g_error_free)
            if (auto glib = os::loadLibrary({"libglib-2.0.so.0"})) g.g_error_free = glib->get<void (*)(GError*)>("g_error_free"), glib.release();
        if (!all) return false;
        if (!g.gst_init_check(nullptr, nullptr, nullptr)) return false;
        // decodebin / videoconvert / appsink must all be installed for files to play
        for (const char* f : {"appsink", "videoconvert", "decodebin"}) {
            void* fac = g.gst_element_factory_find(f);
            if (!fac) return false;
            g.gst_object_unref(fac);
        }
        g.ok = true;
        return true;
    }();
    (void)once;
    return g;
}
#endif
} // namespace

struct SystemVideo::Impl {
    u32 w = 0, h = 0;
    f32 fps = 30.f;
    std::vector<std::vector<u32>> frames;
    widgets::VideoSource src;

    bool run(const std::string& desc, u32 maxFrames, std::string* error) {
        auto fail = [&](std::string e) {
            if (error) *error = std::move(e);
            frames.clear(), w = h = 0;
            return false;
        };
#if FUSE_MEDIA_GST
        Gst& g = gst();
        if (!g.ok) return fail("GStreamer (core, app, decodebin, videoconvert) is not available");
        GError* err = nullptr;
        void* pipe = g.gst_parse_launch(desc.c_str(), &err);
        if (err) {
            std::string m = err->message ? err->message : "parse error";
            if (g.g_error_free) g.g_error_free(err);
            if (pipe) g.gst_object_unref(pipe);
            return fail(m);
        }
        if (!pipe) return fail("gst_parse_launch failed");
        void* sink = g.gst_bin_get_by_name(pipe, "fusesink");
        if (!sink) {
            g.gst_object_unref(pipe);
            return fail("pipeline has no sink");
        }
        constexpr int kNull = 1, kPlaying = 4, kFailure = 0;
        frames.clear(), w = h = 0;
        std::string why;
        if (g.gst_element_set_state(pipe, kPlaying) == kFailure) why = "could not start the decoder (unsupported or missing file)";
        constexpr u64 kTimeout = 5'000'000'000ull; // 5 s per frame: a stalled pipeline (error) ends the decode
        while (why.empty() && frames.size() < maxFrames) {
            void* sample = g.gst_app_sink_try_pull_sample(sink, kTimeout);
            if (!sample) {
                if (!g.gst_app_sink_is_eos(sink) && frames.empty()) why = "decoder produced no frames";
                break;
            }
            if (frames.empty()) {
                void* st = g.gst_caps_get_structure(g.gst_sample_get_caps(sample), 0);
                int iw = 0, ih = 0, fn = 0, fd = 1;
                g.gst_structure_get_int(st, "width", &iw);
                g.gst_structure_get_int(st, "height", &ih);
                if (g.gst_structure_get_fraction(st, "framerate", &fn, &fd) && fn > 0 && fd > 0) fps = f32(fn) / f32(fd);
                w = u32(std::max(iw, 0)), h = u32(std::max(ih, 0));
            }
            GstMapInfo mi{};
            void* buf = g.gst_sample_get_buffer(sample);
            if (w && h && buf && g.gst_buffer_map(buf, &mi, 1)) {
                const std::size_t n = std::size_t(w) * h;
                const std::size_t stride = mi.size / h; // RGBA rows, padded to 4 bytes (already a multiple)
                if (stride >= std::size_t(w) * 4) {
                    std::vector<u32> px(n);
                    for (u32 y = 0; y < h; ++y) std::memcpy(px.data() + std::size_t(y) * w, mi.data + y * stride, std::size_t(w) * 4);
                    for (u32& p : px) p |= 0xFF000000u; // opaque
                    frames.push_back(std::move(px));
                }
                g.gst_buffer_unmap(buf, &mi);
            }
            g.gst_mini_object_unref(sample);
        }
        if (why.empty() && g.gst_app_sink_is_eos(sink)) {
            // let the rest of the pipeline (e.g. a muxer branch writing a file) finish before tearing it down
            if (void* bus = g.gst_element_get_bus(pipe)) {
                constexpr int kEos = 1, kError = 2;
                if (void* msg = g.gst_bus_timed_pop_filtered(bus, kTimeout, kEos | kError)) g.gst_mini_object_unref(msg);
                g.gst_object_unref(bus);
            }
        }
        g.gst_element_set_state(pipe, kNull);
        g.gst_object_unref(sink);
        g.gst_object_unref(pipe);
        if (!why.empty()) return fail(why);
        if (frames.empty()) return fail("no video frames");
        src = {};
        src.user = this, src.width = w, src.height = h, src.frames = u32(frames.size()), src.fps = fps;
        src.frame = [](void* u, u32 i, u32* rgba) {
            auto* s = static_cast<Impl*>(u);
            if (s->frames.empty()) return;
            const auto& f = s->frames[std::min<std::size_t>(i, s->frames.size() - 1)];
            std::memcpy(rgba, f.data(), f.size() * 4);
        };
        return true;
#else
        (void)desc, (void)maxFrames;
        return fail("no system video decoder backend on this platform");
#endif
    }
};

SystemVideo::SystemVideo() : m(std::make_unique<Impl>()) {}
SystemVideo::~SystemVideo() = default;
bool SystemVideo::available() {
#if FUSE_MEDIA_GST
    return gst().ok;
#else
    return false;
#endif
}
std::string SystemVideo::backendName() {
#if FUSE_MEDIA_GST
    if (!gst().ok) return {};
    u32 a = 0, b = 0, c = 0, d = 0;
    gst().gst_version(&a, &b, &c, &d);
    char s[48];
    std::snprintf(s, sizeof s, "gstreamer %u.%u.%u", a, b, c);
    return s;
#else
    return {};
#endif
}
static const char* kTail = " ! videoconvert ! video/x-raw,format=RGBA ! appsink name=fusesink sync=false max-buffers=4";
bool SystemVideo::openFile(const std::string& path, u32 maxFrames, std::string* error) {
    if (path.find_first_of("\"\\") != std::string::npos) {
        if (error) *error = "unsupported character in path";
        return false;
    }
    return m->run("filesrc location=\"" + path + "\" ! decodebin" + kTail, maxFrames, error);
}
bool SystemVideo::openPipeline(const std::string& d, u32 maxFrames, std::string* error) { return m->run(d + kTail, maxFrames, error); }
bool SystemVideo::valid() const { return !m->frames.empty(); }
u32 SystemVideo::width() const { return m->w; }
u32 SystemVideo::height() const { return m->h; }
u32 SystemVideo::frameCount() const { return u32(m->frames.size()); }
f32 SystemVideo::fps() const { return m->fps; }
const u32* SystemVideo::frame(u32 i) const { return i < m->frames.size() ? m->frames[i].data() : nullptr; }
const widgets::VideoSource& SystemVideo::source() const { return m->src; }

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
