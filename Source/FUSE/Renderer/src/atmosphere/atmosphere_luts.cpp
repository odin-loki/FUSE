// WP-8.2 Hillaire atmosphere: CPU reference. See include/fuse/renderer/atmosphere/atmosphere_luts.hpp.
// Every function here has a twin in shaders/atmosphere/at_sample.* (sampling) or at_common.* (integration);
// keep the operations and their order in step.
#include <fuse/renderer/atmosphere/atmosphere_luts.hpp>

#include <fuse/renderer/atmosphere/sky_scatter.hpp>
#include <fuse/renderer/atmosphere/sun_disk.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::atmosphere {

namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kInvFourPi = 1.f / (4.f * 3.14159265358979323846f);
constexpr f32 kMinAltitude = 1.f; ///< metres: sample / camera radii stay inside the shell

using math::Vec3;

Vec3 v3(f32 x, f32 y, f32 z) { return Vec3{x, y, z}; }
Vec3 mul(const Vec3& a, const Vec3& b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
Vec3 expNeg(const Vec3& t) { return v3(std::exp(-t.x), std::exp(-t.y), std::exp(-t.z)); }
f32 clampf(f32 v, f32 lo, f32 hi) { return std::max(lo, std::min(hi, v)); }
Vec3 texelRgb(const AtTexel& t) { return v3(t.r, t.g, t.b); }
AtTexel texel(const Vec3& v, f32 a) { return AtTexel{v.x, v.y, v.z, a}; }

/// 1 - exp(-x) for x >= 0 without the cancellation of small x.
f32 oneMinusExp(f32 x) {
    if (x < 0.02f) {
        return x * (1.f - x * (0.5f - x * (1.f / 6.f - x * (1.f / 24.f))));
    }
    return 1.f - std::exp(-x);
}

/// Integral of exp(-h / H) over a segment whose end heights are hA, hB (log-linear in between).
f32 logLinear(f32 hA, f32 hB, f32 len, f32 scaleH) {
    const f32 dA = std::exp(-hA / scaleH);
    const f32 d = (hB - hA) / scaleH;
    if (std::fabs(d) < 0.02f) {
        return len * dA * (1.f - d * (0.5f - d * (1.f / 6.f - d * (1.f / 24.f))));
    }
    return len * dA * ((1.f - std::exp(-d)) / d);
}

f32 ozoneDensity(const AtParams& p, f32 h) {
    return std::max(0.f, 1.f - std::fabs(h - p.ozoneCenter) / p.ozoneHalfWidth);
}

/// Fraction of the solar disk above the horizon when its centre sits `offset` radii above it. The circular
/// segment of height h = 1 - |offset| (in radii) has area (x - sin x) / (2 pi) of the disk, x = 4 asin(sqrt(h / 2));
/// the series of x - sin x for small x keeps a barely visible (or barely hidden) sun free of cancellation.
f32 visibleSunFraction(f32 offset) {
    const f32 d = clampf(offset, -1.f, 1.f);
    const f32 h = 1.f - std::fabs(d);
    const f32 x = 4.f * std::asin(std::sqrt(0.5f * h));
    f32 seg = 0.f;
    if (x < 0.5f) {
        const f32 x2 = x * x;
        seg = x * x2 * (1.f / 6.f) * (1.f - x2 * (1.f / 20.f) * (1.f - x2 * (1.f / 42.f) * (1.f - x2 * (1.f / 72.f))));
    } else {
        seg = x - std::sin(x);
    }
    seg = seg / (2.f * kPi);
    return d >= 0.f ? 1.f - seg : seg;
}

struct Medium {
    Vec3 scatR;  ///< Rayleigh scattering (rgb)
    f32 scatM;   ///< Mie scattering
    Vec3 ext;    ///< total extinction (rgb)
};

Medium medium(const AtParams& p, f32 h) {
    const f32 hc = std::max(0.f, h);
    const f32 dR = std::exp(-hc / p.rayleighHeight);
    const f32 dM = std::exp(-hc / p.mieHeight);
    const f32 dO = ozoneDensity(p, hc);
    Medium m{};
    m.scatR = v3(p.rayleighScattering[0] * dR, p.rayleighScattering[1] * dR, p.rayleighScattering[2] * dR);
    m.scatM = p.mieScattering * dM;
    const f32 extM = p.mieExtinction * dM;
    m.ext = v3(m.scatR.x + extM + p.ozoneAbsorption[0] * dO, m.scatR.y + extM + p.ozoneAbsorption[1] * dO,
               m.scatR.z + extM + p.ozoneAbsorption[2] * dO);
    return m;
}

f32 rayleighPhase(f32 nu) { return (3.f / (16.f * kPi)) * (1.f + nu * nu); }

f32 miePhase(f32 nu, f32 g) {
    const f32 g2 = g * g;
    const f32 base = std::max(1e-6f, 1.f + g2 - 2.f * g * nu);
    const f32 denom = base * std::sqrt(base);
    return (3.f / (8.f * kPi)) * ((1.f - g2) * (1.f + nu * nu)) / ((2.f + g2) * denom);
}

/// March state shared by the three integrators.
struct March {
    Vec3 L{};
    Vec3 fms{};
    Vec3 thr{1.f, 1.f, 1.f};
};

enum : u32 { kPhaseSky = 0, kPhaseIsotropic = 1 };

/// One midpoint step at distance t (length dt) of a ray from radius r0 (cos zenith mu, cos to the sun nu,
/// sun cos zenith at the origin muS0). Energy-conserving step integral (Hillaire 2020 §5.1).
void scatterStep(const AtParams& p, const AtLutView& lut, f32 r0, f32 mu, f32 nu, f32 muS0, f32 t, f32 dt, u32 phaseMode,
                 bool useMs, f32 phaseR, f32 phaseM, March& m) {
    const f32 h = at_height(p, r0, mu, t);
    const f32 r = p.bottomRadius + std::max(0.f, h);
    const Medium md = medium(p, h);
    const f32 muS = clampf((r0 * muS0 + t * nu) / r, -1.f, 1.f);
    const Vec3 sunT = at_sun_transmittance(p, lut, r, muS);
    const Vec3 scat = v3(md.scatR.x + md.scatM, md.scatR.y + md.scatM, md.scatR.z + md.scatM);
    Vec3 phaseScat{};
    if (phaseMode == kPhaseIsotropic) {
        phaseScat = scat * kInvFourPi;
    } else {
        phaseScat = v3(md.scatR.x * phaseR + md.scatM * phaseM, md.scatR.y * phaseR + md.scatM * phaseM,
                       md.scatR.z * phaseR + md.scatM * phaseM);
    }
    Vec3 S = mul(sunT, phaseScat);
    if (useMs) {
        S = S + mul(at_multiscatter(p, lut, r, muS), scat);
    }
    const f32 ext[3] = {md.ext.x, md.ext.y, md.ext.z};
    const f32 s[3] = {S.x, S.y, S.z};
    const f32 sc[3] = {scat.x, scat.y, scat.z};
    f32 L[3] = {m.L.x, m.L.y, m.L.z};
    f32 F[3] = {m.fms.x, m.fms.y, m.fms.z};
    f32 T[3] = {m.thr.x, m.thr.y, m.thr.z};
    for (u32 c = 0; c < 3u; ++c) {
        const f32 od = ext[c] * dt;
        // (1 - exp(-ext dt)) / ext: the in-segment integral of exp(-ext s) ds.
        const f32 w = ext[c] > 1e-20f ? oneMinusExp(od) / ext[c] : dt;
        L[c] = L[c] + T[c] * (s[c] * w);
        F[c] = F[c] + T[c] * (sc[c] * w);
        T[c] = T[c] * std::exp(-od);
    }
    m.L = v3(L[0], L[1], L[2]);
    m.fms = v3(F[0], F[1], F[2]);
    m.thr = v3(T[0], T[1], T[2]);
}

/// Quadratic sample distribution t = tEnd u^2 over `steps` midpoint samples.
void marchQuadratic(const AtParams& p, const AtLutView& lut, f32 r0, f32 mu, f32 nu, f32 muS0, f32 tEnd, u32 steps,
                    u32 phaseMode, bool useMs, March& m) {
    const f32 phaseR = rayleighPhase(nu);
    const f32 phaseM = miePhase(nu, p.mieG);
    const f32 inv = 1.f / static_cast<f32>(steps);
    for (u32 i = 0; i < steps; ++i) {
        const f32 u0 = static_cast<f32>(i) * inv;
        const f32 u1 = static_cast<f32>(i + 1u) * inv;
        const f32 t0 = tEnd * u0 * u0;
        const f32 t1 = tEnd * u1 * u1;
        const f32 t = 0.5f * (t0 + t1);
        scatterStep(p, lut, r0, mu, nu, muS0, t, t1 - t0, phaseMode, useMs, phaseR, phaseM, m);
    }
}

f32 lengthOf(const Vec3& v) { return std::sqrt(v.dot(v)); }

} // namespace

