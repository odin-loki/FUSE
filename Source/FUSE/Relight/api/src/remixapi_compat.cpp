// FUSE Relight RL-6.2: the Remix API 0.6 compatible front end (remixapi_compat.h) over ApiRuntime.
//
// Semantics follow the documented contract of public/include/remix/remix_c.h@0867d3c (MIT); the implementation is
// FUSE's own (no code from rtx_remix_api.cpp). Mapping:
//
//   InitializeLibrary        sType + version negotiation (major.minor 0.6, any patch; the interface is written up to
//                            the entries of the caller's patch), starts the runtime (replacements from the
//                            relight.replace.* options) when it is not running
//   Startup / Shutdown       Startup records nothing else (FUSE renders through the D3D9 device); Shutdown stops the
//                            runtime and destroys every object
//   CreateMaterial           MaterialInfo + Opaque / OpaqueSubsurface / Translucent / Portal EXT -> the AperturePBR
//                            parameters by USD token (the same names mods use), sanitized, -> the BSDF material
//   CreateMesh               HardcodedVertex surfaces (position / normal / texcoord; colour and skinning are not used)
//   CreateLight              LightInfo + Sphere / Rect / Disk / Cylinder / Distant EXT (shaping included) or USD EXT
//                            (lightFromUsd); DomeLight EXT is accepted and never enters the light set
//   SetupCamera              CameraInfo (view / projection) or its Parameterized EXT
//   DrawInstance             InstanceInfo (category bits, 3x4 transform, doubleSided); pNext EXTs are ignored
//   DrawLightInstance        the light joins this frame's light set
//   SetConfigVariable        the "Relight API" option layer
//   Present                  ends the API frame; with a registered D3D9 device (d3d9.dll only) it then presents that
//                            device (PresentEx with hwndOverride)
//   dxvk_CreateD3D9          d3d9.dll only: this DLL's Direct3DCreate9Ex
//   dxvk_RegisterD3D9Device  remembers the device Present presents
//   other entries            REMIXAPI_ERROR_CODE_GENERAL_FAILURE (no FUSE system behind them yet): SetCameraMediumMaterial,
//                            dxvk_GetExternalSwapchain, dxvk_GetVkImage, dxvk_CopyRenderingOutput,
//                            dxvk_SetDefaultOutput, pick_RequestObjectPicking, pick_HighlightObjects
#include <fuse/relight/api/api_runtime.hpp>
#include <fuse/relight/api/remixapi_compat.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#if defined(_WIN32) && defined(FUSE_RELIGHT_API_BUILD_DLL)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#define FUSE_RELIGHT_API_HAVE_D3D9 1
#endif

using namespace fuse::relight;
using api::ApiRuntime;
namespace lk = fuse::relight::lightk;
namespace imp = fuse::relight::mods::import;

