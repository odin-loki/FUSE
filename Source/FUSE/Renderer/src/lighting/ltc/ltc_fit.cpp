// WP-2.2 offline tool: fits the linearly transformed cosine (LTC) table of FUSE's specular lobe and
// writes src/lighting/ltc/ltc_lut_data.inc. Not part of the engine build (target fuse_ltc_fit,
// EXCLUDE_FROM_ALL); rerun it only when the BRDF changes:
//
//   cmake --build <tree> --target fuse_ltc_fit && <tree>/.../fuse_ltc_fit <path>/ltc_lut_data.inc
//
// Method: Heitz, Dupuy, Hill, Neubelt, "Real-Time Polygonal-Light Shading with Linearly Transformed
// Cosines", SIGGRAPH 2016, section 4 (the fitting procedure of its reference code, re-implemented here):
// for each (perceptual roughness r, t = sqrt(1 - cos(theta_v))) on a 64 x 64 grid, from the roughest
// row to the smoothest, fit M = [X Y Z] * [[m11, 0, m13], [0, m22, 0], [0, 0, 1]] with Z the lobe's
// average direction, minimising sum |f_brdf cos - f_ltc|^3 / (pdf_brdf + pdf_ltc) over 32 x 32
// stratified samples drawn from both distributions (multiple importance sampling), with Nelder-Mead;
// each fit starts from its neighbour's result. The BRDF is exactly shaders/common/brdf.glsl's
// specular lobe with F = 1: GGX D, height-correlated Smith visibility, alpha = max(r, 0.045)^2.
// The table stores M^-1 normalised so that M^-1[1][1] = 1 (4 floats per texel, row-major
// (m00, m02, m20, m22)); normalisation does not change the transformed distribution.
//
// Output data are FUSE's own (computed by this program; no third-party data), same licence as FUSE.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kN = 64;
constexpr int kSamples = 32;
constexpr double kPi = 3.14159265358979323846;
constexpr double kMinRoughness = 0.045;

struct V3 {
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalize(V3 a) { return a * (1.0 / length(a)); }

/// Row-major 3x3.
struct M3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    V3 operator*(V3 v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
};
M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        }
    }
    return r;
}
double det(const M3& a) {
    return a.m[0][0] * (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) -
           a.m[0][1] * (a.m[1][0] * a.m[2][2] - a.m[1][2] * a.m[2][0]) +
           a.m[0][2] * (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]);
}
M3 inverse(const M3& a) {
    const double d = det(a);
    M3 r{};
    r.m[0][0] = (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) / d;
    r.m[0][1] = (a.m[0][2] * a.m[2][1] - a.m[0][1] * a.m[2][2]) / d;
    r.m[0][2] = (a.m[0][1] * a.m[1][2] - a.m[0][2] * a.m[1][1]) / d;
    r.m[1][0] = (a.m[1][2] * a.m[2][0] - a.m[1][0] * a.m[2][2]) / d;
    r.m[1][1] = (a.m[0][0] * a.m[2][2] - a.m[0][2] * a.m[2][0]) / d;
    r.m[1][2] = (a.m[0][2] * a.m[1][0] - a.m[0][0] * a.m[1][2]) / d;
    r.m[2][0] = (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]) / d;
    r.m[2][1] = (a.m[0][1] * a.m[2][0] - a.m[0][0] * a.m[2][1]) / d;
    r.m[2][2] = (a.m[0][0] * a.m[1][1] - a.m[0][1] * a.m[1][0]) / d;
    return r;
}

// --- the BRDF (brdf.glsl specular lobe, F = 1), times cos --------------------------------------
struct Eval {
    double value = 0; ///< f cos
    double pdf = 0;   ///< GGX NDF sampling pdf of L
};