// --- resolve ---------------------------------------------------------------------------------------------
bool resolve_at_params(const AtmosphereLutSettings& s, const AtmosphereLutView& view, AtParams& out) {
    const AtmosphereLutSizes& z = s.sizes;
    const AtmosphereLutSampling& n = s.sampling;
    const AtmosphereParams& a = s.atmosphere;
    if (z.transWidth < 2u || z.transHeight < 2u || z.msWidth < 2u || z.msHeight < 2u || z.skyWidth < 2u ||
        z.skyHeight < 2u || z.apWidth < 2u || z.apHeight < 2u || z.apDepth < 2u) {
        return false;
    }
    if (n.transSteps == 0u || n.msSteps == 0u || n.msDirSqrt == 0u || n.skySteps == 0u || n.apStepsPerSlice == 0u) {
        return false;
    }
    if (!(a.earth_radius > 0.f) || !(a.atmo_radius > a.earth_radius + 2.f * kMinAltitude) ||
        !(a.rayleigh_scale_h > 0.f) || !(a.mie_scale_h > 0.f) || !(s.ozoneHalfWidth > 0.f) ||
        !(std::fabs(a.mie_scatter_dir) < 1.f)) {
        return false;
    }
    if (!(view.tanHalfFovX > 0.f) || !(view.tanHalfFovY > 0.f) || !(view.aerialMaxDistance > 0.f)) {
        return false;
    }
    const f32 sunLen = lengthOf(view.sunDirection);
    const f32 fl = lengthOf(view.forward);
    const f32 rl = lengthOf(view.right);
    const f32 ul = lengthOf(view.up);
    if (!(sunLen > 0.f) || !(fl > 0.f) || !(rl > 0.f) || !(ul > 0.f)) {
        return false;
    }
    AtParams p{};
    p.transWidth = z.transWidth;
    p.transHeight = z.transHeight;
    p.msWidth = z.msWidth;
    p.msHeight = z.msHeight;
    p.skyWidth = z.skyWidth;
    p.skyHeight = z.skyHeight;
    p.apWidth = z.apWidth;
    p.apHeight = z.apHeight;
    p.apDepth = z.apDepth;
    p.flags = s.multiScattering ? static_cast<u32>(kAtFlagMultiScatter) : 0u;
    p.transSteps = n.transSteps;
    p.msSteps = n.msSteps;
    p.msDirSqrt = n.msDirSqrt;
    p.skySteps = n.skySteps;
    p.apStepsPerSlice = n.apStepsPerSlice;
    p.bottomRadius = a.earth_radius;
    p.topRadius = a.atmo_radius;
    p.rayleighHeight = a.rayleigh_scale_h;
    p.mieHeight = a.mie_scale_h;
    p.rayleighScattering[0] = a.rayleigh_coeff.x;
    p.rayleighScattering[1] = a.rayleigh_coeff.y;
    p.rayleighScattering[2] = a.rayleigh_coeff.z;
    p.mieScattering = a.mie_coeff;
    p.mieExtinction = atmosphere_mie_extinction(a);
    p.mieG = a.mie_scatter_dir;
    p.msFactor = s.multiScatterFactor;
    p.ozoneAbsorption[0] = s.ozoneAbsorption.x;
    p.ozoneAbsorption[1] = s.ozoneAbsorption.y;
    p.ozoneAbsorption[2] = s.ozoneAbsorption.z;
    p.ozoneCenter = s.ozoneCenter;
    p.ozoneHalfWidth = s.ozoneHalfWidth;
    p.sunAngularRadius = sun_angular_radius_rad();
    p.sunDiskNorm = 1.f / sun_disk_weighted_solid_angle(p.sunAngularRadius);
    p.groundAlbedo[0] = s.groundAlbedo.x;
    p.groundAlbedo[1] = s.groundAlbedo.y;
    p.groundAlbedo[2] = s.groundAlbedo.z;
    p.sunIlluminance[0] = view.sunIlluminance.x;
    p.sunIlluminance[1] = view.sunIlluminance.y;
    p.sunIlluminance[2] = view.sunIlluminance.z;

    // Camera relative to the planet centre, in double (world positions are small next to the radius).
    const f64 cx = view.cameraPosition.x;
    const f64 cy = static_cast<f64>(view.cameraPosition.y) + a.earth_radius;
    const f64 cz = view.cameraPosition.z;
    const f64 len = std::sqrt(cx * cx + cy * cy + cz * cz);
    f64 ux = 0.0, uy = 1.0, uz = 0.0;
    if (len > 0.0) {
        ux = cx / len;
        uy = cy / len;
        uz = cz / len;
    }
    const f64 radius = std::clamp(len, static_cast<f64>(a.earth_radius) + kMinAltitude,
                                  static_cast<f64>(a.atmo_radius) - kMinAltitude);
    const f64 sx = view.sunDirection.x / static_cast<f64>(sunLen);
    const f64 sy = view.sunDirection.y / static_cast<f64>(sunLen);
    const f64 sz = view.sunDirection.z / static_cast<f64>(sunLen);
    p.sunDir[0] = static_cast<f32>(sx);
    p.sunDir[1] = static_cast<f32>(sy);
    p.sunDir[2] = static_cast<f32>(sz);
    p.sunDir[3] = static_cast<f32>(std::clamp(sx * ux + sy * uy + sz * uz, -1.0, 1.0));
    p.up[0] = static_cast<f32>(ux);
    p.up[1] = static_cast<f32>(uy);
    p.up[2] = static_cast<f32>(uz);
    p.up[3] = static_cast<f32>(radius);
    p.cameraPos[0] = view.cameraPosition.x;
    p.cameraPos[1] = view.cameraPosition.y;
    p.cameraPos[2] = view.cameraPosition.z;
    p.cameraPos[3] = view.aerialMaxDistance;
    p.camForward[0] = view.forward.x / fl;
    p.camForward[1] = view.forward.y / fl;
    p.camForward[2] = view.forward.z / fl;
    p.camForward[3] = view.tanHalfFovX;
    p.camRight[0] = view.right.x / rl;
    p.camRight[1] = view.right.y / rl;
    p.camRight[2] = view.right.z / rl;
    p.camRight[3] = view.tanHalfFovY;
    p.camUp[0] = view.up.x / ul;
    p.camUp[1] = view.up.y / ul;
    p.camUp[2] = view.up.z / ul;
    p.camUp[3] = 0.f;
    const u64 keep[6] = {out.transmittance, out.multiscatter, out.skyView, out.aerialScatter, out.aerialTransmittance,
                         out.reserved0};
    out = p;
    out.transmittance = keep[0];
    out.multiscatter = keep[1];
    out.skyView = keep[2];
    out.aerialScatter = keep[3];
    out.aerialTransmittance = keep[4];
    out.reserved0 = keep[5];
    return true;
}

