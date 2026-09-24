// FUSE Relight RL-4.4: the Relight light set (see light_set.hpp).
#include <fuse/relight/render/lights/light_set.hpp>

#include "light_kernels.hpp"

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/relight/mods/import/light_table.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/scene/lights/legacy_light.hpp>
#include <fuse/relight/scene/lights/light_options.hpp>
#include <fuse/relight/scene/lights/light_translator.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <variant>

namespace fuse::relight::render::lights {

namespace lt = renderer::light_tree;
using lk::float3;
using lk::float4;
using lk::RlLight;

namespace {

float maxc(const float3& c) { return std::max(c.x, std::max(c.y, c.z)); }

bool finite3(const float3& c) { return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z); }

void arr(const float3& v, f32 (&out)[3]) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

float3 xformPoint(const float* m, const float3& p) {
    if (m == nullptr) {
        return p;
    }
    return {p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12], p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
            p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]};
}

float3 xformVector(const float* m, const float3& d) {
    if (m == nullptr) {
        return d;
    }
    return {d.x * m[0] + d.y * m[4] + d.z * m[8], d.x * m[1] + d.y * m[5] + d.z * m[9],
            d.x * m[2] + d.y * m[6] + d.z * m[10]};
}

template <class T>
bool optionValue(const char* name, T& out) {
    if (const options::OptionBase* o = options::OptionManager::findOption(name)) {
        const options::OptionValue v = o->getResolvedValue();
        if (const T* x = std::get_if<T>(&v)) {
            out = *x;
            return true;
        }
    }
    return false;
}

} // namespace

// ---- conversions ---------------------------------------------------------------------------------------------------

lk::RlConvertParams convertParamsFromOptions() {
    using scene::LightOptions;
    lk::RlConvertParams p{};
    p.sphereRadius = LightOptions::lightConversionSphereLightFixedRadius() * LightOptions::sceneScale();
    p.intensityFactor = LightOptions::lightConversionIntensityFactor();
    p.maxIntensity = LightOptions::lightConversionMaxIntensity();
    p.distantIntensity = LightOptions::lightConversionDistantLightFixedIntensity();
    p.distantAngle = LightOptions::lightConversionDistantLightFixedAngle();
    p.leastSquares = LightOptions::calculateLightIntensityUsingLeastSquares() ? 1u : 0u;
    return p;
}

void packD3dLight(const tap::Light& l, float4* w) {
    w[0] = float4(static_cast<float>(l.type), l.diffuse.r, l.diffuse.g, l.diffuse.b);
    w[1] = float4(l.position.x, l.position.y, l.position.z, l.range);
    w[2] = float4(l.direction.x, l.direction.y, l.direction.z, l.falloff);
    w[3] = float4(l.attenuation0, l.attenuation1, l.attenuation2, l.theta);
    w[4] = float4(l.phi, 0.f, 0.f, 0.f);
}

lt::LightTreeLight treeProxy(const RlLight& L, u32 source) {
    f32 p[3];
    arr(L.position, p);
    const float r = maxc(L.radiance);
    switch (L.kind) {
    case lk::kRlKindSphere: {
        const float intensity = r * lk::kRlPi * L.radius * L.radius;
        if ((L.flags & lk::kRlFlagShaped) != 0u && L.cosCone > -1.f) {
            f32 axis[3];
            arr(L.axis, axis);
            const float cosInner = std::min(1.f, L.cosCone + L.softness);
            return lt::makeSpotLight(p, axis, cosInner, L.cosCone, intensity, source);
        }
        return lt::makePointLight(p, intensity, source);
    }
    case lk::kRlKindCylinder: {
        // Projected area bound 2r x 2h: an intensity upper bound of the tube seen from any side.
        const float intensity = r * 4.f * L.radius * lk::length(L.u);
        return lt::makePointLight(p, intensity, source);
    }
    case lk::kRlKindRect:
    case lk::kRlKindDisk: {
        f32 u[3];
        f32 v[3];
        arr(L.u, u);
        arr(L.v, v);
        const bool two = (L.flags & lk::kRlFlagTwoSided) != 0u;
        return L.kind == lk::kRlKindRect ? lt::makeRectLight(p, u, v, r, two, source)
                                         : lt::makeDiskLight(p, u, v, r, two, source);
    }
    case lk::kRlKindTriangle: {
        f32 v1[3];
        f32 v2[3];
        arr(L.position + L.u, v1);
        arr(L.position + L.v, v2);
        return lt::makeTriangleLight(p, v1, v2, r, (L.flags & lk::kRlFlagTwoSided) != 0u, source);
    }
    case lk::kRlKindDistant:
    default: {
        f32 d[3];
        arr(L.u, d);
        return lt::makeDirectionalLight(d, r, source);
    }
    }
}