namespace {

struct ChainHeader {
    remixapi_StructType sType;
    void* pNext;
};

/// The first struct of type `type` in a pNext chain (bounded walk).
template <typename T>
const T* findExt(const void* pNext, remixapi_StructType type) {
    const auto* h = static_cast<const ChainHeader*>(pNext);
    for (int guard = 0; h != nullptr && guard < 64; ++guard) {
        if (h->sType == type) {
            return reinterpret_cast<const T*>(h);
        }
        h = static_cast<const ChainHeader*>(h->pNext);
    }
    return nullptr;
}

remixapi_ErrorCode toRemix(fuse_relight_Result r) {
    switch (r) {
    case FUSE_RELIGHT_SUCCESS: return REMIXAPI_ERROR_CODE_SUCCESS;
    case FUSE_RELIGHT_ERROR_NOT_INITIALIZED: return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    case FUSE_RELIGHT_ERROR_INCOMPATIBLE_VERSION: return REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION;
    case FUSE_RELIGHT_ERROR_INVALID_ARGUMENT:
    case FUSE_RELIGHT_ERROR_STRUCT_SIZE:
    case FUSE_RELIGHT_ERROR_UNKNOWN_HANDLE:
    case FUSE_RELIGHT_ERROR_UNKNOWN_OPTION: return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    default: return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
}

remixapi_ErrorCode finish(ApiRuntime& rt, fuse_relight_Result r) {
    if (r != FUSE_RELIGHT_SUCCESS && r != FUSE_RELIGHT_ERROR_NOT_INITIALIZED) {
        rt.reject();
    }
    return toRemix(r);
}

/// remixapi_Path (wchar_t: UTF-16 on Windows, UTF-32 elsewhere) -> UTF-8.
std::string utf8(const wchar_t* s) {
    std::string out;
    if (s == nullptr) {
        return out;
    }
    auto put = [&](std::uint32_t c) {
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    };
    for (; *s != 0; ++s) {
        std::uint32_t c = std::uint32_t(*s);
        if constexpr (sizeof(wchar_t) == 2) {
            c &= 0xFFFFu;
            if (c >= 0xD800 && c < 0xDC00 && (std::uint32_t(s[1]) & 0xFFFFu) >= 0xDC00 &&
                (std::uint32_t(s[1]) & 0xFFFFu) < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + ((std::uint32_t(s[1]) & 0xFFFFu) - 0xDC00);
                ++s;
            }
        }
        put(c);
    }
    return out;
}

class ParamWriter {
public:
    explicit ParamWriter(imp::SurfaceType t) : m_params(imp::defaultMaterialParams(t)) {}
    void f(const char* name, float v) { set(name, {v, 0.f, 0.f}); }
    void b(const char* name, bool v) { f(name, v ? 1.f : 0.f); }
    void v3(const char* name, const remixapi_Float3D& v) { set(name, {v.x, v.y, v.z}); }
    void tex(const char* name, remixapi_Path path) {
        if (path == nullptr || path[0] == 0) {
            return;
        }
        if (imp::ParamValue* p = find(name)) {
            p->asset = utf8(path);
            m_params.authored.insert(name);
        }
    }
    imp::MaterialParams take() { return std::move(m_params); }

private:
    imp::ParamValue* find(const char* name) {
        const auto it = m_params.values.find(name);
        return it == m_params.values.end() ? nullptr : &it->second;
    }
    void set(const char* name, std::array<float, 3> v) {
        if (imp::ParamValue* p = find(name)) {
            p->value = v;
            m_params.authored.insert(name);
        }
    }
    imp::MaterialParams m_params;
};

template <typename H>
H toHandle(api::Handle h) {
    return reinterpret_cast<H>(static_cast<std::uintptr_t>(h));
}
template <typename H>
api::Handle fromHandle(H h) {
    return static_cast<api::Handle>(reinterpret_cast<std::uintptr_t>(h));
}

lk::float3 f3(const remixapi_Float3D& v) { return lk::float3(v.x, v.y, v.z); }

constexpr float kDegToRad = 3.14159265358979323846f / 180.f;

void applyShaping(lk::RlLight& L, remixapi_Bool has, const remixapi_LightInfoLightShaping& s) {
    if (has) {
        render::lights::setShaping(L, f3(s.direction), s.coneAngleDegrees * kDegToRad, s.coneSoftness, s.focusExponent);
    }
}

/// Planar lights emit along `direction`: flip v when u x v points away.
void orient(lk::float3& u, lk::float3& v, const remixapi_Float3D& direction) {
    const lk::float3 n(u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x);
    if (n.x * direction.x + n.y * direction.y + n.z * direction.z < 0.f) {
        v = lk::float3(-v.x, -v.y, -v.z);
    }
}

// ---- interface functions ----------------------------------------------------------------------------------------

remixapi_ErrorCode REMIXAPI_CALL rlShutdown(void) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    return toRemix(rt.shutdown());
}

remixapi_ErrorCode REMIXAPI_CALL rlStartup(const remixapi_StartupInfo* info) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (info == nullptr || info->sType != REMIXAPI_STRUCT_TYPE_STARTUP_INFO) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!rt.initialized()) {
        return toRemix(rt.initialize(1, ""));
    }
    return REMIXAPI_ERROR_CODE_SUCCESS;
}