bool at_static_equal(const AtParams& a, const AtParams& b) {
    auto same = [](const f32* x, const f32* y, u32 n) {
        for (u32 i = 0; i < n; ++i) {
            if (x[i] != y[i]) {
                return false;
            }
        }
        return true;
    };
    return a.transWidth == b.transWidth && a.transHeight == b.transHeight && a.msWidth == b.msWidth &&
           a.msHeight == b.msHeight && a.transSteps == b.transSteps && a.msSteps == b.msSteps &&
           a.msDirSqrt == b.msDirSqrt && a.bottomRadius == b.bottomRadius && a.topRadius == b.topRadius &&
           a.rayleighHeight == b.rayleighHeight && a.mieHeight == b.mieHeight &&
           same(a.rayleighScattering, b.rayleighScattering, 3) && a.mieScattering == b.mieScattering &&
           a.mieExtinction == b.mieExtinction && a.mieG == b.mieG && a.msFactor == b.msFactor &&
           same(a.ozoneAbsorption, b.ozoneAbsorption, 3) && a.ozoneCenter == b.ozoneCenter &&
           a.ozoneHalfWidth == b.ozoneHalfWidth && a.sunAngularRadius == b.sunAngularRadius &&
           same(a.groundAlbedo, b.groundAlbedo, 3);
}

