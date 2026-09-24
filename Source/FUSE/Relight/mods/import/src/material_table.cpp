/*
* Copyright (c) 2021-2026, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/
// Modifications Copyright (c) 2026 FUSE contributors (MIT)
// Ported from dxvk-remix src/dxvk/rtx_render/rtx_material_data.h@0867d3c (parameter tables) and
// src/dxvk/rtx_render/rtx_mod_usd.cpp@0867d3c (material type selection). See material_table.hpp.
#include <fuse/relight/mods/import/material_table.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::relight::mods::import {

namespace {

using V3 = std::array<float, 3>;
constexpr V3 s(float v) { return {v, 0.f, 0.f}; }
constexpr V3 v3(float a, float b, float c) { return {a, b, c}; }

constexpr ParamDesc tex(std::string_view name, bool base = false) { return {name, ParamType::Texture, {}, {}, {}, base}; }
constexpr ParamDesc f(std::string_view name, float lo, float hi, float def, bool base = false) {
    return {name, ParamType::Float, s(lo), s(hi), s(def), base};
}
constexpr ParamDesc vec(std::string_view name, V3 lo, V3 hi, V3 def, bool base = false) {
    return {name, ParamType::Vec3, lo, hi, def, base};
}
constexpr ParamDesc b(std::string_view name, bool def) { return {name, ParamType::Bool, s(0.f), s(1.f), s(def ? 1.f : 0.f), false}; }
constexpr ParamDesc u8(std::string_view name, float lo, float hi, float def, bool base = false) {
    return {name, ParamType::U8, s(lo), s(hi), s(def), base};
}

// lss::Mdl::Filter / WrapMode.
constexpr float kFilterNearest = 0.f, kFilterLinear = 1.f;
constexpr float kWrapClamp = 0.f, kWrapRepeat = 1.f, kWrapClip = 3.f;

// LIST_OPAQUE_MATERIAL_TEXTURES + LIST_OPAQUE_MATERIAL_CONSTANTS.
constexpr ParamDesc kOpaque[] = {
    tex("diffuse_texture", true),
    tex("normalmap_texture", true),
    tex("tangent_texture"),
    tex("height_texture", true),
    tex("reflectionroughness_texture"),
    tex("metallic_texture"),
    tex("emissive_mask_texture", true),
    tex("subsurface_transmittance_texture"),
    tex("subsurface_thickness_texture"),
    tex("subsurface_single_scattering_texture"),
    tex("subsurface_radius_texture"),
    tex("secondary_texture"),
    f("anisotropy", 0.f, 1.f, 0.f),
    f("emissive_intensity", 0.f, kFloat16Max, 40.f),
    vec("diffuse_color_constant", v3(0.f, 0.f, 0.f), v3(1.f, 1.f, 1.f), v3(0.2f, 0.2f, 0.2f), true),
    f("opacity_constant", 0.f, 1.f, 1.f, true),
    f("reflection_roughness_constant", 0.f, 1.f, .5f, true),
    f("metallic_constant", 0.f, 1.f, 0.f, true),
    vec("emissive_color_constant", v3(0.f, 0.f, 0.f), v3(1.f, 1.f, 1.f), v3(1.0f, 0.1f, 0.1f)),
    b("enable_emission", false),
    u8("sprite_sheet_rows", 0.f, 255.f, 0.f),
    u8("sprite_sheet_cols", 0.f, 255.f, 0.f),
    u8("sprite_sheet_fps", 0.f, 255.f, 0.f),
    b("enable_thin_film", false),
    b("thin_film_thickness_from_albedo_alpha", false),
    f("thin_film_thickness_constant", .001f, kThinFilmMaxThickness, 200.f),
    b("use_legacy_alpha_state", true),
    b("blend_enabled", false),
    {"blend_type", ParamType::BlendType, s(0.f), s(float(kBlendTypeMax)), s(float(kBlendTypeAlpha)), false},
    b("inverted_blend", false),
    {"alpha_test_type", ParamType::AlphaTestType, s(float(kAlphaTestNever)), s(float(kAlphaTestAlways)),
     s(float(kAlphaTestAlways)), false},
    u8("alpha_test_reference_value", 0.f, 255.f, 0.f, true),
    f("displace_in", 0.f, kFloat16Max, 0.05f),
    f("displace_out", 0.f, kFloat16Max, 0.0f),
    vec("subsurface_transmittance_color", v3(0.f, 0.f, 0.f), v3(1.f, 1.f, 1.f), v3(0.5f, 0.5f, 0.5f)),
    f("subsurface_measurement_distance", 0.f, kFloat16Max, 0.f),
    vec("subsurface_single_scattering_albedo", v3(0.f, 0.f, 0.f), v3(1.f, 1.f, 1.f), v3(0.5f, 0.5f, 0.5f)),
    f("subsurface_volumetric_anisotropy", -1.f, 1.f, 0.f),
    b("subsurface_diffusion_profile", false),
    vec("subsurface_radius", v3(0.f, 0.f, 0.f), v3(kFloat16Max, kFloat16Max, kFloat16Max), v3(0.5f, 0.5f, 0.5f)),
    f("subsurface_radius_scale", 0.f, kFloat16Max, 1.f),
    f("subsurface_max_sample_radius", 0.f, kFloat16Max, 16.f),
    u8("filter_mode", kFilterNearest, kFilterLinear, kFilterLinear),
    u8("wrap_mode_u", kWrapClamp, kWrapClip, kWrapRepeat),
    u8("wrap_mode_v", kWrapClamp, kWrapClip, kWrapRepeat),
    b("enable_dlss_control_mask", true),
    f("dlss_control_mask_intensity", 0.f, 1.f, 1.f),
    f("dlss_control_mask_tone_strength", 0.f, 1.f, 0.3f),
    f("dlss_control_mask_structural_strength", 0.f, 1.f, 0.7f),
};

// LIST_TRANSLUCENT_MATERIAL_TEXTURES + LIST_TRANSLUCENT_MATERIAL_CONSTANTS.
constexpr ParamDesc kTranslucent[] = {
    tex("normalmap_texture", true),
    tex("transmittance_texture"),
    tex("emissive_mask_texture", true),
    f("ior_constant", 1.f, 3.f, 1.3f),
    vec("transmittance_color", v3(0.f, 0.f, 0.f), v3(1.f, 1.f, 1.f), v3(0.97f, 0.97f, 0.97f), true),
    f("transmittance_measurement_distance", .001f, kFloat16Max, 1.f),
    b("enable_emission", false),
    f("emissive_intensity", 0.f, kFloat16Max, 40.f),
    vec("emissive_color_constant", v3(0.f, 0.f, 0.f), v3(1.f, 1.f, 1.f), v3(1.0f, 0.1f, 0.1f)),
    u8("sprite_sheet_rows", 0.f, 255.f, 0.f),
    u8("sprite_sheet_cols", 0.f, 255.f, 0.f),
    u8("sprite_sheet_fps", 0.f, 255.f, 0.f),
    b("thin_walled", false),
    f("thin_wall_thickness", .001f, kFloat16Max, .001f),
    b("use_diffuse_layer", false),
    u8("filter_mode", kFilterNearest, kFilterLinear, kFilterLinear),
    u8("wrap_mode_u", kWrapClamp, kWrapClip, kWrapRepeat),
    u8("wrap_mode_v", kWrapClamp, kWrapClip, kWrapRepeat),
};

// LIST_PORTAL_MATERIAL_TEXTURES (MaskTexture only) + LIST_PORTAL_MATERIAL_CONSTANTS.
constexpr ParamDesc kPortal[] = {
    tex("emissive_mask_texture", true),
    u8("portal_index", 0.f, 255.f, 0.f),
    u8("sprite_sheet_rows", 0.f, 255.f, 0.f),
    u8("sprite_sheet_cols", 0.f, 255.f, 0.f),
    u8("sprite_sheet_fps", 0.f, 255.f, 0.f),
    f("rotation_speed", 0.f, kFloat16Max, 0.f),
    b("enable_emission", false),
    f("emissive_intensity", 0.f, kFloat16Max, 40.f),
    u8("filter_mode", kFilterNearest, kFilterLinear, kFilterLinear),
    u8("wrap_mode_u", kWrapClamp, kWrapClip, kWrapRepeat),
    u8("wrap_mode_v", kWrapClamp, kWrapClip, kWrapRepeat),
};

int components(ParamType t) { return t == ParamType::Vec3 ? 3 : t == ParamType::Texture ? 0 : 1; }
bool isInteger(ParamType t) {
    return t == ParamType::Bool || t == ParamType::U8 || t == ParamType::BlendType || t == ParamType::AlphaTestType;
}

std::string metadataString(const usd::Attribute& a, std::string_view key) {
    for (const auto& [k, v] : a.metadata) {
        if (k == key) {
            if (auto str = v.asString()) {
                return *str;
            }
        }
    }
    return {};
}

float param(const MaterialParams& p, std::string_view name, int c = 0) {
    const auto it = p.values.find(std::string(name));
    return it == p.values.end() ? 0.f : it->second.value[static_cast<std::size_t>(c)];
}

json::Value num(double d) { return json::Value::number(d); }

json::Value paramJson(const ParamDesc& d, const ParamValue& v) {
    switch (d.type) {
    case ParamType::Bool: return json::Value::boolean(v.value[0] != 0.f);
    case ParamType::Vec3: {
        json::Value a = json::Value::array();
        for (float c : v.value) {
            a.push(num(double(c)));
        }
        return a;
    }
    default: return num(double(v.value[0]));
    }
}

bool paramFromJson(const ParamDesc& d, const json::Value& j, ParamValue& out) {
    out.type = d.type;
    switch (d.type) {
    case ParamType::Bool:
        if (j.kind != json::Value::Kind::Bool) {
            return false;
        }
        out.value = s(j.b ? 1.f : 0.f);
        return true;
    case ParamType::Vec3:
        if (!j.isArray() || j.a.size() != 3) {
            return false;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            if (!j.a[i].isNumber()) {
                return false;
            }
            out.value[i] = static_cast<float>(j.a[i].n);
        }
        return true;
    default:
        if (!j.isNumber()) {
            return false;
        }
        out.value = s(static_cast<float>(j.n));
        return true;
    }
}

} // namespace

const char* surfaceTypeName(SurfaceType t) {
    switch (t) {
    case SurfaceType::Opaque: return "opaque";
    case SurfaceType::Translucent: return "translucent";
    case SurfaceType::Portal: return "portal";
    }
    return "opaque";
}

std::optional<SurfaceType> surfaceTypeFromName(std::string_view name) {
    for (SurfaceType t : {SurfaceType::Opaque, SurfaceType::Translucent, SurfaceType::Portal}) {
        if (name == surfaceTypeName(t)) {
            return t;
        }
    }
    return std::nullopt;
}

SurfaceType surfaceTypeFromMdl(std::string_view sourceAsset, bool hasLegacyRayPortalIndex, bool* known) {
    if (known) {
        *known = sourceAsset.empty() || sourceAsset.find("AperturePBR_Opacity.mdl") != std::string_view::npos ||
                 sourceAsset.find("AperturePBR_Portal.mdl") != std::string_view::npos ||
                 sourceAsset.find("AperturePBR_Translucent.mdl") != std::string_view::npos;
    }
    if (sourceAsset.find("AperturePBR_Portal.mdl") != std::string_view::npos) {
        return SurfaceType::Portal;
    }
    if (sourceAsset.find("AperturePBR_Translucent.mdl") != std::string_view::npos) {
        return hasLegacyRayPortalIndex ? SurfaceType::Portal : SurfaceType::Translucent;
    }
    return SurfaceType::Opaque;
}

const char* paramTypeName(ParamType t) {
    switch (t) {
    case ParamType::Texture: return "texture";
    case ParamType::Float: return "float";
    case ParamType::Vec3: return "float3";
    case ParamType::Bool: return "bool";
    case ParamType::U8: return "uint8";
    case ParamType::BlendType: return "blend_type";
    case ParamType::AlphaTestType: return "alpha_test_type";
    }
    return "float";
}

std::span<const ParamDesc> materialParamTable(SurfaceType t) {
    switch (t) {
    case SurfaceType::Opaque: return kOpaque;
    case SurfaceType::Translucent: return kTranslucent;
    case SurfaceType::Portal: return kPortal;
    }
    return kOpaque;
}

const ParamDesc* findParam(SurfaceType t, std::string_view name) {
    for (const ParamDesc& d : materialParamTable(t)) {
        if (d.name == name) {
            return &d;
        }
    }
    return nullptr;
}

MaterialParams defaultMaterialParams(SurfaceType t) {
    MaterialParams p;
    p.surface = t;
    for (const ParamDesc& d : materialParamTable(t)) {
        ParamValue v;
        v.type = d.type;
        v.value = d.type == ParamType::Texture ? V3{} : d.defaultValue;
        p.values.emplace(std::string(d.name), std::move(v));
    }
    return p;
}

MaterialParams readMaterialParams(const usd::Prim* shader, SurfaceType surface, bool allowUnprefixed,
                                  std::vector<ParamIssue>* issues) {
    MaterialParams p = defaultMaterialParams(surface);
    auto issue = [&](const std::string& name, const std::string& msg) {
        if (issues) {
            issues->push_back({name, msg});
        }
    };
    if (!shader) {
        return p;
    }
    auto find = [&](std::string_view token) -> const usd::Attribute* {
        if (const usd::Attribute* a = shader->attribute("inputs:" + std::string(token))) {
            return a;
        }
        return allowUnprefixed ? shader->attribute(token) : nullptr;
    };
    auto flag = [&](std::string_view token) {
        const usd::Attribute* a = shader->attribute("inputs:" + std::string(token));
        return a && a->hasDefault && a->defaultValue->asBool().value_or(false);
    };
    p.ignoreMaterial = flag("ignore_material");
    p.preloadTextures = flag("preload_textures");

    for (const ParamDesc& d : materialParamTable(surface)) {
        const usd::Attribute* a = find(d.name);
        if (!a) {
            continue;
        }
        const std::string name(d.name);
        p.authored.insert(name); // HasAttribute: the parameter is "dirty" even without a value
        ParamValue& out = p.values[name];
        if (!a->hasDefault || a->defaultValue->isNone()) {
            continue; // declared (or blocked) only: the default stays
        }
        const usd::Value& v = a->defaultValue;
        if (d.type == ParamType::Texture) {
            if (v.kind == usd::Value::Kind::Asset || v.kind == usd::Value::Kind::String) {
                out.asset = v.text;
                out.colorSpace = metadataString(*a, "colorSpace");
            } else {
                issue(name, "not an asset value");
            }
            continue;
        }
        if (d.type == ParamType::Vec3) {
            const auto n = v.asNumbers();
            if (!n || n->size() != 3) {
                issue(name, "expected 3 numbers");
                continue;
            }
            for (std::size_t i = 0; i < 3; ++i) {
                out.value[i] = static_cast<float>((*n)[i]);
            }
            continue;
        }
        if (d.type == ParamType::Bool) {
            const auto bv = v.asBool();
            if (!bv) {
                issue(name, "expected a bool");
                continue;
            }
            out.value = s(*bv ? 1.f : 0.f);
            continue;
        }
        const auto n = v.asNumber();
        if (!n || std::isnan(*n)) {
            issue(name, "expected a number");
            continue;
        }
        double x = *n;
        if (isInteger(d.type)) {
            // Integer parameters: upstream reads the attribute's integer type; fractional values are truncated.
            x = std::trunc(x);
        }
        out.value = s(static_cast<float>(std::clamp(x, -double(3.0e38f), double(3.0e38f))));
    }
    for (const std::string& n : sanitizeMaterialParams(p)) {
        issue(n, "clamped to the Remix range");
    }
    return p;
}

std::vector<std::string> sanitizeMaterialParams(MaterialParams& p) {
    std::vector<std::string> changed;
    for (const ParamDesc& d : materialParamTable(p.surface)) {
        if (d.type == ParamType::Texture) {
            continue;
        }
        auto it = p.values.find(std::string(d.name));
        if (it == p.values.end()) {
            continue;
        }
        bool c = false;
        for (int i = 0; i < components(d.type); ++i) {
            const auto k = static_cast<std::size_t>(i);
            const float before = it->second.value[k];
            float after = std::clamp(before, d.minValue[k], d.maxValue[k]);
            if (std::isnan(before)) {
                after = d.defaultValue[k];
            }
            if (!(after == before)) {
                it->second.value[k] = after;
                c = true;
            }
        }
        if (c) {
            changed.emplace_back(d.name);
        }
    }
    return changed;
}

std::string_view textureSetSlot(SurfaceType t, std::string_view name) {
    const ParamDesc* d = findParam(t, name);
    if (!d || d->type != ParamType::Texture || !d->inBaseMaterial) {
        return {};
    }
    if (name == "diffuse_texture") {
        return "albedo";
    }
    if (name == "normalmap_texture") {
        return "normal";
    }
    if (name == "height_texture") {
        return "height";
    }
    if (name == "emissive_mask_texture") {
        return "emissive";
    }
    return {};
}

std::string materialModel(const MaterialParams& p) {
    if (p.surface == SurfaceType::Translucent) {
        return "Translucent";
    }
    if (p.surface == SurfaceType::Portal) {
        return "Opaque";
    }
    if (param(p, "blend_enabled") != 0.f) {
        return "Translucent";
    }
    if (param(p, "alpha_test_type") != float(kAlphaTestAlways)) {
        return "Masked";
    }
    if (param(p, "subsurface_measurement_distance") > 0.f || param(p, "subsurface_diffusion_profile") != 0.f) {
        return "Subsurface";
    }
    return "Opaque";
}

json::Value materialPayload(const MaterialParams& p, const std::string& textureSetId) {
    json::Value m = json::Value::object();
    m["model"] = json::Value::string(materialModel(p));
    m["textureSetId"] = json::Value::string(textureSetId);
    json::Value base = json::Value::array();
    float rgba[4] = {1.f, 1.f, 1.f, 1.f};
    float roughness = 0.5f, metallic = 0.f, alphaCutoff = 0.5f;
    bool twoSided = false;
    if (p.surface == SurfaceType::Opaque) {
        rgba[0] = param(p, "diffuse_color_constant", 0);
        rgba[1] = param(p, "diffuse_color_constant", 1);
        rgba[2] = param(p, "diffuse_color_constant", 2);
        rgba[3] = param(p, "opacity_constant");
        roughness = param(p, "reflection_roughness_constant");
        metallic = param(p, "metallic_constant");
    } else if (p.surface == SurfaceType::Translucent) {
        rgba[0] = param(p, "transmittance_color", 0);
        rgba[1] = param(p, "transmittance_color", 1);
        rgba[2] = param(p, "transmittance_color", 2);
        roughness = 0.f;
        twoSided = param(p, "thin_walled") != 0.f;
    }
    for (float c : rgba) {
        base.push(num(double(c)));
    }
    m["baseColor"] = std::move(base);
    m["roughness"] = num(double(roughness));
    m["metallic"] = num(double(metallic));
    m["emissiveNits"] = num(param(p, "enable_emission") != 0.f ? double(param(p, "emissive_intensity")) : 0.0);
    // alpha_test_reference_value / 255 exactly (the ext does not repeat it).
    m["alphaCutoff"] = p.surface == SurfaceType::Opaque ? num(double(param(p, "alpha_test_reference_value")) / 255.0)
                                                         : num(double(alphaCutoff));
    m["twoSided"] = json::Value::boolean(twoSided);
    m["physicalCategory"] = json::Value::string("");
    m["layerRecipe"] = json::Value::string("");
    return m;
}

json::Value materialExtPayload(const MaterialParams& p, const TextureExtras& extras) {
    json::Value e = json::Value::object();
    e["surface"] = json::Value::string(surfaceTypeName(p.surface));
    json::Value mdl = json::Value::object();
    mdl["sourceAsset"] = json::Value::string(p.mdlSourceAsset);
    mdl["subIdentifier"] = json::Value::string(p.mdlSubIdentifier);
    e["mdl"] = std::move(mdl);
    e["ignore_material"] = json::Value::boolean(p.ignoreMaterial);
    e["preload_textures"] = json::Value::boolean(p.preloadTextures);
    json::Value params = json::Value::object();
    json::Value textures = json::Value::object();
    for (const ParamDesc& d : materialParamTable(p.surface)) {
        const std::string name(d.name);
        const auto it = p.values.find(name);
        if (it == p.values.end()) {
            continue;
        }
        if (d.type == ParamType::Texture) {
            if (!p.authored.count(name)) {
                continue;
            }
            json::Value t = json::Value::object();
            t["asset"] = json::Value::string(it->second.asset);
            t["colorSpace"] = json::Value::string(it->second.colorSpace);
            const std::string_view slot = textureSetSlot(p.surface, name);
            t["slot"] = slot.empty() ? json::Value() : json::Value::string(std::string(slot));
            if (auto x = extras.find(name); x != extras.end() && x->second.isObject()) {
                for (const auto& [k, v] : x->second.o) {
                    t[k] = v;
                }
            }
            textures[name] = std::move(t);
            continue;
        }
        if (d.inBaseMaterial) {
            continue; // carried by the base Material
        }
        params[name] = paramJson(d, it->second);
    }
    e["params"] = std::move(params);
    e["textures"] = std::move(textures);
    json::Value authored = json::Value::array();
    for (const std::string& a : p.authored) {
        authored.push(json::Value::string(a));
    }
    e["authored"] = std::move(authored);
    return e;
}

std::optional<MaterialParams> materialParamsFromPoco(const json::Value& m, const json::Value& e, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<MaterialParams> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    if (!m.isObject() || !e.isObject()) {
        return fail("material / material_ext payload is not an object");
    }
    const auto surface = surfaceTypeFromName(e.str("surface"));
    if (!surface) {
        return fail("material_ext.surface '" + e.str("surface") + "' is unknown");
    }
    MaterialParams p = defaultMaterialParams(*surface);
    if (const json::Value* mdl = e.get("mdl")) {
        p.mdlSourceAsset = mdl->str("sourceAsset");
        p.mdlSubIdentifier = mdl->str("subIdentifier");
    }
    p.ignoreMaterial = e.flag("ignore_material");
    p.preloadTextures = e.flag("preload_textures");
    if (const json::Value* a = e.get("authored"); a && a->isArray()) {
        for (const json::Value& n : a->a) {
            if (!findParam(*surface, n.s)) {
                return fail("material_ext.authored names unknown parameter '" + n.s + "'");
            }
            p.authored.insert(n.s);
        }
    }
    const json::Value* params = e.get("params");
    const json::Value* textures = e.get("textures");
    const json::Value* baseColor = m.get("baseColor");
    if (!params || !params->isObject() || !textures || !textures->isObject() || !baseColor || !baseColor->isArray() ||
        baseColor->a.size() != 4) {
        return fail("material / material_ext payload is missing params, textures or baseColor");
    }
    auto baseF = [&](std::size_t i) { return static_cast<float>(baseColor->a[i].n); };
    for (const ParamDesc& d : materialParamTable(*surface)) {
        const std::string name(d.name);
        ParamValue& v = p.values[name];
        if (d.type == ParamType::Texture) {
            if (const json::Value* t = textures->get(name)) {
                v.asset = t->str("asset");
                v.colorSpace = t->str("colorSpace");
            }
            continue;
        }
        if (d.inBaseMaterial) {
            if (name == "diffuse_color_constant" || name == "transmittance_color") {
                v.value = {baseF(0), baseF(1), baseF(2)};
            } else if (name == "opacity_constant") {
                v.value = s(baseF(3));
            } else if (name == "reflection_roughness_constant") {
                v.value = s(static_cast<float>(m.num("roughness")));
            } else if (name == "metallic_constant") {
                v.value = s(static_cast<float>(m.num("metallic")));
            } else if (name == "alpha_test_reference_value") {
                v.value = s(static_cast<float>(std::lround(m.num("alphaCutoff") * 255.0)));
            } else {
                return fail("no base mapping for " + name);
            }
            continue;
        }
        const json::Value* j = params->get(name);
        if (!j || !paramFromJson(d, *j, v)) {
            return fail("material_ext.params." + name + " is missing or has the wrong type");
        }
    }
    return p;
}

} // namespace fuse::relight::mods::import
