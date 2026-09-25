// FUSE Relight RL-4.3: RL-3.2 MaterialParams -> bsdf::BsdfMaterial (see material_bsdf.hpp).
#include <fuse/relight/render/material/material_bsdf.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::relight::render::material {

namespace {

using mods::import::MaterialParams;
using mods::import::ParamType;
using mods::import::SurfaceType;

float scalar(const MaterialParams& p, const char* name, float fallback) {
    const auto it = p.values.find(name);
    return it == p.values.end() ? fallback : it->second.value[0];
}

bsdf::float3 vec3(const MaterialParams& p, const char* name, const bsdf::float3& fallback) {
    const auto it = p.values.find(name);
    if (it == p.values.end()) {
        return fallback;
    }
    return {it->second.value[0], it->second.value[1], it->second.value[2]};
}

bool flag(const MaterialParams& p, const char* name) { return scalar(p, name, 0.f) != 0.f; }

} // namespace

bsdf::float3 gammaToLinear(const bsdf::float3& c) {
    auto g = [](float v) { return std::pow(std::max(v, 0.f), 2.2f); };
    return {g(c.x), g(c.y), g(c.z)};
}

bsdf::BsdfMaterial bsdfMaterialFromParams(const MaterialParams& p, const BsdfMapOptions& options) {
    bsdf::BsdfMaterial m = bsdf::bsdfMaterialDefault();
    m.diffuseModel = options.diffuseModel;
    const bool emissive = flag(p, "enable_emission");
    const float intensity = scalar(p, "emissive_intensity", 40.f);
    switch (p.surface) {
    case SurfaceType::Opaque: {
        m.model = bsdf::kBsdfModelOpaque;
        m.albedo = gammaToLinear(vec3(p, "diffuse_color_constant", bsdf::float3(0.2f)));
        m.opacity = scalar(p, "opacity_constant", 1.f);
        m.roughness = scalar(p, "reflection_roughness_constant", 0.5f);
        m.metallic = scalar(p, "metallic_constant", 0.f);
        m.anisotropy = scalar(p, "anisotropy", 0.f);
        if (flag(p, "enable_thin_film")) {
            m.flags |= bsdf::kBsdfFlagThinFilm;
            m.thinFilmThickness = scalar(p, "thin_film_thickness_constant", 200.f);
        }
        if (emissive) {
            m.emission = gammaToLinear(vec3(p, "emissive_color_constant", bsdf::float3(1.f, 0.1f, 0.1f))) * intensity;
        }
        const bsdf::float3 radius = vec3(p, "subsurface_radius", bsdf::float3(0.5f)) * scalar(p, "subsurface_radius_scale", 1.f);
        const float measurement = scalar(p, "subsurface_measurement_distance", 0.f);
        m.sssTransmittance = gammaToLinear(vec3(p, "subsurface_transmittance_color", bsdf::float3(0.5f)));
        m.sssMeasurementDistance = measurement;
        m.sssSingleScatterAlbedo = vec3(p, "subsurface_single_scattering_albedo", bsdf::float3(0.5f));
        m.sssAnisotropy = scalar(p, "subsurface_volumetric_anisotropy", 0.f);
        m.sssRadius = radius;
        if (flag(p, "subsurface_diffusion_profile") && std::max(radius.x, std::max(radius.y, radius.z)) > 0.f) {
            m.flags |= bsdf::kBsdfFlagSssDiffusion;
        } else if (measurement > 0.f) {
            m.flags |= bsdf::kBsdfFlagSssThin;
        }
        if (options.hairCards) {
            m.model = bsdf::kBsdfModelHair;
            m.hairBetaM = std::clamp(m.roughness, 0.02f, 1.f);
            m.hairBetaN = std::clamp(m.roughness, 0.02f, 1.f);
            m.hairSigmaA = bsdf::bsdfHairSigmaAFromColor(m.albedo, m.hairBetaN);
            m.ior = options.hairIor;
            m.hairAlpha = options.hairAlphaRadians;
        }
        break;
    }
    case SurfaceType::Translucent: {
        m.model = bsdf::kBsdfModelTranslucent;
        m.albedo = bsdf::float3(1.f);
        m.ior = scalar(p, "ior_constant", 1.3f);
        m.transmittance = gammaToLinear(vec3(p, "transmittance_color", bsdf::float3(0.97f)));
        if (flag(p, "thin_walled")) {
            m.flags |= bsdf::kBsdfFlagThinWalled;
            m.mediumDistance = scalar(p, "thin_wall_thickness", 0.001f);
        } else {
            m.mediumDistance = scalar(p, "transmittance_measurement_distance", 1.f);
        }
        if (flag(p, "use_diffuse_layer")) {
            m.flags |= bsdf::kBsdfFlagDiffuseLayer;
            m.layerColor = bsdf::float3(0.f);
            m.layerOpacity = 0.f;
        }
        if (emissive) {
            m.emission = gammaToLinear(vec3(p, "emissive_color_constant", bsdf::float3(1.f, 0.1f, 0.1f))) * intensity;
        }
        break;
    }
    case SurfaceType::Portal: {
        m.model = bsdf::kBsdfModelPortal;
        m.opacity = 1.f;
        if (emissive) {
            m.emission = bsdf::float3(intensity);
        }
        break;
    }
    }
    return m;
}

BsdfTextureSlots bsdfTextureSlots(const MaterialParams& p) {
    BsdfTextureSlots slots;
    for (const auto& desc : mods::import::materialParamTable(p.surface)) {
        if (desc.type != ParamType::Texture) {
            continue;
        }
        const auto it = p.values.find(std::string(desc.name));
        if (it != p.values.end() && !it->second.asset.empty()) {
            slots.params.emplace_back(desc.name);
        }
    }
    return slots;
}

} // namespace fuse::relight::render::material