Eval brdfEval(V3 v, V3 l, double alpha) {
    Eval e{};
    if (v.z <= 0.0 || l.z <= 0.0) {
        return e;
    }
    const V3 h = normalize(v + l);
    const double a2 = alpha * alpha;
    const double noh = std::max(h.z, 0.0);
    const double voh = std::max(dot(v, h), 0.0);
    const double dd = (noh * a2 - noh) * noh + 1.0;
    const double d = a2 / (kPi * dd * dd);
    const double nov = std::max(v.z, 1e-4);
    const double nol = l.z;
    const double gv = nol * std::sqrt(nov * nov * (1.0 - a2) + a2);
    const double gl = nov * std::sqrt(nol * nol * (1.0 - a2) + a2);
    const double vis = 0.5 / std::max(gv + gl, 1e-5);
    e.value = d * vis * nol;
    e.pdf = voh > 0.0 ? d * noh / (4.0 * voh) : 0.0;
    return e;
}

V3 brdfSample(V3 v, double alpha, double u1, double u2) {
    const double phi = 2.0 * kPi * u1;
    const double a2 = alpha * alpha;
    const double c2 = (1.0 - u2) / (1.0 + (a2 - 1.0) * u2);
    const double ct = std::sqrt(std::max(c2, 0.0));
    const double st = std::sqrt(std::max(1.0 - c2, 0.0));
    const V3 h{st * std::cos(phi), st * std::sin(phi), ct};
    return h * (2.0 * dot(v, h)) - v;
}

// --- the LTC ---------------------------------------------------------------------------------------
struct Ltc {
    double magnitude = 1.0;
    double m11 = 1.0, m22 = 1.0, m13 = 0.0;
    V3 X{1, 0, 0}, Y{0, 1, 0}, Z{0, 0, 1};
    M3 M{}, invM{};
    double detM = 1.0;

    void update() {
        M3 basis{};
        basis.m[0][0] = X.x, basis.m[0][1] = Y.x, basis.m[0][2] = Z.x;
        basis.m[1][0] = X.y, basis.m[1][1] = Y.y, basis.m[1][2] = Z.y;
        basis.m[2][0] = X.z, basis.m[2][1] = Y.z, basis.m[2][2] = Z.z;
        M3 s{};
        s.m[0][0] = m11, s.m[0][1] = 0, s.m[0][2] = m13;
        s.m[1][0] = 0, s.m[1][1] = m22, s.m[1][2] = 0;
        s.m[2][0] = 0, s.m[2][1] = 0, s.m[2][2] = 1;
        M = mul(basis, s);
        invM = inverse(M);
        detM = std::fabs(det(M));
    }
    double eval(V3 l) const {
        const V3 lo = normalize(invM * l);
        const V3 lm = M * lo;
        const double len = length(lm);
        const double jacobian = detM / (len * len * len);
        const double d = std::max(0.0, lo.z) / kPi;
        return magnitude * d / jacobian;
    }
    V3 sample(double u1, double u2) const {
        const double theta = std::acos(std::sqrt(u1));
        const double phi = 2.0 * kPi * u2;
        return normalize(M * V3{std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)});
    }
};

double computeError(const Ltc& ltc, V3 v, double alpha) {
    double error = 0.0;
    for (int j = 0; j < kSamples; ++j) {
        for (int i = 0; i < kSamples; ++i) {
            const double u1 = (i + 0.5) / kSamples;
            const double u2 = (j + 0.5) / kSamples;
            for (int pass = 0; pass < 2; ++pass) {
                const V3 l = pass == 0 ? ltc.sample(u1, u2) : brdfSample(v, alpha, u1, u2);
                const Eval b = brdfEval(v, l, alpha);
                const double evalLtc = ltc.eval(l);
                const double pdfLtc = evalLtc / ltc.magnitude;
                double e = std::fabs(b.value - evalLtc);
                e = e * e * e;
                const double denom = pdfLtc + b.pdf;
                if (denom > 0.0) {
                    error += e / denom;
                }
            }
        }
    }
    return error / (kSamples * kSamples);
}

