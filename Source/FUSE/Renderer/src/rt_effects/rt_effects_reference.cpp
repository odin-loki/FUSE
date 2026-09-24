// WP-6.2 offline reference (see include/fuse/renderer/rt_effects/rt_effects_reference.hpp).
#include <fuse/renderer/rt_effects/rt_effects_reference.hpp>

#include <algorithm>
#include <cmath>
#include <random>

namespace fuse::renderer::rt_effects {

namespace {

using V3 = RtfxVec3<f64>;

/// Running mean / variance (Welford) of up to three channels.
struct Accumulator {
    f64 mean[3] = {0.0, 0.0, 0.0};
    f64 m2[3] = {0.0, 0.0, 0.0};
    u32 n = 0;
    f64 hitSum = 0.0; ///< running mean of the hit distances (Welford)
    f64 hitM2 = 0.0;
    u32 hits = 0;

    void addHit(f64 t) {
        ++hits;
        const f64 d = t - hitSum;
        hitSum += d / static_cast<f64>(hits);
        hitM2 += d * (t - hitSum);
    }

    void add(const f64 v[3]) {
        ++n;
        for (u32 c = 0; c < 3u; ++c) {
            const f64 d = v[c] - mean[c];
            mean[c] += d / static_cast<f64>(n);
            m2[c] += d * (v[c] - mean[c]);
        }
    }
    RtfxEstimate finish() const {
        RtfxEstimate e{};
        for (u32 c = 0; c < 3u; ++c) {
            e.mean[c] = mean[c];
            e.variance[c] = n > 1u ? m2[c] / static_cast<f64>(n - 1u) : 0.0;
        }
        e.samples = n;
        e.hits = hits;
        e.hitDistance = hits > 0u ? hitSum : static_cast<f64>(kRtfxNoHit);
        e.hitDistanceVariance = hits > 1u ? hitM2 / static_cast<f64>(hits - 1u) : 0.0;
        return e;
    }
};

rt::RtProbeRay makeRay(const V3& o, const V3& d, f64 tMax) {
    rt::RtProbeRay r{};
    r.origin[0] = static_cast<f32>(o.x);
    r.origin[1] = static_cast<f32>(o.y);
    r.origin[2] = static_cast<f32>(o.z);
    r.direction[0] = static_cast<f32>(d.x);
    r.direction[1] = static_cast<f32>(d.y);
    r.direction[2] = static_cast<f32>(d.z);
    r.tMin = 0.f;
    r.tMax = static_cast<f32>(tMax);
    return r;
}

bool hardKind(u32 kind) { return kind == kRtfxLightDirectional || kind == kRtfxLightPoint; }

} // namespace

RtEffectsReference::RtEffectsReference(const rt::RtReferenceScene& bvh, const gpu_scene::GpuScene& scene,
                                       const Material::GPUMaterial* materials, u32 materialCount)
    : m_bvh(bvh), m_scene(scene), m_materials(materials), m_materialCount(materials != nullptr ? materialCount : 0u) {}

RtfxSurface RtEffectsReference::surfaceFromGBuffer(const RtfxFrameConstants& c, u32 px, u32 py, f32 depth, f32 octX, f32 octY,
                                                   f32 roughness) {
    RtfxSurface s{};
    if (!(depth < 1.f) || !std::isfinite(depth)) {
        return s;
    }
    s.valid = true;
    s.position = rtfxReconstruct<f64>(c.invViewProj, px, py, static_cast<f64>(c.invWidth), static_cast<f64>(c.invHeight),
                                      static_cast<f64>(depth));
    s.normal = rtfxOctDecode<f64>(octX, octY);
    const V3 camera = rtfxLoad<f64>(c.cameraPosition);
    s.origin = rtfxRayOrigin<f64>(s.position, s.normal, camera, c.normalBias, c.viewBias);
    s.view = rtfxNormalize(rtfxSub(camera, s.position), s.normal);
    s.roughness = roughness;
    return s;
}

rt::RtRefHit RtEffectsReference::trace(const V3& origin, const V3& dir, f64 tMax, u32 cullMask) const {
    return m_bvh.trace(makeRay(origin, dir, tMax), cullMask & rt::kRtMaskAll);
}

RtfxEstimate RtEffectsReference::shadow(const RtfxFrameConstants& c, const RtfxShadowLight& light, const RtfxSurface& s,
                                        u32 sqrtSamples, u64 seed) const {
    Accumulator acc;
    if (!s.valid) {
        const f64 one[3] = {1.0, 1.0, 1.0};
        acc.add(one);
        return acc.finish();
    }
    const u32 n = hardKind(light.kind) ? 1u : std::max(sqrtSamples, 1u);
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<f64> uni(0.0, 1.0);
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const f64 u = n == 1u ? 0.5 : (static_cast<f64>(i) + uni(rng)) / static_cast<f64>(n);
            const f64 v = n == 1u ? 0.5 : (static_cast<f64>(j) + uni(rng)) / static_cast<f64>(n);
            V3 dir{};
            f64 tMax = 0.0;
            if (!rtfxShadowRay<f64>(light, s.origin, u, v, static_cast<f64>(c.farDistance), dir, tMax)) {
                continue;
            }
            f64 visible = 1.0;
            if (tMax > 0.0) {
                const rt::RtRefHit h = trace(s.origin, dir, tMax, c.shadowCullMask);
                if (h.hit) {
                    visible = 0.0;
                    acc.addHit(h.t);
                }
            }
            const f64 value[3] = {visible, visible, visible};
            acc.add(value);
        }
    }
    return acc.finish();
}

