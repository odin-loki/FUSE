#include <fuse/renderer/look/lut3d.hpp>

#include <fuse/compute_kernel/launch.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fuse::renderer::look {

using math::Vec3;

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

bool parseFloats(std::string_view s, f32* out, u32 count) {
    std::string buf(s);
    const char* p = buf.c_str();
    for (u32 i = 0; i < count; ++i) {
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p || !std::isfinite(v)) {
            return false;
        }
        out[i] = static_cast<f32>(v);
        p = end;
    }
    while (*p != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*p))) {
            return false;
        }
        ++p;
    }
    return true;
}

bool parseU32(std::string_view s, u32& out) {
    s = trim(s);
    if (s.empty()) {
        return false;
    }
    u64 v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10u + static_cast<u64>(c - '0');
        if (v > 0xffffffffull) {
            return false;
        }
    }
    out = static_cast<u32>(v);
    return true;
}

f32 sample1d(const std::vector<Vec3>& table, u32 channel, f32 x) {
    const u32 n = static_cast<u32>(table.size());
    const f32 f = kernels::saturate(x) * static_cast<f32>(n - 1u);
    const u32 i0 = std::min(static_cast<u32>(f), n - 2u);
    const f32 t = f - static_cast<f32>(i0);
    const auto ch = [channel](const Vec3& v) { return channel == 0u ? v.x : (channel == 1u ? v.y : v.z); };
    return ch(table[i0]) + (ch(table[i0 + 1u]) - ch(table[i0])) * t;
}

} // namespace

Lut3D Lut3D::identity(u32 size) {
    Lut3D lut;
    size = std::clamp(size, kLutMinSize, kLutMaxSize);
    lut.size = size;
    lut.title = "identity";
    lut.data.resize(static_cast<size_t>(size) * size * size);
    const f32 inv = 1.f / static_cast<f32>(size - 1u);
    for (u32 b = 0; b < size; ++b) {
        for (u32 g = 0; g < size; ++g) {
            for (u32 r = 0; r < size; ++r) {
                lut.data[kernels::lut_index(r, g, b, size)] =
                    Vec3{static_cast<f32>(r) * inv, static_cast<f32>(g) * inv, static_cast<f32>(b) * inv};
            }
        }
    }
    return lut;
}

Vec3 lut_sample(const Lut3D& lut, const Vec3& rgb, LutInterpolation mode) {
    if (!lut.valid()) {
        return rgb;
    }
    return mode == LutInterpolation::Trilinear ? kernels::lut_sample_trilinear(lut.data.data(), lut.size, rgb)
                                               : kernels::lut_sample_tetrahedral(lut.data.data(), lut.size, rgb);
}

