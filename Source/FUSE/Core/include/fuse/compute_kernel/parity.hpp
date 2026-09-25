#pragma once

// Backend parity harness (host only): run the same kernel on two backends and compare the outputs.
// Today: CpuReference vs CpuParallel in unit tests. Later: CpuReference vs Cuda / VulkanCompute with a
// float tolerance (device transcendental functions differ from the host libm by a few ulps).

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/types.hpp>

#include <cmath>
#include <cstring>
#include <span>

namespace fuse::kernel {

struct ParityReport {
    bool ok = false;          ///< Both launches succeeded and no element mismatched.
    bool launches_ok = false; ///< Both launches succeeded (and the output sizes matched).
    Backend backend_a = Backend::CpuReference;
    Backend backend_b = Backend::CpuReference;
    u64 compared = 0;         ///< Scalars compared.
    u64 mismatches = 0;
    u64 first_mismatch = ~0ull; ///< Scalar index of the first mismatch.
    f64 max_abs_error = 0.0;
    f64 max_rel_error = 0.0;

    /// Folds another comparison (e.g. a second output surface) into this report.
    void merge(const ParityReport& other) {
        if (other.mismatches != 0u && mismatches == 0u) {
            first_mismatch = compared + other.first_mismatch;
        }
        compared += other.compared;
        mismatches += other.mismatches;
        max_abs_error = other.max_abs_error > max_abs_error ? other.max_abs_error : max_abs_error;
        max_rel_error = other.max_rel_error > max_rel_error ? other.max_rel_error : max_rel_error;
        launches_ok = launches_ok && other.launches_ok;
        ok = launches_ok && mismatches == 0u;
    }
};

struct Tolerance {
    f64 abs = 0.0; ///< |a - b| <= abs passes ...
    f64 rel = 0.0; ///< ... or |a - b| <= rel * max(|a|, |b|).
    static constexpr Tolerance exact() { return {}; }
};

/// Bitwise element comparison (integer / deterministic outputs; also distinguishes -0.f and NaN payloads).
template <typename T>
ParityReport compare_bitwise(std::span<const T> a, std::span<const T> b) {
    ParityReport report{};
    report.launches_ok = a.size() == b.size();
    const usize n = a.size() < b.size() ? a.size() : b.size();
    for (usize i = 0; i < n; ++i) {
        if (std::memcmp(&a[i], &b[i], sizeof(T)) != 0) {
            if (report.mismatches == 0u) {
                report.first_mismatch = i;
            }
            ++report.mismatches;
        }
    }
    report.compared = n;
    report.ok = report.launches_ok && report.mismatches == 0u;
    return report;
}

/// Float comparison with tolerance; NaN only matches NaN. `T` is a float, or an aggregate of
/// `sizeof(T) / sizeof(f32)` floats (e.g. math::Vec4) compared component-wise.
template <typename T>
ParityReport compare_floats(std::span<const T> a, std::span<const T> b, Tolerance tol) {
    static_assert(sizeof(T) % sizeof(f32) == 0u, "compare_floats compares f32 components");
    constexpr usize kComponents = sizeof(T) / sizeof(f32);
    ParityReport report{};
    report.launches_ok = a.size() == b.size();
    const usize n = a.size() < b.size() ? a.size() : b.size();
    for (usize i = 0; i < n; ++i) {
        f32 ca[kComponents];
        f32 cb[kComponents];
        std::memcpy(ca, &a[i], sizeof(T));
        std::memcpy(cb, &b[i], sizeof(T));
        for (usize c = 0; c < kComponents; ++c) {
            const f64 va = ca[c];
            const f64 vb = cb[c];
            bool match = false;
            if (std::isnan(va) || std::isnan(vb)) {
                match = std::isnan(va) && std::isnan(vb);
            } else {
                const f64 diff = std::fabs(va - vb);
                const f64 mag = std::fmax(std::fabs(va), std::fabs(vb));
                const f64 rel = mag > 0.0 ? diff / mag : 0.0;
                report.max_abs_error = std::fmax(report.max_abs_error, diff);
                report.max_rel_error = std::fmax(report.max_rel_error, rel);
                match = diff <= tol.abs || diff <= tol.rel * mag;
            }
            if (!match) {
                if (report.mismatches == 0u) {
                    report.first_mismatch = i * kComponents + c;
                }
                ++report.mismatches;
            }
        }
    }
    report.compared = n * kComponents;
    report.ok = report.launches_ok && report.mismatches == 0u;
    return report;
}

/// Runs `body` on backend `a` with `params_a` and on backend `b` with `params_b` (params differ only
/// in their output pointers), then returns `compare()` — a callable producing a ParityReport from
/// the two output sets (typically compare_bitwise / compare_floats over the caller's buffers).
template <typename Body, typename Params, typename Compare>
ParityReport run_parity(Backend a, Backend b, const KernelLaunch& launch_desc, const Body& body,
                        const Params& params_a, const Params& params_b, Compare&& compare,
                        const LaunchOptions& options = {}) {
    const LaunchResult ra = launch(a, launch_desc, body, params_a, options);
    const LaunchResult rb = launch(b, launch_desc, body, params_b, options);
    ParityReport report = compare();
    report.backend_a = ra.backend;
    report.backend_b = rb.backend;
    report.launches_ok = report.launches_ok && ra.ok && rb.ok;
    report.ok = report.launches_ok && report.mismatches == 0u;
    return report;
}

} // namespace fuse::kernel
