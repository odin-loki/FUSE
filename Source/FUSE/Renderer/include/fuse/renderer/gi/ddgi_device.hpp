#pragma once

// Device-resident DDGI probe update (B5.6, docs/compute-kernels.md). The irradiance / distance atlases, update
// counts and texel directions stay in CUDA device memory across updates. An update uploads only the scheduled
// probe list, the rotated ray set and the scene boxes, runs the single-source trace + blend kernels
// (fuse/renderer/gi/ddgi_probe_kernel.hpp, the bodies the CPU backends run) on the resident atlases on its own
// stream, and times the cycle with CUDA events. The host DdgiCpuVolume keeps the desc / config, builds the
// launch (DdgiCpuVolume::prepareUpdate, the same parameters its CPU backends use) and is the CPU reference;
// download() copies the device atlases back into it.
//
// Available in FUSE_HAS_CUDA builds with a CUDA device (available()); anywhere else every call fails cleanly
// and message() says why. Device code: kernels/ddgi_probe_update.cu. Host routing + no-CUDA stubs:
// src/gi/ddgi_cpu.cpp. Gate: tests/test_b5_ddgi_device.cpp (fuse_b5_ddgi_device).

#include <fuse/renderer/gi/ddgi_cpu.hpp>
#include <fuse/renderer/gi/ddgi_probe_kernel.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace fuse::renderer {

/// Launch parameters of one probe update over a DdgiCpuVolume's host arrays (DdgiCpuVolume::prepareUpdate).
/// Used in place: `blend.fast_response_texels` points at `fast_response`.
struct DdgiUpdateLaunch {
    ddgi_kernel::TraceParams trace{};
    ddgi_kernel::BlendParams blend{};
    ddgi_kernel::ProbeStateParams state{};
    DdgiCpuUpdateStats stats{};
    /// Entries of the probe list (out-of-range entries are skipped by the kernels).
    u32 slots = 0;
    /// A probe is listed twice: the blend is order-dependent (the CPU path runs it serially on CpuReference).
    bool duplicates = false;
    /// Probe relocation / classification on: the probe-state launch follows the blend (CPU backends only).
    bool states = false;
    /// The blend's global fast-response counter.
    u32 fast_response = 0;
};

/// CUDA-event timing of one device update, in milliseconds on the volume's stream.
struct DdgiDeviceTiming {
    f32 upload_ms = 0.f; ///< probe list + ray set + scene boxes (host -> device) and the counter reset
    f32 trace_ms = 0.f;  ///< "ddgi_probe_trace"
    f32 blend_ms = 0.f;  ///< "ddgi_probe_update"
    f32 cycle_ms = 0.f;  ///< first -> last event: the whole update cycle (upload + trace + blend)
};

/// Device-resident mirror of one DdgiCpuVolume (see the file comment). Not copyable.
class DdgiDeviceVolume {
public:
    DdgiDeviceVolume();
    ~DdgiDeviceVolume();
    DdgiDeviceVolume(const DdgiDeviceVolume&) = delete;
    DdgiDeviceVolume& operator=(const DdgiDeviceVolume&) = delete;

    /// True in a FUSE_HAS_CUDA build with a usable CUDA device (kernel::backend_available(Backend::Cuda)).
    static bool available();

    /// Allocates the device atlases for `volume` (ready, probe relocation / classification off) and uploads its
    /// irradiance / distance atlases, update counts and texel directions. Upload again after the host volume
    /// changes (init, a CPU-backend update).
    bool upload(const DdgiCpuVolume& volume);
    /// Rolling update on the device: the same schedule, LoadScale and launch parameters as
    /// DdgiCpuVolume::update. `volume` supplies desc / config / scratch; its host atlases are not modified.
    DdgiCpuUpdateStats update(DdgiCpuVolume& volume, const DdgiCpuScene& scene, u32 frame_index);
    /// Trace + blend an explicit probe list (out-of-range entries skipped; a probe listed twice is refused —
    /// run such lists on a CPU backend).
    DdgiCpuUpdateStats updateProbes(DdgiCpuVolume& volume,
                                    const DdgiCpuScene& scene,
                                    const u32* probe_indices,
                                    u32 probe_count,
                                    u32 frame_index);
    /// Copies the device atlases and update counts into `volume` (the volume that was uploaded).
    bool download(DdgiCpuVolume& volume);

    /// On: every kernel launch synchronises (kernel::LaunchOptions::synchronize), so the kernel stats and
    /// profiler scopes of "ddgi_probe_trace" / "ddgi_probe_update" carry GPU-inclusive durations — the
    /// profiler's pass breakdown. Off (default): launches are asynchronous; only the CUDA events time the GPU.
    void setSynchronousLaunches(bool on) { m_synchronous = on; }
    bool synchronousLaunches() const { return m_synchronous; }

    /// upload() succeeded (the atlases are on the device).
    bool resident() const { return m_resident; }
    /// The last call succeeded.
    bool ok() const { return m_ok; }
    /// Why the last call failed (CUDA error string, missing device, unsupported list), empty after success.
    const std::string& message() const { return m_message; }
    /// CUDA-event timing of the last successful update (zero before the first).
    const DdgiDeviceTiming& lastTiming() const { return m_timing; }

private:
    struct Impl;
    /// Runs `launch` on the resident atlases (kernels/ddgi_probe_update.cu; stub without CUDA).
    bool launchOnDevice(DdgiUpdateLaunch& launch);
    bool fail(const char* message);

    std::unique_ptr<Impl> m_impl;
    std::vector<u32> m_schedule;
    DdgiDeviceTiming m_timing{};
    std::string m_message;
    bool m_resident = false;
    bool m_synchronous = false;
    bool m_ok = false;
};

} // namespace fuse::renderer