remixapi_ErrorCode REMIXAPI_CALL rlCreateMaterial(const remixapi_MaterialInfo* info, remixapi_MaterialHandle* out) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (info == nullptr || out == nullptr || info->sType != REMIXAPI_STRUCT_TYPE_MATERIAL_INFO) {
        return finish(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    const auto* opaque = findExt<remixapi_MaterialInfoOpaqueEXT>(info->pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT);
    const auto* sss = findExt<remixapi_MaterialInfoOpaqueSubsurfaceEXT>(
        info->pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT);
    const auto* translucent =
        findExt<remixapi_MaterialInfoTranslucentEXT>(info->pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT);
    const auto* portal = findExt<remixapi_MaterialInfoPortalEXT>(info->pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT);
    const imp::SurfaceType surface = translucent != nullptr ? imp::SurfaceType::Translucent
                                     : portal != nullptr    ? imp::SurfaceType::Portal
                                                            : imp::SurfaceType::Opaque;
    ParamWriter p(surface);
    // MaterialInfo (every surface type has these tokens where upstream's tables have them).
    p.tex(surface == imp::SurfaceType::Opaque ? "diffuse_texture" : "unused", info->albedoTexture);
    p.tex("normalmap_texture", info->normalTexture);
    p.tex("tangent_texture", info->tangentTexture);
    p.tex("emissive_mask_texture", info->emissiveTexture);
    p.f("emissive_intensity", info->emissiveIntensity);
    p.v3("emissive_color_constant", info->emissiveColorConstant);
    p.b("enable_emission", info->emissiveIntensity > 0.f);
    p.f("sprite_sheet_rows", float(info->spriteSheetRow));
    p.f("sprite_sheet_cols", float(info->spriteSheetCol));
    p.f("sprite_sheet_fps", float(info->spriteSheetFps));
    p.f("filter_mode", float(info->filterMode));
    p.f("wrap_mode_u", float(info->wrapModeU));
    p.f("wrap_mode_v", float(info->wrapModeV));
    if (surface == imp::SurfaceType::Opaque && opaque != nullptr) {
        const remixapi_MaterialInfoOpaqueEXT& o = *opaque;
        p.tex("reflectionroughness_texture", o.roughnessTexture);
        p.tex("metallic_texture", o.metallicTexture);
        p.f("anisotropy", o.anisotropy);
        p.v3("diffuse_color_constant", o.albedoConstant);
        p.f("opacity_constant", o.opacityConstant);
        p.f("reflection_roughness_constant", o.roughnessConstant);
        p.f("metallic_constant", o.metallicConstant);
        p.b("enable_thin_film", o.thinFilmThickness_hasvalue != 0);
        if (o.thinFilmThickness_hasvalue) {
            p.f("thin_film_thickness_constant", o.thinFilmThickness_value);
        }
        p.b("thin_film_thickness_from_albedo_alpha", o.alphaIsThinFilmThickness != 0);
        p.tex("height_texture", o.heightTexture);
        p.f("displace_in", o.displaceIn);
        p.f("displace_out", o.displaceOut);
        p.b("use_legacy_alpha_state", o.useDrawCallAlphaState != 0);
        p.b("blend_enabled", o.blendType_hasvalue != 0);
        if (o.blendType_hasvalue) {
            p.f("blend_type", float(o.blendType_value));
        }
        p.b("inverted_blend", o.invertedBlend != 0);
        p.f("alpha_test_type", float(o.alphaTestType));
        p.f("alpha_test_reference_value", float(o.alphaReferenceValue));
        p.b("enable_dlss_control_mask", o.enableDlssControlMask != 0);
        p.f("dlss_control_mask_intensity", o.dlssControlMaskIntensity);
        p.f("dlss_control_mask_tone_strength", o.dlssControlMaskToneStrength);
        p.f("dlss_control_mask_structural_strength", o.dlssControlMaskStructuralStrength);
        if (sss != nullptr) {
            const remixapi_MaterialInfoOpaqueSubsurfaceEXT& s = *sss;
            p.tex("subsurface_transmittance_texture", s.subsurfaceTransmittanceTexture);
            p.tex("subsurface_thickness_texture", s.subsurfaceThicknessTexture);
            p.tex("subsurface_single_scattering_texture", s.subsurfaceSingleScatteringAlbedoTexture);
            p.tex("subsurface_radius_texture", s.subsurfaceRadiusTexture);
            p.v3("subsurface_transmittance_color", s.subsurfaceTransmittanceColor);
            p.f("subsurface_measurement_distance", s.subsurfaceMeasurementDistance);
            p.v3("subsurface_single_scattering_albedo", s.subsurfaceSingleScatteringAlbedo);
            p.f("subsurface_volumetric_anisotropy", s.subsurfaceVolumetricAnisotropy);
            p.b("subsurface_diffusion_profile", s.subsurfaceDiffusionProfile != 0);
            p.v3("subsurface_radius", s.subsurfaceRadius);
            p.f("subsurface_radius_scale", s.subsurfaceRadiusScale);
            p.f("subsurface_max_sample_radius", s.subsurfaceMaxSampleRadius);
        }
    } else if (surface == imp::SurfaceType::Translucent) {
        const remixapi_MaterialInfoTranslucentEXT& t = *translucent;
        p.tex("transmittance_texture", t.transmittanceTexture);
        p.f("ior_constant", t.refractiveIndex);
        p.v3("transmittance_color", t.transmittanceColor);
        p.f("transmittance_measurement_distance", t.transmittanceMeasurementDistance);
        p.b("thin_walled", t.thinWallThickness_hasvalue != 0);
        if (t.thinWallThickness_hasvalue) {
            p.f("thin_wall_thickness", t.thinWallThickness_value);
        }
        p.b("use_diffuse_layer", t.useDiffuseLayer != 0);
    } else if (surface == imp::SurfaceType::Portal) {
        p.f("portal_index", float(portal->rayPortalIndex));
        p.f("rotation_speed", portal->rotationSpeed);
    }
    api::Handle h = 0;
    const fuse_relight_Result r = rt.createMaterial(info->hash, p.take(), h);
    if (r == FUSE_RELIGHT_SUCCESS) {
        *out = toHandle<remixapi_MaterialHandle>(h);
    }
    return finish(rt, r);
}

remixapi_ErrorCode REMIXAPI_CALL rlDestroyMaterial(remixapi_MaterialHandle handle) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    return finish(rt, rt.destroyMaterial(fromHandle(handle)));
}

