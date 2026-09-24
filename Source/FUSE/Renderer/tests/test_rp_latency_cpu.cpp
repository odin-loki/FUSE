// WP-4.4 CPU gates for the latency providers and the present-timing model (stub tree too; no GPU, no display).
//
//   pacing      present-timing model: FIFO 60 Hz with a fast GPU -> every display interval is one refresh (1 bucket);
//               GPU slower than the refresh on FIFO -> the documented 16.7 / 33.3 ms judder (2 buckets); VRR -> the
//               render interval (1 bucket); mailbox -> refresh cadence with dropped frames and lower latency than
//               FIFO; frame limiter; FSR-style 2x interpolation on VRR -> displayed fps = 2x render fps, generated
//               and real images evenly paced (<= 2 one-millisecond buckets, the MetalFX guidance), also with noise.
//   latency     latency bounds with and without a pacing latency provider (recording mock, LowLatency): GPU-bound
//               VRR (CPU run-ahead removed: latency <= CPU + GPU + margin, throughput unchanged), FIFO with a fast
//               GPU (swapchain queueing removed), CPU-bound (no change), frame limiter under the provider, frame
//               generation adds between the FG cost and one rendered frame + FG cost (research §5 accounting).
//   markers     every frame: sleep then InputSample, SimulationStart, SimulationEnd, RenderSubmitStart,
//               RenderSubmitEnd, PresentStart, PresentEnd in order; timestamps monotonic
//               (input <= sim <= submit <= GPU <= present <= display); deterministic (two runs bit-identical).
//   reflex      NvReflexPluginLatencyProvider over an in-process mock of the plugin latency extension ABI
//               (fuse_nv_latency_abi.h): resolve (good table / ABI major mismatch / truncated / null entries / no
//               export), settings -> FuseNvLatencyOptions (mode, boost, limiter interval), sleep / marker ids / frame
//               ids pass through, reports converted, provider status errors reported.
//   low_latency2  NvLowLatency2Provider over a mock VK_NV_low_latency2 dispatch: sleep-mode arguments, one
//               increasing timeline value per sleep, presentID = frame + 1 on markers, timings mapped back.
//   anti_lag    AmdAntiLagProvider over a mock vkAntiLagUpdateAMD: mode / maxFPS mapping, INPUT stage at sleep with
//               the frame index, PRESENT stage at PresentStart only.
//   selection   select_latency_backend (auto order, explicit preferences, reasons); create_latency_provider without
//               sources -> None; the stub / Lavapipe bindings report the extensions unavailable.
#include <fuse/renderer/present/latency/latency_provider.hpp>
#include <fuse/renderer/present/latency/present_timing.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace fuse::renderer::present;
using fuse::f32;
using fuse::f64;
using fuse::i32;
using fuse::u32;
using fuse::u64;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

bool near(f64 a, f64 b, f64 tol) { return std::fabs(a - b) <= tol; }

/// Pacing mock: behaves like a Reflex-class provider (pacing, limiter) and records every call.
class RecordingProvider final : public ILatencyProvider {
public:
    struct Call {
        u64 frame;
        int what; ///< -1 sleep, else LatencyMarker
    };
    explicit RecordingProvider(bool paces) : m_paces(paces) {}
    LatencyBackend backend() const override { return LatencyBackend::None; }
    const char* name() const override { return "recording_mock"; }
    LatencyCaps caps() const override {
        LatencyCaps c{};
        c.low_latency = m_paces;
        c.markers = true;
        c.fps_limit = true;
        return c;
    }
    bool setSettings(const LatencySettings& s) override {
        m_settings = s;
        return true;
    }
    LatencySettings settings() const override { return m_settings; }
    void sleep(u64 frameId) override { calls.push_back({frameId, -1}); }
    void marker(u64 frameId, LatencyMarker m) override { calls.push_back({frameId, static_cast<int>(m)}); }
    u32 timings(LatencyFrameReport*, u32) override { return 0u; }
    std::vector<Call> calls;

private:
    bool m_paces;
    LatencySettings m_settings{};
};

void printStats(const char* label, const PresentTimingResult& r) {
    const PresentTimingStats& s = r.stats;
    std::printf("  %-44s render %6.2f fps  displayed %6.2f fps  latency mean %6.2f p95 %6.2f max %6.2f ms  "
                "interval %6.3f +- %5.3f [%6.3f, %6.3f] ms  buckets %u  dropped %u  generated %u  sleep %5.2f ms\n",
                label, s.render_fps, s.displayed_fps, s.latency_mean_ms, s.latency_p95_ms, s.latency_max_ms, s.interval_mean_ms,
                s.interval_stddev_ms, s.interval_min_ms, s.interval_max_ms, s.pacing_buckets, s.dropped, s.generated, s.sleep_mean_ms);
}

