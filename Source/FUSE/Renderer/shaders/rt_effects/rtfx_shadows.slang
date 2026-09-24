// WP-6.2 "rt.shadows", Slang twin of rtfx_shadows.comp (see there).
#include "gpu_scene.slang"
#include "rtfx_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const RtfxFrame F = fuseRtfxFrame();
    const uint2 p = tid.xy;
    if (p.x >= F.view.width || p.y >= F.view.height) {
        return;
    }
    const uint pixels = F.view.width * F.view.height;
    const uint pixel = p.y * F.view.width + p.x;
    const float depth = fuseRtfxFetch(F.gbufferDepth, int2(p)).x;
    const bool surface = depth < 1.0 && !isnan(depth) && !isinf(depth);
    float3 origin = float3(0.0, 0.0, 0.0);
    if (surface) {
        const float3 position = fuseRtfxReconstruct(F, p, depth);
        const float4 rt0 = fuseRtfxFetch(F.gbufferNormal, int2(p));
        const float3 normal = fuseRtfxOctDecode(rt0.x, rt0.y);
        origin = fuseRtfxRayOrigin(position, normal, fuseRtfxVec3(F.cameraPosition), F.normalBias, F.viewBias);
    }
    float* visibility = (float*)F.view.visibility;
    float* hitDistance = (float*)F.hitDistance;
    for (uint c = 0u; c < F.view.count; ++c) {
        const RtfxShadowLight light = F.shadowLights[c];
        const uint samples = max(light.samples, 1u);
        const uint seed = fuseRtfxPixelSeed(p.x, p.y, c, F.seed);
        uint visible = 0u;
        uint hits = 0u;
        float hitSum = 0.0;
        for (uint s = 0u; s < samples; ++s) {
            if (!surface) {
                visible += 1u;
                if (s == 0u) {
                    fuseRtfxRecord(pc.dump, c * pixels + pixel, float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0), 0.0, kFuseRtfxNoHit,
                                   kFuseRtfxInvalid, kFuseRtfxInvalid, 0u);
                }
                continue;
            }
            const float2 uv = fuseRtfxSample2(F.frameIndex * samples + s, seed);
            float3 dir;
            float tMax;
            const bool ok = fuseRtfxShadowRay(light, origin, uv.x, uv.y, F.farDistance, dir, tMax);
            float t = kFuseRtfxNoHit;
            uint instance = kFuseRtfxInvalid;
            uint primitive = kFuseRtfxInvalid;
            const bool traced = ok && tMax > 0.0;
            if (traced) {
                t = fuseRtfxTrace(F.tlas, F.shadowCullMask, origin, dir, tMax, instance, primitive);
            }
            if (t >= 0.0) {
                hits += 1u;
                hitSum += t;
            } else {
                visible += 1u;
            }
            if (s == 0u) {
                fuseRtfxRecord(pc.dump, c * pixels + pixel, origin, dir, tMax, t, instance, primitive,
                               (traced ? kFuseRtfxRayTraced : 0u) | (t >= 0.0 ? kFuseRtfxRayHit : 0u));
            }
        }
        visibility[c * pixels + pixel] = float(visible) / float(samples);
        hitDistance[c * pixels + pixel] = hits > 0u ? hitSum / float(hits) : kFuseRtfxNoHit;
    }
}
