// FUSE Relight RL-4.3 CPU gates for the Relight BSDF (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.8, row
// `bsdf_eval/sample/pdf`). One executable, one suite per ctest (rl_bsdf_<suite>):
//
//   lut          the Kulla-Conty albedo table: ranges, CpuReference == CpuParallel bake, and (when the renderer is in
//                the tree) the Schlick split against WP-2.2's DFG LUT (same quantity, independent bake)
//   furnace      white furnace: albedo 1 -> 1 +- 1% (opaque Lambert at every roughness / metallic / opacity, thick and
//                thin dielectrics, the diffuse layer at full coverage, hair with sigma_a = 0), by sampling (mean
//                weight) and, for the non-dirac lobes, by quadrature of bsdfEval; the non-conserving options
//                (Burley, Hammon, thin film, thin SSS, anisotropy) are reported with their measured albedo
//   reciprocity  f(wo, wi) == f(wi, wo) (<= 1e-4 relative) for every non-hair model; dirac reflection / transmission
//                throughputs equal in both directions; hair is not reciprocal (Chiang 2016) and is reported only
//   chi2         Pearson chi^2 of 400k bsdfSample directions against bsdfPdf integrated over 24 x 48 (theta, phi)
//                bins (+ one bin for dirac and rejected samples), p >= 0.01 (Sidak-corrected over all tests)
//   pdf          the pdf integrates to the valid non-dirac sampling fraction (every model), to 1 where nothing is
//                rejected (hair, pure cosine lobes); SSS radius and Henyey-Greenstein densities integrate to 1
//   sss          Burley normalized diffusion: profile normalization, Golubev inverse CDF round trip, radius chi^2;
//                Henyey-Greenstein sampling chi^2; the SSS flag on diffusion samples
//   thinfilm     Belcour-Barla reflectance: zero thickness == the bare interface, matched indices == no
//                interference, values in [0, 1], iridescence varies with thickness
//   parity       bsdf_eval / bsdf_pdf / bsdf_sample: CpuReference == CpuParallel (0, 2, 4 workers) bit-exact, stats
//                names and item counts, a Cuda request without a device falls back to CpuParallel
//   mapping      RL-3.2 MaterialParams -> BsdfMaterial for Opaque / Translucent / Portal (+ HairCards), texture
//                slots, the packed GPU layout round trip
#include "bsdf_test_common.hpp"

#include <fuse/compute_kernel/stats.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/relight/render/material/material_bsdf.hpp>
#if defined(FUSE_RL_BSDF_HAS_DFG)
#include <fuse/renderer/lighting/ltc/ltc_kernel.hpp>
#include <fuse/renderer/lighting/ltc/ltc_lut.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace rl_bsdf_test;
namespace rm = fuse::relight::render::material;
namespace kernel = fuse::kernel;

int g_failures = 0;

void check(bool ok, const char* fmt, ...) {
    if (ok) {
        return;
    }
    ++g_failures;
    std::fprintf(stderr, "FAIL: ");
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
}

constexpr double kPi = 3.14159265358979323846;

const float* lut() { return rm::sharedAlbedoLut().data(); }

float3 eval(const BsdfMaterial& m, const float3& wo, const float3& wi) { return bsdf::bsdfEval(lut(), m, wo, wi); }
float pdf(const BsdfMaterial& m, const float3& wo, const float3& wi) { return bsdf::bsdfPdf(lut(), m, wo, wi); }
bsdf::BsdfSample sampleDir(const BsdfMaterial& m, const float3& wo, const float4& u) {
    return bsdf::bsdfSample(lut(), m, wo, u);
}

double lum(const float3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }
bool finite3(const float3& c) { return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z); }

/// Integrates g(w) over the sphere with a midpoint grid uniform in (theta, phi).
template <typename F>
void integrateSphere(int nTheta, int nPhi, F&& g) {
    for (int t = 0; t < nTheta; ++t) {
        const double theta = (t + 0.5) * kPi / nTheta;
        const double dOmega = std::sin(theta) * (kPi / nTheta) * (2.0 * kPi / nPhi);
        for (int p = 0; p < nPhi; ++p) {
            const double phi = (p + 0.5) * 2.0 * kPi / nPhi;
            const float3 w = dirFromAngles(float(theta), float(phi));
            g(w, dOmega);
        }
    }
}

/// Mean sample weight (luminance) and the throughput of dirac samples, N samples.
struct SampleStats {
    double meanWeight = 0.0;  ///< E[weight] over all samples (invalid = 0): the directional albedo
    double stderrWeight = 0.0;
    double validNonDelta = 0.0;
    double delta = 0.0;
    bool finite = true;
};

SampleStats sampleStats(const BsdfMaterial& m, const float3& wo, int n, std::uint64_t seed) {
    Rng rng(seed);
    SampleStats s;
    double sum = 0.0;
    double sum2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const bsdf::BsdfSample smp = sampleDir(m, wo, rng.uniform4());
        double w = 0.0;
        if ((smp.flags & bsdf::kBsdfSampleValid) != 0u) {
            s.finite = s.finite && finite3(smp.weight) && std::isfinite(smp.pdf);
            w = lum(smp.weight);
            if ((smp.flags & bsdf::kBsdfSampleDelta) != 0u) {
                s.delta += 1.0;
            } else {
                s.validNonDelta += 1.0;
            }
        }
        sum += w;
        sum2 += w * w;
    }
    s.meanWeight = sum / n;
    s.stderrWeight = std::sqrt(std::max(0.0, sum2 / n - s.meanWeight * s.meanWeight) / n);
    s.validNonDelta /= n;
    s.delta /= n;
    return s;
}

/// Quadrature of the projected eval over the sphere (the non-dirac directional albedo).
double evalAlbedo(const BsdfMaterial& m, const float3& wo, int nTheta, int nPhi) {
    double sum = 0.0;
    integrateSphere(nTheta, nPhi, [&](const float3& wi, double dOmega) { sum += lum(eval(m, wo, wi)) * dOmega; });
    return sum;
}

double pdfIntegral(const BsdfMaterial& m, const float3& wo, int nTheta, int nPhi) {
    double sum = 0.0;
    integrateSphere(nTheta, nPhi, [&](const float3& wi, double dOmega) { sum += double(pdf(m, wo, wi)) * dOmega; });
    return sum;
}

// ---- chi^2 helpers ---------------------------------------------------------------------------------------------

