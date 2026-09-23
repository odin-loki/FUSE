#pragma once

// Global load-scale knob for profiling / scalability runs. Demos, benchmarks and kernel hosts read
// it to size their work (render resolution, object / probe / light counts) so one binary can sweep
// load without code changes: `FUSE_LOAD_SCALE="res=0.5,objects=4,probes=2,lights=8"` or a single
// number applied to every axis (`FUSE_LOAD_SCALE=2`).

#include <fuse/types.hpp>

namespace fuse::kernel {

struct LoadScale {
    f32 resolution = 1.f; ///< Linear scale of render extents (width and height each).
    f32 objects = 1.f;    ///< Scene object / instance / particle / body counts.
    f32 probes = 1.f;     ///< GI probe counts.
    f32 lights = 1.f;     ///< Light counts.

    bool operator==(const LoadScale&) const = default;
};

LoadScale load_scale();
/// Non-finite or negative components are clamped to 0; extents clamp to >= 1 in scaled_extent().
void set_load_scale(const LoadScale& scale);

/// round(base * scale); 0 only when base or scale is 0 (a non-zero base never scales to 0 items).
u32 scaled_count(u32 base, f32 scale);
/// round(base * scale) clamped to >= 1 (render extents).
u32 scaled_extent(u32 base, f32 scale);

/// Parses "2" (all axes) or comma-separated `key=value` with keys res|resolution, objects, probes,
/// lights. Unmentioned axes keep their value in `inOut`. False (and `inOut` untouched) on bad input.
bool parse_load_scale(const char* text, LoadScale& inOut);
/// Applies the FUSE_LOAD_SCALE environment variable when set and valid; returns whether it applied.
bool load_scale_from_env();

/// RAII override for tests and profiling sweeps (restores the previous scale).
class ScopedLoadScale {
public:
    explicit ScopedLoadScale(const LoadScale& scale) : m_previous(load_scale()) { set_load_scale(scale); }
    ~ScopedLoadScale() { set_load_scale(m_previous); }
    ScopedLoadScale(const ScopedLoadScale&) = delete;
    ScopedLoadScale& operator=(const ScopedLoadScale&) = delete;

private:
    LoadScale m_previous;
};

} // namespace fuse::kernel