RtfxEstimate RtEffectsReference::reflection(const RtfxFrameConstants& c, const RtfxSurface& s, u32 sqrtSamples, u64 seed) const {
    Accumulator acc;
    if (!s.valid) {
        const f64 zero[3] = {0.0, 0.0, 0.0};
        acc.add(zero);
        return acc.finish();
    }
    const bool mirror = !(s.roughness >= static_cast<f64>(c.mirrorRoughness));
    const u32 n = mirror ? 1u : std::max(sqrtSamples, 1u);
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<f64> uni(0.0, 1.0);
    for (u32 j = 0; j < n; ++j) {
        for (u32 i = 0; i < n; ++i) {
            const f64 u = n == 1u ? 0.5 : (static_cast<f64>(i) + uni(rng)) / static_cast<f64>(n);
            const f64 v = n == 1u ? 0.5 : (static_cast<f64>(j) + uni(rng)) / static_cast<f64>(n);
            V3 dir{};
            f64 value[3] = {0.0, 0.0, 0.0};
            if (rtfxReflectionRay<f64>(s.normal, s.view, s.roughness, static_cast<f64>(c.mirrorRoughness), u, v, dir)) {
                const rt::RtRefHit h = trace(s.origin, dir, static_cast<f64>(c.farDistance), c.reflectionCullMask);
                const V3 radiance = shadeHit(c, s.origin, dir, h);
                value[0] = radiance.x;
                value[1] = radiance.y;
                value[2] = radiance.z;
                if (h.hit) {
                    acc.addHit(h.t);
                }
            }
            acc.add(value);
        }
    }
    return acc.finish();
}