bool lut_parse_cube(std::string_view text, Lut3D& out, CubeParseResult& result, u32 target_size) {
    result = {};
    std::string title;
    std::vector<Vec3> table3d;
    std::vector<Vec3> table1d;
    f32 range1dMin = 0.f;
    f32 range1dMax = 1.f;
    bool dataStarted = false;
    u32 lineNo = 0;
    const auto fail = [&result, &lineNo](const std::string& msg) {
        result.ok = false;
        result.line = lineNo;
        result.error = "line " + std::to_string(lineNo) + ": " + msg;
        return false;
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? text.size() + 1u : nl + 1u;
        ++lineNo;
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const bool numeric = line.front() == '-' || line.front() == '+' || line.front() == '.' ||
                             std::isdigit(static_cast<unsigned char>(line.front()));
        if (numeric) {
            f32 v[3];
            if (!parseFloats(line, v, 3)) {
                return fail("expected three numbers");
            }
            dataStarted = true;
            const Vec3 value{v[0], v[1], v[2]};
            if (table1d.size() < result.size_1d) {
                table1d.push_back(value);
            } else {
                table3d.push_back(value);
            }
            continue;
        }
        if (dataStarted) {
            return fail("keyword after table data");
        }
        const size_t sp = line.find_first_of(" \t");
        const std::string_view key = line.substr(0, sp);
        const std::string_view rest = sp == std::string_view::npos ? std::string_view{} : trim(line.substr(sp));
        if (key == "TITLE") {
            if (rest.size() >= 2u && rest.front() == '"' && rest.back() == '"') {
                title = std::string(rest.substr(1, rest.size() - 2u));
            } else {
                title = std::string(rest);
            }
        } else if (key == "LUT_3D_SIZE") {
            if (!parseU32(rest, result.size_3d) || result.size_3d < kLutMinSize || result.size_3d > 256u) {
                return fail("LUT_3D_SIZE must be 2..256");
            }
        } else if (key == "LUT_1D_SIZE") {
            if (!parseU32(rest, result.size_1d) || result.size_1d < 2u || result.size_1d > 65536u) {
                return fail("LUT_1D_SIZE must be 2..65536");
            }
        } else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX") {
            f32 v[3];
            if (!parseFloats(rest, v, 3)) {
                return fail("DOMAIN_* needs three numbers");
            }
            (key == "DOMAIN_MIN" ? result.domain_min : result.domain_max) = Vec3{v[0], v[1], v[2]};
        } else if (key == "LUT_3D_INPUT_RANGE" || key == "LUT_1D_INPUT_RANGE") {
            f32 v[2];
            if (!parseFloats(rest, v, 2)) {
                return fail("LUT_*_INPUT_RANGE needs two numbers");
            }
            if (key == "LUT_3D_INPUT_RANGE") {
                result.domain_min = Vec3{v[0], v[0], v[0]};
                result.domain_max = Vec3{v[1], v[1], v[1]};
            } else {
                range1dMin = v[0];
                range1dMax = v[1];
            }
        } else {
            return fail("unknown keyword '" + std::string(key) + "'");
        }
    }
    lineNo = 0;
    if (result.size_3d == 0u && result.size_1d == 0u) {
        return fail("missing LUT_3D_SIZE / LUT_1D_SIZE");
    }
    if (table1d.size() != result.size_1d) {
        return fail("1D table has " + std::to_string(table1d.size()) + " entries, expected " +
                    std::to_string(result.size_1d));
    }
    const size_t expect3d = static_cast<size_t>(result.size_3d) * result.size_3d * result.size_3d;
    if (table3d.size() != expect3d) {
        return fail("3D table has " + std::to_string(table3d.size()) + " entries, expected " +
                    std::to_string(expect3d));
    }
    for (int c = 0; c < 3; ++c) {
        const f32 lo = c == 0 ? result.domain_min.x : (c == 1 ? result.domain_min.y : result.domain_min.z);
        const f32 hi = c == 0 ? result.domain_max.x : (c == 1 ? result.domain_max.y : result.domain_max.z);
        if (!(hi > lo)) {
            return fail("DOMAIN_MAX must exceed DOMAIN_MIN");
        }
    }

    const bool unitDomain = result.domain_min.x == 0.f && result.domain_min.y == 0.f && result.domain_min.z == 0.f &&
                            result.domain_max.x == 1.f && result.domain_max.y == 1.f && result.domain_max.z == 1.f;
    Lut3D native;
    if (result.size_3d != 0u) {
        native.size = result.size_3d;
        native.data = std::move(table3d);
    }
    const u32 size = target_size != 0u ? std::clamp(target_size, kLutMinSize, kLutMaxSize)
                                       : (result.size_3d != 0u ? std::min(result.size_3d, kLutMaxSize) : 33u);
    if (result.size_1d == 0u && unitDomain && size == native.size) {
        out = std::move(native);
        out.title = title;
        result.ok = true;
        return true;
    }
    // Bake: engine lattice value x in [0,1] -> (1D shaper over its input range) -> (3D table over DOMAIN).
    // The Adobe spec's DOMAIN_* applies to whichever single table is present; the LUT_1D_INPUT_RANGE /
    // LUT_3D_INPUT_RANGE pair (shaper + cube files) set each table's range separately.
    const bool has1dRange = range1dMin != 0.f || range1dMax != 1.f;
    const Vec3 d1min = has1dRange || native.valid() ? Vec3{range1dMin, range1dMin, range1dMin} : result.domain_min;
    const Vec3 d1max = has1dRange || native.valid() ? Vec3{range1dMax, range1dMax, range1dMax} : result.domain_max;
    const Vec3 d3min = result.domain_min;
    const Vec3 d3max = result.domain_max;
    Lut3D baked;
    baked.size = size;
    baked.data.resize(static_cast<size_t>(size) * size * size);
    const f32 inv = 1.f / static_cast<f32>(size - 1u);
    for (u32 b = 0; b < size; ++b) {
        for (u32 g = 0; g < size; ++g) {
            for (u32 r = 0; r < size; ++r) {
                Vec3 x{static_cast<f32>(r) * inv, static_cast<f32>(g) * inv, static_cast<f32>(b) * inv};
                if (!table1d.empty()) {
                    x = Vec3{sample1d(table1d, 0, (x.x - d1min.x) / (d1max.x - d1min.x)),
                             sample1d(table1d, 1, (x.y - d1min.y) / (d1max.y - d1min.y)),
                             sample1d(table1d, 2, (x.z - d1min.z) / (d1max.z - d1min.z))};
                }
                if (native.valid()) {
                    const Vec3 idx{(x.x - d3min.x) / (d3max.x - d3min.x), (x.y - d3min.y) / (d3max.y - d3min.y),
                                   (x.z - d3min.z) / (d3max.z - d3min.z)};
                    x = kernels::lut_sample_tetrahedral(native.data.data(), native.size, idx);
                }
                baked.data[kernels::lut_index(r, g, b, size)] = x;
            }
        }
    }
    out = std::move(baked);
    out.title = title;
    result.resampled = true;
    result.ok = true;
    return true;
}

