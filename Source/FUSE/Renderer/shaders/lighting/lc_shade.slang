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
        GpuLight* lights = fuseGpuSceneLights(fuseGpuScene(F.scene));
        float3 radiance = s.emissive + fuseLcVec3(F.ambient) * s.albedo * s.ao;
        uint* directional = fuseLcWords(F.directional);
        const uint directionalCount = fuseLcWords(F.listHeader)[kFuseLcHeaderDirectional];
        for (uint i = 0u; i < directionalCount; ++i) {
            const uint slot = directional[i];
            if (slot < F.lightCount) {
                radiance = radiance + fuseLcLight(lights[slot], s, v);
            }
        }
        uint* grid = fuseLcWords(F.grid);
        uint* list = fuseLcWords(F.lightList);
        const uint offset = grid[cluster * 2u];
        const uint count = grid[cluster * 2u + 1u];
        for (uint i = 0u; i < count; ++i) {
            const uint slot = list[offset + i];
            if (slot < F.lightCount) {
                radiance = radiance + fuseLcLight(lights[slot], s, v);
            }
        }
        result = float4(radiance, 1.0);
    }
    fuse_lc_images[F.output_ & 0xFFFFFu][p] = result;
    if (pc.out_ != 0ull) {
        ((float4*)pc.out_)[p.y * F.width + p.x] = result;
    }
}