PresentTimingConfig base() {
    PresentTimingConfig c{};
    c.frames = 240;
    c.warmup_frames = 24;
    return c;
}

// ---- pacing ---------------------------------------------------------------------------------------------------
int runPacing() {
    std::printf("pacing:\n");
    {
        PresentTimingConfig c = base();
        c.mode = PresentMode::Fifo;
        c.cpu_sim_ms = 3.0;
        c.cpu_render_ms = 2.0;
        c.gpu_ms = 10.0;
        const PresentTimingResult r = simulate_present_timing(c);
        printStats("FIFO 60 Hz, GPU 10 ms", r);
        expect(r.stats.pacing_buckets == 1u, "FIFO fast GPU: one pacing bucket");
        expect(near(r.stats.interval_min_ms, 1000.0 / 60.0, 1e-6) && near(r.stats.interval_max_ms, 1000.0 / 60.0, 1e-6),
               "FIFO fast GPU: every interval = 1 refresh");
        expect(near(r.stats.displayed_fps, 60.0, 0.01), "FIFO fast GPU: 60 displayed fps");
        expect(r.stats.dropped == 0u, "FIFO drops nothing");
    }
    {
        PresentTimingConfig c = base();
        c.mode = PresentMode::Fifo;
        c.gpu_ms = 20.0;
        c.cpu_sim_ms = 3.0;
        const PresentTimingResult r = simulate_present_timing(c);
        printStats("FIFO 60 Hz, GPU 20 ms (judder)", r);
        expect(r.stats.pacing_buckets == 2u, "FIFO slow GPU: 16.7 / 33.3 ms judder = 2 buckets");
        expect(near(r.stats.render_fps, 50.0, 0.05), "FIFO slow GPU: 50 rendered fps");
    }
    {
        PresentTimingConfig c = base();
        c.mode = PresentMode::Vrr;
        c.refresh_hz = 144.0;
        c.gpu_ms = 20.0;
        c.cpu_sim_ms = 3.0;
        const PresentTimingResult r = simulate_present_timing(c);
        printStats("VRR 144 Hz, GPU 20 ms", r);
        expect(r.stats.pacing_buckets == 1u && near(r.stats.interval_mean_ms, 20.0, 1e-6), "VRR: intervals = render interval");
    }
    f64 fifoLatency = 0.0;
    {
        PresentTimingConfig c = base();
        c.mode = PresentMode::Fifo;
        c.gpu_ms = 5.0;
        c.cpu_sim_ms = 2.0;
        c.cpu_render_ms = 1.0;
        fifoLatency = simulate_present_timing(c).stats.latency_mean_ms;
        c.mode = PresentMode::Mailbox;
        const PresentTimingResult r = simulate_present_timing(c);
        printStats("mailbox 60 Hz, GPU 5 ms", r);
        expect(near(r.stats.displayed_fps, 60.0, 0.01) && r.stats.pacing_buckets == 1u, "mailbox: refresh cadence");
        expect(r.stats.dropped > 0u && r.stats.render_fps > 1.5 * 60.0, "mailbox: renders ahead of the refresh and drops");
        expect(r.stats.latency_mean_ms < fifoLatency, "mailbox latency < FIFO latency");
    }
    {
        RecordingProvider limiter(false);
        LatencySettings s{};
        s.fps_limit = 40.f;
        limiter.setSettings(s);
        PresentTimingConfig c = base();
        c.mode = PresentMode::Vrr;
        c.refresh_hz = 144.0;
        c.gpu_ms = 8.0;
        c.cpu_sim_ms = 3.0;
        c.provider = &limiter;
        const PresentTimingResult r = simulate_present_timing(c);
        printStats("VRR, frame limiter 40 fps", r);
        expect(near(r.stats.render_fps, 40.0, 0.01) && near(r.stats.interval_mean_ms, 25.0, 1e-6) && r.stats.pacing_buckets == 1u,
               "limiter: 25 ms cadence");
    }
    // Frame generation (2x interpolation) on VRR.
    for (int noise = 0; noise < 2; ++noise) {
        PresentTimingConfig c = base();
        c.mode = PresentMode::Vrr;
        c.refresh_hz = 240.0;
        c.gpu_ms = 16.0;
        c.cpu_sim_ms = 4.0;
        c.cpu_render_ms = 2.0;
        c.frame_gen = FrameGenMode::Interpolate2x;
        c.fg_gpu_ms = 1.5;
        if (noise != 0) {
            c.gpu_jitter_ms = 0.4;
            c.cpu_jitter_ms = 0.5;
        }
        const PresentTimingResult r = simulate_present_timing(c);
        printStats(noise != 0 ? "VRR 240 Hz, FG 2x, GPU 16 +- 0.4 ms" : "VRR 240 Hz, FG 2x, GPU 16 ms", r);
        expect(near(r.stats.displayed_fps, 2.0 * r.stats.render_fps, 0.02 * r.stats.render_fps), "FG: displayed = 2x rendered fps");
        expect(r.stats.pacing_buckets <= 2u, "FG: <= 2 pacing buckets");
        expect(r.stats.interval_stddev_ms <= (noise != 0 ? 0.5 : 1e-3), "FG: even pacing (generated at the midpoint)");
        expect(r.stats.generated >= c.frames - c.warmup_frames - 1u, "FG: one generated image per rendered frame");
    }
    return 0;
}

