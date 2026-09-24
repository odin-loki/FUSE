#pragma once

// WP-4.4 present-timing model: a deterministic discrete-event simulation of one CPU thread (simulation + render
// submission), the GPU queue (render + optional frame-interpolation work), the swapchain (images, acquire
// back-pressure, frames in flight) and the display (FIFO / mailbox / immediate / VRR), with an optional latency
// provider pacing the CPU and optional 2x frame interpolation (FSR 3.1 FG style: the generated frame between
// frames i - 1 and i is presented after frame i finished rendering, the real frame i half a frame interval later).
//
// It is the CPU timeline simulator of the research plan's latency accounting (docs/research/upscaling-framegen-
// and-post-injectors.md §5): every frame gets input sample, sim start / end, render submit start / end, GPU start /
// end, present and display timestamps; latency = display of the frame's real image - input sample (click-to-photon
// without scan-out); pacing = display-interval histogram (the MetalFX guidance: <= 2 buckets). The gates
// (fuse_rp_latency_*) pin pacing and latency bounds with and without a latency provider; hardware measurements
// (Reflex / Anti-Lag / VK_EXT_present_timing) are a HW item.
//
// Latency-provider model (Reflex / Anti-Lag / low_latency2 "just in time" pacing): the frame's CPU start is
// delayed so that its render submission ends when the bottleneck is predicted to accept it — the GPU (previous
// frame's GPU end), the display (FIFO: the vblank after the previous frame's display, minus the frame's GPU time)
// and the frame limiter — using the previous frame's measured CPU / GPU times plus a safety margin. The provider's
// sleep() / marker() are called at the simulated times (a recording mock observes them).

#include <fuse/renderer/present/latency/latency_provider.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer::present {

enum class PresentMode : u8 {
    Fifo = 0,  ///< vsync, one image per vblank, in order
    Mailbox,   ///< vsync, newest image at each vblank, older ones dropped
    Immediate, ///< no vsync (tearing): displayed when presented
    Vrr,       ///< variable refresh: displayed when presented, at most refresh_hz
};
const char* present_mode_name(PresentMode mode);

enum class FrameGenMode : u8 {
    Off = 0,
    Interpolate2x, ///< one generated frame between every two rendered frames
};

struct PresentTimingConfig {
    u32 frames = 240;
    u32 warmup_frames = 24;       ///< excluded from the statistics
    PresentMode mode = PresentMode::Fifo;
    f64 refresh_hz = 60.0;        ///< Fifo / Mailbox refresh; Vrr maximum refresh
    u32 swapchain_images = 3;
    u32 max_frames_in_flight = 2; ///< CPU may start frame i once frame i - max has finished on the GPU
    f64 cpu_sim_ms = 4.0;
    f64 cpu_render_ms = 2.0;      ///< render submission
    f64 gpu_ms = 10.0;
    f64 cpu_jitter_ms = 0.0;      ///< deterministic hash noise, uniform in [-j, +j]
    f64 gpu_jitter_ms = 0.0;
    u32 seed = 1u;
    FrameGenMode frame_gen = FrameGenMode::Off;
    f64 fg_gpu_ms = 1.5;          ///< interpolation cost on the GPU queue
    f64 fg_pacing_smoothing = 0.1; ///< EMA weight of the real-frame interval estimate the FG pacer uses
    f64 latency_margin_ms = 0.5;  ///< provider safety margin
    /// Optional provider: paces the CPU when pacing() (its settings().fps_limit applies when caps().fps_limit);
    /// sleep() / marker() are called at the simulated times through the frame (the provider must not block).
    ILatencyProvider* provider = nullptr;
};

struct SimFrame {
    u64 id = 0;
    f64 natural_start = 0.0; ///< earliest CPU start without the provider (CPU free, fence, limiter-free)
    f64 sleep_ms = 0.0;      ///< provider delay
    f64 input = 0.0;         ///< input sample (= sim start)
    f64 sim_start = 0.0, sim_end = 0.0;
    f64 submit_start = 0.0, submit_end = 0.0; ///< submit_start includes the swapchain acquire wait
    f64 gpu_start = 0.0, gpu_end = 0.0;
    f64 present = 0.0;       ///< real image handed to the presentation engine
    f64 display = -1.0;      ///< real image first shown (-1: dropped by mailbox)
    f64 generated_present = -1.0; ///< FG: generated image (between frames id - 1 and id)
    f64 generated_display = -1.0;
    f64 latency_ms = -1.0;   ///< display - input (-1 when dropped)
};

struct PresentTimingStats {
    f64 render_fps = 0.0;      ///< rendered frames per second (steady state)
    f64 displayed_fps = 0.0;   ///< images shown per second (generated ones included)
    f64 latency_mean_ms = 0.0, latency_min_ms = 0.0, latency_p50_ms = 0.0, latency_p95_ms = 0.0, latency_max_ms = 0.0;
    f64 interval_mean_ms = 0.0, interval_stddev_ms = 0.0, interval_min_ms = 0.0, interval_max_ms = 0.0;
    u32 pacing_buckets = 0;    ///< 1 ms histogram buckets holding >= 1% of the display intervals each
    u32 dropped = 0;           ///< mailbox-replaced real frames
    u32 generated = 0;         ///< generated images shown
    f64 sleep_mean_ms = 0.0;
};

struct PresentTimingResult {
    std::vector<SimFrame> frames;
    std::vector<f64> display_times; ///< every image shown, in display order
    PresentTimingStats stats;
};

/// Runs the simulation (deterministic: the same config gives bit-identical results).
PresentTimingResult simulate_present_timing(const PresentTimingConfig& config);

/// Display-interval histogram with `bucket_ms`-wide buckets; returns the number of buckets holding at least
/// `min_fraction` of the intervals.
u32 pacing_bucket_count(const std::vector<f64>& intervals, f64 bucket_ms = 1.0, f64 min_fraction = 0.01);

} // namespace fuse::renderer::present