RlLight makeSphereLight(const float3& center, float radius, const float3& radiance) {
    RlLight L = lk::rlLightNone();
    L.kind = lk::kRlKindSphere;
    L.position = center;
    L.radius = radius;
    L.radiance = radiance;
    L.area = lk::rlLightArea(L);
    return L;
}

RlLight makeRectLight(const float3& center, const float3& halfU, const float3& halfV, const float3& radiance,
                      bool twoSided) {
    RlLight L = lk::rlLightNone();
    L.kind = lk::kRlKindRect;
    L.position = center;
    L.u = halfU;
    L.v = halfV;
    L.radiance = radiance;
    L.flags = twoSided ? lk::kRlFlagTwoSided : 0u;
    L.area = lk::rlLightArea(L);
    L.axis = lk::rlPlanarNormal(L);
    return L;
}

RlLight makeDiskLight(const float3& center, const float3& radiusU, const float3& radiusV, const float3& radiance,
                      bool twoSided) {
    RlLight L = makeRectLight(center, radiusU, radiusV, radiance, twoSided);
    L.kind = lk::kRlKindDisk;
    L.area = lk::rlLightArea(L);
    return L;
}

RlLight makeCylinderLight(const float3& center, const float3& axisHalfLength, float radius, const float3& radiance) {
    RlLight L = lk::rlLightNone();
    L.kind = lk::kRlKindCylinder;
    L.position = center;
    L.u = axisHalfLength;
    L.radius = radius;
    L.radiance = radiance;
    L.area = lk::rlLightArea(L);
    return L;
}

RlLight makeDistantLight(const float3& direction, float halfAngle, const float3& irradiance) {
    RlLight L = lk::rlLightNone();
    L.kind = lk::kRlKindDistant;
    L.u = lk::rlSafeNormalize(direction, float3(0.f, 0.f, 1.f));
    L.radius = std::max(halfAngle, 0.f);
    L.radiance = irradiance;
    L.area = lk::rlLightArea(L);
    return L;
}

RlLight makeTriangleLight(const float3& v0, const float3& v1, const float3& v2, const float3& radiance, bool twoSided) {
    RlLight L = lk::rlLightNone();
    L.kind = lk::kRlKindTriangle;
    L.position = v0;
    L.u = v1 - v0;
    L.v = v2 - v0;
    L.radiance = radiance;
    L.flags = twoSided ? lk::kRlFlagTwoSided : 0u;
    L.area = lk::rlLightArea(L);
    L.axis = lk::rlPlanarNormal(L);
    return L;
}

void setShaping(RlLight& L, const float3& axis, float coneAngle, float softness, float focus) {
    L.axis = lk::rlSafeNormalize(axis, float3(0.f, 0.f, 1.f));
    L.cosCone = std::cos(std::clamp(coneAngle, 0.f, lk::kRlPi));
    L.softness = std::max(softness, 0.f);
    L.focus = std::max(focus, 0.f);
    const bool shaped = L.cosCone > -1.f || L.softness != 0.f || L.focus != 0.f;
    L.flags = shaped ? (L.flags | lk::kRlFlagShaped) : (L.flags & ~lk::kRlFlagShaped);
}