/// Regularized upper incomplete gamma Q(a, x) (Numerical Recipes gser / gcf).
double gammaQ(double a, double x) {
    if (x <= 0.0) {
        return 1.0;
    }
    const double gln = std::lgamma(a);
    if (x < a + 1.0) {
        double ap = a;
        double del = 1.0 / a;
        double sum = del;
        for (int n = 0; n < 1000; ++n) {
            ap += 1.0;
            del *= x / ap;
            sum += del;
            if (std::fabs(del) < std::fabs(sum) * 1e-15) {
                break;
            }
        }
        return 1.0 - sum * std::exp(-x + a * std::log(x) - gln);
    }
    double b = x + 1.0 - a;
    double c = 1.0 / 1e-300;
    double d = 1.0 / b;
    double h = d;
    for (int i = 1; i < 1000; ++i) {
        const double an = -i * (i - a);
        b += 2.0;
        d = an * d + b;
        if (std::fabs(d) < 1e-300) {
            d = 1e-300;
        }
        c = b + an / c;
        if (std::fabs(c) < 1e-300) {
            c = 1e-300;
        }
        d = 1.0 / d;
        const double del = d * c;
        h *= del;
        if (std::fabs(del - 1.0) < 1e-15) {
            break;
        }
    }
    return std::exp(-x + a * std::log(x) - gln) * h;
}

/// Pearson chi^2 with bins of expected count < 5 pooled (Mitsuba's chi2 test convention). Returns the p-value.
double chi2PValue(const std::vector<double>& observed, const std::vector<double>& expected, int* dofOut, double* chi2Out) {
    std::vector<std::size_t> order(observed.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return expected[a] < expected[b]; });
    double pooledObs = 0.0;
    double pooledExp = 0.0;
    double chi2 = 0.0;
    int bins = 0;
    for (std::size_t k : order) {
        if (expected[k] <= 0.0 && observed[k] <= 0.0) {
            continue;
        }
        if (expected[k] < 5.0) {
            pooledObs += observed[k];
            pooledExp += expected[k];
            continue;
        }
        if (pooledExp > 0.0 && pooledExp < 5.0) {
            // Fold the pool into this bin.
            const double o = observed[k] + pooledObs;
            const double e = expected[k] + pooledExp;
            chi2 += (o - e) * (o - e) / e;
            pooledObs = 0.0;
            pooledExp = 0.0;
        } else {
            chi2 += (observed[k] - expected[k]) * (observed[k] - expected[k]) / expected[k];
        }
        ++bins;
    }
    if (pooledExp >= 5.0) {
        chi2 += (pooledObs - pooledExp) * (pooledObs - pooledExp) / pooledExp;
        ++bins;
    } else if (pooledObs > 0.0 && pooledExp <= 0.0) {
        chi2 = 1e30; // samples where the pdf is zero
    }
    const int dof = std::max(bins - 1, 1);
    *dofOut = dof;
    *chi2Out = chi2;
    return gammaQ(0.5 * dof, 0.5 * chi2);
}

constexpr int kChiTheta = 24;
constexpr int kChiPhi = 48;
constexpr int kChiSub = 12;
constexpr int kChiSamples = 400000;

struct ChiResult {
    double p = 1.0;
    double chi2 = 0.0;
    int dof = 0;
    double pdfIntegral = 0.0;
    double validFraction = 0.0;
    double deltaFraction = 0.0;
};

ChiResult chiSquare(const BsdfMaterial& m, const float3& wo, std::uint64_t seed) {
    const int nBins = kChiTheta * kChiPhi;
    std::vector<double> expected(std::size_t(nBins) + 1u, 0.0);
    std::vector<double> observed(std::size_t(nBins) + 1u, 0.0);
    double integral = 0.0;
    for (int t = 0; t < kChiTheta * kChiSub; ++t) {
        const double theta = (t + 0.5) * kPi / (kChiTheta * kChiSub);
        const double dOmega = std::sin(theta) * (kPi / (kChiTheta * kChiSub)) * (2.0 * kPi / (kChiPhi * kChiSub));
        for (int p = 0; p < kChiPhi * kChiSub; ++p) {
            const double phi = (p + 0.5) * 2.0 * kPi / (kChiPhi * kChiSub);
            const double v = double(pdf(m, wo, dirFromAngles(float(theta), float(phi)))) * dOmega;
            expected[std::size_t((t / kChiSub) * kChiPhi + p / kChiSub)] += v;
            integral += v;
        }
    }
    Rng rng(seed);
    ChiResult r;
    for (int i = 0; i < kChiSamples; ++i) {
        const bsdf::BsdfSample s = sampleDir(m, wo, rng.uniform4());
        if ((s.flags & bsdf::kBsdfSampleValid) == 0u || (s.flags & bsdf::kBsdfSampleDelta) != 0u) {
            observed[std::size_t(nBins)] += 1.0;
            r.deltaFraction += (s.flags & bsdf::kBsdfSampleDelta) != 0u ? 1.0 : 0.0;
            continue;
        }
        const float3 w = s.wi;
        const double theta = std::acos(std::clamp(double(w.z), -1.0, 1.0));
        double phi = std::atan2(double(w.y), double(w.x));
        if (phi < 0.0) {
            phi += 2.0 * kPi;
        }
        const int tb = std::min(int(theta / kPi * kChiTheta), kChiTheta - 1);
        const int pb = std::min(int(phi / (2.0 * kPi) * kChiPhi), kChiPhi - 1);
        observed[std::size_t(tb * kChiPhi + pb)] += 1.0;
        r.validFraction += 1.0;
    }
    for (double& e : expected) {
        e *= kChiSamples;
    }
    expected[std::size_t(nBins)] = std::max(0.0, 1.0 - integral) * kChiSamples;
    r.pdfIntegral = integral;
    r.validFraction /= kChiSamples;
    r.deltaFraction /= kChiSamples;
    r.p = chi2PValue(observed, expected, &r.dof, &r.chi2);
    return r;
}

/// The (wo) directions each config is tested from.
std::vector<float3> testDirections(const Config& c) {
    std::vector<float3> d{dirFromAngles(0.35f, 0.3f), dirFromAngles(1.15f, 2.1f)};
    if (c.twoSided) {
        d.push_back(dirFromAngles(2.3f, 4.0f));
    }
    if (c.hair) {
        d = {normalize(float3(0.3f, 0.8f, 0.5f)), normalize(float3(-0.7f, -0.2f, 0.6f)), normalize(float3(0.05f, -0.9f, -0.4f))};
    }
    return d;
}