// ---- latency --------------------------------------------------------------------------------------------------
PresentTimingResult runWith(PresentTimingConfig c, bool withProvider, RecordingProvider* p) {
    if (withProvider) {
        c.provider = p;
    }
    return simulate_present_timing(c);
}

int runLatency() {
    std::printf("latency (with / without a pacing provider):\n");
    RecordingProvider provider(true);
    LatencySettings on{};
    on.mode = LatencyMode::LowLatency;
    provider.setSettings(on);
    {
        PresentTimingConfig c = base();
        c.mode = PresentMode::Vrr;
        c.refresh_hz = 240.0;
        c.cpu_sim_ms = 2.0;
        c.cpu_render_ms = 1.0;
        c.gpu_ms = 12.0;
        c.max_frames_in_flight = 2;
        const PresentTimingResult off = runWith(c, false, &provider);
        const PresentTimingResult pace = runWith(c, true, &provider);
        printStats("GPU-bound VRR, no provider", off);
        printStats("GPU-bound VRR, provider", pace);
        const f64 bound = c.cpu_sim_ms + c.cpu_render_ms + c.gpu_ms + c.latency_margin_ms + 0.5;
        expect(pace.stats.latency_max_ms <= bound, "GPU-bound: provider latency <= CPU + GPU + margin");
        // Without the provider the CPU runs max_frames_in_flight GPU frames ahead: latency = frames in flight x GPU.
        expect(off.stats.latency_mean_ms >= c.max_frames_in_flight * c.gpu_ms - 0.5, "GPU-bound: run-ahead queue without provider");
        expect(off.stats.latency_mean_ms - pace.stats.latency_mean_ms >=
                   c.gpu_ms - (c.cpu_sim_ms + c.cpu_render_ms) - c.latency_margin_ms - 0.5,
               "GPU-bound: provider removes the queued frame");
        expect(near(pace.stats.render_fps, off.stats.render_fps, 0.01 * off.stats.render_fps), "GPU-bound: throughput unchanged");
    }
    {
        PresentTimingConfig c = base();
        c.mode = PresentMode::Fifo;
        c.cpu_sim_ms = 2.0;
        c.cpu_render_ms = 1.0;
        c.gpu_ms = 4.0;
        const PresentTimingResult off = runWith(c, false, &provider);
        const PresentTimingResult pace = runWith(c, true, &provider);
        printStats("FIFO 60 Hz fast GPU, no provider", off);
        printStats("FIFO 60 Hz fast GPU, provider", pace);
        expect(off.stats.latency_mean_ms >= 2.0 * 1000.0 / 60.0, "FIFO: swapchain queueing without provider >= 2 refreshes");
        expect(pace.stats.latency_max_ms <= c.cpu_sim_ms + c.cpu_render_ms + c.gpu_ms + c.latency_margin_ms + 0.5,
               "FIFO: provider latency <= CPU + GPU + margin");
        expect(near(pace.stats.displayed_fps, 60.0, 0.01) && pace.stats.pacing_buckets == 1u, "FIFO: provider keeps 60 fps pacing");
    }
    {
        PresentTimingConfig c = base();
        c.mode = PresentMode::Vrr;
        c.refresh_hz = 240.0;
        c.cpu_sim_ms = 9.0;
        c.cpu_render_ms = 3.0;
        c.gpu_ms = 4.0;
        const PresentTimingResult off = runWith(c, false, &provider);
        const PresentTimingResult pace = runWith(c, true, &provider);
        printStats("CPU-bound VRR, no provider", off);
        printStats("CPU-bound VRR, provider", pace);
        expect(near(off.stats.latency_mean_ms, pace.stats.latency_mean_ms, 0.1) && near(off.stats.render_fps, pace.stats.render_fps, 0.01),
               "CPU-bound: provider changes nothing");
        expect(near(pace.stats.latency_mean_ms, c.cpu_sim_ms + c.cpu_render_ms + c.gpu_ms, 1e-6), "CPU-bound: latency = CPU + GPU");
    }
    {
        RecordingProvider limited(true);
        LatencySettings s{};
        s.mode = LatencyMode::LowLatency;
        s.fps_limit = 50.f;
        limited.setSettings(s);
        PresentTimingConfig c = base();
        c.mode = PresentMode::Vrr;
        c.refresh_hz = 240.0;
        c.cpu_sim_ms = 3.0;
        c.gpu_ms = 10.0;
        c.provider = &limited;
        const PresentTimingResult r = simulate_present_timing(c);
        printStats("VRR, provider + limiter 50 fps", r);
        expect(near(r.stats.render_fps, 50.0, 0.01), "limiter + provider: 50 fps");
        expect(r.stats.latency_max_ms <= c.cpu_sim_ms + c.cpu_render_ms + c.gpu_ms + 0.5, "limiter sleeps before input: latency = CPU + GPU");
    }
    {
        PresentTimingConfig c = base();
        c.mode = PresentMode::Vrr;
        c.refresh_hz = 240.0;
        c.cpu_sim_ms = 4.0;
        c.cpu_render_ms = 2.0;
        c.gpu_ms = 16.0;
        const PresentTimingResult noFg = runWith(c, true, &provider);
        c.frame_gen = FrameGenMode::Interpolate2x;
        c.fg_gpu_ms = 1.5;
        const PresentTimingResult fgPace = runWith(c, true, &provider);
        const PresentTimingResult fgOff = runWith(c, false, &provider);
        printStats("VRR GPU 16 ms, provider, no FG", noFg);
        printStats("VRR GPU 16 ms, provider, FG 2x", fgPace);
        printStats("VRR GPU 16 ms, no provider, FG 2x", fgOff);
        const f64 frame = 1000.0 / fgPace.stats.render_fps;
        const f64 added = fgPace.stats.latency_mean_ms - noFg.stats.latency_mean_ms;
        std::printf("  FG added latency %.3f ms (rendered frame %.3f ms, FG cost %.3f ms; accounting bound [FG, frame + FG])\n", added,
                    frame, c.fg_gpu_ms);
        expect(added >= c.fg_gpu_ms - 1e-6 && added <= frame + c.fg_gpu_ms + 0.5, "FG adds between the FG cost and one frame + FG cost");
        expect(fgOff.stats.latency_mean_ms > fgPace.stats.latency_mean_ms + 0.5 * c.gpu_ms, "FG: provider still removes queueing");
        expect(near(fgPace.stats.displayed_fps, 2.0 * fgPace.stats.render_fps, 0.02 * fgPace.stats.render_fps), "FG: displayed 2x");
    }
    return 0;
}