bool lightFromUsd(const mods::import::LightParams& params, const float* m, RlLight& out, const char** warning) {
    out = lk::rlLightNone();
    auto value = [&](const char* name, float def) {
        const auto it = params.values.find(name);
        return it == params.values.end() ? def : it->second[0];
    };
    float3 color(1.f, 1.f, 1.f);
    if (const auto it = params.values.find("color"); it != params.values.end()) {
        color = float3(it->second[0], it->second[1], it->second[2]);
    }
    if (warning != nullptr) {
        *warning = value("enableColorTemperature", 0.f) != 0.f ? "enableColorTemperature is not applied" : nullptr;
    }
    // UsdLux: radiance = color x intensity x 2^exposure (Remix: LightData::calculateRadiance without temperature).
    const float scale = value("intensity", 1.f) * std::exp2(value("exposure", 0.f));
    const float3 radiance = color * scale;
    const float3 origin = xformPoint(m, float3(0.f, 0.f, 0.f));
    const float3 ax = xformVector(m, float3(1.f, 0.f, 0.f));
    const float3 ay = xformVector(m, float3(0.f, 1.f, 0.f));
    const float3 az = xformVector(m, float3(0.f, 0.f, 1.f));
    const float sx = lk::length(ax);
    const float sy = lk::length(ay);
    const float sz = lk::length(az);
    const float3 down = lk::rlSafeNormalize(-az, float3(0.f, 0.f, -1.f)); // UsdLux emission / shaping axis (-Z)
    constexpr float kDeg = 3.14159265358979323846f / 180.f;
    const std::string& t = params.usdType;
    if (t == "SphereLight") {
        out = makeSphereLight(origin, value("radius", 0.f) * std::max({sx, sy, sz}), radiance);
    } else if (t == "RectLight") {
        out = makeRectLight(origin, ax * (0.5f * value("width", 0.f)), ay * (-0.5f * value("height", 0.f)), radiance);
    } else if (t == "DiskLight") {
        const float r = value("radius", 0.f);
        out = makeDiskLight(origin, ax * r, ay * -r, radiance);
    } else if (t == "CylinderLight") {
        out = makeCylinderLight(origin, ax * (0.5f * value("length", 0.f)), value("radius", 0.f) * 0.5f * (sy + sz),
                                radiance);
    } else if (t == "DistantLight") {
        out = makeDistantLight(down, 0.5f * value("angle", 0.f) * kDeg, radiance);
    } else {
        return false;
    }
    out.volumetricScale = value("volumetric_radiance_scale", 1.f);
    if (t != "DistantLight") {
        const float cone = value("shaping:cone:angle", 180.f);
        const float softness = value("shaping:cone:softness", 0.f);
        const float focus = value("shaping:focus", 0.f);
        if (cone < 180.f || softness != 0.f || focus != 0.f) {
            setShaping(out, down, cone * kDeg, softness, focus);
        }
    }
    return out.area > 0.f || out.kind == lk::kRlKindDistant;
}

FallbackLight fallbackLightFromOptions() {
    FallbackLight f;
    std::int32_t mode = 1;
    if (optionValue("rtx.fallbackLightMode", mode)) {
        f.mode = static_cast<u32>(std::clamp(mode, 0, 2));
    }
    options::Vec3f v;
    if (optionValue("rtx.fallbackLightRadiance", v)) {
        f.radiance = float3(v.x, v.y, v.z);
    }
    if (optionValue("rtx.fallbackLightDirection", v)) {
        f.direction = float3(v.x, v.y, v.z);
    }
    float angle = 0.f;
    if (optionValue("rtx.fallbackLightAngle", angle)) {
        f.angle = angle;
    }
    return f;
}

// ---- RelightLightSet ------------------------------------------------------------------------------------------------

void RelightLightSet::reserve(u32 lights, u32 gameLights) {
    m_d3d.reserve(std::size_t(gameLights) * lk::kRlD3dWords);
    m_gameEntries.reserve(gameLights);
    m_frameHashes.reserve(gameLights);
    m_hostTable.reserve(std::size_t(lights) * lk::kRlLightWords);
    m_hostEntries.reserve(lights);
    m_table.reserve(std::size_t(lights + gameLights) * lk::kRlLightWords);
    m_entries.reserve(lights + gameLights);
    m_proxies.reserve(lights + gameLights);
    m_prevKinds.reserve(lights + gameLights);
    m_tree.reserve(lights + gameLights);
}

void RelightLightSet::beginFrame() {
    m_params = convertParamsFromOptions();
    m_d3d.clear();
    m_hostTable.clear();
    m_gameEntries.clear();
    m_hostEntries.clear();
    m_frameHashes.clear();
    m_gameCount = 0;
    const u32 builds = m_stats.builds;
    const u32 refits = m_stats.refits;
    m_stats = LightSetStats{};
    m_stats.builds = builds;
    m_stats.refits = refits;
}

