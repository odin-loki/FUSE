// WP-4.4 present-timing model (see include/fuse/renderer/present/latency/present_timing.hpp).
#include <fuse/renderer/present/latency/present_timing.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::present {

namespace {

using i64 = long long;

/// Deterministic uniform [0, 1) from (seed, frame, stream): integer hash (splitmix-style), no libm.
f64 hash01(u32 seed, u64 frame, u32 stream) {
    u64 x = (static_cast<u64>(seed) << 32) ^ (frame * 0x9E3779B97F4A7C15ull) ^ (static_cast<u64>(stream) * 0xBF58476D1CE4E5B9ull);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return static_cast<f64>(x >> 11) * (1.0 / 9007199254740992.0);
}

f64 noisy(f64 base, f64 jitter, u32 seed, u64 frame, u32 stream) {
    if (!(jitter > 0.0)) {
        return base;
    }
    const f64 v = base + jitter * (2.0 * hash01(seed, frame, stream) - 1.0);
    return std::max(v, 0.05 * base);
}

/// Display model: one call per presented image, in present order.
class Display {
public:
    explicit Display(const PresentTimingConfig& c)
        : m_mode(c.mode), m_period(c.refresh_hz > 0.0 ? 1000.0 / c.refresh_hz : 0.0) {}

    /// Returns the display time of the image presented at `present` (Mailbox: tentative; see dropped()).
    /// `image` identifies it for the mailbox drop bookkeeping.
    f64 present(f64 present, u64 image) {
        switch (m_mode) {
        case PresentMode::Immediate: {
            const f64 d = std::max(present, m_last);
            m_last = d;
            return d;
        }
        case PresentMode::Vrr: {
            const f64 d = m_hasLast ? std::max(present, m_last + m_period) : present;
            m_last = d;
            m_hasLast = true;
            return d;
        }
        case PresentMode::Fifo: {
            const f64 earliest = m_hasLast ? std::max(present, m_last + m_period) : present;
            const f64 d = vblankAtOrAfter(earliest);
            m_last = d;
            m_hasLast = true;
            return d;
        }
        case PresentMode::Mailbox:
        default: {
            if (m_hasPending && present <= m_pendingDisplay) {
                // The pending image is replaced before its vblank: dropped, the new one takes the slot.
                m_dropped = m_pendingImage;
                m_hasDropped = true;
                m_pendingImage = image;
                m_pendingPresent = present;
                return m_pendingDisplay;
            }
            if (m_hasPending) {
                m_last = m_pendingDisplay;
                m_hasLast = true;
            }
            const f64 earliest = m_hasLast ? std::max(present, m_last + m_period) : present;
            m_pendingDisplay = vblankAtOrAfter(earliest);
            m_pendingImage = image;
            m_pendingPresent = present;
            m_hasPending = true;
            return m_pendingDisplay;
        }
        }
    }
    /// Mailbox: the image dropped by the last present() call, if any.
    bool takeDropped(u64& image) {
        if (!m_hasDropped) {
            return false;
        }
        image = m_dropped;
        m_hasDropped = false;
        return true;
    }

private:
    f64 vblankAtOrAfter(f64 t) const {
        if (!(m_period > 0.0)) {
            return t;
        }
        const f64 k = std::ceil(t / m_period - 1e-9);
        return k * m_period;
    }

    PresentMode m_mode;
    f64 m_period;
    f64 m_last = 0.0;
    bool m_hasLast = false;
    bool m_hasPending = false;
    f64 m_pendingDisplay = 0.0;
    f64 m_pendingPresent = 0.0;
    u64 m_pendingImage = 0;
    bool m_hasDropped = false;
    u64 m_dropped = 0;
};

f64 percentile(std::vector<f64> v, f64 p) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const f64 pos = p * static_cast<f64>(v.size() - 1u);
    const usize lo = static_cast<usize>(std::floor(pos));
    const usize hi = std::min(lo + 1u, v.size() - 1u);
    const f64 t = pos - static_cast<f64>(lo);
    return v[lo] + (v[hi] - v[lo]) * t;
}

} // namespace