// ---- suites ---------------------------------------------------------------------------------------------------

void suiteLut() {
    const rm::AlbedoLut& shared = rm::sharedAlbedoLut();
    check(shared.valid(), "albedo table not baked");
    if (!shared.valid()) {
        return;
    }
    const float* t = shared.data();
    const int n = bsdf::kBsdfLutSize;
    bool ok = true;
    for (int i = 0; i < n * n; ++i) {
        const float a = t[bsdf::kBsdfLutA + i];
        const float b = t[bsdf::kBsdfLutB + i];
        ok = ok && std::isfinite(a) && std::isfinite(b) && a >= 0.f && b >= 0.f && a + b <= 1.0005f;
    }
    for (int r = 0; r < n; ++r) {
        const float e = t[bsdf::kBsdfLutAavg + r] + t[bsdf::kBsdfLutBavg + r];
        ok = ok && e > 0.f && e <= 1.0005f;
    }
    check(ok, "table entries out of [0, 1]");
    const float e10 = bsdf::bsdfLutFetch(t, bsdf::kBsdfLutA, 1.f, 0.f) + bsdf::bsdfLutFetch(t, bsdf::kBsdfLutB, 1.f, 0.f);
    const float e11 = bsdf::bsdfLutFetch(t, bsdf::kBsdfLutA, 1.f, 1.f) + bsdf::bsdfLutFetch(t, bsdf::kBsdfLutB, 1.f, 1.f);
    std::printf("E(mu=1, rho=0) = %.5f, E(mu=1, rho=1) = %.5f, E_avg(rho=1) = %.5f\n", e10, e11,
                bsdf::bsdfLutAvg(t, bsdf::kBsdfLutAavg, 1.f) + bsdf::bsdfLutAvg(t, bsdf::kBsdfLutBavg, 1.f));
    check(e10 > 0.995f && e11 < 0.5f && e11 > 0.25f, "E(mu, rho) implausible");

    rm::AlbedoLut ref;
    rm::AlbedoLut par;
    fuse::jobs::JobScheduler::instance().shutdown();
    fuse::jobs::JobScheduler::instance().initialize(3u);
    check(rm::bakeAlbedoLut(ref, kernel::Backend::CpuReference) && rm::bakeAlbedoLut(par, kernel::Backend::CpuParallel),
          "bake failed");
    fuse::jobs::JobScheduler::instance().shutdown();
    check(ref.size() == par.size() && std::memcmp(ref.data(), par.data(), ref.size() * sizeof(float)) == 0,
          "CpuReference and CpuParallel bakes differ");
    std::printf("bake CpuReference == CpuParallel: %s\n",
                std::memcmp(ref.data(), par.data(), ref.size() * sizeof(float)) == 0 ? "bit-exact" : "DIFFERENT");

#if defined(FUSE_RL_BSDF_HAS_DFG)
    // WP-2.2's DFG LUT stores the same Schlick split (A, B) of the height-correlated GGX albedo; the two bakes are
    // independent (quadrature and parameterization differ), so they agree to the tables' interpolation error.
    const auto& dfg = fuse::renderer::ltc::sharedBrdfLut();
    double worst = 0.0;
    for (float mu : {0.25f, 0.5f, 0.75f, 1.f}) {
        for (float rho : {0.3f, 0.5f, 0.7f, 0.9f}) {
            float da = 0.f;
            float db = 0.f;
            fuse::renderer::ltc::sample_dfg(dfg.data(), mu, rho, da, db);
            const float a = bsdf::bsdfLutFetch(t, bsdf::kBsdfLutA, mu, rho);
            const float b = bsdf::bsdfLutFetch(t, bsdf::kBsdfLutB, mu, rho);
            worst = std::max(worst, std::max(std::fabs(double(a - da)), std::fabs(double(b - db))));
        }
    }
    std::printf("vs WP-2.2 DFG LUT: max |dA|, |dB| = %.4f\n", worst);
    check(worst <= 0.02, "albedo table disagrees with the WP-2.2 DFG LUT (%.4f)", worst);
#else
    std::printf("vs WP-2.2 DFG LUT: renderer not in this tree, skipped\n");
#endif
}

