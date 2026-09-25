#pragma once

// WP-7.3 path-tracing mode: the BSDF of the GPU-scene materials (Material::GPUMaterial rows: base colour,
// metallic = baseColor.w, roughness = roughnessEmissive.x), C++ side. The kernels implement the same model in
// shaders/pathtrace/pt_common.{glsl,slang} (pt_bsdf_*); the CPU reference (pt_reference.hpp) evaluates it in double
// precision. Scalar-templated, header only, device-safe.
//
// Model (a convex mix of two energy-conserving lobes, so a white furnace never gains energy):
//
//   f(wo, wi) = (1 - m) a / pi  +  m F(a, wo.h) D(h) G2(wo, wi) / (4 |wo.n| |wi.n|)       (both above the surface)
//
//   a      base colour, m metallic in [0, 1]
//   D      GGX / Trowbridge-Reitz, alpha = max(roughness, minRoughness)^2
//   G2     Smith height-correlated masking-shadowing 1 / (1 + Lambda(wo) + Lambda(wi))
//   F      Schlick with F0 = a (a conductor tinted by the base colour)
//
// Sampling: the specular lobe with probability m (visible-normal sampling, Heitz 2018 "Sampling the GGX
// Distribution of Visible Normals"), else the cosine-weighted Lambert lobe; the pdf is the mixture
//   pdf(wi) = (1 - m) cos(wi) / pi + m G1(wo) D(h) / (4 cos(wo))
// so one-sample lobe selection is unbiased for every direction either lobe can produce.
//
// Dielectric specular (a Fresnel-weighted GGX layer over the diffuse base), textures, normal maps, transmission
// and the other shading models are not modelled (WP-7.3 open issues).

#include <fuse/types.hpp>

#include <cmath>

