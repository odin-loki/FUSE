#pragma once

// Independent double-precision brute-force single-scattering reference of the B5 atmosphere model
// (spherical shells, Rayleigh + Mie with Mie extinction = mie_coeff / 0.9, hard planet shadow, no ground).
// This is the reference of tests/test_b5_atmosphere_gates.cpp ("RefAtmosphere", row 1: midpoint along the
// view ray, composite Simpson for every optical depth), lifted verbatim into a header so the WP-8.2
// Hillaire LUT gates judge the sky against the same integral. It shares no code with the implementations.
// Header-only; test / tool use (it is slow: O(viewSteps x lightIntervals) exp() per ray).

#include <fuse/renderer/atmosphere/atmosphere_params.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::atmosphere {

class BruteForceSingleScatter {
public:
    explicit BruteForceSingleScatter(const AtmosphereParams& p)
        : R(p.earth_radius), Rt(p.atmo_radius), betaR{p.rayleigh_coeff.x, p.rayleigh_coeff.y, p.rayleigh_coeff.z},
          betaMsca(p.mie_coeff), betaMext(static_cast<f64>(p.mie_coeff) / 0.9), HR(p.rayleigh_scale_h),
          HM(p.mie_scale_h), g(p.mie_scatter_dir) {}

    /// Single-scattered radiance per unit solar irradiance at `altitude` metres, view direction (vx, vy, vz)
    /// (y up), sun direction (sx, sy, sz). viewSteps midpoint samples; lightIntervals Simpson intervals.
    void inscatter(f64 altitude, f64 vx, f64 vy, f64 vz, f64 sx, f64 sy, f64 sz, int viewSteps, int lightIntervals,
                   f64 out[3]) const {
        const f64 vl = std::sqrt(vx * vx + vy * vy + vz * vz);
        const f64 sl = std::sqrt(sx * sx + sy * sy + sz * sz);
        const f64 mu = vy / vl;
        const f64 nu = (vx * sx + vy * sy + vz * sz) / (vl * sl);
        const f64 r0 = R + altitude;
        const f64 tEnd = hitsGround(r0, mu) ? distGround(r0, mu) : distTop(r0, mu);
        const f64 phaseR = 3.0 / (16.0 * kPi) * (1.0 + nu * nu);
        const f64 g2 = g * g;
        const f64 phaseM =
            3.0 / (8.0 * kPi) * (1.0 - g2) * (1.0 + nu * nu) / ((2.0 + g2) * std::pow(1.0 + g2 - 2.0 * g * nu, 1.5));
        out[0] = out[1] = out[2] = 0.0;
        const f64 dt = tEnd / viewSteps;
        f64 viewR = 0.0;
        f64 viewM = 0.0;
        f64 prevT = 0.0;
        for (int i = 0; i < viewSteps; ++i) {
            const f64 t = dt * (i + 0.5);
            viewR += simpson([&](f64 s) { return std::exp(-height(r0, mu, s) / HR); }, prevT, t, 8);
            viewM += simpson([&](f64 s) { return std::exp(-height(r0, mu, s) / HM); }, prevT, t, 8);
            prevT = t;
            const f64 h = height(r0, mu, t);
            const f64 r = R + h;
            const f64 muS = (r0 * sy / sl + t * nu) / r;
            if (hitsGround(r, muS)) {
                continue;
            }
            const f64 lenS = distTop(r, muS);
            const f64 sunR = simpson([&](f64 s) { return std::exp(-height(r, muS, s) / HR); }, 0.0, lenS, lightIntervals);
            const f64 sunM = simpson([&](f64 s) { return std::exp(-height(r, muS, s) / HM); }, 0.0, lenS, lightIntervals);
            const f64 rhoR = std::exp(-h / HR);
            const f64 rhoM = std::exp(-h / HM);
            for (int c = 0; c < 3; ++c) {
                const f64 tau = betaR[c] * (viewR + sunR) + betaMext * (viewM + sunM);
                out[c] += std::exp(-tau) * (betaR[c] * rhoR * phaseR + betaMsca * rhoM * phaseM) * dt;
            }
        }
    }

    /// Transmittance from radius r0 along cos(zenith) mu to the top (0 when the ground occludes).
    void transmittance(f64 r0, f64 mu, int intervals, f64 out[3]) const {
        if (hitsGround(r0, mu)) {
            out[0] = out[1] = out[2] = 0.0;
            return;
        }
        const f64 len = distTop(r0, mu);
        const f64 pr = simpson([&](f64 s) { return std::exp(-height(r0, mu, s) / HR); }, 0.0, len, intervals);
        const f64 pm = simpson([&](f64 s) { return std::exp(-height(r0, mu, s) / HM); }, 0.0, len, intervals);
        for (int c = 0; c < 3; ++c) {
            out[c] = std::exp(-(betaR[c] * pr + betaMext * pm));
        }
    }

private:
    static constexpr f64 kPi = 3.14159265358979323846;

    template <typename F>
    static f64 simpson(F f, f64 a, f64 b, int intervals) {
        if (intervals % 2 != 0) {
            ++intervals;
        }
        const f64 h = (b - a) / intervals;
        f64 sum = f(a) + f(b);
        for (int i = 1; i < intervals; ++i) {
            sum += f(a + h * i) * ((i % 2 != 0) ? 4.0 : 2.0);
        }
        return sum * h / 3.0;
    }

    f64 height(f64 r0, f64 mu, f64 t) const { return std::sqrt(r0 * r0 + 2.0 * r0 * mu * t + t * t) - R; }
    f64 distTop(f64 r0, f64 mu) const {
        const f64 disc = r0 * r0 * (mu * mu - 1.0) + Rt * Rt;
        return -r0 * mu + std::sqrt(std::max(0.0, disc));
    }
    bool hitsGround(f64 r0, f64 mu) const { return mu < 0.0 && r0 * r0 * (mu * mu - 1.0) + R * R >= 0.0; }
    f64 distGround(f64 r0, f64 mu) const {
        return -r0 * mu - std::sqrt(std::max(0.0, r0 * r0 * (mu * mu - 1.0) + R * R));
    }

    f64 R;
    f64 Rt;
    f64 betaR[3];
    f64 betaMsca;
    f64 betaMext;
    f64 HR;
    f64 HM;
    f64 g;
};

} // namespace fuse::renderer::atmosphere
