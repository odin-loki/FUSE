// WP-8.2 "atmos.aerial". Slang twin of at_aerial.comp (see there).
#include "at_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const AtParams P = at_load(pc.params);
    const uint2 p = tid.xy;
    if (p.x >= P.apWidth || p.y >= P.apHeight) {
        return;
    }
    const float ndcX = (float(p.x) + 0.5) / float(P.apWidth) * 2.0 - 1.0;
    const float ndcY = (float(p.y) + 0.5) / float(P.apHeight) * 2.0 - 1.0;
    const float sx = ndcX * P.camForward[3];
    const float sy = ndcY * P.camRight[3];
    float3 dir = float3(P.camForward[0] + P.camRight[0] * sx - P.camUp[0] * sy,
                        P.camForward[1] + P.camRight[1] * sx - P.camUp[1] * sy,
                        P.camForward[2] + P.camRight[2] * sx - P.camUp[2] * sy);
    dir = dir * (1.0 / sqrt(at_dot(dir, dir)));
    const float r0 = P.up[3];
    const float mu = dir.x * P.up[0] + dir.y * P.up[1] + dir.z * P.up[2];
    const float nu = dir.x * P.sunDir[0] + dir.y * P.sunDir[1] + dir.z * P.sunDir[2];
    const float muS0 = P.sunDir[3];
    const float tGround = at_distance_to_ground(P, r0, mu);
    const float tLimit = tGround >= 0.0 ? tGround : at_distance_to_top(P, r0, mu);
    const bool useMs = (P.flags & AT_FLAG_MULTI_SCATTER) != 0u;
    const float phaseR = at_rayleigh_phase(nu);
    const float phaseM = at_mie_phase(nu, P.mieG);
    const uint k = P.apStepsPerSlice;
    const uint stride = P.apWidth * P.apHeight;
    const uint base = p.y * P.apWidth + p.x;
    float4* scatter = at_texels(P.aerialScatter);
    float4* trans = at_texels(P.aerialTransmittance);
    AtMarch m = at_march_init();
    float tPrev = 0.0;
    for (uint z = 0u; z < P.apDepth; ++z) {
        const float s = float(z + 1u) / float(P.apDepth);
        const float tNext = min(P.cameraPos[3] * s * s, tLimit);
        const float dt = max(0.0, tNext - tPrev) / float(k);
        for (uint i = 0u; i < k; ++i) {
            if (dt > 0.0) {
                const float t = tPrev + (float(i) + 0.5) * dt;
                at_scatter_step(P, r0, mu, nu, muS0, t, dt, AT_PHASE_SKY, useMs, phaseR, phaseM, m);
            }
        }
        tPrev = max(tPrev, tNext);
        scatter[base + z * stride] = float4(m.L, 0.0);
        trans[base + z * stride] = float4(m.thr, 0.0);
    }
}
