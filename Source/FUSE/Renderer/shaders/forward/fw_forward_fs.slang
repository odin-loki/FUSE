// WP-2.3 forward transparency, fragment stage. Slang twin of fw_forward.frag (see there).
#include "fw_includes.slang"

struct FSIn {
    [[vk::location(0)]] float2 uv : UV;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float3 tangent : TANGENT;
    [[vk::location(3)]] nointerpolation float sign_ : SIGN;
    [[vk::location(4)]] nointerpolation uint draw : DRAW;
};

[shader("fragment")]
[earlydepthstencil]
float4 main(FSIn input, float4 fragCoord : SV_Position, uint primitive : SV_PrimitiveID) : SV_Target0 {
    const float2 dx = ddx(input.uv);
    const float2 dy = ddy(input.uv);
    const FwFrame f = fuseFwFrame();
    const LcFrame F = ((LcFrame*)f.lighting)[0];
    const FwDraw draw = ((FwDraw*)f.draws)[input.draw];
    GpuSceneHeader* scene = fuseGpuScene(f.scene);
    const GpuInstance inst = fuseGpuSceneInstances(scene)[draw.slot];
    const GpuMesh mesh = fuseGpuSceneMeshes(scene)[inst.mesh];
    const uint material = fuseMrTriangleMaterial(scene, inst, mesh, primitive);
    const MrGBuffer g = fuseMrShade(scene, material, f.sampler_, kFuseMrFeatureTextures | kFuseMrFeatureNormalMap, input.uv, dx,
                                    dy, input.normal, input.tangent, input.sign_, fragCoord.z, float2(0.0, 0.0));
    const float4 rt0 = fuseFwHalf4(g.rt0);
    const float4 rt1 = fuseFwUnorm8(g.rt1);
    const float4 rt2 = fuseFwUnorm8(g.rt2);
    const float4 rt5 = fuseFwHalf4(g.rt5);

    const float sx = fragCoord.x * F.invWidth;
    const float sy = fragCoord.y * F.invHeight;
    const float viewDepth = fuseLcViewDepth(fragCoord.z, F.nearPlane, F.farPlane, (F.flags & kFuseLcFlagReversedZ) != 0u);
    uint cluster = 0u;
    if (!(viewDepth > 0.0) || isinf(viewDepth) || !fuseLcMapCluster(sx, sy, viewDepth, F, cluster)) {
        discard;
    }
    LcSurface s;
    s.position = fuseLcViewToWorld(fuseLcViewPosition(sx, sy, viewDepth, F), F);
    s.normal = fuseLcOctDecode(rt0.x, rt0.y);
    s.ao = rt0.w;
    s.albedo = rt1.xyz;
    s.roughness = rt2.x;
    s.metallic = rt2.y;
    s.emissive = rt5.xyz;
    const float3 v = fuseLcSafeNormalize(fuseLcVec3(F.cameraPosition) - s.position, s.normal);
    const float3 radiance = fuseFwShade(F, s, v, cluster);
    const float alpha = clamp(draw.opacity, 0.0, 1.0);
    if (f.dump != 0ull) {
        const uint p = uint(fragCoord.y) * f.width + uint(fragCoord.x);
        FwDumpTexel t;
        t.radiance = float4(radiance, alpha);
        t.rt0 = rt0;
        t.rt1 = rt1;
        t.rt2 = rt2;
        t.rt5 = rt5;
        t.depth = fragCoord.z;
        t.covered = 1u;
        t.slot = draw.slot;
        t.cluster = cluster;
        ((FwDumpTexel*)f.dump)[input.draw * f.width * f.height + p] = t;
    }
    return float4(radiance * alpha, alpha);
}