void suiteFurnace() {
    const float thetas[] = {0.05f, 0.5f, 1.0f, 1.35f, 1.5f};
    int gated = 0;
    double worst = 0.0;
    // Opaque, Lambert, white: every roughness / metallic / opacity.
    for (float rough : {0.05f, 0.2f, 0.4f, 0.7f, 1.f}) {
        for (float metal : {0.f, 0.5f, 1.f}) {
            for (float opacity : {1.f, 0.5f}) {
                BsdfMaterial m = opaque(1.f, rough, metal);
                m.opacity = opacity;
                for (float th : thetas) {
                    const float3 wo = dirFromAngles(th, 0.7f);
                    const SampleStats s = sampleStats(m, wo, 100000, 11u);
                    const double err = std::fabs(s.meanWeight - 1.0);
                    worst = std::max(worst, err);
                    ++gated;
                    check(s.finite && err <= 0.01, "furnace opaque r=%.2f m=%.1f o=%.1f theta=%.2f: %.4f", rough, metal,
                          opacity, th, s.meanWeight);
                    if (rough >= 0.2f && th <= 1.35f) {
                        const double q = evalAlbedo(m, wo, 256, 512) + (1.0 - opacity);
                        worst = std::max(worst, std::fabs(q - 1.0));
                        check(std::fabs(q - 1.0) <= 0.01, "furnace (quadrature) opaque r=%.2f m=%.1f theta=%.2f: %.4f",
                              rough, metal, th, q);
                    }
                }
            }
        }
    }
    std::printf("opaque Lambert white furnace: %d configurations, worst |albedo - 1| = %.4f\n", gated, worst);

    // Translucent: thick (outside), thin walls, the diffuse layer at full coverage.
    struct TCase {
        const char* name;
        BsdfMaterial m;
    };
    const TCase tcases[] = {{"thick", translucent(false, 0.f)},
                            {"thin", translucent(true, 0.f)},
                            {"layer_d0.5", translucent(false, 0.5f)},
                            {"thin_layer_d1", translucent(true, 1.f)}};
    for (const TCase& tc : tcases) {
        double tw = 0.0;
        for (float th : thetas) {
            const SampleStats s = sampleStats(tc.m, dirFromAngles(th, 1.1f), 100000, 12u);
            tw = std::max(tw, std::fabs(s.meanWeight - 1.0));
        }
        std::printf("translucent %-14s white furnace worst |albedo - 1| = %.5f\n", tc.name, tw);
        check(tw <= 0.01, "furnace translucent %s: %.4f", tc.name, tw);
    }
    {
        // From inside the medium: <= 1 (TIR reflects everything; the layer does not scatter back in).
        double worstInside = 0.0;
        for (float th : thetas) {
            const SampleStats s = sampleStats(translucent(false, 0.5f), dirFromAngles(kPi - th, 0.4f), 50000, 13u);
            worstInside = std::max(worstInside, s.meanWeight);
            const SampleStats s0 = sampleStats(translucent(false, 0.f), dirFromAngles(float(kPi) - th, 0.4f), 50000, 14u);
            check(std::fabs(s0.meanWeight - 1.0) <= 1e-4, "furnace translucent thick from inside: %.5f", s0.meanWeight);
        }
        std::printf("translucent layer from inside: max albedo %.4f (<= 1)\n", worstInside);
        check(worstInside <= 1.0001, "translucent layer from inside gains energy");
    }

    // Hair, sigma_a = 0: the lobes are normalized and A_p sums to 1 (Chiang 2016 section 4).
    double hairWorst = 0.0;
    int hairGated = 0;
    for (float bm : {0.2f, 0.3f, 0.6f}) {
        for (float bn : {0.3f, 0.8f}) {
            for (float h : {-0.8f, 0.f, 0.5f}) {
                for (float alpha : {0.f, 0.0349066f}) {
                    const BsdfMaterial m = hairMaterial(bm, bn, h, float3(0.f), alpha);
                    for (const float3& wo : {normalize(float3(0.3f, 0.8f, 0.5f)), normalize(float3(-0.6f, 0.1f, -0.7f))}) {
                        const double q = evalAlbedo(m, wo, 512, 512);
                        const SampleStats s = sampleStats(m, wo, 20000, 15u);
                        hairWorst = std::max(hairWorst, std::max(std::fabs(q - 1.0), std::fabs(s.meanWeight - 1.0)));
                        ++hairGated;
                        check(std::fabs(q - 1.0) <= 0.01 && std::fabs(s.meanWeight - 1.0) <= 0.01,
                              "furnace hair bm=%.1f bn=%.1f h=%.1f alpha=%.3f: quadrature %.4f sampling %.4f", bm, bn, h,
                              alpha, q, s.meanWeight);
                    }
                }
            }
        }
    }
    std::printf("hair (sigma_a = 0) white furnace: %d configurations, worst |albedo - 1| = %.4f\n", hairGated, hairWorst);

    // Reported only (not energy-conserving by construction, or approximate energy compensation).
    auto report = [&](const char* name, const BsdfMaterial& m) {
        double lo = 1e9;
        double hi = 0.0;
        for (float th : thetas) {
            const SampleStats s = sampleStats(m, dirFromAngles(th, 0.2f), 50000, 16u);
            check(s.finite, "%s: non-finite sample", name);
            lo = std::min(lo, s.meanWeight);
            hi = std::max(hi, s.meanWeight);
        }
        std::printf("reported %-28s albedo in [%.4f, %.4f]\n", name, lo, hi);
        return hi;
    };
    {
        BsdfMaterial m = opaque(1.f, 0.5f, 0.f);
        m.diffuseModel = bsdf::kBsdfDiffuseBurley;
        report("opaque Burley white", m);
        m.diffuseModel = bsdf::kBsdfDiffuseHammon;
        report("opaque Hammon (upstream) white", m);
    }
    {
        // Anisotropy: the albedo table is indexed by the projected roughness of each direction (exact masking,
        // approximate albedo). Gated at 2.5% up to |a| = 0.5 over every view azimuth; stronger anisotropy is reported.
        double worstAniso = 0.0;
        for (float rough : {0.2f, 0.5f, 0.8f, 1.f}) {
            for (float an : {0.3f, -0.5f, 0.5f}) {
                BsdfMaterial m = opaque(1.f, rough, 0.f);
                m.anisotropy = an;
                for (float th : thetas) {
                    for (float ph : {0.f, 0.8f, 1.57f}) {
                        const SampleStats s = sampleStats(m, dirFromAngles(th, ph), 40000, 17u);
                        worstAniso = std::max(worstAniso, std::fabs(s.meanWeight - 1.0));
                        check(std::fabs(s.meanWeight - 1.0) <= 0.025, "furnace anisotropic r=%.1f a=%.1f theta=%.2f phi=%.2f: %.4f",
                              rough, an, th, ph, s.meanWeight);
                    }
                }
            }
        }
        std::printf("opaque anisotropic (|a| <= 0.5) white furnace: worst |albedo - 1| = %.4f\n", worstAniso);
        BsdfMaterial m = opaque(1.f, 0.5f, 0.f);
        m.anisotropy = 0.8f;
        report("opaque anisotropic 0.8 white", m);
        m.anisotropy = 1.f;
        m.roughness = 0.8f;
        report("opaque anisotropic 1.0 r0.8 white", m);
    }
    {
        BsdfMaterial m = opaque(1.f, 0.4f, 1.f);
        m.flags |= bsdf::kBsdfFlagThinFilm;
        m.thinFilmThickness = 400.f;
        report("opaque thin film white metal", m);
    }
    {
        BsdfMaterial m = opaque(1.f, 0.5f, 0.f);
        m.flags |= bsdf::kBsdfFlagSssThin;
        m.sssMeasurementDistance = 0.3f;
        m.sssTransmittance = float3(1.f);
        m.sssSingleScatterAlbedo = float3(1.f);
        report("opaque thin SSS (upstream)", m);
    }
}

