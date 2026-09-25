#pragma once

// Host driver of the native TAAU kernel (taau_kernel.hpp, kernel name "taau"): owns the display-resolution
// history (ping-pong) and the previous frame's render-resolution depth / motion used for disocclusion and
// velocity rejection, and launches the single-source body on a compute backend (CpuReference = deterministic
// ground truth, CpuParallel = production CPU path, Cuda when a device entry is supplied).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/taa/taau_kernel.hpp>
#include <fuse/renderer/upscale/upscale_inputs.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

struct TaauFrameStats {
    bool resolved = false;
    bool history_used = false;   ///< False on the first frame / after a reset or resize.
    kernel::Backend backend = kernel::Backend::CpuReference; ///< Backend that ran.
    u64 duration_ns = 0;
};

class TaauUpscaler {
public:
    explicit TaauUpscaler(const taau_kernel::Settings& settings = {}) : m_settings(settings) {}

    /// (Re)allocates history for `resolution`; any change invalidates history. False when invalid.
    bool resize(const UpscaleResolution& resolution);
    void invalidate() { m_valid = false; }

    /// Upscales one frame into `output` (display_width * display_height linear RGB). Honours
    /// `inputs.reset_history`. False on invalid inputs (see validateUpscaleInputs).
    bool upscale(const UpscaleInputs& inputs, math::Vec3* output, kernel::Backend backend = kernel::Backend::CpuParallel);

    taau_kernel::Settings& settings() { return m_settings; }
    const taau_kernel::Settings& settings() const { return m_settings; }
    const UpscaleResolution& resolution() const { return m_resolution; }
    bool hasHistory() const { return m_valid; }
    /// Current display history (exposed RGB + accumulated weight).
    const std::vector<math::Vec4>& history() const { return m_history[m_current]; }
    const TaauFrameStats& lastStats() const { return m_stats; }

private:
    taau_kernel::Settings m_settings{};
    UpscaleResolution m_resolution{};
    std::vector<math::Vec4> m_history[2];
    u32 m_current = 0;
    std::vector<f32> m_prevDepth;
    std::vector<math::Vec2> m_prevMotion;
    bool m_valid = false;
    TaauFrameStats m_stats{};
};

/// Spatial-only upscale baseline (kernel "upscale_spatial"): bilinear, Catmull-Rom or Lanczos-2 of the
/// jittered render colour at each display pixel. False on invalid inputs.
bool spatialUpscale(const UpscaleInputs& inputs, taau_kernel::SpatialFilter filter, math::Vec3* output,
                    kernel::Backend backend = kernel::Backend::CpuParallel);

} // namespace fuse::renderer