// ---- markers / determinism ------------------------------------------------------------------------------------
int runMarkers() {
    RecordingProvider provider(true);
    LatencySettings on{};
    on.mode = LatencyMode::LowLatency;
    provider.setSettings(on);
    PresentTimingConfig c = base();
    c.frames = 120;
    c.mode = PresentMode::Fifo;
    c.gpu_ms = 9.0;
    c.gpu_jitter_ms = 1.5;
    c.cpu_jitter_ms = 1.0;
    c.provider = &provider;
    const PresentTimingResult r = simulate_present_timing(c);
    const int order[] = {-1,
                         static_cast<int>(LatencyMarker::InputSample),
                         static_cast<int>(LatencyMarker::SimulationStart),
                         static_cast<int>(LatencyMarker::SimulationEnd),
                         static_cast<int>(LatencyMarker::RenderSubmitStart),
                         static_cast<int>(LatencyMarker::RenderSubmitEnd),
                         static_cast<int>(LatencyMarker::PresentStart),
                         static_cast<int>(LatencyMarker::PresentEnd)};
    const u32 per = static_cast<u32>(sizeof(order) / sizeof(order[0]));
    bool orderOk = provider.calls.size() == static_cast<size_t>(c.frames) * per;
    for (size_t k = 0; orderOk && k < provider.calls.size(); ++k) {
        orderOk = provider.calls[k].frame == k / per && provider.calls[k].what == order[k % per];
    }
    expect(orderOk, "markers: sleep + 7 markers per frame, in protocol order");
    bool mono = true;
    for (const SimFrame& f : r.frames) {
        mono = mono && f.input <= f.sim_start && f.sim_start <= f.sim_end && f.sim_end <= f.submit_start && f.submit_start <= f.submit_end &&
               f.submit_end <= f.gpu_start && f.gpu_start < f.gpu_end && f.gpu_end <= f.present && f.present <= f.display &&
               f.natural_start <= f.sim_start && near(f.latency_ms, f.display - f.input, 1e-9);
    }
    expect(mono, "timestamps monotonic per frame");
    const PresentTimingResult again = simulate_present_timing(c);
    bool same = again.frames.size() == r.frames.size() && again.display_times == r.display_times;
    for (size_t k = 0; same && k < r.frames.size(); ++k) {
        same = std::memcmp(&again.frames[k], &r.frames[k], sizeof(SimFrame)) == 0;
    }
    expect(same, "simulation deterministic (bit-identical rerun)");
    std::printf("markers: %zu provider calls over %u frames, ordered; timestamps monotonic; rerun bit-identical\n",
                provider.calls.size(), c.frames);
    return 0;
}