void suiteReciprocity() {
    int pairs = 0;
    double worst = 0.0;
    for (const Config& c : allConfigs()) {
        if (c.hair || c.m.model == bsdf::kBsdfModelPortal) {
            continue;
        }
        Rng rng(0x5eedu + std::uint64_t(pairs));
        for (int i = 0; i < 4000; ++i) {
            float3 wo = rng.sphere();
            float3 wi = rng.sphere();
            if (!c.twoSided) {
                wo.z = std::fabs(wo.z);
            }
            if (c.m.model == bsdf::kBsdfModelOpaque && (c.m.flags & bsdf::kBsdfFlagSssThin) == 0u) {
                wi.z = std::fabs(wi.z);
            }
            if (std::fabs(wo.z) < 0.02f || std::fabs(wi.z) < 0.02f) {
                continue;
            }
            const float3 a = eval(c.m, wo, wi) / std::fabs(wi.z);
            const float3 b = eval(c.m, wi, wo) / std::fabs(wo.z);
            for (int k = 0; k < 3; ++k) {
                const double x = a[k];
                const double y = b[k];
                const double err = std::fabs(x - y) / (std::max(std::fabs(x), std::fabs(y)) + 1e-6);
                worst = std::max(worst, err);
                if (err > 1e-4) {
                    check(false, "reciprocity %s: f(o,i)=%g f(i,o)=%g (wo %.3f %.3f %.3f wi %.3f %.3f %.3f)", c.name.c_str(),
                          x, y, wo.x, wo.y, wo.z, wi.x, wi.y, wi.z);
                    i = 1 << 30;
                    break;
                }
            }
            ++pairs;
        }
    }
    std::printf("non-dirac reciprocity: %d pairs, worst relative |f(o,i) - f(i,o)| = %.2e\n", pairs, worst);

    // Dirac lobes: the throughput of a reflection / transmission equals that of the reversed path.
    int deltaPairs = 0;
    double deltaWorst = 0.0;
    for (const Config& c : allConfigs()) {
        if (c.m.model != bsdf::kBsdfModelTranslucent) {
            continue;
        }
        Rng rng(77u);
        for (int i = 0; i < 500; ++i) {
            float3 wo = rng.sphere();
            if (std::fabs(wo.z) < 0.05f) {
                continue;
            }
            for (float ux : {0.0005f, 0.25f, 0.5f, 0.75f, 0.9995f}) {
                const bsdf::BsdfSample s = sampleDir(c.m, wo, float4(ux, 0.3f, 0.6f, 0.f));
                if ((s.flags & bsdf::kBsdfSampleDelta) == 0u) {
                    continue;
                }
                const double t1 = lum(s.weight) * s.pdf;
                const bsdf::uint lobe = s.flags >> bsdf::kBsdfLobeShift;
                bool found = false;
                for (float vx : {0.0005f, 0.25f, 0.5f, 0.75f, 0.9995f}) {
                    const bsdf::BsdfSample r = sampleDir(c.m, s.wi, float4(vx, 0.3f, 0.6f, 0.f));
                    if ((r.flags >> bsdf::kBsdfLobeShift) != lobe || (r.flags & bsdf::kBsdfSampleDelta) == 0u) {
                        continue;
                    }
                    found = true;
                    const double t2 = lum(r.weight) * r.pdf;
                    const double dir = length(r.wi - wo);
                    const double err = std::fabs(t1 - t2) / std::max(std::max(t1, t2), 1e-6);
                    deltaWorst = std::max(deltaWorst, err);
                    check(dir < 1e-4 && err <= 1e-4, "dirac reciprocity %s lobe %u: %.6f vs %.6f (dir %.2e)", c.name.c_str(),
                          lobe, t1, t2, dir);
                    break;
                }
                // A transmission into the medium at an angle beyond the critical cone cannot be reversed (TIR);
                // the forward throughput is then 0 as well (never sampled).
                check(found || t1 == 0.0, "dirac reverse lobe not found (%s)", c.name.c_str());
                ++deltaPairs;
            }
        }
    }
    std::printf("dirac reciprocity: %d paths, worst relative throughput difference = %.2e\n", deltaPairs, deltaWorst);
    std::printf("hair: not reciprocal by construction (Chiang 2016: A_p and h follow wo), not gated\n");
}

void suiteChi2() {
    std::vector<std::pair<std::string, ChiResult>> results;
    std::uint64_t seed = 1000u;
    for (const Config& c : allConfigs()) {
        if (c.m.model == bsdf::kBsdfModelPortal) {
            continue;
        }
        for (const float3& wo : testDirections(c)) {
            results.emplace_back(c.name, chiSquare(c.m, wo, seed++));
        }
    }
    const double alpha = 1.0 - std::pow(1.0 - 0.01, 1.0 / double(results.size()));
    for (const auto& [name, r] : results) {
        std::printf("chi2 %-24s chi2 = %9.1f dof = %4d p = %.4f  (int pdf = %.4f, dirac %.4f)\n", name.c_str(), r.chi2,
                    r.dof, r.p, r.pdfIntegral, r.deltaFraction);
        check(r.p >= alpha, "chi2 %s rejected: p = %.3g < %.3g", name.c_str(), r.p, alpha);
    }
    std::printf("chi2: %zu tests, Sidak-corrected significance %.2e\n", results.size(), alpha);
}

void suitePdf() {
    for (const Config& c : allConfigs()) {
        if (c.m.model == bsdf::kBsdfModelPortal) {
            continue;
        }
        const float3 wo = testDirections(c)[0];
        const double integral = pdfIntegral(c.m, wo, 480, 960);
        const SampleStats s = sampleStats(c.m, wo, 400000, 21u);
        const double tol = std::max(2e-3, 4.0 * std::sqrt(s.validNonDelta * (1.0 - s.validNonDelta) / 400000.0));
        std::printf("pdf %-24s int pdf = %.5f, valid non-dirac fraction = %.5f, dirac = %.5f\n", c.name.c_str(), integral,
                    s.validNonDelta, s.delta);
        check(std::fabs(integral - s.validNonDelta) <= tol, "pdf %s: integral %.5f vs sampled fraction %.5f", c.name.c_str(),
              integral, s.validNonDelta);
        if (c.hair) {
            check(std::fabs(integral - 1.0) <= 1e-3, "pdf %s does not integrate to 1: %.5f", c.name.c_str(), integral);
        }
    }
    // Pure cosine lobes lose nothing: diffuse layer at full coverage (thin walls) integrates to exactly 1.
    {
        const double integral = pdfIntegral(translucent(true, 1.f), dirFromAngles(0.6f, 0.f), 480, 960);
        std::printf("pdf thin wall, full diffuse layer: %.5f\n", integral);
        check(std::fabs(integral - 1.0) <= 1e-3, "pdf cosine lobe integral %.5f", integral);
    }
    // SSS radius pdf and Henyey-Greenstein integrate to 1.
    for (float d : {0.05f, 1.f, 4.f}) {
        double sum = 0.0;
        const int n = 200000;
        const double rMax = 80.0 * d;
        for (int i = 0; i < n; ++i) {
            const double r = (i + 0.5) * rMax / n;
            sum += double(bsdf::bsdfBurleyRadiusPdf(float(r), d)) * rMax / n;
        }
        check(std::fabs(sum - 1.0) <= 1e-3, "Burley radius pdf (d = %.2f) integrates to %.5f", d, sum);
        std::printf("pdf Burley radius d=%.2f: %.5f\n", d, sum);
    }
    for (float g : {-0.7f, 0.f, 0.3f, 0.9f}) {
        double sum = 0.0;
        const int n = 400000;
        for (int i = 0; i < n; ++i) {
            const double c = -1.0 + (i + 0.5) * 2.0 / n;
            sum += 2.0 * kPi * double(bsdf::bsdfHgEval(g, float(c))) * 2.0 / n;
        }
        check(std::fabs(sum - 1.0) <= 1e-3, "Henyey-Greenstein g = %.1f integrates to %.5f", g, sum);
        std::printf("pdf Henyey-Greenstein g=%+.1f: %.5f\n", g, sum);
    }
}