// --- geometry -------------------------------------------------------------------------------------------
f32 at_height(const AtParams& p, f32 r0, f32 mu, f32 t) {
    const f32 rg = p.bottomRadius;
    const f32 h0 = r0 - rg;
    const f32 k = t * (2.f * r0 * mu + t);
    const f32 numerator = h0 * (r0 + rg) + k;
    const f32 r = std::sqrt(std::max(0.f, r0 * r0 + k));
    return numerator / std::max(1.f, r + rg);
}

f32 at_distance_to_top(const AtParams& p, f32 r0, f32 mu) {
    const f32 rt = p.topRadius;
    const f32 inside = (rt - r0) * (rt + r0);
    const f32 disc = inside + r0 * r0 * mu * mu;
    if (disc < 0.f) {
        return 0.f;
    }
    const f32 root = std::sqrt(disc);
    const f32 t = mu > 0.f ? inside / std::max(1e-6f, r0 * mu + root) : -r0 * mu + root;
    return std::max(0.f, t);
}

f32 at_distance_to_ground(const AtParams& p, f32 r0, f32 mu) {
    if (mu >= 0.f) {
        return -1.f;
    }
    const f32 rg = p.bottomRadius;
    const f32 above = (r0 - rg) * (r0 + rg);
    const f32 disc = r0 * r0 * mu * mu - above;
    if (disc < 0.f) {
        return -1.f;
    }
    const f32 denom = -r0 * mu + std::sqrt(disc);
    if (denom <= 0.f) {
        return 0.f;
    }
    return std::max(0.f, above / denom);
}

// --- parametrisations -----------------------------------------------------------------------------------
void at_transmittance_r_mu(const AtParams& p, f32 xMu, f32 xR, f32& r, f32& mu) {
    const f32 rg = p.bottomRadius;
    const f32 rt = p.topRadius;
    const f32 H = std::sqrt((rt - rg) * (rt + rg));
    const f32 rho = H * xR;
    r = std::sqrt(rho * rho + rg * rg);
    const f32 dMin = rt - r;
    const f32 dMax = rho + H;
    const f32 d = dMin + xMu * (dMax - dMin);
    mu = d <= 0.f ? 1.f : (H * H - rho * rho - d * d) / (2.f * r * d);
    mu = clampf(mu, -1.f, 1.f);
}

void at_transmittance_uv(const AtParams& p, f32 r, f32 mu, f32& xMu, f32& xR) {
    const f32 rg = p.bottomRadius;
    const f32 rt = p.topRadius;
    const f32 H = std::sqrt((rt - rg) * (rt + rg));
    const f32 rc = clampf(r, rg, rt);
    const f32 rho = std::sqrt(std::max(0.f, (rc - rg) * (rc + rg)));
    const f32 d = at_distance_to_top(p, rc, mu);
    const f32 dMin = rt - rc;
    const f32 dMax = rho + H;
    xMu = dMax - dMin > 0.f ? (d - dMin) / (dMax - dMin) : 0.f;
    xR = rho / H;
}

void at_sky_view_dir(const AtParams& p, f32 r, f32 u, f32 v, f32& mu, f32& lightViewCos) {
    const f32 rg = p.bottomRadius;
    const f32 vHorizon = std::sqrt(std::max(0.f, (r - rg) * (r + rg)));
    const f32 beta = std::acos(clampf(vHorizon / r, -1.f, 1.f));
    const f32 zenithHorizon = kPi - beta;
    f32 viewZenith = 0.f;
    if (v < 0.5f) {
        f32 c = 1.f - 2.f * v;
        c = 1.f - c * c;
        viewZenith = zenithHorizon * c;
    } else {
        f32 c = 2.f * v - 1.f;
        c = c * c;
        viewZenith = zenithHorizon + beta * c;
    }
    mu = std::cos(viewZenith);
    lightViewCos = 1.f - 2.f * u * u;
}

void at_sky_view_uv(const AtParams& p, f32 r, f32 mu, f32 lightViewCos, f32& u, f32& v) {
    const f32 rg = p.bottomRadius;
    const f32 vHorizon = std::sqrt(std::max(0.f, (r - rg) * (r + rg)));
    const f32 beta = std::acos(clampf(vHorizon / r, -1.f, 1.f));
    const f32 zenithHorizon = kPi - beta;
    const f32 viewZenith = std::acos(clampf(mu, -1.f, 1.f));
    if (viewZenith <= zenithHorizon) {
        const f32 c = viewZenith / zenithHorizon;
        v = 0.5f * (1.f - std::sqrt(std::max(0.f, 1.f - c)));
    } else {
        const f32 c = (viewZenith - zenithHorizon) / beta;
        v = 0.5f + 0.5f * std::sqrt(std::max(0.f, c));
    }
    u = std::sqrt(std::max(0.f, 0.5f - 0.5f * lightViewCos));
}