// ---- Reflex plugin mock ---------------------------------------------------------------------------------------
struct ReflexMock {
    FuseNvLatencyOptions options{};
    u32 optionCalls = 0;
    std::vector<u64> sleeps;
    std::vector<std::pair<u64, u32>> markers;
    FuseNvStatus failWith = FUSE_NV_OK;
    int variant = 0; ///< 0 good, 1 wrong major, 2 truncated, 3 null entry
} g_reflex;
FuseNvContext* const kMockContext = reinterpret_cast<FuseNvContext*>(static_cast<uintptr_t>(0x1234));

FuseNvStatus mockSetOptions(FuseNvContext* ctx, const FuseNvLatencyOptions* o) {
    if (ctx != kMockContext || o == nullptr || o->struct_size != sizeof(FuseNvLatencyOptions)) {
        return FUSE_NV_ERR_INVALID_ARGUMENT;
    }
    g_reflex.options = *o;
    ++g_reflex.optionCalls;
    return g_reflex.failWith;
}
FuseNvStatus mockSleep(FuseNvContext* ctx, uint64_t frame) {
    g_reflex.sleeps.push_back(frame);
    return ctx == kMockContext ? FUSE_NV_OK : FUSE_NV_ERR_INVALID_ARGUMENT;
}
FuseNvStatus mockMarker(FuseNvContext*, uint64_t frame, FuseNvLatencyMarker marker) {
    g_reflex.markers.emplace_back(frame, marker);
    return FUSE_NV_OK;
}
FuseNvStatus mockReports(FuseNvContext*, FuseNvLatencyFrameReport* out, uint32_t capacity, uint32_t* count) {
    const uint32_t n = capacity < 3u ? capacity : 3u;
    for (uint32_t i = 0; i < n; ++i) {
        if (out[i].struct_size != sizeof(FuseNvLatencyFrameReport)) {
            return FUSE_NV_ERR_INVALID_ARGUMENT;
        }
        out[i].frame_id = 100u + i;
        out[i].input_sample_us = 1000u * i;
        out[i].sim_start_us = 1000u * i + 10u;
        out[i].gpu_render_end_us = 1000u * i + 9000u;
    }
    *count = n;
    return FUSE_NV_OK;
}
FuseNvStatus mockGetApi(uint32_t hostAbi, FuseNvLatencyApi* api) {
    if (FUSE_NV_PLUGIN_ABI_MAJOR_OF(hostAbi) != FUSE_NV_LATENCY_ABI_MAJOR) {
        return FUSE_NV_ERR_ABI_MISMATCH;
    }
    const uint32_t hostSize = api->struct_size;
    *api = FuseNvLatencyApi{};
    api->struct_size = hostSize < sizeof(FuseNvLatencyApi) ? hostSize : static_cast<uint32_t>(sizeof(FuseNvLatencyApi));
    api->abi_version = FUSE_NV_LATENCY_ABI_VERSION;
    api->set_options = &mockSetOptions;
    api->sleep = &mockSleep;
    api->set_marker = &mockMarker;
    api->get_reports = &mockReports;
    if (g_reflex.variant == 1) {
        api->abi_version = (2u << 16);
    } else if (g_reflex.variant == 2) {
        api->struct_size = static_cast<uint32_t>(offsetof(FuseNvLatencyApi, get_reports));
    } else if (g_reflex.variant == 3) {
        api->sleep = nullptr;
    }
    return FUSE_NV_OK;
}

