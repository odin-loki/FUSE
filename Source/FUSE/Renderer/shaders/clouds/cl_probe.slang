// WP-8.3 "clouds.probe". Slang twin of cl_probe.comp (see there).
#include "cl_common.slang"

[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const uint i = tid.x;
    if (i >= pc.mode) {
        return;
    }
    const CloudParams P = cl_load(pc.params);
    float4* src = cl_texels(pc.src);
    float4* dst = cl_texels(pc.dst);
    const float4 a = src[i * 2u];
    const float4 b = src[i * 2u + 1u];
    const ClRay ray = cl_integrate(P, a.xyz, b.xyz, a.w);
    dst[i * 2u] = float4(ray.scatter, ray.transmittance);
    dst[i * 2u + 1u] = float4(ray.depth, ray.hasCloud, cl_density_at(P, a.xyz), 0.0);
}
