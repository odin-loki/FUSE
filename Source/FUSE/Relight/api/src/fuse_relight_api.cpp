// FUSE Relight RL-6.2: the FUSE-native C entry points (fuse_relight_api.h) over ApiRuntime.
#include <fuse/relight/api/api_runtime.hpp>
#include <fuse/relight/api/fuse_relight_api.h>

#include <algorithm>
#include <cstring>
#include <string>

using namespace fuse::relight;
using api::ApiRuntime;

namespace {

/// A 1.0 input struct: non-null and at least its 1.0 size (sizeof in this header, the first version).
template <typename T>
fuse_relight_Result checkInput(const T* desc) {
    if (desc == nullptr) {
        return FUSE_RELIGHT_ERROR_INVALID_ARGUMENT;
    }
    return desc->structSize >= sizeof(T) ? FUSE_RELIGHT_SUCCESS : FUSE_RELIGHT_ERROR_STRUCT_SIZE;
}

fuse_relight_Result rejectIfFailed(ApiRuntime& rt, fuse_relight_Result r) {
    if (r != FUSE_RELIGHT_SUCCESS && r != FUSE_RELIGHT_ERROR_NOT_INITIALIZED) {
        rt.reject();
    }
    return r;
}

std::string str(const char* s) { return s != nullptr ? std::string(s) : std::string(); }

fuse_relight_Result copyRecord(const fuse_relight_FrameRecord& rec, fuse_relight_FrameRecord* out) {
    if (out == nullptr) {
        return FUSE_RELIGHT_SUCCESS;
    }
    const std::uint32_t size = out->structSize;
    if (size < sizeof(std::uint32_t) * 2) {
        return FUSE_RELIGHT_ERROR_STRUCT_SIZE;
    }
    std::memcpy(out, &rec, std::min<std::size_t>(size, sizeof(rec)));
    out->structSize = size; // the caller's size stays (it describes the caller's struct)
    return FUSE_RELIGHT_SUCCESS;
}

} // namespace