int runReflex() {
    FuseNvLatencyApi api{};
    std::string reason;
    expect(!resolve_nv_latency_api(static_cast<FuseNvPluginGetLatencyApiFn>(nullptr), api, reason), "reflex: no export -> unavailable");
    std::printf("reflex: no export: %s\n", reason.c_str());
    for (int v = 1; v <= 3; ++v) {
        g_reflex.variant = v;
        expect(!resolve_nv_latency_api(&mockGetApi, api, reason), "reflex: bad table rejected");
        std::printf("reflex: variant %d rejected: %s\n", v, reason.c_str());
    }
    g_reflex.variant = 0;
    expect(resolve_nv_latency_api(&mockGetApi, api, reason) && api.sleep == &mockSleep, "reflex: good table resolves");

    NvReflexPluginLatencyProvider p(api, kMockContext);
    expect(p.backend() == LatencyBackend::NvReflexPlugin && p.caps().low_latency && p.caps().boost && p.caps().reports, "reflex caps");
    LatencySettings s{};
    s.mode = LatencyMode::LowLatencyBoost;
    s.fps_limit = 120.f;
    expect(p.setSettings(s), "reflex: settings accepted");
    expect(g_reflex.options.low_latency == 1u && g_reflex.options.boost == 1u && g_reflex.options.minimum_interval_us == 8333u &&
               g_reflex.options.use_markers_to_optimize == 1u,
           "reflex: options mapped (on + boost, 120 fps -> 8333 us)");
    s.mode = LatencyMode::Off;
    s.fps_limit = 0.f;
    expect(p.setSettings(s) && g_reflex.options.low_latency == 0u && g_reflex.options.boost == 0u && g_reflex.options.minimum_interval_us == 0u,
           "reflex: off");
    LatencySettings bad{};
    bad.fps_limit = -1.f;
    const u32 callsBefore = g_reflex.optionCalls;
    expect(!p.setSettings(bad) && g_reflex.optionCalls == callsBefore, "reflex: invalid settings rejected before the ABI");
    g_reflex.failWith = FUSE_NV_ERR_RUNTIME_FAILURE;
    LatencySettings on{};
    on.mode = LatencyMode::LowLatency;
    expect(!p.setSettings(on) && p.lastStatus() == FUSE_NV_ERR_RUNTIME_FAILURE && p.settings().mode == LatencyMode::Off,
           "reflex: provider error keeps the old settings");
    g_reflex.failWith = FUSE_NV_OK;
    expect(p.setSettings(on), "reflex: on");

    // Drive 4 frames through the simulation: every call reaches the ABI with the right ids.
    PresentTimingConfig c = base();
    c.frames = 4;
    c.warmup_frames = 0;
    c.provider = &p;
    simulate_present_timing(c);
    expect(g_reflex.sleeps.size() == 4u && g_reflex.sleeps[3] == 3u, "reflex: one sleep per frame, frame ids");
    bool markersOk = g_reflex.markers.size() == 28u;
    for (size_t k = 0; markersOk && k < g_reflex.markers.size(); ++k) {
        markersOk = g_reflex.markers[k].first == k / 7u && g_reflex.markers[k].second < FUSE_NV_LATENCY_MARKER_COUNT;
    }
    expect(markersOk && g_reflex.markers[1].second == FUSE_NV_LATENCY_MARKER_SIMULATION_START &&
               g_reflex.markers[0].second == FUSE_NV_LATENCY_MARKER_INPUT_SAMPLE &&
               g_reflex.markers[6].second == FUSE_NV_LATENCY_MARKER_PRESENT_END,
           "reflex: markers = VkLatencyMarkerNV / PCL ids");
    LatencyFrameReport reports[8];
    const u32 n = p.timings(reports, 8u);
    expect(n == 3u && reports[2].frame_id == 102u && reports[1].sim_start_us == 1010u && reports[2].input_to_gpu_end_us() == 9000u,
           "reflex: reports converted");
    std::printf("reflex: options / sleep / %zu markers / %u reports through the mock ABI\n", g_reflex.markers.size(), n);
    return 0;
}

