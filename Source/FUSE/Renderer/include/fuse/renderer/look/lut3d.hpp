#pragma once

// 3D colour LUTs for the look system: storage, tetrahedral / trilinear sampling, `.cube` import and
// export (the openly published Adobe Cube LUT Specification 1.0 text format, including the DOMAIN_* /
// LUT_*_INPUT_RANGE keywords written by common grading tools), generation from grading parameters,
// resampling and blending.
//
// Domain: every engine LUT maps sRGB-encoded display values [0, 1]^3 -> [0, 1]^3 (red fastest).

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/look/look_kernels.hpp>
#include <fuse/renderer/look/look_params.hpp>
#include <fuse/types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace fuse::renderer::look {

inline constexpr u32 kLutMinSize = 2u;
inline constexpr u32 kLutMaxSize = 128u;
/// Engine LUT sizes the look system bakes at.
inline constexpr u32 kLutSizeDefault = 32u;
inline constexpr u32 kLutSizeHigh = 64u;

struct Lut3D {
    u32 size = 0;
    std::vector<math::Vec3> data; ///< size^3, red fastest
    std::string title;

    bool valid() const { return size >= kLutMinSize && data.size() == static_cast<size_t>(size) * size * size; }
    const math::Vec3& at(u32 r, u32 g, u32 b) const { return data[kernels::lut_index(r, g, b, size)]; }
    kernel::Span<const math::Vec3> span() const {
        return {data.data(), static_cast<u32>(data.size())};
    }

    static Lut3D identity(u32 size);
};

enum class LutInterpolation : u8 { Tetrahedral = 0, Trilinear = 1 };

math::Vec3 lut_sample(const Lut3D& lut, const math::Vec3& rgb, LutInterpolation mode = LutInterpolation::Tetrahedral);

struct CubeParseResult {
    bool ok = false;
    std::string error; ///< "line N: ..." on failure
    u32 line = 0;
    u32 size_3d = 0; ///< LUT_3D_SIZE (0 when absent)
    u32 size_1d = 0; ///< LUT_1D_SIZE (0 when absent)
    math::Vec3 domain_min{0.f, 0.f, 0.f};
    math::Vec3 domain_max{1.f, 1.f, 1.f};
    bool resampled = false; ///< the file was baked/resampled to the requested size
};

/// Parses `.cube` text. 3D tables are kept as-is (or resampled to `target_size` when non-zero and
/// different). 1D-only tables and 1D shaper + 3D tables are baked into a 3D table of `target_size`
/// (or the 3D size / 33 when zero). A non-unit DOMAIN_* is baked into the engine [0, 1] domain.
bool lut_parse_cube(std::string_view text, Lut3D& out, CubeParseResult& result, u32 target_size = 0);
bool lut_load_cube(const char* path, Lut3D& out, CubeParseResult& result, u32 target_size = 0);
/// Writes `.cube` text (TITLE, LUT_3D_SIZE, DOMAIN_MIN/MAX, %.9g values — lossless for f32).
std::string lut_write_cube(const Lut3D& lut);

/// Resamples `src` to `size` with tetrahedral interpolation (exact for identity / affine LUTs).
void lut_resample(const Lut3D& src, u32 size, Lut3D& out);

/// Lattice-wise blend out = sum w_k * luts[k] (all the same size; weights used as given).
/// Allocation-free once `out` has the right size.
bool lut_blend(const Lut3D* const* luts, const f32* weights, u32 count, Lut3D& out,
               kernel::Backend backend = kernel::Backend::CpuParallel);
/// out = lerp(a, b, t)
bool lut_lerp(const Lut3D& a, const Lut3D& b, f32 t, Lut3D& out, kernel::Backend backend = kernel::Backend::CpuParallel);

/// Grading constants for the kernels from the resolved look (white balance matrix, curve tangents,
/// stage flags; neutral stages are flagged off so neutral grading is an exact identity).
kernels::GradeParams make_grade_params(const LookResolved& look);

/// Chromaticity of the light a white-balance setting neutralises: the Planckian locus (Kim et al. cubic
/// fit) offset so 6504 K is exactly D65, plus `tint` * 0.02 on y.
void white_balance_source_xy(f32 temperature_k, f32 tint, double& x, double& y);
/// Bradford chromatic adaptation (linear Rec.709) mapping the `white_balance_source_xy` white onto D65
/// (the Rec.709 white, i.e. neutral RGB). Camera convention: values above 6504 K warm the image, below
/// cool it. Row-major 3x3.
void white_balance_matrix(f32 temperature_k, f32 tint, f32 out[9]);

/// Generates the grading LUT for `look` (no external LUTs). Allocation-free once `out` is sized.
void lut_generate_grade(const LookResolved& look, u32 size, Lut3D& out,
                        kernel::Backend backend = kernel::Backend::CpuParallel);

} // namespace fuse::renderer::look