const char* present_mode_name(PresentMode mode) {
    switch (mode) {
    case PresentMode::Fifo: return "fifo";
    case PresentMode::Mailbox: return "mailbox";
    case PresentMode::Immediate: return "immediate";
    case PresentMode::Vrr: return "vrr";
    default: return "?";
    }
}

u32 pacing_bucket_count(const std::vector<f64>& intervals, f64 bucketMs, f64 minFraction) {
    if (intervals.empty() || !(bucketMs > 0.0)) {
        return 0u;
    }
    std::vector<i64> keys;
    keys.reserve(intervals.size());
    for (const f64 v : intervals) {
        keys.push_back(static_cast<i64>(std::floor(v / bucketMs + 1e-9)));
    }
    std::sort(keys.begin(), keys.end());
    const f64 need = minFraction * static_cast<f64>(intervals.size());
    u32 buckets = 0;
    usize i = 0;
    while (i < keys.size()) {
        usize j = i;
        while (j < keys.size() && keys[j] == keys[i]) {
            ++j;
        }
        if (static_cast<f64>(j - i) >= need) {
            ++buckets;
        }
        i = j;
    }
    return buckets;
}

PresentTimingResult simulate_present_timing(const PresentTimingConfig& c) {
    PresentTimingResult result{};
    const u32 n = c.frames;
    result.frames.resize(n);
    if (n == 0u) {
        return result;
    }
    const bool fg = c.frame_gen == FrameGenMode::Interpolate2x;
    const u32 images = std::max(c.swapchain_images, 2u);
    const u32 inFlight = std::max(c.max_frames_in_flight, 1u);
    ILatencyProvider* provider = c.provider;
    const bool pacing = provider != nullptr && provider->pacing();
    f64 limitMs = 0.0;
    if (provider != nullptr && provider->caps().fps_limit && provider->settings().fps_limit > 0.f) {
        limitMs = 1000.0 / static_cast<f64>(provider->settings().fps_limit);
    }
    const f64 period = c.refresh_hz > 0.0 ? 1000.0 / c.refresh_hz : 0.0;

    Display display(c);
    // Per real frame: GPU completion of all its work (render + FG), release time of its swapchain image, the
    // display time of the image shown right after its real image (= release; -1 until known).
    std::vector<f64> gpuDone(n, 0.0);
    std::vector<f64> cpuSim(n, 0.0), cpuRender(n, 0.0), gpuWork(n, 0.0);
    // Shown images in present order: (frame, generated?, display).
    struct Image {
        u32 frame;
        bool generated;
        f64 present;
        f64 display;
        bool dropped;
    };
    std::vector<Image> shown;
    shown.reserve(static_cast<usize>(n) * (fg ? 2u : 1u));
    std::vector<i64> realImageIndex(n, -1);

    auto releaseOf = [&](u32 frame) -> f64 {
        // The image of real frame `frame` is released when the next image is shown (or when it was replaced).
        const i64 idx = realImageIndex[frame];
        if (idx < 0) {
            return 0.0;
        }
        const Image& img = shown[static_cast<usize>(idx)];
        if (img.dropped) {
            // Replaced by the next presented image.
            return static_cast<usize>(idx) + 1u < shown.size() ? shown[static_cast<usize>(idx) + 1u].present : img.present;
        }
        for (usize k = static_cast<usize>(idx) + 1u; k < shown.size(); ++k) {
            if (!shown[k].dropped) {
                return shown[k].display;
            }
        }
        return img.display + period; // nothing presented after it yet: assume it is replaced one refresh later
    };

    auto addImage = [&](u32 frame, bool generated, f64 presentTime) -> usize {
        Image img{frame, generated, presentTime, 0.0, false};
        img.display = display.present(presentTime, shown.size());
        u64 droppedIdx = 0;
        if (display.takeDropped(droppedIdx)) {
            shown[static_cast<usize>(droppedIdx)].dropped = true;
        }
        shown.push_back(img);
        return shown.size() - 1u;
    };

    f64 intervalEstimate = 0.0; // FG pacer: EMA of the real-frame interval
    for (u32 i = 0; i < n; ++i) {
        SimFrame& f = result.frames[i];
        f.id = i;
        cpuSim[i] = noisy(c.cpu_sim_ms, c.cpu_jitter_ms, c.seed, i, 1u);
        cpuRender[i] = noisy(c.cpu_render_ms, c.cpu_jitter_ms * 0.5, c.seed, i, 2u);
        gpuWork[i] = noisy(c.gpu_ms, c.gpu_jitter_ms, c.seed, i, 3u);
        const bool generates = fg && i > 0u;
        const f64 gpuTotal = gpuWork[i] + (generates ? c.fg_gpu_ms : 0.0);

        const f64 cpuFree = i > 0u ? result.frames[i - 1u].submit_end : 0.0;
        const f64 fence = i >= inFlight ? gpuDone[i - inFlight] : 0.0;
        f.natural_start = std::max(cpuFree, fence);
        f64 start = f.natural_start;
        if (limitMs > 0.0 && i > 0u) {
            start = std::max(start, result.frames[i - 1u].sim_start + limitMs);
        }
        if (pacing && i > 0u) {
            // Just-in-time start from the previous frame's measured CPU / GPU times.
            const f64 predCpu = cpuSim[i - 1u] + cpuRender[i - 1u];
            const f64 predGpu = gpuWork[i - 1u] + (generates ? c.fg_gpu_ms : 0.0);
            f64 target = gpuDone[i - 1u] - predCpu - c.latency_margin_ms; // GPU bottleneck
            const SimFrame& prev = result.frames[i - 1u];
            if ((c.mode == PresentMode::Fifo || c.mode == PresentMode::Mailbox || c.mode == PresentMode::Vrr) && period > 0.0 &&
                prev.display >= 0.0) {
                // Display bottleneck: the frame's real image should be ready for the slot after the previous one
                // (FG: two images per rendered frame).
                const f64 slot = prev.display + period * (fg ? 2.0 : 1.0);
                target = std::max(target, slot - predGpu - predCpu - c.latency_margin_ms);
            }
            start = std::max(start, target);
        }
        f.sleep_ms = start - f.natural_start;
        if (provider != nullptr) {
            provider->sleep(i);
        }
        f.sim_start = start;
        f.input = start;
        if (provider != nullptr) {
            provider->marker(i, LatencyMarker::InputSample);
            provider->marker(i, LatencyMarker::SimulationStart);
        }
        f.sim_end = f.sim_start + cpuSim[i];
        if (provider != nullptr) {
            provider->marker(i, LatencyMarker::SimulationEnd);
        }
        const f64 acquire = i >= images ? releaseOf(i - images) : 0.0;
        f.submit_start = std::max(f.sim_end, acquire);
        if (provider != nullptr) {
            provider->marker(i, LatencyMarker::RenderSubmitStart);
        }
        f.submit_end = f.submit_start + cpuRender[i];
        if (provider != nullptr) {
            provider->marker(i, LatencyMarker::RenderSubmitEnd);
        }
        f.gpu_start = std::max(f.submit_end, i > 0u ? gpuDone[i - 1u] : 0.0);
        f.gpu_end = f.gpu_start + gpuWork[i];
        gpuDone[i] = f.gpu_start + gpuTotal;

        if (provider != nullptr) {
            provider->marker(i, LatencyMarker::PresentStart);
        }
        if (generates) {
            const f64 fgEnd = gpuDone[i];
            const f64 realInterval = gpuDone[i] - gpuDone[i - 1u];
            intervalEstimate = intervalEstimate > 0.0 ? intervalEstimate + c.fg_pacing_smoothing * (realInterval - intervalEstimate)
                                                      : realInterval;
            f.generated_present = fgEnd;
            const usize g = addImage(i, true, fgEnd);
            f.generated_display = shown[g].display;
            // The FG pacer holds the real frame back by half a real-frame interval after the generated one.
            f.present = std::max(fgEnd, shown[g].display + 0.5 * intervalEstimate);
        } else {
            f.present = gpuDone[i];
        }
        realImageIndex[i] = static_cast<i64>(addImage(i, false, f.present));
        if (provider != nullptr) {
            provider->marker(i, LatencyMarker::PresentEnd);
        }
        f.display = shown[static_cast<usize>(realImageIndex[i])].display;
    }

    // Final display times (mailbox drops are known only now) and statistics.
    for (u32 i = 0; i < n; ++i) {
        SimFrame& f = result.frames[i];
        const Image& img = shown[static_cast<usize>(realImageIndex[i])];
        f.display = img.dropped ? -1.0 : img.display;
        f.latency_ms = img.dropped ? -1.0 : f.display - f.input;
    }
    for (const Image& img : shown) {
        if (!img.dropped) {
            result.display_times.push_back(img.display);
        }
    }
    for (usize k = 0; k < shown.size(); ++k) {
        if (shown[k].generated && shown[k].frame < n) {
            result.frames[shown[k].frame].generated_display = shown[k].dropped ? -1.0 : shown[k].display;
        }
    }

    PresentTimingStats& s = result.stats;
    const u32 w = std::min(c.warmup_frames, n > 1u ? n - 1u : 0u);
    std::vector<f64> lat;
    f64 sleepSum = 0.0;
    u32 sleepCount = 0;
    for (u32 i = w; i < n; ++i) {
        const SimFrame& f = result.frames[i];
        if (f.latency_ms >= 0.0) {
            lat.push_back(f.latency_ms);
        } else {
            ++s.dropped;
        }
        sleepSum += f.sleep_ms;
        ++sleepCount;
    }
    if (!lat.empty()) {
        f64 sum = 0.0;
        for (const f64 v : lat) {
            sum += v;
        }
        s.latency_mean_ms = sum / static_cast<f64>(lat.size());
        s.latency_min_ms = *std::min_element(lat.begin(), lat.end());
        s.latency_max_ms = *std::max_element(lat.begin(), lat.end());
        s.latency_p50_ms = percentile(lat, 0.5);
        s.latency_p95_ms = percentile(lat, 0.95);
    }
    s.sleep_mean_ms = sleepCount > 0u ? sleepSum / static_cast<f64>(sleepCount) : 0.0;
    if (n - w >= 2u) {
        const f64 span = result.frames[n - 1u].sim_start - result.frames[w].sim_start;
        s.render_fps = span > 0.0 ? 1000.0 * static_cast<f64>(n - 1u - w) / span : 0.0;
    }
    // Display intervals of the images shown from the first steady frame's first image on.
    f64 firstSteady = -1.0;
    for (u32 i = w; i < n && firstSteady < 0.0; ++i) {
        const SimFrame& f = result.frames[i];
        firstSteady = f.generated_display >= 0.0 ? f.generated_display : f.display;
    }
    std::vector<f64> steadyDisplays;
    for (const f64 t : result.display_times) {
        if (firstSteady >= 0.0 && t >= firstSteady) {
            steadyDisplays.push_back(t);
        }
    }
    for (usize k = 0; k < shown.size(); ++k) {
        if (shown[k].generated && !shown[k].dropped && shown[k].frame >= w) {
            ++s.generated;
        }
    }
    if (steadyDisplays.size() >= 2u) {
        std::vector<f64> intervals;
        for (usize k = 1; k < steadyDisplays.size(); ++k) {
            intervals.push_back(steadyDisplays[k] - steadyDisplays[k - 1u]);
        }
        f64 sum = 0.0, sq = 0.0;
        for (const f64 v : intervals) {
            sum += v;
        }
        s.interval_mean_ms = sum / static_cast<f64>(intervals.size());
        for (const f64 v : intervals) {
            sq += (v - s.interval_mean_ms) * (v - s.interval_mean_ms);
        }
        s.interval_stddev_ms = std::sqrt(sq / static_cast<f64>(intervals.size()));
        s.interval_min_ms = *std::min_element(intervals.begin(), intervals.end());
        s.interval_max_ms = *std::max_element(intervals.begin(), intervals.end());
        const f64 span = steadyDisplays.back() - steadyDisplays.front();
        s.displayed_fps = span > 0.0 ? 1000.0 * static_cast<f64>(steadyDisplays.size() - 1u) / span : 0.0;
        s.pacing_buckets = pacing_bucket_count(intervals, 1.0, 0.01);
    }
    return result;
}

} // namespace fuse::renderer::present