void suiteSss() {
    // Normalization of the profile itself: 2 pi int R(r) r dr = 1.
    for (float albedo : {0.2f, 0.8f}) {
        const float d = 1.f / bsdf::bsdfBurleyScale(albedo);
        double sum = 0.0;
        const int n = 400000;
        const double rMax = 80.0 * d;
        for (int i = 0; i < n; ++i) {
            const double r = (i + 0.5) * rMax / n;
            sum += 2.0 * kPi * r * double(bsdf::bsdfBurleyProfile(float(r), d)) * rMax / n;
        }
        std::printf("sss profile A=%.1f (s = %.3f): 2 pi int R r dr = %.5f\n", albedo, 1.0 / d, sum);
        check(std::fabs(sum - 1.0) <= 1e-3, "Burley profile normalization %.5f", sum);
    }
    // Golubev inverse CDF: CDF(sample(u)) == u.
    double worst = 0.0;
    for (int i = 1; i < 2000; ++i) {
        const float u = float(i) / 2000.f;
        const float d = 0.7f;
        const double r = bsdf::bsdfBurleyRadiusSample(u, d);
        const double cdf = 1.0 - 0.25 * std::exp(-r / d) - 0.75 * std::exp(-r / (3.0 * d));
        worst = std::max(worst, std::fabs(cdf - u));
    }
    std::printf("sss inverse CDF round trip: max |CDF(r(u)) - u| = %.2e\n", worst);
    check(worst <= 2e-5, "Burley inverse CDF error %.2e", worst);
    // Radius sampling chi^2 over 50 equal-probability bins.
    {
        const int bins = 50;
        const int n = 500000;
        const float d = 0.7f;
        std::vector<double> observed(bins, 0.0);
        std::vector<double> expected(bins, double(n) / bins);
        Rng rng(99u);
        for (int i = 0; i < n; ++i) {
            const double r = bsdf::bsdfBurleyRadiusSample(rng.uniform(), d);
            const double cdf = 1.0 - 0.25 * std::exp(-r / d) - 0.75 * std::exp(-r / (3.0 * d));
            observed[std::size_t(std::min(int(cdf * bins), bins - 1))] += 1.0;
        }
        int dof = 0;
        double chi2 = 0.0;
        const double p = chi2PValue(observed, expected, &dof, &chi2);
        std::printf("sss radius chi2 = %.1f dof = %d p = %.4f\n", chi2, dof, p);
        check(p >= 0.001, "Burley radius sampling chi2 p = %.4f", p);
    }
    // Henyey-Greenstein cosine sampling chi^2 (bins in cos, expected from the phase function).
    for (float g : {-0.5f, 0.f, 0.8f}) {
        const int bins = 60;
        const int n = 500000;
        std::vector<double> observed(bins, 0.0);
        std::vector<double> expected(bins, 0.0);
        for (int b = 0; b < bins; ++b) {
            for (int s = 0; s < 64; ++s) {
                const double c = -1.0 + (b + (s + 0.5) / 64.0) * 2.0 / bins;
                expected[std::size_t(b)] += 2.0 * kPi * double(bsdf::bsdfHgEval(g, float(c))) * (2.0 / bins / 64.0) * n;
            }
        }
        Rng rng(123u);
        for (int i = 0; i < n; ++i) {
            const double c = bsdf::bsdfHgSampleCos(g, rng.uniform());
            observed[std::size_t(std::clamp(int((c + 1.0) * 0.5 * bins), 0, bins - 1))] += 1.0;
        }
        int dof = 0;
        double chi2 = 0.0;
        const double p = chi2PValue(observed, expected, &dof, &chi2);
        std::printf("Henyey-Greenstein g=%+.1f chi2 = %.1f dof = %d p = %.4f\n", g, chi2, dof, p);
        check(p >= 0.001, "HG sampling chi2 (g = %.1f) p = %.4f", g, p);
    }
    // Diffusion-profile materials flag their cosine samples for the probe.
    {
        BsdfMaterial m = opaque(0.8f, 0.5f, 0.f);
        m.flags |= bsdf::kBsdfFlagSssDiffusion;
        Rng rng(5u);
        int flagged = 0;
        int diffuse = 0;
        for (int i = 0; i < 2000; ++i) {
            const bsdf::BsdfSample s = sampleDir(m, dirFromAngles(0.4f, 0.f), rng.uniform4());
            if ((s.flags >> bsdf::kBsdfLobeShift) == bsdf::kBsdfLobeDiffuse) {
                ++diffuse;
                flagged += (s.flags & bsdf::kBsdfSampleSss) != 0u ? 1 : 0;
            }
        }
        check(diffuse > 0 && flagged == diffuse, "SSS flag on diffusion samples (%d / %d)", flagged, diffuse);
        // The disk sample's planar pdf integrates to 1.
        const float3 dd(1.f, 0.4f, 0.2f);
        double sum = 0.0;
        const int n = 400000;
        const double rMax = 80.0;
        for (int i = 0; i < n; ++i) {
            const double r = (i + 0.5) * rMax / n;
            const double pr = (bsdf::bsdfBurleyRadiusPdf(float(r), dd.x) + bsdf::bsdfBurleyRadiusPdf(float(r), dd.y) +
                               bsdf::bsdfBurleyRadiusPdf(float(r), dd.z)) / 3.0;
            sum += pr * rMax / n;
        }
        const float3 disk = bsdf::bsdfSssDiskSample(0.5f, 0.5f, 0.25f, dd);
        const double rr = std::sqrt(double(disk.x) * disk.x + double(disk.y) * disk.y);
        const double expect = (bsdf::bsdfBurleyRadiusPdf(float(rr), dd.x) + bsdf::bsdfBurleyRadiusPdf(float(rr), dd.y) +
                               bsdf::bsdfBurleyRadiusPdf(float(rr), dd.z)) / 3.0 / (2.0 * kPi * rr);
        check(std::fabs(sum - 1.0) <= 1e-3 && std::fabs(disk.z - expect) <= 1e-5 * expect,
              "disk sample pdf (integral %.5f, pdf %.6g vs %.6g)", sum, disk.z, expect);
    }
}