bool RelightLightSet::addGameLight(const tap::Light& light) {
    if (light.type < lk::kRlD3dPoint || light.type > lk::kRlD3dDirectional || !scene::acceptsGameLightType(light.type)) {
        ++m_stats.gameRejected;
        return false;
    }
    float4 words[lk::kRlD3dWords];
    packD3dLight(light, words);
    const RlLight L = lk::rlConvertD3dLight(lk::rlD3dLightUnpack(words), m_params);
    // FUSE addition to RL-1.5's rules: a non-finite radiance (a black D3D diffuse divides by zero) is dropped too.
    if (L.kind == lk::kRlKindNone || !finite3(L.radiance)) {
        ++m_stats.gameRejected;
        return false;
    }
    const hash::Hash64 h = scene::stableLightHash(light);
    if (std::find(m_frameHashes.begin(), m_frameHashes.end(), h) != m_frameHashes.end()) {
        ++m_stats.gameRejected;
        return false;
    }
    m_frameHashes.push_back(h);
    m_d3d.insert(m_d3d.end(), words, words + lk::kRlD3dWords);
    m_gameEntries.push_back(LightEntry{h, LightOrigin::Game, light.index});
    ++m_stats.gameLights;
    return true;
}

u32 RelightLightSet::addGameLights(const tap::Light* lights, u32 count) {
    u32 added = 0;
    for (u32 i = 0; i < count; ++i) {
        if (lights[i].enabled && addGameLight(lights[i])) {
            ++added;
        }
    }
    return added;
}

bool RelightLightSet::pushLight(const RlLight& L, LightOrigin origin, u64 key, u32 source) {
    if (L.kind == lk::kRlKindNone || L.kind >= lk::kRlKindCount || !finite3(L.radiance) ||
        !(maxc(L.radiance) > 0.f) || (L.kind != lk::kRlKindDistant && !(L.area > 0.f))) {
        return false;
    }
    float4 w[lk::kRlLightWords];
    lk::rlLightPack(L, w);
    m_hostTable.insert(m_hostTable.end(), w, w + lk::kRlLightWords);
    m_hostEntries.push_back(LightEntry{key, origin, source});
    return true;
}

bool RelightLightSet::addLight(const RlLight& light, u64 key) {
    if (!pushLight(light, LightOrigin::Authored, key, 0u)) {
        return false;
    }
    ++m_stats.authored;
    return true;
}

u32 RelightLightSet::addEmissiveTriangles(const EmissiveMesh& mesh) {
    if (mesh.positions == nullptr || !(maxc(mesh.radiance) > 0.f) || !finite3(mesh.radiance)) {
        return 0;
    }
    const u32 triangles = mesh.indices != nullptr ? mesh.indexCount / 3u : mesh.vertexCount / 3u;
    auto vertex = [&](u32 i) {
        const auto* base = reinterpret_cast<const unsigned char*>(mesh.positions) + std::size_t(i) * mesh.stride;
        float xyz[3];
        std::memcpy(xyz, base, sizeof(xyz));
        return xformPoint(mesh.objectToWorld, float3(xyz[0], xyz[1], xyz[2]));
    };
    auto index = [&](u32 k) -> u32 {
        if (mesh.indices == nullptr) {
            return k;
        }
        return mesh.index32 ? static_cast<const std::uint32_t*>(mesh.indices)[k]
                            : static_cast<const std::uint16_t*>(mesh.indices)[k];
    };
    u32 added = 0;
    for (u32 t = 0; t < triangles; ++t) {
        const u32 i0 = index(3u * t);
        const u32 i1 = index(3u * t + 1u);
        const u32 i2 = index(3u * t + 2u);
        if (i0 >= mesh.vertexCount || i1 >= mesh.vertexCount || i2 >= mesh.vertexCount ||
            (m_emissiveLimit != 0u && m_stats.emissiveTriangles >= m_emissiveLimit)) {
            ++m_stats.emissiveSkipped;
            continue;
        }
        const RlLight L = makeTriangleLight(vertex(i0), vertex(i1), vertex(i2), mesh.radiance, mesh.twoSided);
        if (!pushLight(L, LightOrigin::Emissive, mesh.key, t)) {
            ++m_stats.emissiveSkipped;
            continue;
        }
        ++m_stats.emissiveTriangles;
        ++added;
    }
    return added;
}

bool RelightLightSet::addFallbackLight(const FallbackLight& f) {
    const bool none = m_gameEntries.empty() && m_hostEntries.empty();
    if (f.mode == 0u || (f.mode == 1u && !none)) {
        return false;
    }
    constexpr float kDeg = 3.14159265358979323846f / 180.f;
    const RlLight L = makeDistantLight(f.direction, 0.5f * f.angle * kDeg, f.radiance);
    if (!pushLight(L, LightOrigin::Authored, 0xFA11BAC4ull, 0u)) {
        return false;
    }
    m_stats.fallback = true;
    return true;
}