// --- texel kernels --------------------------------------------------------------------------------------
AtTexel transmittance_texel(const AtParams& p, u32 x, u32 y) {
    const f32 xMu = static_cast<f32>(x) / static_cast<f32>(p.transWidth - 1u);
    const f32 xR = static_cast<f32>(y) / static_cast<f32>(p.transHeight - 1u);
    f32 r = 0.f;
    f32 mu = 0.f;
    at_transmittance_r_mu(p, xMu, xR, r, mu);
    const f32 len = at_distance_to_top(p, r, mu);
    const f32 dt = len / static_cast<f32>(p.transSteps);
    f32 pathR = 0.f;
    f32 pathM = 0.f;
    f32 pathO = 0.f;
    f32 hPrev = at_height(p, r, mu, 0.f);
    f32 oPrev = ozoneDensity(p, hPrev);
    for (u32 i = 1; i <= p.transSteps; ++i) {
        const f32 hNext = at_height(p, r, mu, dt * static_cast<f32>(i));
        const f32 oNext = ozoneDensity(p, hNext);
        pathR = pathR + logLinear(hPrev, hNext, dt, p.rayleighHeight);
        pathM = pathM + logLinear(hPrev, hNext, dt, p.mieHeight);
        pathO = pathO + 0.5f * (oPrev + oNext) * dt;
        hPrev = hNext;
        oPrev = oNext;
    }
    const f32 m = p.mieExtinction * pathM;
    const Vec3 tau = v3(p.rayleighScattering[0] * pathR + m + p.ozoneAbsorption[0] * pathO,
                        p.rayleighScattering[1] * pathR + m + p.ozoneAbsorption[1] * pathO,
                        p.rayleighScattering[2] * pathR + m + p.ozoneAbsorption[2] * pathO);
    return texel(expNeg(tau), 0.f);
}

void multiscatter_integrals(const AtParams& p, const AtLutView& lut, f32 r, f32 muS, Vec3& l2, Vec3& fms) {
    const f32 sunX = std::sqrt(std::max(0.f, 1.f - muS * muS));
    const u32 n = p.msDirSqrt;
    const f32 invN = 1.f / static_cast<f32>(n);
    Vec3 sumL{};
    Vec3 sumF{};
    for (u32 a = 0; a < n; ++a) {
        const f32 theta = 2.f * kPi * ((static_cast<f32>(a) + 0.5f) * invN);
        const f32 ct = std::cos(theta);
        for (u32 b = 0; b < n; ++b) {
            const f32 cosPhi = 1.f - 2.f * ((static_cast<f32>(b) + 0.5f) * invN);
            const f32 sinPhi = std::sqrt(std::max(0.f, 1.f - cosPhi * cosPhi));
            const f32 mu = cosPhi;
            const f32 nu = ct * sinPhi * sunX + cosPhi * muS;
            const f32 tGround = at_distance_to_ground(p, r, mu);
            const bool ground = tGround >= 0.f;
            const f32 tEnd = ground ? tGround : at_distance_to_top(p, r, mu);
            March m{};
            marchQuadratic(p, lut, r, mu, nu, muS, tEnd, p.msSteps, kPhaseIsotropic, false, m);
            if (ground) {
                // Lambertian ground at the end of the ray, lit by the transmitted sun.
                const f32 muG = clampf((r * muS + tEnd * nu) / p.bottomRadius, -1.f, 1.f);
                const Vec3 sunT = at_sun_transmittance(p, lut, p.bottomRadius, muG);
                const f32 k = std::max(0.f, muG) / kPi;
                m.L = m.L + v3(m.thr.x * sunT.x * p.groundAlbedo[0] * k, m.thr.y * sunT.y * p.groundAlbedo[1] * k,
                               m.thr.z * sunT.z * p.groundAlbedo[2] * k);
            }
            sumL = sumL + m.L;
            sumF = sumF + m.fms;
        }
    }
    const f32 invCount = invN * invN;
    l2 = sumL * invCount;
    fms = sumF * invCount;
}

AtTexel multiscatter_texel(const AtParams& p, const AtLutView& lut, u32 x, u32 y) {
    const f32 muS = static_cast<f32>(x) / static_cast<f32>(p.msWidth - 1u) * 2.f - 1.f;
    const f32 top = p.topRadius - p.bottomRadius;
    const f32 h = clampf(static_cast<f32>(y) / static_cast<f32>(p.msHeight - 1u) * top, kMinAltitude, top - kMinAltitude);
    const f32 r = p.bottomRadius + h;
    Vec3 l2{};
    Vec3 f{};
    multiscatter_integrals(p, lut, r, muS, l2, f);
    const Vec3 psi = v3(l2.x / (1.f - f.x), l2.y / (1.f - f.y), l2.z / (1.f - f.z));
    return texel(psi * p.msFactor, std::max(f.x, std::max(f.y, f.z)));
}

Vec3 at_integrate_sky(const AtParams& p, const AtLutView& lut, f32 r0, f32 mu, f32 nu, f32 muS0, u32 steps,
                      Vec3* transmittance) {
    const f32 tGround = at_distance_to_ground(p, r0, mu);
    const f32 tEnd = tGround >= 0.f ? tGround : at_distance_to_top(p, r0, mu);
    return at_integrate_sky_to(p, lut, r0, mu, nu, muS0, tEnd, steps, transmittance);
}

Vec3 at_integrate_sky_to(const AtParams& p, const AtLutView& lut, f32 r0, f32 mu, f32 nu, f32 muS0, f32 tEnd, u32 steps,
                         Vec3* transmittance) {
    March m{};
    if (tEnd > 0.f) {
        marchQuadratic(p, lut, r0, mu, nu, muS0, tEnd, steps, kPhaseSky, (p.flags & kAtFlagMultiScatter) != 0u, m);
    }
    if (transmittance != nullptr) {
        *transmittance = m.thr;
    }
    return m.L;
}

