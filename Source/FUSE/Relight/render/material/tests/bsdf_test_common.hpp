// FUSE Relight RL-4.3 tests: shared material configurations and a deterministic RNG (CPU gates and the Lavapipe
// parity gate use the same set).
#pragma once

#include <fuse/relight/render/material/bsdf_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace rl_bsdf_test {

namespace bsdf = fuse::relight::bsdf;
using bsdf::BsdfMaterial;
using bsdf::float3;
using bsdf::float4;

/// PCG32 (O'Neill 2014): deterministic across compilers.
struct Rng {
    std::uint64_t state = 0x853c49e6748fea9bull;
    std::uint64_t inc = 0xda3e39cb94b95bdbull;
    explicit Rng(std::uint64_t seed) {
        state = 0u;
        inc = (seed << 1u) | 1u;
        next();
        state += 0x853c49e6748fea9bull + seed;
        next();
    }
    std::uint32_t next() {
        const std::uint64_t old = state;
        state = old * 6364136223846793005ull + inc;
        const auto xorshifted = static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
        const auto rot = static_cast<std::uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }
    /// Uniform in [0, 1) with 24 bits.
    float uniform() { return float(next() >> 8u) * (1.f / 16777216.f); }
    float4 uniform4() { return {uniform(), uniform(), uniform(), uniform()}; }
    float3 sphere() {
        const float z = 1.f - 2.f * uniform();
        const float r = std::sqrt(std::max(0.f, 1.f - z * z));
        const float phi = 6.28318530717959f * uniform();
        return {r * std::cos(phi), r * std::sin(phi), z};
    }
};

inline float3 dirFromAngles(float theta, float phi) {
    return {std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
}

struct Config {
    std::string name;
    BsdfMaterial m{};
    bool hair = false;      ///< directions over the whole sphere (fiber frame)
    bool twoSided = false;  ///< wo may lie below the surface
};

inline BsdfMaterial opaque(float albedo, float roughness, float metallic) {
    BsdfMaterial m = bsdf::bsdfMaterialDefault();
    m.albedo = float3(albedo);
    m.roughness = roughness;
    m.metallic = metallic;
    return m;
}

inline BsdfMaterial hairMaterial(float betaM, float betaN, float h, float3 sigmaA, float alpha) {
    BsdfMaterial m = bsdf::bsdfMaterialDefault();
    m.model = bsdf::kBsdfModelHair;
    m.ior = 1.55f;
    m.hairBetaM = betaM;
    m.hairBetaN = betaN;
    m.hairH = h;
    m.hairSigmaA = sigmaA;
    m.hairAlpha = alpha;
    return m;
}

inline BsdfMaterial translucent(bool thin, float layer) {
    BsdfMaterial m = bsdf::bsdfMaterialDefault();
    m.model = bsdf::kBsdfModelTranslucent;
    m.ior = 1.5f;
    m.transmittance = float3(1.f);
    m.mediumDistance = thin ? 0.01f : 1.f;
    if (thin) {
        m.flags |= bsdf::kBsdfFlagThinWalled;
    }
    if (layer > 0.f) {
        m.flags |= bsdf::kBsdfFlagDiffuseLayer;
        m.layerColor = float3(1.f);
        m.layerOpacity = layer;
    }
    return m;
}

/// Every surface model / option the gates cover (non-white variants included; see `white` in the tests).
inline std::vector<Config> allConfigs() {
    std::vector<Config> c;
    auto add = [&](const char* name, const BsdfMaterial& m, bool hair = false, bool twoSided = false) {
        c.push_back({name, m, hair, twoSided});
    };
    add("opaque_lambert_r0.3", opaque(0.8f, 0.3f, 0.f));
    add("opaque_lambert_r0.6", opaque(0.5f, 0.6f, 0.f));
    add("opaque_lambert_r1.0", opaque(0.9f, 1.f, 0.f));
    add("opaque_metal_r0.4", opaque(0.9f, 0.4f, 1.f));
    {
        BsdfMaterial m = opaque(0.f, 0.5f, 1.f);
        m.albedo = float3(0.95f, 0.64f, 0.54f);
        add("opaque_copper_r0.5", m);
    }
    add("opaque_mixed_m0.5", opaque(0.7f, 0.45f, 0.5f));
    {
        BsdfMaterial m = opaque(0.6f, 0.5f, 0.3f);
        m.anisotropy = 0.6f;
        add("opaque_aniso_0.6", m);
    }
    {
        BsdfMaterial m = opaque(0.6f, 0.4f, 0.f);
        m.opacity = 0.4f;
        add("opaque_opacity_0.4", m);
    }
    {
        BsdfMaterial m = opaque(0.7f, 0.5f, 0.f);
        m.diffuseModel = bsdf::kBsdfDiffuseBurley;
        add("opaque_burley", m);
    }
    {
        BsdfMaterial m = opaque(0.7f, 0.5f, 0.f);
        m.diffuseModel = bsdf::kBsdfDiffuseHammon;
        add("opaque_hammon", m);
    }
    {
        BsdfMaterial m = opaque(0.4f, 0.35f, 0.6f);
        m.flags |= bsdf::kBsdfFlagThinFilm;
        m.thinFilmThickness = 380.f;
        add("opaque_thin_film", m);
    }
    {
        BsdfMaterial m = opaque(0.6f, 0.5f, 0.f);
        m.flags |= bsdf::kBsdfFlagSssThin;
        m.sssMeasurementDistance = 0.3f;
        m.sssTransmittance = float3(0.8f, 0.5f, 0.3f);
        m.sssSingleScatterAlbedo = float3(0.9f, 0.7f, 0.5f);
        m.sssAnisotropy = 0.3f;
        add("opaque_sss_thin", m, false, true);
    }
    {
        BsdfMaterial m = opaque(0.8f, 0.5f, 0.f);
        m.flags |= bsdf::kBsdfFlagSssDiffusion;
        m.sssRadius = float3(1.f, 0.4f, 0.2f);
        add("opaque_sss_diffusion", m);
    }
    add("translucent_thick", translucent(false, 0.f), false, true);
    add("translucent_thin", translucent(true, 0.f), false, true);
    {
        BsdfMaterial m = translucent(false, 0.5f);
        m.transmittance = float3(0.8f, 0.6f, 0.9f);
        m.layerColor = float3(0.9f, 0.8f, 0.5f);
        add("translucent_layer", m, false, true);
    }
    {
        BsdfMaterial m = translucent(true, 0.3f);
        m.transmittance = float3(0.5f, 0.7f, 0.9f);
        add("translucent_thin_layer", m, false, true);
    }
    {
        BsdfMaterial m = bsdf::bsdfMaterialDefault();
        m.model = bsdf::kBsdfModelPortal;
        add("portal", m, false, true);
    }
    add("hair_default", hairMaterial(0.3f, 0.3f, 0.2f, float3(0.f), 0.f), true, true);
    add("hair_tilt", hairMaterial(0.25f, 0.5f, -0.6f, float3(0.2f, 0.4f, 0.8f), 0.0349066f), true, true);
    add("hair_rough", hairMaterial(0.7f, 0.8f, 0.7f, float3(1.2f, 1.5f, 2.f), 0.05f), true, true);
    return c;
}

} // namespace rl_bsdf_test