bool lut_load_cube(const char* path, Lut3D& out, CubeParseResult& result, u32 target_size) {
    result = {};
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error = std::string("cannot open ") + (path != nullptr ? path : "(null)");
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return lut_parse_cube(ss.str(), out, result, target_size);
}

std::string lut_write_cube(const Lut3D& lut) {
    std::string s;
    s.reserve(lut.data.size() * 40u + 128u);
    s += "# FUSE look LUT (.cube, Adobe Cube LUT Specification 1.0)\n";
    s += "TITLE \"" + lut.title + "\"\n";
    s += "LUT_3D_SIZE " + std::to_string(lut.size) + "\n";
    s += "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n";
    char buf[96];
    for (const Vec3& v : lut.data) {
        std::snprintf(buf, sizeof(buf), "%.9g %.9g %.9g\n", static_cast<double>(v.x), static_cast<double>(v.y),
                      static_cast<double>(v.z));
        s += buf;
    }
    return s;
}

void lut_resample(const Lut3D& src, u32 size, Lut3D& out) {
    size = std::clamp(size, kLutMinSize, kLutMaxSize);
    Lut3D result;
    result.size = size;
    result.title = src.title;
    result.data.resize(static_cast<size_t>(size) * size * size);
    const f32 inv = 1.f / static_cast<f32>(size - 1u);
    for (u32 b = 0; b < size; ++b) {
        for (u32 g = 0; g < size; ++g) {
            for (u32 r = 0; r < size; ++r) {
                const Vec3 x{static_cast<f32>(r) * inv, static_cast<f32>(g) * inv, static_cast<f32>(b) * inv};
                result.data[kernels::lut_index(r, g, b, size)] = lut_sample(src, x);
            }
        }
    }
    out = std::move(result);
}

bool lut_blend(const Lut3D* const* luts, const f32* weights, u32 count, Lut3D& out, kernel::Backend backend) {
    if (count == 0u || count > kernels::kMaxLutBlend || luts == nullptr || weights == nullptr || luts[0] == nullptr) {
        return false;
    }
    const u32 size = luts[0]->size;
    kernels::LutBlendParams p{};
    for (u32 k = 0; k < count; ++k) {
        if (luts[k] == nullptr || !luts[k]->valid() || luts[k]->size != size || luts[k] == &out) {
            return false;
        }
        p.src[k] = luts[k]->span();
        p.weight[k] = weights[k];
    }
    const size_t n3 = static_cast<size_t>(size) * size * size;
    if (out.data.size() != n3) {
        out.data.resize(n3);
    }
    out.size = size;
    p.count = count;
    p.dst = {out.data.data(), static_cast<u32>(n3)};
    const kernel::KernelLaunch desc{kernels::LutBlendKernel::kName, kernel::extent1(static_cast<u32>(n3)),
                                    kernels::kLinearWorkgroup};
    return kernel::launch(backend, desc, kernels::LutBlendKernel{}, p).ok;
}

