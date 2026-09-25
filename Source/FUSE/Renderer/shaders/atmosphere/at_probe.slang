// WP-8.2 "atmos.probe". Slang twin of at_probe.comp (see there).
#include "at_common.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.mode) {
        return;
    }
    const AtParams P = at_load(pc.params);
    float4* src = at_texels(pc.src);
    float4* dst = at_texels(pc.dst);
    const float4 a = src[i * 3u];
    const float4 b = src[i * 3u + 1u];
    const float4 c = src[i * 3u + 2u];
    float3 s;
    float3 t;
    at_aerial(P, b.xy, b.z, s, t);
    dst[i * 6u] = float4(at_sky_radiance(P, a.xyz, true), 0.0);
    dst[i * 6u + 1u] = float4(at_sky_radiance(P, a.xyz, false), 0.0);
    dst[i * 6u + 2u] = float4(at_sun_illuminance_at(P, c.xyz), 0.0);
    dst[i * 6u + 3u] = float4(s, 0.0);
    dst[i * 6u + 4u] = float4(t, 0.0);
    dst[i * 6u + 5u] = float4(at_multiscatter(P, P.up[3], P.sunDir[3]), 0.0);
}