AtTexel sky_view_texel(const AtParams& p, const AtLutView& lut, u32 x, u32 y) {
    const f32 u = static_cast<f32>(x) / static_cast<f32>(p.skyWidth - 1u);
    const f32 v = static_cast<f32>(y) / static_cast<f32>(p.skyHeight - 1u);
    const f32 r = p.up[3];
    const f32 muS0 = p.sunDir[3];
    f32 mu = 0.f;
    f32 lvc = 0.f;
    at_sky_view_dir(p, r, u, v, mu, lvc);
    const f32 sinV = std::sqrt(std::max(0.f, 1.f - mu * mu));
    const f32 sunX = std::sqrt(std::max(0.f, 1.f - muS0 * muS0));
    const f32 nu = sinV * lvc * sunX + mu * muS0;
    // The row decides the ground test (the horizon row v = 0.5 is a tangent ray: sky side), so a texel on the
    // horizon does not flip between a ground hit and a grazing ray with the last ulp of cos().
    f32 tEnd = 0.f;
    if (v > 0.5f) {
        const f32 tGround = at_distance_to_ground(p, r, mu);
        tEnd = tGround >= 0.f ? tGround : std::max(0.f, -r * mu);
    } else {
        tEnd = at_distance_to_top(p, r, mu);
    }
    return texel(at_integrate_sky_to(p, lut, r, mu, nu, muS0, tEnd, p.skySteps), 0.f);
}

void aerial_column(const AtParams& p, const AtLutView& lut, u32 x, u32 y, AtTexel* scatter, AtTexel* transmittance) {
    const f32 ndcX = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(p.apWidth) * 2.f - 1.f;
    const f32 ndcY = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(p.apHeight) * 2.f - 1.f;
    const f32 sx = ndcX * p.camForward[3];
    const f32 sy = ndcY * p.camRight[3];
    Vec3 dir = v3(p.camForward[0] + p.camRight[0] * sx - p.camUp[0] * sy,
                  p.camForward[1] + p.camRight[1] * sx - p.camUp[1] * sy,
                  p.camForward[2] + p.camRight[2] * sx - p.camUp[2] * sy);
    dir = dir * (1.f / std::sqrt(dir.dot(dir)));
    const f32 r0 = p.up[3];
    const f32 mu = dir.x * p.up[0] + dir.y * p.up[1] + dir.z * p.up[2];
    const f32 nu = dir.x * p.sunDir[0] + dir.y * p.sunDir[1] + dir.z * p.sunDir[2];
    const f32 muS0 = p.sunDir[3];
    const f32 tGround = at_distance_to_ground(p, r0, mu);
    const f32 tLimit = tGround >= 0.f ? tGround : at_distance_to_top(p, r0, mu);
    const bool useMs = (p.flags & kAtFlagMultiScatter) != 0u;
    const f32 phaseR = rayleighPhase(nu);
    const f32 phaseM = miePhase(nu, p.mieG);
    const u32 k = p.apStepsPerSlice;
    const usize stride = static_cast<usize>(p.apWidth) * p.apHeight;
    March m{};
    f32 tPrev = 0.f;
    for (u32 z = 0; z < p.apDepth; ++z) {
        const f32 s = static_cast<f32>(z + 1u) / static_cast<f32>(p.apDepth);
        const f32 tNext = std::min(p.cameraPos[3] * s * s, tLimit);
        const f32 dt = std::max(0.f, tNext - tPrev) / static_cast<f32>(k);
        for (u32 i = 0; i < k; ++i) {
            if (dt > 0.f) {
                const f32 t = tPrev + (static_cast<f32>(i) + 0.5f) * dt;
                scatterStep(p, lut, r0, mu, nu, muS0, t, dt, kPhaseSky, useMs, phaseR, phaseM, m);
            }
        }
        tPrev = std::max(tPrev, tNext);
        scatter[z * stride] = texel(m.L, 0.f);
        transmittance[z * stride] = texel(m.thr, 0.f);
    }
}

// --- builders --------------------------------------------------------------------------------------------
void build_transmittance_lut(const AtParams& p, std::vector<AtTexel>& out) {
    out.resize(static_cast<usize>(p.transWidth) * p.transHeight);
    for (u32 y = 0; y < p.transHeight; ++y) {
        for (u32 x = 0; x < p.transWidth; ++x) {
            out[static_cast<usize>(y) * p.transWidth + x] = transmittance_texel(p, x, y);
        }
    }
}

void build_multiscatter_lut(const AtParams& p, const AtLutView& lut, std::vector<AtTexel>& out) {
    out.resize(static_cast<usize>(p.msWidth) * p.msHeight);
    for (u32 y = 0; y < p.msHeight; ++y) {
        for (u32 x = 0; x < p.msWidth; ++x) {
            out[static_cast<usize>(y) * p.msWidth + x] = multiscatter_texel(p, lut, x, y);
        }
    }
}

void build_sky_view_lut(const AtParams& p, const AtLutView& lut, std::vector<AtTexel>& out) {
    out.resize(static_cast<usize>(p.skyWidth) * p.skyHeight);
    for (u32 y = 0; y < p.skyHeight; ++y) {
        for (u32 x = 0; x < p.skyWidth; ++x) {
            out[static_cast<usize>(y) * p.skyWidth + x] = sky_view_texel(p, lut, x, y);
        }
    }
}