bool lut_lerp(const Lut3D& a, const Lut3D& b, f32 t, Lut3D& out, kernel::Backend backend) {
    const Lut3D* luts[2] = {&a, &b};
    const f32 w[2] = {1.f - t, t};
    return lut_blend(luts, w, 2u, out, backend);
}

namespace {

void planckXy(double t, double& x, double& y) {
    t = std::clamp(t, 1667.0, 25000.0);
    const double t2 = t * t;
    const double t3 = t2 * t;
    if (t <= 4000.0) {
        x = -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910;
    } else {
        x = -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    }
    const double x2 = x * x;
    const double x3 = x2 * x;
    if (t <= 2222.0) {
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    }
}

constexpr double kD65x = 0.31271;
constexpr double kD65y = 0.32902;

} // namespace

void white_balance_source_xy(f32 temperature_k, f32 tint, double& x, double& y) {
    double px, py, rx, ry;
    planckXy(temperature_k, px, py);
    planckXy(6504.0, rx, ry);
    // Offset the Planckian locus so 6504 K lands exactly on D65 (the Rec.709 white).
    x = px + (kD65x - rx);
    y = py + (kD65y - ry) + 0.02 * static_cast<double>(tint);
}

void white_balance_matrix(f32 temperature_k, f32 tint, f32 out[9]) {
    // Linear Rec.709 <-> XYZ (D65) and the Bradford cone response.
    static constexpr double kRgbToXyz[9] = {0.4124564, 0.3575761, 0.1804375, 0.2126729, 0.7151522,
                                            0.0721750, 0.0193339, 0.1191920, 0.9503041};
    static constexpr double kXyzToRgb[9] = {3.2404542, -1.5371385, -0.4985314, -0.9692660, 1.8760108,
                                            0.0415560, 0.0556434,  -0.2040259, 1.0572252};
    static constexpr double kBradford[9] = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
    static constexpr double kBradfordInv[9] = {0.9869929, -0.1470543, 0.1599627, 0.4323053, 0.5183603,
                                               0.0492912, -0.0085287, 0.0400428, 0.9684867};
    const auto mul = [](const double* a, const double* b, double* o) {
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
            }
        }
    };
    const auto lms_of = [](double x, double y, double lms[3]) {
        const double X = x / y;
        const double Z = (1.0 - x - y) / y;
        lms[0] = kBradford[0] * X + kBradford[1] + kBradford[2] * Z;
        lms[1] = kBradford[3] * X + kBradford[4] + kBradford[5] * Z;
        lms[2] = kBradford[6] * X + kBradford[7] + kBradford[8] * Z;
    };
    double xs, ys;
    white_balance_source_xy(temperature_k, tint, xs, ys);
    const double xr = kD65x;
    const double yr = kD65y;
    double src[3], ref[3];
    lms_of(xs, ys, src);
    lms_of(xr, yr, ref);
    const double diag[9] = {ref[0] / src[0], 0, 0, 0, ref[1] / src[1], 0, 0, 0, ref[2] / src[2]};
    double t0[9], t1[9], t2[9], t3[9];
    mul(kBradford, kRgbToXyz, t0);
    mul(diag, t0, t1);
    mul(kBradfordInv, t1, t2);
    mul(kXyzToRgb, t2, t3);
    for (int i = 0; i < 9; ++i) {
        out[i] = static_cast<f32>(t3[i]);
    }
}