remixapi_ErrorCode REMIXAPI_CALL rlCreateMesh(const remixapi_MeshInfo* info, remixapi_MeshHandle* out) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (info == nullptr || out == nullptr || info->sType != REMIXAPI_STRUCT_TYPE_MESH_INFO || info->surfaces_count == 0 ||
        info->surfaces_values == nullptr) {
        return finish(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    std::vector<api::ApiSurface> surfaces(info->surfaces_count);
    for (std::uint32_t i = 0; i < info->surfaces_count; ++i) {
        const remixapi_MeshInfoSurfaceTriangles& s = info->surfaces_values[i];
        if (s.vertices_values == nullptr || s.vertices_count == 0 || s.vertices_count > 0xFFFFFFFFull ||
            (s.indices_count != 0 && s.indices_values == nullptr) || s.indices_count > 0xFFFFFFFFull) {
            return finish(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
        }
        api::ApiSurface& d = surfaces[i];
        const std::size_t n = std::size_t(s.vertices_count);
        d.positions.resize(n * 3);
        d.normals.resize(n * 3);
        d.texcoords.resize(n * 2);
        for (std::size_t v = 0; v < n; ++v) {
            const remixapi_HardcodedVertex& hv = s.vertices_values[v];
            std::memcpy(&d.positions[v * 3], hv.position, sizeof(hv.position));
            std::memcpy(&d.normals[v * 3], hv.normal, sizeof(hv.normal));
            std::memcpy(&d.texcoords[v * 2], hv.texcoord, sizeof(hv.texcoord));
        }
        if (s.indices_count != 0) {
            d.indices.assign(s.indices_values, s.indices_values + s.indices_count);
        }
        d.material = fromHandle(s.material);
    }
    api::Handle h = 0;
    const fuse_relight_Result r = rt.createMesh(info->hash, std::move(surfaces), h);
    if (r == FUSE_RELIGHT_SUCCESS) {
        *out = toHandle<remixapi_MeshHandle>(h);
    }
    return finish(rt, r);
}

remixapi_ErrorCode REMIXAPI_CALL rlDestroyMesh(remixapi_MeshHandle handle) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    return finish(rt, rt.destroyMesh(fromHandle(handle)));
}

remixapi_ErrorCode REMIXAPI_CALL rlSetupCamera(const remixapi_CameraInfo* info) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (info == nullptr || info->sType != REMIXAPI_STRUCT_TYPE_CAMERA_INFO || unsigned(info->type) > 2u) {
        return finish(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    api::ApiCamera c;
    c.type = std::uint32_t(info->type);
    if (const auto* p = findExt<remixapi_CameraInfoParameterizedEXT>(info->pNext,
                                                                     REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT)) {
        // Left-handed D3D view (rows: right, up, forward) and perspective projection from the parameters.
        const lk::float3 pos = f3(p->position), r = f3(p->right), u = f3(p->up), f = f3(p->forward);
        auto dot = [](const lk::float3& a, const lk::float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        c.view = {r.x, u.x, f.x, 0.f, r.y, u.y, f.y, 0.f, r.z, u.z, f.z, 0.f, -dot(pos, r), -dot(pos, u), -dot(pos, f), 1.f};
        const float yScale = 1.f / std::tan(0.5f * p->fovYInDegrees * kDegToRad);
        const float xScale = p->aspect != 0.f ? yScale / p->aspect : yScale;
        const float range = p->farPlane - p->nearPlane;
        const float q = range != 0.f ? p->farPlane / range : 1.f;
        c.projection = {xScale, 0.f, 0.f, 0.f, 0.f, yScale, 0.f, 0.f, 0.f, 0.f, q, 1.f, 0.f, 0.f, -q * p->nearPlane, 0.f};
    } else {
        std::memcpy(c.view.data(), info->view, sizeof(info->view));
        std::memcpy(c.projection.data(), info->projection, sizeof(info->projection));
    }
    return finish(rt, rt.setCamera(c));
}

remixapi_ErrorCode REMIXAPI_CALL rlDrawInstance(const remixapi_InstanceInfo* info) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (info == nullptr || info->sType != REMIXAPI_STRUCT_TYPE_INSTANCE_INFO) {
        return finish(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    api::ApiInstance inst;
    inst.mesh = fromHandle(info->mesh);
    inst.apiCategories = info->categoryFlags;
    inst.objectToWorld = api::matrixFromRemixTransform(info->transform.matrix);
    inst.doubleSided = info->doubleSided != 0;
    return finish(rt, rt.drawInstance(inst));
}

remixapi_ErrorCode REMIXAPI_CALL rlCreateLight(const remixapi_LightInfo* info, remixapi_LightHandle* out) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (info == nullptr || out == nullptr || info->sType != REMIXAPI_STRUCT_TYPE_LIGHT_INFO) {
        return finish(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    const lk::float3 radiance = f3(info->radiance);
    lk::RlLight L = lk::rlLightNone();
    bool supported = true;
    if (const auto* usd = findExt<remixapi_LightInfoUSDEXT>(info->pNext, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT)) {
        imp::LightParams params;
        switch (usd->lightType) {
        case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT: params.usdType = "SphereLight"; break;
        case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT: params.usdType = "RectLight"; break;
        case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT: params.usdType = "DiskLight"; break;
        case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT: params.usdType = "CylinderLight"; break;
        case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT: params.usdType = "DistantLight"; break;
        case REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT: supported = false; break;
        default: return finish(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
        }
        if (supported) {
            for (const imp::LightParamDesc& d : imp::lightParamTable()) {
                params.values[std::string(d.name)] = d.defaultValue;
            }
            auto set = [&](const char* name, const float* v, float scale = 1.f) {
                if (v != nullptr) {
                    params.values[name] = {*v * scale, 0.f, 0.f};
                    params.authored.insert(name);
                }
            };
            constexpr float kRadToDeg = 180.f / 3.14159265358979323846f;
            set("radius", usd->pRadius);
            set("width", usd->pWidth);
            set("height", usd->pHeight);
            set("length", usd->pLength);
            set("angle", usd->pAngleRadians, kRadToDeg);
            set("colorTemperature", usd->pColorTemp);
            set("exposure", usd->pExposure);
            set("intensity", usd->pIntensity);
            set("shaping:cone:angle", usd->pConeAngleRadians, kRadToDeg);
            set("shaping:cone:softness", usd->pConeSoftness);
            set("shaping:focus", usd->pFocus);
            set("volumetric_radiance_scale", usd->pVolumetricRadianceScale);
            if (usd->pEnableColorTemp != nullptr) {
                params.values["enableColorTemperature"] = {*usd->pEnableColorTemp ? 1.f : 0.f, 0.f, 0.f};
            }
            if (usd->pColor != nullptr) {
                params.values["color"] = {usd->pColor->x, usd->pColor->y, usd->pColor->z};
            }
            const scene::instances::Mat4f m = api::matrixFromRemixTransform(usd->transform.matrix);
            if (!render::lights::lightFromUsd(params, m.data(), L)) {
                return finish(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
            }
        }
    } else if (const auto* s = findExt<remixapi_LightInfoSphereEXT>(info->pNext, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT)) {
        L = render::lights::makeSphereLight(f3(s->position), s->radius, radiance);
        applyShaping(L, s->shaping_hasvalue, s->shaping_value);
        L.volumetricScale = s->volumetricRadianceScale;
    } else if (const auto* s = findExt<remixapi_LightInfoRectEXT>(info->pNext, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT)) {
        lk::float3 u = f3(s->xAxis) * (0.5f * s->xSize), v = f3(s->yAxis) * (0.5f * s->ySize);
        orient(u, v, s->direction);
        L = render::lights::makeRectLight(f3(s->position), u, v, radiance);
        applyShaping(L, s->shaping_hasvalue, s->shaping_value);
        L.volumetricScale = s->volumetricRadianceScale;
    } else if (const auto* s = findExt<remixapi_LightInfoDiskEXT>(info->pNext, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT)) {
        lk::float3 u = f3(s->xAxis) * s->xRadius, v = f3(s->yAxis) * s->yRadius;
        orient(u, v, s->direction);
        L = render::lights::makeDiskLight(f3(s->position), u, v, radiance);
        applyShaping(L, s->shaping_hasvalue, s->shaping_value);
        L.volumetricScale = s->volumetricRadianceScale;
    } else if (const auto* s =
                   findExt<remixapi_LightInfoCylinderEXT>(info->pNext, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT)) {
        L = render::lights::makeCylinderLight(f3(s->position), f3(s->axis) * (0.5f * s->axisLength), s->radius, radiance);
        L.volumetricScale = s->volumetricRadianceScale;
    } else if (const auto* s = findExt<remixapi_LightInfoDistantEXT>(info->pNext, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT)) {
        L = render::lights::makeDistantLight(f3(s->direction), 0.5f * s->angularDiameterDegrees * kDegToRad, radiance);
        L.volumetricScale = s->volumetricRadianceScale;
    } else if (findExt<remixapi_LightInfoDomeEXT>(info->pNext, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT) != nullptr) {
        supported = false;
    } else {
        return finish(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT); // no light shape
    }
    api::Handle h = 0;
    const fuse_relight_Result r = rt.createLight(info->hash, L, supported, h);
    if (r == FUSE_RELIGHT_SUCCESS) {
        *out = toHandle<remixapi_LightHandle>(h);
    }
    return finish(rt, r);
}

remixapi_ErrorCode REMIXAPI_CALL rlDestroyLight(remixapi_LightHandle handle) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    return finish(rt, rt.destroyLight(fromHandle(handle)));
}

remixapi_ErrorCode REMIXAPI_CALL rlDrawLightInstance(remixapi_LightHandle handle) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    return finish(rt, rt.drawLight(fromHandle(handle)));
}

remixapi_ErrorCode REMIXAPI_CALL rlSetConfigVariable(const char* key, const char* value) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (key == nullptr || value == nullptr) {
        return finish(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    return finish(rt, rt.setOption(key, value));
}

#if FUSE_RELIGHT_API_HAVE_D3D9
IDirect3DDevice9Ex* g_device = nullptr;
#endif

remixapi_ErrorCode REMIXAPI_CALL rlPresent(const remixapi_PresentInfo* info) {
    ApiRuntime& rt = ApiRuntime::global();
    remixapi_HWND hwnd = nullptr;
    {
        const auto lock = rt.lock();
        if (info != nullptr && info->sType != REMIXAPI_STRUCT_TYPE_PRESENT_INFO) {
            return finish(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
        }
        hwnd = info != nullptr ? info->hwndOverride : nullptr;
        const fuse_relight_Result r = rt.endFrame();
        if (r != FUSE_RELIGHT_SUCCESS) {
            return toRemix(r);
        }
    }
#if FUSE_RELIGHT_API_HAVE_D3D9
    // Outside the API lock: the device's Present runs the tap (which may read the API frame).
    if (g_device != nullptr) {
        const HRESULT hr = g_device->PresentEx(nullptr, nullptr, reinterpret_cast<HWND>(hwnd), nullptr, 0);
        if (FAILED(hr)) {
            return static_cast<remixapi_ErrorCode>(hr);
        }
    }
#else
    (void)hwnd;
#endif
    return REMIXAPI_ERROR_CODE_SUCCESS;
}

remixapi_ErrorCode REMIXAPI_CALL rlCreateD3D9(remixapi_Bool editorModeEnabled, IDirect3D9Ex** out) {
    (void)editorModeEnabled;
    if (out == nullptr) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
#if FUSE_RELIGHT_API_HAVE_D3D9
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&rlCreateD3D9), &self)) {
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    using PFN_Create = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
    const auto create = reinterpret_cast<PFN_Create>(reinterpret_cast<void*>(GetProcAddress(self, "Direct3DCreate9Ex")));
    if (create == nullptr) {
        return REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE;
    }
    const HRESULT hr = create(D3D_SDK_VERSION, out);
    return SUCCEEDED(hr) ? REMIXAPI_ERROR_CODE_SUCCESS : static_cast<remixapi_ErrorCode>(hr);
#else
    *out = nullptr;
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE; // only Relight's d3d9.dll creates D3D9 objects
#endif
}

remixapi_ErrorCode REMIXAPI_CALL rlRegisterD3D9Device(IDirect3DDevice9Ex* device) {
    if (device == nullptr) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
#if FUSE_RELIGHT_API_HAVE_D3D9
    const auto lock = ApiRuntime::global().lock();
    if (g_device != nullptr && g_device != device) {
        return REMIXAPI_ERROR_CODE_ALREADY_EXISTS;
    }
    g_device = device; // not AddRef'd (upstream keeps a plain pointer too); Shutdown forgets it
    return REMIXAPI_ERROR_CODE_SUCCESS;
#else
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
#endif
}

remixapi_ErrorCode REMIXAPI_CALL rlSetCameraMediumMaterial(const remixapi_CameraMediumInfo*) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
}
remixapi_ErrorCode REMIXAPI_CALL rlGetExternalSwapchain(uint64_t*, uint64_t*, uint64_t*) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
}
remixapi_ErrorCode REMIXAPI_CALL rlGetVkImage(IDirect3DSurface9*, uint64_t*) { return REMIXAPI_ERROR_CODE_GENERAL_FAILURE; }
remixapi_ErrorCode REMIXAPI_CALL rlCopyRenderingOutput(IDirect3DSurface9*, remixapi_dxvk_CopyRenderingOutputType) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
}
remixapi_ErrorCode REMIXAPI_CALL rlSetDefaultOutput(remixapi_dxvk_CopyRenderingOutputType, const remixapi_Float4D*) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
}
remixapi_ErrorCode REMIXAPI_CALL rlRequestObjectPicking(const remixapi_Rect2D*, PFN_remixapi_pick_RequestObjectPickingUserCallback,
                                                        void*) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
}
remixapi_ErrorCode REMIXAPI_CALL rlHighlightObjects(const uint32_t*, uint32_t, uint8_t, uint8_t, uint8_t) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
}

