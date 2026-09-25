// WP-6.2 "rt.reflections", Slang twin of rtfx_reflections.comp (see there).
#include "gpu_scene.slang"
#include "rtfx_common.slang"
#include "atmosphere/at_sample.slang"

// Radiance of a reflection ray that misses (rtfx_reflections.comp fuse_rtfx_miss).
float3 fuseRtfxMiss(RtfxFrame F, float3 dir) {
    if ((F.flags & kFuseRtfxFlagAtmosphereSky) != 0u && F.atmosphere != uint64_t(0)) {
        return at_sky_radiance(at_load(F.atmosphere), dir, false);
    }
    return fuseRtfxVec3(F.sky);
}

// First-bounce radiance of the closest hit (RtEffectsReference::shadeHit).
float3 fuseRtfxShadeHit(RtfxFrame F, float3 origin, float3 dir, float t, uint instance, uint primitive) {
    GpuSceneHeader* scene = fuseGpuScene(F.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[instance];
    const GpuTransform xf = fuseGpuSceneTransforms(scene)[instance];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    uint* indices = fuseGpuSceneIndices(scene);
    uint* vpos = (uint*)mesh.positions;
    float3 w[3];
    for (uint k = 0u; k < 3u; ++k) {
        const uint vi = indices[mesh.firstIndex + 3u * primitive + k];
        const uint w0 = vpos[vi * 2u];
        const uint w1 = vpos[vi * 2u + 1u];
        const float3 q = float3(float(w0 & 0xFFFFu), float(w0 >> 16u), float(w1 & 0xFFFFu));
        const float3 local = float3(mesh.quantOffset[0] + q.x * mesh.quantStep[0], mesh.quantOffset[1] + q.y * mesh.quantStep[1],
                                    mesh.quantOffset[2] + q.z * mesh.quantStep[2]);
        w[k] = fuseGpuTransformPoint(xf, local);
    }
    float3 n = fuseRtfxNormalize(cross(w[1] - w[0], w[2] - w[0]), float3(0.0, 1.0, 0.0));
    if (dot(n, dir) > 0.0) {
        n = -n;
    }
    float3 albedo = float3(kFuseRtfxDefaultAlbedo, kFuseRtfxDefaultAlbedo, kFuseRtfxDefaultAlbedo);
    float metallic = 0.0;
    float3 emissive = float3(0.0, 0.0, 0.0);
    if (inst.material < scene.counts[kFuseGpuSceneMaterials]) {
        const RtfxMaterial m = ((RtfxMaterial*)scene.addresses[kFuseGpuSceneMaterials])[inst.material];
        albedo = m.baseColor.xyz;
        metallic = clamp(m.baseColor.w, 0.0, 1.0);
        emissive = max(m.roughnessEmissive.yzw, float3(0.0, 0.0, 0.0)) * max(m.emissiveIntensity, 0.0);
    }
    const float3 point = origin + dir * t;
    float3 irradiance = fuseRtfxVec3(F.ambient) * kFuseRtfxPi;
    for (uint i = 0u; i < min(F.hitLightCount, kFuseRtfxMaxHitLights); ++i) {
        const RtfxHitLight l = F.hitLights[i];
        const float3 toLight = fuseRtfxNormalize(fuseRtfxVec3(l.direction), float3(0.0, 1.0, 0.0));
        const float cosine = dot(n, toLight);
        if (!(cosine > 0.0)) {
            continue;
        }
        float visible = 1.0;
        if (l.shadowed != 0u && (F.flags & kFuseRtfxFlagHitShadows) != 0u) {
            const float3 o = fuseRtfxRayOrigin(point, n, fuseRtfxVec3(F.cameraPosition), F.normalBias, F.viewBias);
            visible = fuseRtfxOccluded(F.tlas, F.shadowCullMask, o, toLight, F.farDistance) ? 0.0 : 1.0;
        }
        irradiance += fuseRtfxVec3(l.irradiance) * (cosine * visible);
    }
    const float diffuse = (1.0 - metallic) / kFuseRtfxPi;
    return emissive + albedo * diffuse * irradiance;
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const RtfxFrame F = fuseRtfxFrame();
    const uint2 p = tid.xy;
    if (p.x >= F.view.width || p.y >= F.view.height) {
        return;
    }
    const uint pixel = p.y * F.view.width + p.x;
    float4* out_ = (float4*)F.reflection;
    const float depth = fuseRtfxFetch(F.gbufferDepth, int2(p)).x;
    if (!(depth < 1.0) || isnan(depth) || isinf(depth)) {
        out_[pixel] = float4(0.0, 0.0, 0.0, kFuseRtfxNoHit);
        fuseRtfxRecord(pc.dump, pixel, float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), 0.0, kFuseRtfxNoHit, kFuseRtfxInvalid,
                       kFuseRtfxInvalid, 0u);
        return;
    }
    const float3 camera = fuseRtfxVec3(F.cameraPosition);
    const float3 position = fuseRtfxReconstruct(F, p, depth);
    const float4 rt0 = fuseRtfxFetch(F.gbufferNormal, int2(p));
    const float roughness = fuseRtfxFetch(F.gbufferRoughMetal, int2(p)).x;
    const float3 normal = fuseRtfxOctDecode(rt0.x, rt0.y);
    const float3 origin = fuseRtfxRayOrigin(position, normal, camera, F.normalBias, F.viewBias);
    const float3 view = fuseRtfxNormalize(camera - position, normal);
    const bool mirror = !(roughness >= F.mirrorRoughness);
    const uint samples = mirror ? 1u : max(F.reflectionSamples, 1u);
    const uint seed = fuseRtfxPixelSeed(p.x, p.y, kFuseRtfxStreamReflection, F.seed);
    float3 sum = float3(0.0, 0.0, 0.0);
    uint hits = 0u;
    float hitSum = 0.0;
    for (uint s = 0u; s < samples; ++s) {
        const float2 uv = fuseRtfxSample2(F.frameIndex * samples + s, seed);
        float3 dir;
        const bool valid = fuseRtfxReflectionRay(normal, view, roughness, F.mirrorRoughness, uv.x, uv.y, dir);
        float t = kFuseRtfxNoHit;
        uint instance = kFuseRtfxInvalid;
        uint primitive = kFuseRtfxInvalid;
        if (valid) {
            t = fuseRtfxTrace(F.tlas, F.reflectionCullMask, origin, dir, F.farDistance, instance, primitive);
            if (t >= 0.0) {
                sum += fuseRtfxShadeHit(F, origin, dir, t, instance, primitive);
                hits += 1u;
                hitSum += t;
            } else {
                sum += fuseRtfxMiss(F, dir);
            }
        }
        if (s == 0u) {
            fuseRtfxRecord(pc.dump, pixel, origin, dir, F.farDistance, t, instance, primitive,
                           (valid ? kFuseRtfxRayTraced : 0u) | (t >= 0.0 ? kFuseRtfxRayHit : 0u));
        }
    }
    const float inv = 1.0 / float(samples);
    out_[pixel] = float4(sum * inv, hits > 0u ? hitSum / float(hits) : kFuseRtfxNoHit);
}