void suiteThinFilm() {
    // Zero thickness: the film vanishes and the result is the bare air -> base interface (Stokes composition of the
    // two amplitudes); the series is truncated at m = 3, so a few 1e-3 remain for strongly reflecting bases.
    double worst = 0.0;
    for (float base : {1.2f, 2.f, 4.f}) {
        for (float c : {1.f, 0.7f, 0.3f}) {
            const float3 r = bsdf::bsdfThinFilmFresnel(c, 1.5f, base, 0.f);
            const float bare = bsdf::bsdfFresnelDielectric(c, base);
            for (int k = 0; k < 3; ++k) {
                worst = std::max(worst, std::fabs(double(r[k]) - bare));
            }
        }
    }
    std::printf("thin film at zero thickness vs bare interface: max |dR| = %.4f\n", worst);
    check(worst <= 0.02, "thin film zero thickness differs from the bare interface by %.4f", worst);
    // Matched indices (base == film): no interference, R = the air -> film reflectance at every thickness.
    double matched = 0.0;
    for (float t : {50.f, 300.f, 900.f}) {
        const float3 r = bsdf::bsdfThinFilmFresnel(0.8f, 1.5f, 1.5f, t);
        for (int k = 0; k < 3; ++k) {
            matched = std::max(matched, std::fabs(double(r[k]) - bsdf::bsdfFresnelDielectric(0.8f, 1.5f)));
        }
    }
    check(matched <= 1e-4, "thin film with matched indices interferes (%.2e)", matched);
    // Iridescence: in [0, 1] and varying with thickness and angle.
    float lo = 1.f;
    float hi = 0.f;
    bool inRange = true;
    for (int i = 0; i <= 100; ++i) {
        const float3 r = bsdf::bsdfThinFilmFresnel(0.9f, 1.5f, 2.5f, 100.f + 9.f * float(i));
        for (int k = 0; k < 3; ++k) {
            inRange = inRange && r[k] >= 0.f && r[k] <= 1.f && std::isfinite(r[k]);
            lo = std::min(lo, r[k]);
            hi = std::max(hi, r[k]);
        }
    }
    std::printf("thin film (film 1.5 on 2.5, 100..1000 nm): R in [%.3f, %.3f]\n", lo, hi);
    check(inRange && hi - lo > 0.05f, "thin film reflectance range [%.3f, %.3f]", lo, hi);
}

bool sameBits(const void* a, const void* b, std::size_t n) { return std::memcmp(a, b, n) == 0; }

rm::BsdfCases parityCases() {
    rm::BsdfCases cases;
    Rng rng(4242u);
    for (const Config& c : allConfigs()) {
        for (int i = 0; i < 67; ++i) {
            float3 wo = rng.sphere();
            if (!c.twoSided) {
                wo.z = std::fabs(wo.z);
            }
            cases.add(c.m, wo, rng.sphere(), rng.uniform4());
        }
    }
    return cases;
}

void suiteParity() {
    const rm::BsdfCases cases = parityCases();
    rm::BsdfResults ref;
    auto& scheduler = fuse::jobs::JobScheduler::instance();
    scheduler.shutdown();
    kernel::KernelStats before{};
    const bool had = kernel::find_kernel_stats(bsdf::kEvalName, before);
    check(rm::runBsdfKernels(kernel::Backend::CpuReference, rm::sharedAlbedoLut(), cases, ref), "CpuReference launch failed");
    kernel::KernelStats after{};
    check(kernel::find_kernel_stats(bsdf::kEvalName, after) && after.items == (had ? before.items : 0u) + cases.count(),
          "bsdf_eval stats item count");
    for (fuse::u32 workers : {0u, 2u, 4u}) {
        scheduler.shutdown();
        scheduler.initialize(workers);
        rm::BsdfResults par;
        check(rm::runBsdfKernels(kernel::Backend::CpuParallel, rm::sharedAlbedoLut(), cases, par), "CpuParallel launch failed");
        const bool same = sameBits(ref.eval.data(), par.eval.data(), ref.eval.size() * sizeof(float3)) &&
                          sameBits(ref.pdf.data(), par.pdf.data(), ref.pdf.size() * sizeof(float)) &&
                          sameBits(ref.samples.data(), par.samples.data(), ref.samples.size() * sizeof(bsdf::BsdfSample));
        std::printf("CpuReference == CpuParallel (%u workers, %u cases): %s\n", workers, cases.count(),
                    same ? "bit-exact" : "DIFFERENT");
        check(same, "CpuReference != CpuParallel at %u workers", workers);
    }
    scheduler.shutdown();
    for (const char* name : {bsdf::kEvalName, bsdf::kPdfName, bsdf::kSampleName, bsdf::kAlbedoLutName, bsdf::kAlbedoAvgName}) {
        kernel::KernelStats s{};
        check(kernel::find_kernel_stats(name, s) && s.launches > 0u, "no stats for %s", name);
    }
    // A GPU request without a device falls back to CpuParallel (recorded).
    rm::BsdfResults fb;
    check(rm::runBsdfKernels(kernel::Backend::Cuda, rm::sharedAlbedoLut(), cases, fb), "Cuda request failed");
    const kernel::LaunchRecord last = kernel::last_launch();
    if (!kernel::backend_available(kernel::Backend::Cuda)) {
        check(last.requested == kernel::Backend::Cuda && last.backend == kernel::Backend::CpuParallel,
              "Cuda request did not fall back to CpuParallel");
        check(sameBits(ref.samples.data(), fb.samples.data(), ref.samples.size() * sizeof(bsdf::BsdfSample)),
              "fallback results differ");
    }
    // Every result is finite.
    bool finite = true;
    for (std::size_t i = 0; i < ref.eval.size(); ++i) {
        finite = finite && finite3(ref.eval[i]) && std::isfinite(ref.pdf[i]) && finite3(ref.samples[i].weight) &&
                 std::isfinite(ref.samples[i].pdf);
    }
    check(finite, "non-finite kernel output");
}

