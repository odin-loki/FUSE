// Asset W0.7 x WP-1.5: the material resolve's layered bin. Slang twin of mr_layered.glsl (see there).
// Include after mr_common.slang (mr_includes.slang does).
#pragma once

#define ML_NO_PUSH_CONSTANTS
#define ML_BINDLESS_TEXTURES
#include "ml_common.slang"

float4 ml_bindless_sample(uint image, uint smp, float2 uv, float2 dx, float2 dy) { return fuseMrSample(image, smp, uv, dx, dy); }

bool fuseMrCameraCentre(float4 vp[4], out float3 centre) {
    const float3 a0 = float3(vp[0].x, vp[1].x, vp[2].x);
    const float3 a1 = float3(vp[0].y, vp[1].y, vp[2].y);
    const float3 a2 = float3(vp[0].w, vp[1].w, vp[2].w);
    const float3 c12 = float3(a1.y * a2.z - a1.z * a2.y, a1.z * a2.x - a1.x * a2.z, a1.x * a2.y - a1.y * a2.x);
    const float3 c20 = float3(a2.y * a0.z - a2.z * a0.y, a2.z * a0.x - a2.x * a0.z, a2.x * a0.y - a2.y * a0.x);
    const float3 c01 = float3(a0.y * a1.z - a0.z * a1.y, a0.z * a1.x - a0.x * a1.z, a0.x * a1.y - a0.y * a1.x);
    const float det = a0.x * c12.x + a0.y * c12.y + a0.z * c12.z;
    centre = float3(0.0);
    if (det == 0.0) {
        return false;
    }
    const float b0 = vp[3].x;
    const float b1 = vp[3].y;
    const float b2 = vp[3].w;
    centre = float3(-(b0 * c12.x + b1 * c20.x + b2 * c01.x) / det, -(b0 * c12.y + b1 * c20.y + b2 * c01.y) / det,
                    -(b0 * c12.z + b1 * c20.z + b2 * c01.z) / det);
    return true;
}

MlSurface fuseMrLayeredSurface(MrFrame f, MrAttributes a, uint layeredIndex) {
    MlSurface s;
    s.position[0] = a.position.x;
    s.position[1] = a.position.y;
    s.position[2] = a.position.z;
    float3 c;
    float dist = 0.0;
    if (fuseMrCameraCentre(f.viewProj, c)) {
        const float d0 = a.position.x - c.x;
        const float d1 = a.position.y - c.y;
        const float d2 = a.position.z - c.z;
        dist = sqrt(d0 * d0 + d1 * d1 + d2 * d2);
    }
    s.viewDistance = dist;
    s.normal[0] = a.normal.x;
    s.normal[1] = a.normal.y;
    s.normal[2] = a.normal.z;
    s.tangentSign = a.tangentSign;
    s.tangent[0] = a.tangent.x;
    s.tangent[1] = a.tangent.y;
    s.tangent[2] = a.tangent.z;
    s.material = layeredIndex;
    s.uv[0] = a.uv.x;
    s.uv[1] = a.uv.y;
    s.reserved[0] = 0.0;
    s.reserved[1] = 0.0;
    s.color[0] = 1.0;
    s.color[1] = 1.0;
    s.color[2] = 1.0;
    s.color[3] = 1.0;
    return s;
}

MrGBuffer fuseMrShadeLayered(GpuSceneHeader* scene, MrFrame f, MrAttributes a) {
    const GpuMaterial m = fuseMrMaterial(scene, a.material);
    MlView v;
    if (f.layered != 0u) {
        v = ml_resolve_view(fuse_buffer_address(f.layered), f.sampler_);
    } else {
        v.materials = 0ull;
        v.materialCount = 0u;
        v.textures = 0ull;
        v.textureCount = 0u;
        v.texels = 0ull;
        v.lut = 0ull;
        v.sampler_ = f.sampler_;
    }
    MlGrad g;
    g.duvdx = a.duvdx;
    g.duvdy = a.duvdy;
    g.dPdx = a.dPdx;
    g.dPdy = a.dPdy;
    const MlResult r = ml_evaluate_grad(v, fuseMrLayeredSurface(f, a, m.padding), g);
    const GBufferOut o = fuseWriteGBuffer(float3(r.normal[0], r.normal[1], r.normal[2]),
                                          float3(r.albedo[0], r.albedo[1], r.albedo[2]), r.roughness, r.metallic,
                                          fuseMaterialEmissiveRadiance(m), r.ao, a.velocity, m.shading_model);
    MrGBuffer g2;
    g2.rt0 = o.rt0;
    g2.rt1 = o.rt1;
    g2.rt2 = o.rt2;
    g2.rt3 = o.rt3;
    g2.rt4 = float4(a.depth, 0.0, 0.0, 0.0);
    g2.rt5 = o.rt5;
    g2.material = a.material;
    return g2;
}

MrGBuffer fuseMrResolvePixel(GpuSceneHeader* scene, MrFrame f, MrAttributes a, uint features) {
    if ((features & kFuseMrFeatureLayered) != 0u && a.bin == kFuseMrBinLayered) {
        return fuseMrShadeLayered(scene, f, a);
    }
    return fuseMrShade(scene, a.material, f.sampler_, features & (kFuseMrFeatureTextures | kFuseMrFeatureNormalMap), a.uv,
                       a.duvdx, a.duvdy, a.normal, a.tangent, a.tangentSign, a.depth, a.velocity);
}