/// Norm (integral of f cos) and average direction of the lobe, importance sampled.
void averageTerms(V3 v, double alpha, double& norm, V3& avg) {
    norm = 0.0;
    avg = {};
    for (int j = 0; j < kSamples; ++j) {
        for (int i = 0; i < kSamples; ++i) {
            const double u1 = (i + 0.5) / kSamples;
            const double u2 = (j + 0.5) / kSamples;
            const V3 l = brdfSample(v, alpha, u1, u2);
            const Eval b = brdfEval(v, l, alpha);
            if (b.pdf <= 0.0) {
                continue;
            }
            const double w = b.value / b.pdf;
            norm += w;
            avg = avg + l * w;
        }
    }
    norm /= kSamples * kSamples;
    avg.y = 0.0;
    avg = normalize(avg);
}

/// Nelder-Mead minimisation in 3 dimensions.
template <typename F>
std::array<double, 3> nelderMead(std::array<double, 3> start, double delta, double tolerance, int maxIters, F f) {
    constexpr int kD = 3;
    std::array<std::array<double, kD>, kD + 1> x{};
    std::array<double, kD + 1> fx{};
    for (int i = 0; i <= kD; ++i) {
        x[i] = start;
        if (i > 0) {
            x[i][i - 1] += delta;
        }
        fx[i] = f(x[i]);
    }
    for (int iter = 0; iter < maxIters; ++iter) {
        int lo = 0, hi = 0, nh = 0;
        for (int i = 0; i <= kD; ++i) {
            if (fx[i] < fx[lo]) lo = i;
            if (fx[i] > fx[hi]) hi = i;
        }
        nh = lo;
        for (int i = 0; i <= kD; ++i) {
            if (i != hi && fx[i] > fx[nh]) nh = i;
        }
        const double range = std::fabs(fx[hi] - fx[lo]);
        const double scale = std::fabs(fx[hi]) + std::fabs(fx[lo]);
        if (range <= tolerance * scale + 1e-30) {
            break;
        }
        std::array<double, kD> c{};
        for (int i = 0; i <= kD; ++i) {
            if (i == hi) continue;
            for (int k = 0; k < kD; ++k) c[k] += x[i][k] / kD;
        }
        auto along = [&](double t) {
            std::array<double, kD> p{};
            for (int k = 0; k < kD; ++k) p[k] = c[k] + t * (x[hi][k] - c[k]);
            return p;
        };
        const std::array<double, kD> xr = along(-1.0);
        const double fr = f(xr);
        if (fr < fx[lo]) {
            const std::array<double, kD> xe = along(-2.0);
            const double fe = f(xe);
            if (fe < fr) {
                x[hi] = xe, fx[hi] = fe;
            } else {
                x[hi] = xr, fx[hi] = fr;
            }
        } else if (fr < fx[nh]) {
            x[hi] = xr, fx[hi] = fr;
        } else {
            const bool outside = fr < fx[hi];
            const std::array<double, kD> xc = along(outside ? -0.5 : 0.5);
            const double fc = f(xc);
            if (fc < (outside ? fr : fx[hi])) {
                x[hi] = xc, fx[hi] = fc;
            } else {
                for (int i = 0; i <= kD; ++i) {
                    if (i == lo) continue;
                    for (int k = 0; k < kD; ++k) x[i][k] = x[lo][k] + 0.5 * (x[i][k] - x[lo][k]);
                    fx[i] = f(x[i]);
                }
            }
        }
    }
    int best = 0;
    for (int i = 1; i <= kD; ++i) {
        if (fx[i] < fx[best]) best = i;
    }
    return x[best];
}

void applyParams(Ltc& ltc, const std::array<double, 3>& p, bool isotropic) {
    const double m11 = std::max(p[0], 1e-7);
    const double m22 = std::max(p[1], 1e-7);
    if (isotropic) {
        ltc.m11 = m11;
        ltc.m22 = m11;
        ltc.m13 = 0.0;
    } else {
        ltc.m11 = m11;
        ltc.m22 = m22;
        ltc.m13 = p[2];
    }
    ltc.update();
}

} // namespace