void suiteMapping() {
    namespace mi = fuse::relight::mods::import;
    auto near = [](float a, float b) { return std::fabs(a - b) <= 1e-5f * std::max(1.f, std::fabs(b)); };
    {
        mi::MaterialParams p = mi::defaultMaterialParams(mi::SurfaceType::Opaque);
        const BsdfMaterial m = rm::bsdfMaterialFromParams(p);
        const float g = std::pow(0.2f, 2.2f);
        check(m.model == bsdf::kBsdfModelOpaque && m.flags == 0u && near(m.albedo.x, g) && near(m.roughness, 0.5f) &&
                  near(m.metallic, 0.f) && near(m.opacity, 1.f) && m.emission.x == 0.f,
              "opaque defaults");
        p.values["enable_emission"].value[0] = 1.f;
        p.values["enable_thin_film"].value[0] = 1.f;
        p.values["thin_film_thickness_constant"].value[0] = 420.f;
        p.values["subsurface_measurement_distance"].value[0] = 0.25f;
        p.values["anisotropy"].value[0] = 0.4f;
        const BsdfMaterial e = rm::bsdfMaterialFromParams(p);
        check(near(e.emission.x, 40.f) && near(e.emission.y, 40.f * std::pow(0.1f, 2.2f)), "opaque emission");
        check((e.flags & bsdf::kBsdfFlagThinFilm) != 0u && near(e.thinFilmThickness, 420.f), "opaque thin film");
        check((e.flags & bsdf::kBsdfFlagSssThin) != 0u && near(e.sssMeasurementDistance, 0.25f) && near(e.anisotropy, 0.4f),
              "opaque thin SSS");
        p.values["subsurface_diffusion_profile"].value[0] = 1.f;
        p.values["subsurface_radius_scale"].value[0] = 2.f;
        const BsdfMaterial d = rm::bsdfMaterialFromParams(p);
        check((d.flags & bsdf::kBsdfFlagSssDiffusion) != 0u && (d.flags & bsdf::kBsdfFlagSssThin) == 0u &&
                  near(d.sssRadius.x, 1.f),
              "opaque diffusion profile");
        rm::BsdfMapOptions hair;
        hair.hairCards = true;
        const BsdfMaterial h = rm::bsdfMaterialFromParams(p, hair);
        check(h.model == bsdf::kBsdfModelHair && near(h.ior, 1.55f) && h.hairSigmaA.x > 0.f && near(h.hairBetaM, 0.5f),
              "HairCards -> hair");
        p.values["diffuse_texture"].asset = "textures/a.dds";
        const rm::BsdfTextureSlots slots = rm::bsdfTextureSlots(p);
        check(slots.params.size() == 1u && slots.params[0] == "diffuse_texture", "texture slots");
    }
    {
        mi::MaterialParams p = mi::defaultMaterialParams(mi::SurfaceType::Translucent);
        const BsdfMaterial m = rm::bsdfMaterialFromParams(p);
        check(m.model == bsdf::kBsdfModelTranslucent && near(m.ior, 1.3f) && near(m.transmittance.x, std::pow(0.97f, 2.2f)) &&
                  near(m.mediumDistance, 1.f) && m.flags == 0u,
              "translucent defaults");
        p.values["thin_walled"].value[0] = 1.f;
        p.values["thin_wall_thickness"].value[0] = 0.02f;
        p.values["use_diffuse_layer"].value[0] = 1.f;
        const BsdfMaterial t = rm::bsdfMaterialFromParams(p);
        check((t.flags & bsdf::kBsdfFlagThinWalled) != 0u && (t.flags & bsdf::kBsdfFlagDiffuseLayer) != 0u &&
                  near(t.mediumDistance, 0.02f),
              "translucent thin walls / diffuse layer");
    }
    {
        mi::MaterialParams p = mi::defaultMaterialParams(mi::SurfaceType::Portal);
        p.values["enable_emission"].value[0] = 1.f;
        const BsdfMaterial m = rm::bsdfMaterialFromParams(p);
        const bsdf::BsdfSample s = sampleDir(m, float3(0.f, 0.f, 1.f), float4(0.5f, 0.5f, 0.5f, 0.5f));
        check(m.model == bsdf::kBsdfModelPortal && near(m.emission.x, 40.f) && s.flags == 0u &&
                  lum(eval(m, float3(0.f, 0.f, 1.f), float3(0.f, 0.f, 1.f))) == 0.0,
              "portal");
    }
    // Packed GPU layout round trip.
    for (const Config& c : allConfigs()) {
        bsdf::float4 words[bsdf::kBsdfMaterialWords];
        bsdf::bsdfMaterialPack(c.m, words);
        const BsdfMaterial u = bsdf::bsdfMaterialUnpack(words);
        bsdf::float4 again[bsdf::kBsdfMaterialWords];
        bsdf::bsdfMaterialPack(u, again);
        check(std::memcmp(words, again, sizeof(words)) == 0 && u.model == c.m.model && u.flags == c.m.flags,
              "pack round trip %s", c.name.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "";
    struct Entry {
        const char* name;
        void (*fn)();
    };
    const Entry suites[] = {{"lut", suiteLut},         {"furnace", suiteFurnace}, {"reciprocity", suiteReciprocity},
                            {"chi2", suiteChi2},       {"pdf", suitePdf},         {"sss", suiteSss},
                            {"thinfilm", suiteThinFilm}, {"parity", suiteParity}, {"mapping", suiteMapping}};
    bool ran = false;
    for (const Entry& e : suites) {
        if (suite.empty() || suite == e.name) {
            std::printf("== %s\n", e.name);
            e.fn();
            ran = true;
        }
    }
    if (!ran) {
        std::fprintf(stderr, "unknown suite '%s'\n", suite.c_str());
        return 2;
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