// ---- VK_NV_low_latency2 mock -----------------------------------------------------------------------------------
struct Ll2Mock {
    bool low = false, boost = false;
    u32 interval = 0;
    u32 modeCalls = 0;
    std::vector<u64> sleepValues;
    std::vector<std::pair<u64, u32>> markers;
} g_ll2;

int runLowLatency2() {
    LowLatency2Dispatch d{};
    d.user = &g_ll2;
    d.set_sleep_mode = [](void* user, bool low, bool boost, u32 interval) -> i32 {
        Ll2Mock& m = *static_cast<Ll2Mock*>(user);
        m.low = low;
        m.boost = boost;
        m.interval = interval;
        ++m.modeCalls;
        return 0;
    };
    d.sleep = [](void* user, u64 value) -> i32 {
        static_cast<Ll2Mock*>(user)->sleepValues.push_back(value);
        return 0;
    };
    d.set_marker = [](void* user, u64 presentId, u32 marker) { static_cast<Ll2Mock*>(user)->markers.emplace_back(presentId, marker); };
    d.get_timings = [](void*, LatencyFrameReport* out, u32 capacity) -> u32 {
        const u32 n = capacity < 2u ? capacity : 2u;
        for (u32 i = 0; i < n; ++i) {
            out[i] = LatencyFrameReport{};
            out[i].frame_id = 5u + i; // presentID
            out[i].gpu_render_end_us = 777u;
        }
        return n;
    };
    NvLowLatency2Provider p(d);
    LatencySettings s{};
    s.mode = LatencyMode::LowLatency;
    s.fps_limit = 60.f;
    expect(p.setSettings(s) && g_ll2.low && !g_ll2.boost && g_ll2.interval == 16667u, "low_latency2: sleep mode on, 60 fps -> 16667 us");
    s.mode = LatencyMode::LowLatencyBoost;
    s.fps_limit = 0.f;
    expect(p.setSettings(s) && g_ll2.low && g_ll2.boost && g_ll2.interval == 0u, "low_latency2: boost, no limit");
    PresentTimingConfig c = base();
    c.frames = 5;
    c.warmup_frames = 0;
    c.provider = &p;
    simulate_present_timing(c);
    bool increasing = g_ll2.sleepValues.size() == 5u;
    for (size_t k = 0; increasing && k < g_ll2.sleepValues.size(); ++k) {
        increasing = g_ll2.sleepValues[k] == k + 1u;
    }
    expect(increasing && p.lastSleepValue() == 5u, "low_latency2: one increasing timeline value per vkLatencySleepNV");
    bool ids = g_ll2.markers.size() == 35u;
    for (size_t k = 0; ids && k < g_ll2.markers.size(); ++k) {
        ids = g_ll2.markers[k].first == k / 7u + 1u;
    }
    expect(ids && g_ll2.markers[2].second == 1u /*SIMULATION_END*/, "low_latency2: presentID = frame + 1 on every marker");
    expect(NvLowLatency2Provider::present_id(0) == 1u, "low_latency2: presentID never 0");
    LatencyFrameReport r[4];
    const u32 n = p.timings(r, 4u);
    expect(n == 2u && r[0].frame_id == 4u && r[1].frame_id == 5u && r[1].gpu_render_end_us == 777u, "low_latency2: presentID -> frame id");
    LowLatency2Dispatch empty{};
    NvLowLatency2Provider q(empty);
    expect(!q.setSettings(s), "low_latency2: empty dispatch rejects settings");
    q.sleep(0);
    q.marker(0, LatencyMarker::SimulationStart);
    std::printf("low_latency2: %u sleep-mode calls, %zu sleeps, %zu markers, %u reports through the mock dispatch\n", g_ll2.modeCalls,
                g_ll2.sleepValues.size(), g_ll2.markers.size(), n);
    return 0;
}

// ---- VK_AMD_anti_lag mock --------------------------------------------------------------------------------------
std::vector<AntiLagUpdate> g_antiLag;