int main(int argc, char** argv) {
    const char* outPath = argc > 1 ? argv[1] : "ltc_lut_data.inc";
    std::vector<M3> tab(static_cast<size_t>(kN) * kN);
    Ltc ltc{};
    for (int a = kN - 1; a >= 0; --a) {
        const double r = std::max(static_cast<double>(a) / (kN - 1), kMinRoughness);
        const double alpha = r * r;
        for (int t = 0; t < kN; ++t) {
            const double x = static_cast<double>(t) / (kN - 1);
            const double ct = 1.0 - x * x;
            const double theta = std::min(1.57, std::acos(ct));
            const V3 v{std::sin(theta), 0.0, std::cos(theta)};
            V3 avg{};
            averageTerms(v, alpha, ltc.magnitude, avg);
            bool isotropic = false;
            if (t == 0) {
                ltc.X = {1, 0, 0};
                ltc.Y = {0, 1, 0};
                ltc.Z = {0, 0, 1};
                if (a == kN - 1) {
                    ltc.m11 = 1.0;
                    ltc.m22 = 1.0;
                } else {
                    ltc.m11 = tab[static_cast<size_t>(a + 1) * kN + t].m[0][0];
                    ltc.m22 = tab[static_cast<size_t>(a + 1) * kN + t].m[1][1];
                }
                ltc.m13 = 0.0;
                isotropic = true;
            } else {
                ltc.X = {avg.z, 0.0, -avg.x};
                ltc.Y = {0, 1, 0};
                ltc.Z = avg;
            }
            ltc.update();
            const std::array<double, 3> start{ltc.m11, ltc.m22, ltc.m13};
            const std::array<double, 3> best = nelderMead(start, 0.05, 1e-5, 100, [&](const std::array<double, 3>& p) {
                Ltc trial = ltc;
                applyParams(trial, p, isotropic);
                return computeError(trial, v, alpha);
            });
            applyParams(ltc, best, isotropic);
            tab[static_cast<size_t>(a) * kN + t] = ltc.M;
        }
        std::fprintf(stderr, "roughness row %d / %d\n", kN - a, kN);
    }

    std::FILE* f = std::fopen(outPath, "w");
    if (f == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", outPath);
        return 1;
    }
    std::fprintf(f,
                 "// FUSE LTC table (WP-2.2): M^-1 of the linearly transformed cosine fitted to FUSE's specular lobe.\n"
                 "// Generated by Source/FUSE/Renderer/src/lighting/ltc/ltc_fit.cpp. Do not edit.\n"
                 "//\n"
                 "// Provenance: FUSE's own fit. No third-party data. The procedure follows Heitz, Dupuy, Hill and\n"
                 "// Neubelt, \"Real-Time Polygonal-Light Shading with Linearly Transformed Cosines\" (SIGGRAPH 2016),\n"
                 "// re-implemented in ltc_fit.cpp against shaders/common/brdf.glsl (GGX, height-correlated Smith, F = 1).\n"
                 "// Licence: same as FUSE.\n"
                 "//\n"
                 "// Layout: %d (perceptual roughness r = j / %d, clamped to 0.045) x %d (t = sqrt(1 - N.V) = i / %d)\n"
                 "// texels, row-major [j][i], 4 IEEE-754 binary32 words each: M^-1 row-major (m00, m02, m20, m22)\n"
                 "// with m11 = 1 and the other entries 0, in the frame (T1 = V projected on the tangent plane,\n"
                 "// T2 = N x T1, N).\n",
                 kN, kN - 1, kN, kN - 1);
    int column = 0;
    for (int j = 0; j < kN; ++j) {
        for (int i = 0; i < kN; ++i) {
            const M3 inv = inverse(tab[static_cast<size_t>(j) * kN + i]);
            const double s = inv.m[1][1];
            const float v[4] = {static_cast<float>(inv.m[0][0] / s), static_cast<float>(inv.m[0][2] / s),
                                static_cast<float>(inv.m[2][0] / s), static_cast<float>(inv.m[2][2] / s)};
            for (float w : v) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &w, 4);
                std::fprintf(f, "0x%08xu,%s", bits, ++column % 8 == 0 ? "\n" : "");
            }
        }
    }
    std::fclose(f);
    return 0;
}