remixapi_ErrorCode REMIXAPI_CALL rlShutdownAndForgetDevice(void) {
#if FUSE_RELIGHT_API_HAVE_D3D9
    {
        const auto lock = ApiRuntime::global().lock();
        g_device = nullptr;
    }
#endif
    return rlShutdown();
}

} // namespace

extern "C" FUSE_RELIGHT_API_EXPORT remixapi_ErrorCode REMIXAPI_CALL remixapi_InitializeLibrary(
    const remixapi_InitializeLibraryInfo* info, remixapi_Interface* out) {
    if (info == nullptr || out == nullptr || info->sType != REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO) {
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    const std::uint64_t major = REMIXAPI_VERSION_GET_MAJOR(info->version);
    const std::uint64_t minor = REMIXAPI_VERSION_GET_MINOR(info->version);
    const std::uint64_t patch = REMIXAPI_VERSION_GET_PATCH(info->version);
    if (major != REMIXAPI_VERSION_MAJOR || minor != REMIXAPI_VERSION_MINOR) {
        return REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION;
    }
    // Field by field: a client built against an older 0.6 patch has a shorter remixapi_Interface (entries are only
    // ever appended). SetCameraMediumMaterial (StructType 27, the newest) is taken as the 0.6.5 addition.
    out->Shutdown = &rlShutdownAndForgetDevice;
    out->CreateMaterial = &rlCreateMaterial;
    out->DestroyMaterial = &rlDestroyMaterial;
    out->CreateMesh = &rlCreateMesh;
    out->DestroyMesh = &rlDestroyMesh;
    out->SetupCamera = &rlSetupCamera;
    out->DrawInstance = &rlDrawInstance;
    out->CreateLight = &rlCreateLight;
    out->DestroyLight = &rlDestroyLight;
    out->DrawLightInstance = &rlDrawLightInstance;
    out->SetConfigVariable = &rlSetConfigVariable;
    out->dxvk_CreateD3D9 = &rlCreateD3D9;
    out->dxvk_RegisterD3D9Device = &rlRegisterD3D9Device;
    out->dxvk_GetExternalSwapchain = &rlGetExternalSwapchain;
    out->dxvk_GetVkImage = &rlGetVkImage;
    out->dxvk_CopyRenderingOutput = &rlCopyRenderingOutput;
    out->dxvk_SetDefaultOutput = &rlSetDefaultOutput;
    out->pick_RequestObjectPicking = &rlRequestObjectPicking;
    out->pick_HighlightObjects = &rlHighlightObjects;
    out->Startup = &rlStartup;
    out->Present = &rlPresent;
    if (patch >= 5) {
        out->SetCameraMediumMaterial = &rlSetCameraMediumMaterial;
    }
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    return rt.initialized() ? REMIXAPI_ERROR_CODE_SUCCESS : toRemix(rt.initialize(1, ""));
}