namespace fuse::renderer::pathtrace {

template <typename T>
struct PtVec3 {
    T x = T(0);
    T y = T(0);
    T z = T(0);
};

template <typename T>
inline PtVec3<T> ptV(T x, T y, T z) {
    return PtVec3<T>{x, y, z};
}
template <typename T>
inline PtVec3<T> operator+(PtVec3<T> a, PtVec3<T> b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
template <typename T>
inline PtVec3<T> operator-(PtVec3<T> a, PtVec3<T> b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
template <typename T>
inline PtVec3<T> operator*(PtVec3<T> a, T s) {
    return {a.x * s, a.y * s, a.z * s};
}
template <typename T>
inline PtVec3<T> ptMul(PtVec3<T> a, PtVec3<T> b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}
template <typename T>
inline T ptDot(PtVec3<T> a, PtVec3<T> b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
template <typename T>
inline PtVec3<T> ptCross(PtVec3<T> a, PtVec3<T> b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
template <typename T>
inline T ptLength(PtVec3<T> a) {
    return std::sqrt(ptDot(a, a));
}
template <typename T>
inline PtVec3<T> ptNormalize(PtVec3<T> a) {
    const T l = ptLength(a);
    return l > T(0) ? a * (T(1) / l) : PtVec3<T>{T(0), T(0), T(1)};
}
template <typename T>
inline T ptLuminance(PtVec3<T> c) {
    return T(0.2126) * c.x + T(0.7152) * c.y + T(0.0722) * c.z;
}
template <typename T>
inline T ptMaxComponent(PtVec3<T> c) {
    const T m = c.x > c.y ? c.x : c.y;
    return m > c.z ? m : c.z;
}

/// Orthonormal basis around a unit n (Duff et al. 2017, "Building an Orthonormal Basis, Revisited").
template <typename T>
inline void ptBasis(PtVec3<T> n, PtVec3<T>& t, PtVec3<T>& b) {
    const T sign = n.z >= T(0) ? T(1) : T(-1);
    const T a = T(-1) / (sign + n.z);
    const T c = n.x * n.y * a;
    t = PtVec3<T>{T(1) + sign * n.x * n.x * a, sign * c, -sign * n.x};
    b = PtVec3<T>{c, sign + n.y * n.y * a, -n.y};
}

template <typename T>
struct PtMaterialT {
    PtVec3<T> albedo{T(0.8), T(0.8), T(0.8)}; ///< kPtDefaultAlbedo
    T metallic = T(0);
    T roughness = T(1);
};

template <typename T>
struct PtBsdfSample {
    PtVec3<T> wi{};     ///< local frame (z = normal)
    PtVec3<T> weight{}; ///< f cos / pdf
    T pdf = T(0);       ///< mixture pdf (solid angle)
    bool valid = false;
};

template <typename T>
inline T ptAlpha(T roughness, T minRoughness) {
    const T r = roughness > minRoughness ? roughness : minRoughness;
    const T rc = r < T(1) ? r : T(1);
    return rc * rc;
}

template <typename T>
inline T ptGgxD(T alpha, T cosH) {
    const T a2 = alpha * alpha;
    const T c2 = cosH * cosH;
    const T k = c2 * (a2 - T(1)) + T(1);
    return a2 / (T(3.14159265358979323846) * k * k);
}

/// Smith Lambda of GGX for a direction at cos theta = c (> 0).
template <typename T>
inline T ptGgxLambda(T alpha, T c) {
    const T c2 = c * c;
    const T t2 = (T(1) - c2) / c2;
    return (std::sqrt(T(1) + alpha * alpha * t2) - T(1)) * T(0.5);
}

template <typename T>
inline PtVec3<T> ptSchlick(PtVec3<T> f0, T c) {
    const T x = T(1) - c;
    const T x2 = x * x;
    const T x5 = x2 * x2 * x;
    return {f0.x + (T(1) - f0.x) * x5, f0.y + (T(1) - f0.y) * x5, f0.z + (T(1) - f0.z) * x5};
}

/// f(wo, wi) in the local frame (0 unless both lie above the surface).
template <typename T>
inline PtVec3<T> ptBsdfEval(const PtMaterialT<T>& m, T minRoughness, PtVec3<T> wo, PtVec3<T> wi) {
    PtVec3<T> f{};
    if (!(wo.z > T(0)) || !(wi.z > T(0))) {
        return f;
    }
    const T kd = (T(1) - m.metallic) / T(3.14159265358979323846);
    f = m.albedo * kd;
    if (m.metallic > T(0)) {
        const T alpha = ptAlpha(m.roughness, minRoughness);
        const PtVec3<T> h = ptNormalize(wo + wi);
        const T d = ptGgxD(alpha, h.z);
        const T g2 = T(1) / (T(1) + ptGgxLambda(alpha, wo.z) + ptGgxLambda(alpha, wi.z));
        const PtVec3<T> fr = ptSchlick(m.albedo, ptDot(wo, h) > T(0) ? ptDot(wo, h) : T(0));
        const T s = m.metallic * d * g2 / (T(4) * wo.z * wi.z);
        f = f + fr * s;
    }
    return f;
}

/// Mixture pdf of sampling wi (solid angle, local frame).
template <typename T>
inline T ptBsdfPdf(const PtMaterialT<T>& m, T minRoughness, PtVec3<T> wo, PtVec3<T> wi) {
    if (!(wo.z > T(0)) || !(wi.z > T(0))) {
        return T(0);
    }
    T pdf = (T(1) - m.metallic) * wi.z / T(3.14159265358979323846);
    if (m.metallic > T(0)) {
        const T alpha = ptAlpha(m.roughness, minRoughness);
        const PtVec3<T> h = ptNormalize(wo + wi);
        const T g1 = T(1) / (T(1) + ptGgxLambda(alpha, wo.z));
        pdf = pdf + m.metallic * g1 * ptGgxD(alpha, h.z) / (T(4) * wo.z);
    }
    return pdf;
}

/// GGX visible-normal sample (Heitz 2018) for wo in the local frame.
template <typename T>
inline PtVec3<T> ptSampleVndf(T alpha, PtVec3<T> wo, T u1, T u2) {
    const PtVec3<T> vh = ptNormalize(PtVec3<T>{alpha * wo.x, alpha * wo.y, wo.z});
    const T lensq = vh.x * vh.x + vh.y * vh.y;
    PtVec3<T> t1{T(1), T(0), T(0)};
    if (lensq > T(0)) {
        const T inv = T(1) / std::sqrt(lensq);
        t1 = PtVec3<T>{-vh.y * inv, vh.x * inv, T(0)};
    }
    const PtVec3<T> t2 = ptCross(vh, t1);
    const T r = std::sqrt(u1);
    const T phi = T(2) * T(3.14159265358979323846) * u2;
    const T p1 = r * std::cos(phi);
    T p2 = r * std::sin(phi);
    const T s = T(0.5) * (T(1) + vh.z);
    const T q = T(1) - p1 * p1;
    p2 = (T(1) - s) * std::sqrt(q > T(0) ? q : T(0)) + s * p2;
    const T w = T(1) - p1 * p1 - p2 * p2;
    const PtVec3<T> nh = t1 * p1 + t2 * p2 + vh * std::sqrt(w > T(0) ? w : T(0));
    return ptNormalize(PtVec3<T>{alpha * nh.x, alpha * nh.y, nh.z > T(0) ? nh.z : T(0)});
}

/// Cosine-weighted hemisphere direction (local frame).
template <typename T>
inline PtVec3<T> ptSampleCosine(T u1, T u2) {
    const T r = std::sqrt(u1);
    const T phi = T(2) * T(3.14159265358979323846) * u2;
    const T z = T(1) - u1;
    return PtVec3<T>{r * std::cos(phi), r * std::sin(phi), std::sqrt(z > T(0) ? z : T(0))};
}

/// One BSDF sample: lobe choice (uLobe < metallic -> GGX), direction, mixture pdf and weight f cos / pdf.
template <typename T>
inline PtBsdfSample<T> ptBsdfSample(const PtMaterialT<T>& m, T minRoughness, PtVec3<T> wo, T uLobe, T u1, T u2) {
    PtBsdfSample<T> s{};
    if (!(wo.z > T(0))) {
        return s;
    }
    if (uLobe < m.metallic) {
        const PtVec3<T> h = ptSampleVndf(ptAlpha(m.roughness, minRoughness), wo, u1, u2);
        const T c = ptDot(wo, h);
        s.wi = h * (T(2) * c) - wo;
    } else {
        s.wi = ptSampleCosine(u1, u2);
    }
    if (!(s.wi.z > T(0))) {
        return s;
    }
    s.pdf = ptBsdfPdf(m, minRoughness, wo, s.wi);
    if (!(s.pdf > T(0))) {
        return s;
    }
    const PtVec3<T> f = ptBsdfEval(m, minRoughness, wo, s.wi);
    s.weight = f * (s.wi.z / s.pdf);
    s.valid = true;
    return s;
}

/// Power heuristic (beta = 2) weight of strategy a against b.
template <typename T>
inline T ptPowerHeuristic(T a, T b) {
    const T a2 = a * a;
    const T b2 = b * b;
    return a2 + b2 > T(0) ? a2 / (a2 + b2) : T(0);
}

} // namespace fuse::renderer::pathtrace