void build_aerial_perspective(const AtParams& p, const AtLutView& lut, std::vector<AtTexel>& scatter,
                              std::vector<AtTexel>& transmittance) {
    const usize n = static_cast<usize>(p.apWidth) * p.apHeight * p.apDepth;
    scatter.resize(n);
    transmittance.resize(n);
    for (u32 y = 0; y < p.apHeight; ++y) {
        for (u32 x = 0; x < p.apWidth; ++x) {
            const usize at = static_cast<usize>(y) * p.apWidth + x;
            aerial_column(p, lut, x, y, scatter.data() + at, transmittance.data() + at);
        }
    }
}

void AtCpuLuts::build(const AtParams& p) {
    build_transmittance_lut(p, transmittance);
    AtLutView v = view();
    build_multiscatter_lut(p, v, multiscatter);
    v = view();
    build_sky_view_lut(p, v, skyView);
    build_aerial_perspective(p, v, aerialScatter, aerialTransmittance);
}

AtLutView AtCpuLuts::view() const {
    AtLutView v{};
    v.transmittance = transmittance.empty() ? nullptr : transmittance.data();
    v.multiscatter = multiscatter.empty() ? nullptr : multiscatter.data();
    v.skyView = skyView.empty() ? nullptr : skyView.data();
    v.aerialScatter = aerialScatter.empty() ? nullptr : aerialScatter.data();
    v.aerialTransmittance = aerialTransmittance.empty() ? nullptr : aerialTransmittance.data();
    return v;
}

// --- sampling helpers -----------------------------------------------------------------------------------
AtTexel at_bilinear(const AtTexel* table, u32 w, u32 h, f32 fx, f32 fy) {
    const f32 cx = clampf(fx, 0.f, static_cast<f32>(w - 1u));
    const f32 cy = clampf(fy, 0.f, static_cast<f32>(h - 1u));
    const u32 x0 = std::min(static_cast<u32>(cx), w - 2u);
    const u32 y0 = std::min(static_cast<u32>(cy), h - 2u);
    const f32 tx = cx - static_cast<f32>(x0);
    const f32 ty = cy - static_cast<f32>(y0);
    const AtTexel& a = table[static_cast<usize>(y0) * w + x0];
    const AtTexel& b = table[static_cast<usize>(y0) * w + x0 + 1u];
    const AtTexel& c = table[static_cast<usize>(y0 + 1u) * w + x0];
    const AtTexel& d = table[static_cast<usize>(y0 + 1u) * w + x0 + 1u];
    auto lerp2 = [&](f32 va, f32 vb, f32 vc, f32 vd) {
        const f32 top = va + (vb - va) * tx;
        const f32 bottom = vc + (vd - vc) * tx;
        return top + (bottom - top) * ty;
    };
    return AtTexel{lerp2(a.r, b.r, c.r, d.r), lerp2(a.g, b.g, c.g, d.g), lerp2(a.b, b.b, c.b, d.b),
                   lerp2(a.a, b.a, c.a, d.a)};
}

Vec3 at_transmittance(const AtParams& p, const AtLutView& lut, f32 r, f32 mu) {
    if (lut.transmittance == nullptr || at_distance_to_ground(p, r, mu) >= 0.f) {
        return {};
    }
    f32 xMu = 0.f;
    f32 xR = 0.f;
    at_transmittance_uv(p, r, mu, xMu, xR);
    return texelRgb(at_bilinear(lut.transmittance, p.transWidth, p.transHeight,
                                xMu * static_cast<f32>(p.transWidth - 1u), xR * static_cast<f32>(p.transHeight - 1u)));
}

Vec3 at_sun_transmittance(const AtParams& p, const AtLutView& lut, f32 r, f32 muS) {
    if (lut.transmittance == nullptr) {
        return {};
    }
    // Sun elevation above the local horizon, asin(sin(sun - horizon)) from sines / cosines only (no atan / sin
    // of the dip: the transmittance next to the horizon column magnifies their last ulp).
    const f32 rg = p.bottomRadius;
    const f32 rc = std::max(r, rg);
    const f32 cosH = rg / rc;
    const f32 muH = -std::sqrt(std::max(0.f, (rc - rg) * (rc + rg))) / rc;
    const f32 cosS = std::sqrt(std::max(0.f, 1.f - muS * muS));
    const f32 above = std::asin(clampf(muS * cosH - cosS * muH, -1.f, 1.f));
    const f32 fraction = visibleSunFraction(above / p.sunAngularRadius);
    if (fraction <= 0.f) {
        return {};
    }
    const f32 muC = std::max(muS, muH + 1e-5f * cosH);
    f32 xMu = 0.f;
    f32 xR = 0.f;
    at_transmittance_uv(p, r, muC, xMu, xR);
    const Vec3 t = texelRgb(at_bilinear(lut.transmittance, p.transWidth, p.transHeight,
                                        xMu * static_cast<f32>(p.transWidth - 1u),
                                        xR * static_cast<f32>(p.transHeight - 1u)));
    return t * fraction;
}

Vec3 at_multiscatter(const AtParams& p, const AtLutView& lut, f32 r, f32 muS) {
    if (lut.multiscatter == nullptr) {
        return {};
    }
    const f32 xS = clampf(muS * 0.5f + 0.5f, 0.f, 1.f);
    const f32 xR = clampf((r - p.bottomRadius) / (p.topRadius - p.bottomRadius), 0.f, 1.f);
    return texelRgb(at_bilinear(lut.multiscatter, p.msWidth, p.msHeight, xS * static_cast<f32>(p.msWidth - 1u),
                                xR * static_cast<f32>(p.msHeight - 1u)));
}