V3 RtEffectsReference::shadeHit(const RtfxFrameConstants& c, const V3& origin, const V3& dir, const rt::RtRefHit& hit,
                                bool* secondaryRobust) const {
    if (secondaryRobust != nullptr) {
        *secondaryRobust = true;
    }
    if (!hit.hit) {
        return rtfxLoad<f64>(c.sky);
    }
    const gpu_scene::GpuInstance& inst = m_scene.instance(hit.instance);
    const gpu_scene::GpuMesh& mesh = m_scene.mesh(inst.mesh);
    const gpu_scene::GpuTransform& xf = m_scene.transform(hit.instance);
    const std::vector<f32>& positions = m_bvh.meshPositions(inst.mesh);
    const u32* indices = m_scene.indexData() + mesh.firstIndex + 3u * hit.primitive;
    V3 w[3];
    for (u32 k = 0; k < 3u; ++k) {
        const f32* p = positions.data() + 3u * indices[k];
        f64 out[3];
        for (u32 r = 0; r < 3u; ++r) {
            out[r] = static_cast<f64>(xf.rows[r][0]) * p[0] + static_cast<f64>(xf.rows[r][1]) * p[1] +
                     static_cast<f64>(xf.rows[r][2]) * p[2] + static_cast<f64>(xf.rows[r][3]);
        }
        w[k] = {out[0], out[1], out[2]};
    }
    V3 n = rtfxNormalize(rtfxCross(rtfxSub(w[1], w[0]), rtfxSub(w[2], w[0])), V3{0.0, 1.0, 0.0});
    if (rtfxDot(n, dir) > 0.0) {
        n = rtfxScale(n, -1.0);
    }
    f64 albedo[3] = {kRtfxDefaultAlbedo, kRtfxDefaultAlbedo, kRtfxDefaultAlbedo};
    f64 metallic = 0.0;
    f64 emissive[3] = {0.0, 0.0, 0.0};
    if (inst.material < m_materialCount) {
        const Material::GPUMaterial& m = m_materials[inst.material];
        albedo[0] = m.baseColor.x;
        albedo[1] = m.baseColor.y;
        albedo[2] = m.baseColor.z;
        metallic = std::clamp(static_cast<f64>(m.baseColor.w), 0.0, 1.0);
        const f64 intensity = std::max(static_cast<f64>(m.emissiveIntensity), 0.0);
        emissive[0] = std::max(static_cast<f64>(m.roughnessEmissive.y), 0.0) * intensity;
        emissive[1] = std::max(static_cast<f64>(m.roughnessEmissive.z), 0.0) * intensity;
        emissive[2] = std::max(static_cast<f64>(m.roughnessEmissive.w), 0.0) * intensity;
    }
    const V3 point = rtfxAdd(origin, rtfxScale(dir, hit.t));
    f64 irradiance[3] = {kRtfxPi * c.ambient[0], kRtfxPi * c.ambient[1], kRtfxPi * c.ambient[2]};
    for (u32 i = 0; i < std::min(c.hitLightCount, kRtfxMaxHitLights); ++i) {
        const RtfxHitLight& l = c.hitLights[i];
        const V3 toLight = rtfxNormalize(rtfxLoad<f64>(l.direction), V3{0.0, 1.0, 0.0});
        const f64 cosine = rtfxDot(n, toLight);
        if (!(cosine > 0.0)) {
            continue;
        }
        f64 visible = 1.0;
        if (l.shadowed != 0u && (c.flags & kRtfxFlagHitShadows) != 0u) {
            const V3 o = rtfxRayOrigin<f64>(point, n, rtfxLoad<f64>(c.cameraPosition), c.normalBias, c.viewBias);
            if (secondaryRobust != nullptr) {
                const rt::RtRefClassified r =
                    m_bvh.traceClassified(makeRay(o, toLight, static_cast<f64>(c.farDistance)), c.shadowCullMask & rt::kRtMaskAll, 1e-4);
                *secondaryRobust = *secondaryRobust && r.robust;
                visible = r.hit.hit ? 0.0 : 1.0;
            } else {
                visible = trace(o, toLight, static_cast<f64>(c.farDistance), c.shadowCullMask).hit ? 0.0 : 1.0;
            }
        }
        for (u32 k = 0; k < 3u; ++k) {
            irradiance[k] += cosine * static_cast<f64>(l.irradiance[k]) * visible;
        }
    }
    const f64 diffuse = (1.0 - metallic) / kRtfxPi;
    return {emissive[0] + albedo[0] * diffuse * irradiance[0], emissive[1] + albedo[1] * diffuse * irradiance[1],
            emissive[2] + albedo[2] * diffuse * irradiance[2]};
}

} // namespace fuse::renderer::rt_effects