bool RelightLightSet::build() {
    m_gameCount = static_cast<u32>(m_gameEntries.size());
    const std::size_t gameWords = std::size_t(m_gameCount) * lk::kRlLightWords;
    m_table.resize(gameWords + m_hostTable.size());
    if (m_gameCount != 0u) {
        lk::ConvertParams p{};
        p.d3d = kernel::Span<const float4>{m_d3d.data(), static_cast<u32>(m_d3d.size())};
        p.out = kernel::Span<float4>{m_table.data(), static_cast<u32>(gameWords)};
        p.params = m_params;
        p.count = m_gameCount;
        if (!kernel::launch(kernel::Backend::CpuReference,
                            kernel::KernelLaunch{lk::kConvertName, kernel::extent1(m_gameCount), lk::kWorkgroup},
                            lk::ConvertKernel{}, p)
                 .ok) {
            return false;
        }
    }
    std::copy(m_hostTable.begin(), m_hostTable.end(), m_table.begin() + static_cast<std::ptrdiff_t>(gameWords));
    m_entries.clear();
    m_entries.insert(m_entries.end(), m_gameEntries.begin(), m_gameEntries.end());
    m_entries.insert(m_entries.end(), m_hostEntries.begin(), m_hostEntries.end());

    const u32 n = static_cast<u32>(m_entries.size());
    m_proxies.resize(n);
    bool sameKinds = m_treeValid && m_prevKinds.size() == n;
    for (u32 i = 0; i < n; ++i) {
        const RlLight L = light(i);
        m_proxies[i] = treeProxy(L, i);
        sameKinds = sameKinds && m_prevKinds[i] == m_proxies[i].kind;
    }
    if (sameKinds && m_tree.refit(m_proxies.data(), n)) {
        ++m_stats.refits;
        return true;
    }
    m_prevKinds.resize(n);
    for (u32 i = 0; i < n; ++i) {
        m_prevKinds[i] = m_proxies[i].kind;
    }
    m_treeValid = m_tree.build(m_proxies.data(), n);
    ++m_stats.builds;
    return m_treeValid;
}

RlLight RelightLightSet::light(u32 index) const {
    if (std::size_t(index + 1u) * lk::kRlLightWords > m_table.size()) {
        return lk::rlLightNone();
    }
    return lk::rlLightUnpack(m_table.data() + std::size_t(index) * lk::kRlLightWords);
}

LightSetSample RelightLightSet::sample(const float3& p, const float3& n, float u0, float u1, float u2) const {
    float4 q[lk::kRlQueryWords] = {float4(p.x, p.y, p.z, u0), float4(n.x, n.y, n.z, u1), float4(0.f, 0.f, 1.f, u2),
                                   float4(-1.f, 0.f, 0.f, 0.f)};
    float4 r[lk::kRlResultWords];
    lk::SetSampleParams prm{};
    prm.tree = m_tree.view();
    prm.table = kernel::Span<const float4>{m_table.data(), static_cast<u32>(m_table.size())};
    prm.lightCount = lightCount();
    lk::rlSetSampleOne(prm, q, r);
    LightSetSample s;
    if (r[0].w < 0.f) {
        return s;
    }
    s.light = static_cast<u32>(r[0].w);
    s.pmf = r[2].w;
    s.pdf = r[1].w;
    s.shape.position = float3(r[0].x, r[0].y, r[0].z);
    s.shape.wi = float3(r[1].x, r[1].y, r[1].z);
    s.shape.radiance = float3(r[2].x, r[2].y, r[2].z);
    s.shape.dist = r[4].x;
    s.shape.flags = static_cast<u32>(r[4].y);
    s.shape.pdf = r[4].z;
    return s;
}

float RelightLightSet::pmf(const float3& p, const float3& n, u32 light) const {
    const f32 pp[3] = {p.x, p.y, p.z};
    const f32 nn[3] = {n.x, n.y, n.z};
    return m_tree.pmf(pp, nn, light);
}

float RelightLightSet::pdf(const float3& p, const float3& n, u32 index, const float3& wi) const {
    if (index >= lightCount()) {
        return 0.f;
    }
    return pmf(p, n, index) * lk::rlLightPdf(light(index), p, wi);
}

float RelightLightSet::pdfAll(const float3& p, const float3& n, const float3& wi) const {
    float sum = 0.f;
    for (u32 i = 0; i < lightCount(); ++i) {
        sum += pdf(p, n, i, wi);
    }
    return sum;
}

} // namespace fuse::relight::render::lights
