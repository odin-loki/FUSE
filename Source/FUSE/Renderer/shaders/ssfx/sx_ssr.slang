// WP-6.3 "ssfx.ssr". Slang twin of sx_ssr.comp (see there).
#include "sx_trace.slang"

float sxContactHardenRoughness(float rayHitDistance, float materialRoughness) {
    if ((F.flags & kSxFlagSsrContact) == 0u || F.ssrContactDistance <= 0.0) {
        return sxClamp(materialRoughness, 0.0, 1.0);
    }
    const float distance = max(rayHitDistance, 0.0);
    const float contactFactor = pow(sxClamp(1.0 - distance / F.ssrContactDistance, 0.0, 1.0), F.ssrContactExponent);
    const float hardened = materialRoughness + (F.ssrContactFloor - materialRoughness) * contactFactor;
    return sxClamp(hardened, F.ssrContactFloor, 1.0);
}

float sxGlossFade(float hitDistance, float roughness) {
    const float r = sxClamp(roughness, 0.0, 1.0);
    const float hardened = r <= F.ssrContactFloor ? r : sxContactHardenRoughness(hitDistance, r);
    return sxClamp(1.0 - hardened, 0.0, 1.0);
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    sxBegin();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const uint i = sxIndex(p.x, p.y);
    SxTrace P;
    P.maxSteps = F.ssrMaxSteps;
    P.stride = F.ssrStride;
    P.thickness = F.ssrThickness;
    P.maxDistance = F.ssrMaxDistance;
    P.fadeEdge = F.ssrFadeEdge;
    P.refineSteps = F.ssrRefineSteps;
    const bool useRoughness = (F.flags & kSxFlagSsrRoughness) != 0u;
    const float r = useRoughness ? sxTexels(F.prepared)[i].y : 0.0;
    float4 result = float4(0.0);
    if (r < 1.0) {
        const SxHit hit = sxTracePixel(F.radiance, P, p.x, p.y);
        if (hit.hit) {
            float fade = 1.0;
            if (useRoughness) {
                fade = sxGlossFade(hit.distance, r);
            }
            result = float4(hit.color, hit.confidence * fade);
        } else if ((F.flags & kSxFlagSky) != 0u) {
            const float3 dir = sxPixelReflection(p.x, p.y);
            if (sxSkyExit(P, p.x, p.y, dir)) {
                result = float4(sxSky(sxNormalize(dir)), useRoughness ? sxGlossFade(P.maxDistance, r) : 1.0);
            }
        }
    }
    sxTexels(F.ssr)[i] = result;
}