kernels::GradeParams make_grade_params(const LookResolved& look) {
    kernels::GradeParams g{};
    const auto& gr = look.grade;
    if (!gr.enabled) {
        return g;
    }
    if (gr.temperature_k != 6504.f || gr.tint != 0.f) {
        white_balance_matrix(gr.temperature_k, gr.tint, g.wb);
        g.flags |= kernels::kGradeWhiteBalance;
    }
    g.lift = gr.lift;
    g.gain = gr.gain;
    if (gr.lift.x != 0.f || gr.lift.y != 0.f || gr.lift.z != 0.f || gr.gain.x != 1.f || gr.gain.y != 1.f ||
        gr.gain.z != 1.f) {
        g.flags |= kernels::kGradeLiftGain;
    }
    if (gr.gamma.x != 1.f || gr.gamma.y != 1.f || gr.gamma.z != 1.f) {
        g.inv_gamma = Vec3{1.f / std::max(gr.gamma.x, 1e-3f), 1.f / std::max(gr.gamma.y, 1e-3f),
                           1.f / std::max(gr.gamma.z, 1e-3f)};
        g.flags |= kernels::kGradeGamma;
    }
    g.contrast = gr.contrast;
    g.contrast_pivot = std::max(gr.contrast_pivot, 1e-3f);
    if (gr.contrast != 1.f) {
        g.flags |= kernels::kGradeContrast;
    }
    g.saturation = gr.saturation;
    if (gr.saturation != 1.f) {
        g.flags |= kernels::kGradeSaturation;
    }
    bool curveIdentity = true;
    for (u32 i = 0; i < 5u; ++i) {
        const f32 x = 0.25f * static_cast<f32>(i);
        g.curve_p[i] = gr.curve[i];
        curveIdentity = curveIdentity && gr.curve[i].x == x && gr.curve[i].y == x && gr.curve[i].z == x;
    }
    if (!curveIdentity) {
        g.flags |= kernels::kGradeCurves;
        // Fritsch-Carlson monotone tangents per channel.
        for (u32 ch = 0; ch < 3u; ++ch) {
            f32 p[5], d[4], m[5];
            for (u32 i = 0; i < 5u; ++i) {
                p[i] = ch == 0u ? gr.curve[i].x : (ch == 1u ? gr.curve[i].y : gr.curve[i].z);
            }
            for (u32 k = 0; k < 4u; ++k) {
                d[k] = (p[k + 1u] - p[k]) / 0.25f;
            }
            m[0] = d[0];
            m[4] = d[3];
            for (u32 k = 1; k < 4u; ++k) {
                m[k] = (d[k - 1u] * d[k] <= 0.f) ? 0.f : 0.5f * (d[k - 1u] + d[k]);
            }
            for (u32 k = 0; k < 4u; ++k) {
                if (d[k] == 0.f) {
                    m[k] = 0.f;
                    m[k + 1u] = 0.f;
                    continue;
                }
                const f32 a = m[k] / d[k];
                const f32 b = m[k + 1u] / d[k];
                const f32 s = a * a + b * b;
                if (s > 9.f) {
                    const f32 tau = 3.f / std::sqrt(s);
                    m[k] = tau * a * d[k];
                    m[k + 1u] = tau * b * d[k];
                }
            }
            for (u32 i = 0; i < 5u; ++i) {
                (ch == 0u ? g.curve_m[i].x : (ch == 1u ? g.curve_m[i].y : g.curve_m[i].z)) = m[i];
            }
        }
    }
    return g;
}

void lut_generate_grade(const LookResolved& look, u32 size, Lut3D& out, kernel::Backend backend) {
    size = std::clamp(size, kLutMinSize, kLutMaxSize);
    const size_t n3 = static_cast<size_t>(size) * size * size;
    if (out.data.size() != n3) {
        out.data.resize(n3);
    }
    out.size = size;
    kernels::LutBakeParams p{};
    p.dst = {out.data.data(), static_cast<u32>(n3)};
    p.n = size;
    p.grade = make_grade_params(look);
    const kernel::KernelLaunch desc{kernels::LutBakeKernel::kName, kernel::extent1(static_cast<u32>(n3)),
                                    kernels::kLinearWorkgroup};
    kernel::launch(backend, desc, kernels::LutBakeKernel{}, p);
}

} // namespace fuse::renderer::look