extern "C" {

FUSE_RELIGHT_API_EXPORT void FUSE_RELIGHT_CALL fuse_relight_GetVersion(uint32_t* major, uint32_t* minor, uint32_t* patch) {
    if (major != nullptr) {
        *major = FUSE_RELIGHT_API_VERSION_MAJOR;
    }
    if (minor != nullptr) {
        *minor = FUSE_RELIGHT_API_VERSION_MINOR;
    }
    if (patch != nullptr) {
        *patch = FUSE_RELIGHT_API_VERSION_PATCH;
    }
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_Initialize(const fuse_relight_InitInfo* info) {
    if (const fuse_relight_Result r = checkInput(info); r != FUSE_RELIGHT_SUCCESS) {
        return r;
    }
    if (info->versionMajor != FUSE_RELIGHT_API_VERSION_MAJOR || info->versionMinor > FUSE_RELIGHT_API_VERSION_MINOR) {
        return FUSE_RELIGHT_ERROR_INCOMPATIBLE_VERSION;
    }
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    return rt.initialize(info->replacementMode, str(info->modDirectory));
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_Shutdown(void) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    return rt.shutdown();
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_CreateMaterial(const fuse_relight_MaterialDesc* desc,
                                                                                    fuse_relight_Handle* out) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (const fuse_relight_Result r = checkInput(desc); r != FUSE_RELIGHT_SUCCESS || out == nullptr) {
        return rejectIfFailed(rt, r != FUSE_RELIGHT_SUCCESS ? r : FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    if (desc->surfaceType > FUSE_RELIGHT_SURFACE_PORTAL || (desc->paramCount != 0 && desc->params == nullptr)) {
        return rejectIfFailed(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    const auto surface = static_cast<mods::import::SurfaceType>(desc->surfaceType);
    mods::import::MaterialParams params = mods::import::defaultMaterialParams(surface);
    for (std::uint32_t i = 0; i < desc->paramCount; ++i) {
        const fuse_relight_MaterialParam& p = desc->params[i];
        const mods::import::ParamDesc* d = p.name != nullptr ? mods::import::findParam(surface, p.name) : nullptr;
        if (d == nullptr) {
            return rejectIfFailed(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT); // not a parameter of this surface type
        }
        mods::import::ParamValue& v = params.values[std::string(d->name)];
        v.type = d->type;
        if (d->type == mods::import::ParamType::Texture) {
            v.asset = str(p.texture);
        } else {
            v.value = {p.value[0], p.value[1], p.value[2]};
        }
        params.authored.insert(std::string(d->name));
    }
    fuse_relight_Handle h = 0;
    const fuse_relight_Result r = rt.createMaterial(desc->hash, std::move(params), h);
    if (r == FUSE_RELIGHT_SUCCESS) {
        *out = h;
    }
    return rejectIfFailed(rt, r);
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_DestroyMaterial(fuse_relight_Handle material) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    return rejectIfFailed(rt, rt.destroyMaterial(material));
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_CreateMesh(const fuse_relight_MeshDesc* desc,
                                                                                fuse_relight_Handle* out) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (const fuse_relight_Result r = checkInput(desc); r != FUSE_RELIGHT_SUCCESS || out == nullptr) {
        return rejectIfFailed(rt, r != FUSE_RELIGHT_SUCCESS ? r : FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    if (desc->surfaceCount == 0 || desc->surfaces == nullptr) {
        return rejectIfFailed(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    std::vector<api::ApiSurface> surfaces(desc->surfaceCount);
    for (std::uint32_t i = 0; i < desc->surfaceCount; ++i) {
        const fuse_relight_SurfaceDesc& s = desc->surfaces[i];
        if (s.positions == nullptr || s.vertexCount == 0 || (s.indexCount != 0 && s.indices == nullptr)) {
            return rejectIfFailed(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
        }
        api::ApiSurface& d = surfaces[i];
        auto gather = [&](const float* base, std::uint32_t stride, std::uint32_t defaultStride, std::uint32_t n,
                          std::vector<float>& dst) {
            if (base == nullptr) {
                return;
            }
            const std::uint32_t st = stride != 0 ? stride : defaultStride;
            dst.resize(std::size_t(s.vertexCount) * n);
            for (std::uint32_t v = 0; v < s.vertexCount; ++v) {
                std::memcpy(&dst[std::size_t(v) * n], reinterpret_cast<const std::uint8_t*>(base) + std::size_t(v) * st,
                            n * sizeof(float));
            }
        };
        gather(s.positions, s.positionStride, 12, 3, d.positions);
        gather(s.normals, s.normalStride, 12, 3, d.normals);
        gather(s.texcoords, s.texcoordStride, 8, 2, d.texcoords);
        if (s.indexCount != 0) {
            d.indices.assign(s.indices, s.indices + s.indexCount);
        }
        d.material = s.material;
    }
    fuse_relight_Handle h = 0;
    const fuse_relight_Result r = rt.createMesh(desc->hash, std::move(surfaces), h);
    if (r == FUSE_RELIGHT_SUCCESS) {
        *out = h;
    }
    return rejectIfFailed(rt, r);
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_DestroyMesh(fuse_relight_Handle mesh) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    return rejectIfFailed(rt, rt.destroyMesh(mesh));
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_CreateLight(const fuse_relight_LightDesc* desc,
                                                                                 fuse_relight_Handle* out) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (const fuse_relight_Result r = checkInput(desc); r != FUSE_RELIGHT_SUCCESS || out == nullptr) {
        return rejectIfFailed(rt, r != FUSE_RELIGHT_SUCCESS ? r : FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    if (desc->usdType == nullptr || !mods::import::isRemixLightType(desc->usdType) ||
        (desc->paramCount != 0 && desc->params == nullptr)) {
        return rejectIfFailed(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    mods::import::LightParams params;
    params.usdType = desc->usdType;
    for (const mods::import::LightParamDesc& d : mods::import::lightParamTable()) {
        params.values[std::string(d.name)] = d.defaultValue;
    }
    for (std::uint32_t i = 0; i < desc->paramCount; ++i) {
        const fuse_relight_LightParam& p = desc->params[i];
        const auto it = p.name != nullptr ? params.values.find(p.name) : params.values.end();
        if (it == params.values.end()) {
            return rejectIfFailed(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
        }
        it->second = {p.value[0], p.value[1], p.value[2]};
        for (const mods::import::LightParamDesc& d : mods::import::lightParamTable()) {
            if (d.name == it->first) { // upstream's ranges (readLightParams clamps the same way)
                for (std::size_t c = 0; c < (d.vec3 ? 3u : 1u); ++c) {
                    it->second[c] = std::clamp(it->second[c], d.minValue[c], d.maxValue[c]);
                }
            }
        }
        params.authored.insert(it->first);
    }
    lightk::RlLight light{};
    if (!render::lights::lightFromUsd(params, desc->transform, light)) {
        return rejectIfFailed(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT); // zero area (radius / size 0)
    }
    fuse_relight_Handle h = 0;
    const fuse_relight_Result r = rt.createLight(desc->hash, light, true, h);
    if (r == FUSE_RELIGHT_SUCCESS) {
        *out = h;
    }
    return rejectIfFailed(rt, r);
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_DestroyLight(fuse_relight_Handle light) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    return rejectIfFailed(rt, rt.destroyLight(light));
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_SetCamera(const fuse_relight_CameraDesc* desc) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (const fuse_relight_Result r = checkInput(desc); r != FUSE_RELIGHT_SUCCESS) {
        return rejectIfFailed(rt, r);
    }
    api::ApiCamera c;
    c.type = desc->type;
    std::copy(desc->view, desc->view + 16, c.view.begin());
    std::copy(desc->projection, desc->projection + 16, c.projection.begin());
    return rejectIfFailed(rt, rt.setCamera(c));
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_DrawInstance(const fuse_relight_InstanceDesc* desc) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (const fuse_relight_Result r = checkInput(desc); r != FUSE_RELIGHT_SUCCESS) {
        return rejectIfFailed(rt, r);
    }
    api::ApiInstance inst;
    inst.mesh = desc->mesh;
    inst.apiCategories = desc->categoryFlags;
    std::copy(desc->transform, desc->transform + 16, inst.objectToWorld.begin());
    inst.doubleSided = desc->doubleSided != 0;
    return rejectIfFailed(rt, rt.drawInstance(inst));
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_DrawLight(fuse_relight_Handle light) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    return rejectIfFailed(rt, rt.drawLight(light));
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_SetOption(const char* key, const char* value) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (key == nullptr || value == nullptr) {
        return rejectIfFailed(rt, FUSE_RELIGHT_ERROR_INVALID_ARGUMENT);
    }
    return rejectIfFailed(rt, rt.setOption(key, value));
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_GetOption(const char* key, char* buffer,
                                                                               uint32_t bufferSize) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (key == nullptr || buffer == nullptr || bufferSize == 0) {
        return FUSE_RELIGHT_ERROR_INVALID_ARGUMENT;
    }
    std::string value;
    const fuse_relight_Result r = rt.getOption(key, value);
    if (r != FUSE_RELIGHT_SUCCESS) {
        return r;
    }
    const std::size_t n = std::min<std::size_t>(value.size(), bufferSize - 1);
    std::memcpy(buffer, value.data(), n);
    buffer[n] = '\0';
    return FUSE_RELIGHT_SUCCESS;
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_EndFrame(fuse_relight_FrameRecord* out) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (out != nullptr && out->structSize < sizeof(std::uint32_t) * 2) {
        return FUSE_RELIGHT_ERROR_STRUCT_SIZE;
    }
    const fuse_relight_Result r = rt.endFrame();
    return r != FUSE_RELIGHT_SUCCESS ? r : copyRecord(rt.lastFrame().record, out);
}

FUSE_RELIGHT_API_EXPORT fuse_relight_Result FUSE_RELIGHT_CALL fuse_relight_GetFrameRecord(fuse_relight_FrameRecord* out) {
    ApiRuntime& rt = ApiRuntime::global();
    const auto lock = rt.lock();
    if (out == nullptr) {
        return FUSE_RELIGHT_ERROR_INVALID_ARGUMENT;
    }
    if (!rt.hasFrame()) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    return copyRecord(rt.lastFrame().record, out);
}

} // extern "C"
