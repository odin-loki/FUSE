// WP-2.1 "light.shade". Slang twin of lc_shade.comp (see there).
#include "lc_includes.slang"

[[vk::binding(1, 0)]] Texture2D<float4> fuse_lc_textures[];
[[vk::binding(0, 0)]] [format("rgba16f")] RWTexture2D<float4> fuse_lc_images[];

float4 fuseLcFetch(uint handle, int2 p) { return fuse_lc_textures[handle & 0xFFFFFu].Load(int3(p, 0)); }

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID) {
    const LcFrame F = fuseLcFrame();
    const uint2 p = tid.xy;
    if (p.x >= F.width || p.y >= F.height) {
        return;
    }
    const int2 ip = int2(p);
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    const float sx = (float(p.x) + 0.5) * F.invWidth;
    const float sy = (float(p.y) + 0.5) * F.invHeight;
    const float depth = fuseLcFetch(F.gbufferDepth, ip).x;
    const float viewDepth = fuseLcViewDepth(depth, F.nearPlane, F.farPlane, (F.flags & kFuseLcFlagReversedZ) != 0u);
    uint cluster = 0u;
    if (viewDepth > 0.0 && !isinf(viewDepth) && fuseLcMapCluster(sx, sy, viewDepth, F, cluster)) {
        const float4 rt0 = fuseLcFetch(F.gbufferNormalAo, ip);
        const float4 rt1 = fuseLcFetch(F.gbufferAlbedo, ip);
        const float4 rt2 = fuseLcFetch(F.gbufferRoughMetal, ip);
        const float4 rt5 = fuseLcFetch(F.gbufferEmissive, ip);
        LcSurface s;
        s.position = fuseLcViewToWorld(fuseLcViewPosition(sx, sy, viewDepth, F), F);
        s.normal = fuseLcOctDecode(rt0.x, rt0.y);
        s.ao = rt0.w;
        s.albedo = rt1.xyz;
        s.roughness = rt2.x;
        s.metallic = rt2.y;
        s.emissive = rt5.xyz;
        const float3 v = fuseLcSafeNormalize(fuseLcVec3(F.cameraPosition) - s.position, s.normal);
        // The light loop (WP-2.2: shared with the forward transparency pass, lc_ltc.slang).
        const float3 radiance = fuseLcShadeLights(F, s, v, cluster);
        result = float4(radiance, 1.0);
    }
    fuse_lc_images[F.output_ & 0xFFFFFu][p] = result;
    if (pc.out_ != 0ull) {
        ((float4*)pc.out_)[p.y * F.width + p.x] = result;
    }
}
