// SystemVideo backend: GStreamer 1.x, loaded at runtime through the os seam (Linux / BSD). Each Decoder owns one
// pipeline "source ! decodebin ! convert ! appsink" kept alive for streaming reads; seeks are flushing + accurate.
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include "media_internal.hpp"

#include "fuse/ui/os.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fuse::ui::media::detail {
namespace {
// Only opaque pointers cross the boundary, plus two stable 1.x layouts: GstMapInfo and GstBuffer's pts field.
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
constexpr std::size_t kBufferPtsOffset = 72; // GstMiniObject (64) + GstBufferPool* pool
constexpr u64 kNone = ~0ull;                  // GST_CLOCK_TIME_NONE
constexpr int kFormatTime = 3, kStateNull = 1, kStatePaused = 3, kStatePlaying = 4, kFailure = 0;
constexpr int kSeekFlush = 1, kSeekAccurate = 2;
constexpr int kMsgEos = 1, kMsgError = 2;

struct Gst {
    std::unique_ptr<os::Library> core, app, gobj;
    int (*gst_init_check)(int*, char***, GError**) = nullptr;
    void* (*gst_parse_launch)(const char*, GError**) = nullptr;
    void* (*gst_bin_get_by_name)(void*, const char*) = nullptr;
    int (*gst_element_set_state)(void*, int) = nullptr;
    int (*gst_element_get_state)(void*, int*, int*, u64) = nullptr;
    int (*gst_element_seek_simple)(void*, int, int, long long) = nullptr;
    int (*gst_element_query_duration)(void*, int, long long*) = nullptr;
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
    void* (*gst_element_get_bus)(void*) = nullptr;
    void* (*gst_bus_timed_pop_filtered)(void*, u64, int) = nullptr;
    void* (*gst_app_sink_try_pull_sample)(void*, u64) = nullptr;
    int (*gst_app_sink_is_eos)(void*) = nullptr;
    void (*g_object_set)(void*, const char*, ...) = nullptr;
    void (*g_error_free)(GError*) = nullptr;
    bool ok = false;
};
Gst& gst() {
    static Gst& g = *new Gst{}; // never unloaded: GStreamer keeps threads past static destruction
    static const bool once = [] {
        g.core = os::loadLibrary({"libgstreamer-1.0.so.0", "libgstreamer-1.0.so"});
        g.app = os::loadLibrary({"libgstapp-1.0.so.0", "libgstapp-1.0.so"});
        g.gobj = os::loadLibrary({"libgobject-2.0.so.0", "libgobject-2.0.so"});
        if (!g.core || !g.app || !g.gobj) return false;
        bool all = true;
        auto L = [&](os::Library& lib, auto& f, const char* n) {
            f = lib.get<std::remove_reference_t<decltype(f)>>(n);
            all = all && f;
        };
#define C(n) L(*g.core, g.n, #n)
        C(gst_init_check); C(gst_parse_launch); C(gst_bin_get_by_name); C(gst_element_set_state); C(gst_element_get_state);
        C(gst_element_seek_simple); C(gst_element_query_duration); C(gst_object_unref); C(gst_sample_get_buffer);
        C(gst_sample_get_caps); C(gst_caps_get_structure); C(gst_structure_get_int); C(gst_structure_get_fraction);
        C(gst_buffer_map); C(gst_buffer_unmap); C(gst_mini_object_unref); C(gst_element_factory_find); C(gst_version);
        C(gst_element_get_bus); C(gst_bus_timed_pop_filtered);
#undef C
        L(*g.app, g.gst_app_sink_try_pull_sample, "gst_app_sink_try_pull_sample");
        L(*g.app, g.gst_app_sink_is_eos, "gst_app_sink_is_eos");
        L(*g.gobj, g.g_object_set, "g_object_set");
        if (auto glib = os::loadLibrary({"libglib-2.0.so.0"})) {
            g.g_error_free = glib->get<void (*)(GError*)>("g_error_free");
            (void)glib.release(); // glib stays loaded with GStreamer anyway
        }
        if (!all || !g.g_error_free) return false;
        if (!g.gst_init_check(nullptr, nullptr, nullptr)) return false;
        for (const char* f : {"appsink", "videoconvert", "audioconvert", "audioresample", "decodebin", "filesrc"}) {
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

const char* tail(StreamKind k) {
    return k == StreamKind::Video ? " ! videoconvert ! video/x-raw,format=RGBA ! appsink name=fusesink sync=false max-buffers=4"
                                  : " ! audioconvert ! audioresample ! audio/x-raw,format=F32LE,layout=interleaved ! appsink "
                                    "name=fusesink sync=false max-buffers=8";
}

class GstDecoder final : public Decoder {
public:
    Gst& g = gst();
    StreamKind kind;
    void* pipe = nullptr;
    void* sink = nullptr;
    void* bus = nullptr;
    StreamInfo si;
    void* pending = nullptr; // a sample pulled while probing, returned by the next read
    std::vector<u32> scratch;

    explicit GstDecoder(StreamKind k) : kind(k) {}
    std::string desc, location;
    bool hasLocation = false, ended = false, ownSink = false, busEnded = false;
    double lastSeek = -1.0; // target of the seek the next read follows (for one restart-and-retry)
    ~GstDecoder() override { teardown(); }
    void teardown() {
        if (pending) g.gst_mini_object_unref(pending), pending = nullptr;
        if (pipe && ownSink && !ended) // a pipeline that also writes somewhere (a file sink) runs to the end on close
            for (int i = 0; i < 100000; ++i) {
                void* s = pull(1'000'000'000ull);
                if (!s) break;
                g.gst_mini_object_unref(s);
            }
        if (pipe && ownSink && !busEnded) // the app sink may finish before the other sinks: wait for the pipeline EOS
            if (void* msg = g.gst_bus_timed_pop_filtered(bus, 5'000'000'000ull, kMsgEos | kMsgError)) g.gst_mini_object_unref(msg);
        if (pipe) g.gst_element_set_state(pipe, kStateNull);
        if (bus) g.gst_object_unref(bus);
        if (sink) g.gst_object_unref(sink);
        if (pipe) g.gst_object_unref(pipe);
        pipe = sink = bus = nullptr;
    }
    std::string busError() {
        if (!bus) return {};
        void* msg = g.gst_bus_timed_pop_filtered(bus, 0, kMsgError);
        if (!msg) return {};
        g.gst_mini_object_unref(msg);
        return kind == StreamKind::Video ? "no decodable video stream" : "no decodable audio stream";
    }
    // pull with short timeouts so a decoder error (posted on the bus) ends the wait early
    void* pull(u64 totalNs = 5'000'000'000ull) {
        for (u64 waited = 0; waited < totalNs; waited += 50'000'000ull) {
            if (void* s = g.gst_app_sink_try_pull_sample(sink, 50'000'000ull)) return s;
            if (g.gst_app_sink_is_eos(sink)) return ended = true, nullptr;
            if (void* msg = g.gst_bus_timed_pop_filtered(bus, 0, kMsgError | kMsgEos)) {
                g.gst_mini_object_unref(msg);
                ended = busEnded = true;
                return nullptr;
            }
        }
        return nullptr;
    }
    // build + start the pipeline; also used to restart after the end, since sources such as videotestsrc
    // num-buffers=N do not produce again after a seek once they hit their end
    bool launch(std::string* err) {
        auto fail = [&](std::string m) { if (err) *err = std::move(m); return false; };
        ended = busEnded = false;
        GError* ge = nullptr;
        pipe = g.gst_parse_launch(desc.c_str(), &ge);
        if (ge) {
            std::string m = ge->message ? ge->message : "parse error";
            g.g_error_free(ge);
            return fail(m);
        }
        if (!pipe) return fail("gst_parse_launch failed");
        sink = g.gst_bin_get_by_name(pipe, "fusesink");
        bus = g.gst_element_get_bus(pipe);
        if (!sink || !bus) return fail("pipeline has no sink");
        if (hasLocation) { // set as a property: any path works, no quoting in the description
            void* src = g.gst_bin_get_by_name(pipe, "fusesrc");
            if (!src) return fail("no source element");
            g.g_object_set(src, "location", location.c_str(), static_cast<void*>(nullptr));
            g.gst_object_unref(src);
        }
        if (g.gst_element_set_state(pipe, kStatePlaying) == kFailure) return fail("could not start the decoder (missing or unsupported file)");
        return true;
    }
    bool start(const std::string& d, const std::string* loc, std::string* err) {
        auto fail = [&](std::string m) { if (err) *err = std::move(m); return false; };
        desc = d, hasLocation = loc != nullptr, location = loc ? *loc : std::string();
        ownSink = d.find("filesink") != std::string::npos || d.find("multifilesink") != std::string::npos;
        if (!launch(err)) return false;
        pending = pull();
        if (!pending) {
            const std::string e = busError();
            return fail(e.empty() ? (kind == StreamKind::Video ? "no video frames" : "no audio") : e);
        }
        void* st = g.gst_caps_get_structure(g.gst_sample_get_caps(pending), 0);
        int a = 0, b = 0, fn = 0, fd = 1;
        if (kind == StreamKind::Video) {
            g.gst_structure_get_int(st, "width", &a);
            g.gst_structure_get_int(st, "height", &b);
            si.width = u32(std::max(a, 0)), si.height = u32(std::max(b, 0));
            si.fps = g.gst_structure_get_fraction(st, "framerate", &fn, &fd) && fn > 0 && fd > 0 ? f32(fn) / f32(fd) : 30.f;
            if (!si.width || !si.height) return fail("bad video caps");
        } else {
            g.gst_structure_get_int(st, "rate", &a);
            g.gst_structure_get_int(st, "channels", &b);
            si.rate = u32(std::max(a, 0)), si.channels = u32(std::max(b, 0));
            if (!si.rate || !si.channels) return fail("bad audio caps");
        }
        long long dur = -1;
        // the duration is known once prerolled; wait for the state change to settle (bounded)
        int cur = 0, pend = 0;
        g.gst_element_get_state(pipe, &cur, &pend, 2'000'000'000ull);
        if (g.gst_element_query_duration(pipe, kFormatTime, &dur) && dur > 0) si.duration = double(dur) / 1e9;
        if (kind == StreamKind::Video) {
            if (si.duration > 0.0) si.frames = u32(std::llround(si.duration * double(si.fps)));
            else { // unknown length (live-style sources): count once, keep nothing
                u32 n = 0, idx = 0;
                scratch.resize(std::size_t(si.width) * si.height);
                while (readFrame(scratch.data(), idx)) n = std::max(n, idx + 1);
                si.frames = n;
                si.duration = double(n) / double(si.fps);
                si.atEnd = true; // no rewind here: a pipeline with side effects (a file sink) runs exactly once
            }
            if (si.frames == 0) return fail("no video frames");
        }
        return true;
    }
    StreamInfo info() const override { return si; }
    void* pullAfterSeek() {
        void* s = pending ? pending : pull();
        pending = nullptr;
        if (!s && lastSeek >= 0.0) { // a source that already ran out ignores seeks: restart it and seek again
            const double t = lastSeek;
            ended = true;
            if (seekTime(t)) s = pull();
        }
        lastSeek = -1.0;
        return s;
    }
    bool seekTime(double t) {
        if (pending) g.gst_mini_object_unref(pending), pending = nullptr;
        if (ended) { // restart rather than seek a finished pipeline
            const bool own = ownSink;
            ownSink = false; // do not drain while restarting
            teardown();
            ownSink = own;
            if (!launch(nullptr)) return false;
            int cur = 0, pend = 0;
            g.gst_element_get_state(pipe, &cur, &pend, 2'000'000'000ull);
            if (t <= 0.0) return true;
        }
        const long long ns = std::llround(std::max(0.0, t) * 1e9);
        // drop a stale EOS from the previous pass, or the next pull would stop at once
        while (void* msg = g.gst_bus_timed_pop_filtered(bus, 0, kMsgEos)) g.gst_mini_object_unref(msg);
        if (!g.gst_element_seek_simple(pipe, kFormatTime, kSeekFlush | kSeekAccurate, ns)) return false;
        int cur = 0, pend = 0;
        g.gst_element_get_state(pipe, &cur, &pend, 2'000'000'000ull);
        lastSeek = ended ? -1.0 : t;
        return true;
    }
    bool seekFrame(u32 frame) override { return seekTime((double(frame) + 0.25) / double(si.fps)); }
    bool seekSeconds(double s) override { return seekTime(s); }
    bool readFrame(u32* rgba, u32& index) override {
        void* sample = pullAfterSeek();
        if (!sample) return false;
        bool ok = false;
        void* buf = g.gst_sample_get_buffer(sample);
        GstMapInfo mi{};
        if (buf && g.gst_buffer_map(buf, &mi, 1 /*READ*/)) {
            const std::size_t stride = mi.size / si.height;
            if (stride >= std::size_t(si.width) * 4) {
                for (u32 y = 0; y < si.height; ++y) std::memcpy(rgba + std::size_t(y) * si.width, mi.data + y * stride, std::size_t(si.width) * 4);
                for (std::size_t i = 0, n = std::size_t(si.width) * si.height; i < n; ++i) rgba[i] |= 0xFF000000u;
                u64 pts = 0;
                std::memcpy(&pts, static_cast<const u8*>(buf) + kBufferPtsOffset, sizeof pts);
                index = pts == kNone ? 0u : u32(std::llround(double(pts) * double(si.fps) / 1e9));
                ok = true;
            }
            g.gst_buffer_unmap(buf, &mi);
        }
        g.gst_mini_object_unref(sample);
        return ok;
    }
    std::size_t readAudio(std::vector<f32>& out) override {
        void* sample = pullAfterSeek();
        if (!sample) return 0;
        std::size_t n = 0;
        void* buf = g.gst_sample_get_buffer(sample);
        GstMapInfo mi{};
        if (buf && g.gst_buffer_map(buf, &mi, 1)) {
            n = mi.size / sizeof(f32);
            const std::size_t at = out.size();
            out.resize(at + n);
            std::memcpy(out.data() + at, mi.data, n * sizeof(f32));
            g.gst_buffer_unmap(buf, &mi);
        }
        g.gst_mini_object_unref(sample);
        return n ? n : 1; // an empty buffer is not the end of the stream
    }
};
} // namespace

bool backendAvailable() { return gst().ok; }
std::string backendName() {
    if (!gst().ok) return {};
    u32 a = 0, b = 0, c = 0, d = 0;
    gst().gst_version(&a, &b, &c, &d);
    char s[48];
    std::snprintf(s, sizeof s, "gstreamer %u.%u.%u", a, b, c);
    return s;
}
std::unique_ptr<Decoder> openFile(const std::string& path, StreamKind kind, std::string* error) {
    if (!gst().ok) {
        if (error) *error = "GStreamer (core, app, decodebin, convert elements) is not available";
        return nullptr;
    }
    auto d = std::make_unique<GstDecoder>(kind);
    if (!d->start(std::string("filesrc name=fusesrc ! decodebin") + tail(kind), &path, error)) return nullptr;
    return d;
}
std::unique_ptr<Decoder> openPipeline(const std::string& desc, StreamKind kind, std::string* error) {
    if (!gst().ok) {
        if (error) *error = "GStreamer is not available";
        return nullptr;
    }
    auto d = std::make_unique<GstDecoder>(kind);
    if (!d->start(desc + tail(kind), nullptr, error)) return nullptr;
    return d;
}

} // namespace fuse::ui::media::detail
#endif