int runAntiLag() {
    AntiLagDispatch d{};
    d.update = [](void*, const AntiLagUpdate& u) { g_antiLag.push_back(u); };
    AmdAntiLagProvider p(d);
    expect(p.caps().low_latency && !p.caps().boost && !p.caps().reports, "anti_lag caps");
    LatencySettings s{};
    s.mode = LatencyMode::LowLatencyBoost;
    s.fps_limit = 72.f;
    expect(p.setSettings(s), "anti_lag: settings");
    expect(g_antiLag.size() == 1u && g_antiLag[0].mode == kAntiLagModeOn && g_antiLag[0].max_fps == 72u && !g_antiLag[0].has_presentation,
           "anti_lag: mode applied at once (boost -> ON), maxFPS");
    PresentTimingConfig c = base();
    c.frames = 3;
    c.warmup_frames = 0;
    c.provider = &p;
    simulate_present_timing(c);
    // Per frame: INPUT at sleep, PRESENT at PresentStart.
    bool ok = g_antiLag.size() == 7u;
    for (u32 f = 0; ok && f < 3u; ++f) {
        const AntiLagUpdate& in = g_antiLag[1u + 2u * f];
        const AntiLagUpdate& pr = g_antiLag[2u + 2u * f];
        ok = in.has_presentation && in.stage == kAntiLagStageInput && in.frame_index == f && pr.has_presentation &&
             pr.stage == kAntiLagStagePresent && pr.frame_index == f && in.mode == kAntiLagModeOn;
    }
    expect(ok, "anti_lag: INPUT at sleep, PRESENT at PresentStart, frame index");
    s.mode = LatencyMode::Off;
    s.fps_limit = 0.f;
    p.setSettings(s);
    expect(g_antiLag.back().mode == kAntiLagModeOff && g_antiLag.back().max_fps == 0u, "anti_lag: off");
    std::printf("anti_lag: %zu vkAntiLagUpdateAMD calls through the mock\n", g_antiLag.size());
    return 0;
}

// ---- selection ---------------------------------------------------------------------------------------------------
int runSelection() {
    const char* why = nullptr;
    LatencyAvailability none{};
    expect(select_latency_backend(LatencyPreference::Auto, none, &why) == LatencyBackend::None, "auto, nothing -> none");
    LatencyAvailability all{true, true, true};
    expect(select_latency_backend(LatencyPreference::Auto, all, &why) == LatencyBackend::NvReflexPlugin, "auto prefers the Reflex plugin");
    LatencyAvailability vk{false, true, true};
    expect(select_latency_backend(LatencyPreference::Auto, vk, &why) == LatencyBackend::NvLowLatency2, "auto: then low_latency2");
    LatencyAvailability amd{false, false, true};
    expect(select_latency_backend(LatencyPreference::Auto, amd, &why) == LatencyBackend::AmdAntiLag, "auto: then anti-lag");
    expect(select_latency_backend(LatencyPreference::AmdAntiLag, all, &why) == LatencyBackend::AmdAntiLag, "explicit anti-lag");
    expect(select_latency_backend(LatencyPreference::NvLowLatency2, amd, &why) == LatencyBackend::None && why != nullptr &&
               std::strstr(why, "not enabled") != nullptr,
           "explicit but unavailable -> none with a reason");
    expect(select_latency_backend(LatencyPreference::None, all, &why) == LatencyBackend::None, "none");
    LatencyProviderHandle h = create_latency_provider(LatencyPreference::Auto, LatencyProviderSources{});
    expect(h.provider != nullptr && h.provider->backend() == LatencyBackend::None && !h.provider->pacing(), "no sources -> none");
    std::printf("selection: %s\n", h.reason.c_str());
    for (u32 b = 0; b < static_cast<u32>(LatencyBackend::Count); ++b) {
        expect(std::strcmp(latency_backend_name(static_cast<LatencyBackend>(b)), "?") != 0, "backend names");
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "pacing";
    int rc = 0;
    if (mode == "pacing") {
        rc = runPacing();
    } else if (mode == "latency") {
        rc = runLatency();
    } else if (mode == "markers") {
        rc = runMarkers();
    } else if (mode == "reflex") {
        rc = runReflex();
    } else if (mode == "low_latency2") {
        rc = runLowLatency2();
    } else if (mode == "anti_lag") {
        rc = runAntiLag();
    } else if (mode == "selection") {
        rc = runSelection();
    } else {
        std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
        return 2;
    }
    if (rc != 0 || g_failures != 0) {
        std::fprintf(stderr, "FAIL %s: %d failure(s)\n", mode.c_str(), g_failures + rc);
        return 1;
    }
    std::printf("PASS %s\n", mode.c_str());
    return 0;
}