Vec3 at_sky_view(const AtParams& p, const AtLutView& lut, const Vec3& dirIn) {
    if (lut.skyView == nullptr) {
        return {};
    }
    const Vec3 dir = dirIn * (1.f / std::sqrt(dirIn.dot(dirIn)));
    const Vec3 up = v3(p.up[0], p.up[1], p.up[2]);
    const Vec3 sun = v3(p.sunDir[0], p.sunDir[1], p.sunDir[2]);
    const f32 mu = dir.dot(up);
    f32 lvc = 1.f;
    Vec3 side = math::cross(up, dir);
    const f32 sideLen = std::sqrt(side.dot(side));
    if (sideLen > 1e-6f) {
        side = side * (1.f / sideLen);
        const Vec3 fwd = math::cross(side, up);
        const f32 lx = sun.dot(fwd);
        const f32 ly = sun.dot(side);
        const f32 l = std::sqrt(lx * lx + ly * ly);
        lvc = l > 1e-6f ? lx / l : 1.f;
    }
    f32 u = 0.f;
    f32 v = 0.f;
    at_sky_view_uv(p, p.up[3], mu, lvc, u, v);
    return texelRgb(at_bilinear(lut.skyView, p.skyWidth, p.skyHeight, u * static_cast<f32>(p.skyWidth - 1u),
                                v * static_cast<f32>(p.skyHeight - 1u)));
}

void at_aerial(const AtParams& p, const AtLutView& lut, f32 u, f32 v, f32 distance, Vec3& scatter, Vec3& trans) {
    scatter = {};
    trans = v3(1.f, 1.f, 1.f);
    if (lut.aerialScatter == nullptr || lut.aerialTransmittance == nullptr) {
        return;
    }
    const u32 w = p.apWidth;
    const u32 h = p.apHeight;
    const u32 d = p.apDepth;
    const f32 fx = u * static_cast<f32>(w) - 0.5f;
    const f32 fy = v * static_cast<f32>(h) - 0.5f;
    const f32 slice = std::sqrt(clampf(distance / p.cameraPos[3], 0.f, 1.f));
    const f32 fz = slice * static_cast<f32>(d) - 1.f;
    const usize stride = static_cast<usize>(w) * h;
    if (fz < 0.f) {
        // Between the camera (scatter 0, transmittance 1) and the first slice.
        const f32 k = fz + 1.f;
        const Vec3 s0 = texelRgb(at_bilinear(lut.aerialScatter, w, h, fx, fy));
        const Vec3 t0 = texelRgb(at_bilinear(lut.aerialTransmittance, w, h, fx, fy));
        scatter = s0 * k;
        trans = v3(1.f + (t0.x - 1.f) * k, 1.f + (t0.y - 1.f) * k, 1.f + (t0.z - 1.f) * k);
        return;
    }
    const f32 cz = std::min(fz, static_cast<f32>(d - 1u));
    const u32 z0 = std::min(static_cast<u32>(cz), d - 2u);
    const f32 tz = cz - static_cast<f32>(z0);
    const Vec3 sA = texelRgb(at_bilinear(lut.aerialScatter + z0 * stride, w, h, fx, fy));
    const Vec3 sB = texelRgb(at_bilinear(lut.aerialScatter + (z0 + 1u) * stride, w, h, fx, fy));
    const Vec3 tA = texelRgb(at_bilinear(lut.aerialTransmittance + z0 * stride, w, h, fx, fy));
    const Vec3 tB = texelRgb(at_bilinear(lut.aerialTransmittance + (z0 + 1u) * stride, w, h, fx, fy));
    scatter = sA + (sB - sA) * tz;
    trans = tA + (tB - tA) * tz;
}

Vec3 at_sky_radiance(const AtParams& p, const AtLutView& lut, const Vec3& dirIn, bool sunDisk) {
    const Vec3 dir = dirIn * (1.f / std::sqrt(dirIn.dot(dirIn)));
    Vec3 L = at_sky_view(p, lut, dir);
    if (sunDisk) {
        const Vec3 sun = v3(p.sunDir[0], p.sunDir[1], p.sunDir[2]);
        const Vec3 c = math::cross(dir, sun);
        const f32 sep = std::atan2(std::sqrt(c.dot(c)), dir.dot(sun));
        if (sep < p.sunAngularRadius) {
            const f32 rho = sep / p.sunAngularRadius;
            const f32 m = std::sqrt(std::max(0.f, 1.f - rho * rho));
            const f32 disk = (1.f - kSunLimbDarkeningU * (1.f - m)) * p.sunDiskNorm;
            const f32 mu = dir.x * p.up[0] + dir.y * p.up[1] + dir.z * p.up[2];
            L = L + at_transmittance(p, lut, p.up[3], mu) * disk;
        }
    }
    return v3(L.x * p.sunIlluminance[0], L.y * p.sunIlluminance[1], L.z * p.sunIlluminance[2]);
}

Vec3 at_sun_illuminance_at(const AtParams& p, const AtLutView& lut, const Vec3& worldPos) {
    const Vec3 q = v3(worldPos.x, worldPos.y + p.bottomRadius, worldPos.z);
    const f32 r = std::sqrt(q.dot(q));
    const f32 muS = (q.x * p.sunDir[0] + q.y * p.sunDir[1] + q.z * p.sunDir[2]) / r;
    const Vec3 t = at_sun_transmittance(p, lut, clampf(r, p.bottomRadius, p.topRadius), muS);
    return v3(t.x * p.sunIlluminance[0], t.y * p.sunIlluminance[1], t.z * p.sunIlluminance[2]);
}

} // namespace fuse::renderer::atmosphere
