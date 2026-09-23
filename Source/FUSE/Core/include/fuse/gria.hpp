#pragma once

#include <fuse/types.hpp>

#if defined(__CUDACC__)
#define FUSE_HOST_DEVICE __host__ __device__
#else
#define FUSE_HOST_DEVICE
#endif

namespace fuse {

/// GRIA α ∈ [0, 1]: 0 = fully reversible/exact, 1 = fully irreversible/approximate (B1.2).
/// A typed value passed by value everywhere (LOD, physics precision, SDF blending). Header-only
/// and constexpr so host code and CUDA kernels evaluate the same definition.
struct Alpha {
    f32 value = 0.f;

    FUSE_HOST_DEVICE constexpr Alpha() = default;
    FUSE_HOST_DEVICE constexpr explicit Alpha(f32 v) : value(clamp01(v)) {}

    FUSE_HOST_DEVICE constexpr f32 get() const { return value; }

    FUSE_HOST_DEVICE constexpr bool is_exact() const { return value < 0.01f; }
    FUSE_HOST_DEVICE constexpr bool is_approximate() const { return value > 0.99f; }
    FUSE_HOST_DEVICE constexpr bool at_edge_of_chaos() const { return value > 0.49f && value < 0.51f; }

    /// Grand Unified Law: α = 1 − H(f(X)) / H(X). A zero-entropy input is exact (α = 0).
    FUSE_HOST_DEVICE static constexpr Alpha from_entropy_ratio(f32 h_output, f32 h_input) {
        return h_input > 0.f ? Alpha(1.f - h_output / h_input) : Alpha(0.f);
    }

    /// Blend two values: α = 0 selects `exact`, α = 1 selects `approximate`.
    FUSE_HOST_DEVICE constexpr f32 blend(f32 exact, f32 approximate) const {
        return exact + (approximate - exact) * value;
    }

    FUSE_HOST_DEVICE constexpr bool operator==(const Alpha& other) const { return value == other.value; }

private:
    FUSE_HOST_DEVICE static constexpr f32 clamp01(f32 v) {
        // NaN compares false on both branches and maps to 0 (exact).
        return v > 0.f ? (v < 1.f ? v : 1.f) : 0.f;
    }
};

inline constexpr Alpha ALPHA_EXACT{0.f};
inline constexpr Alpha ALPHA_CHAOS_EDGE{0.5f};
inline constexpr Alpha ALPHA_APPROXIMATE{1.f};

} // namespace fuse
