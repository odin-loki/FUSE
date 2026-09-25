// Asset W0.7 x WP-1.5: the material resolve's layered bin. A material row with FUSE_GPU_MATERIAL_LAYERED is
// evaluated as a layered material (shaders/material_layers/ml_common.glsl: triplanar / stochastic tiling / detail
// maps / height-blended and wet layers) from what the resolve reconstructs out of the visibility buffer: world
// position + its analytic per-pixel derivatives, interpolated normal / tangent / bitangent sign, UV0 + derivatives.
// Textures are the MlResolveTable's mip-mapped bindless images, sampled with textureGrad through the frame's sampler
// (the projection coordinate's derivatives: UV0's, or the world position's for triplanar); the texel-pool manual
// bilinear filter of materials.eval is not used here. CPU twin: material_resolve::layered_surface
// (resolve_reference.hpp) + material_layers::ml_evaluate with the trilinear filter of ml_mips.hpp.
// GLSL twin of mr_layered.slang. Include after mr_common.glsl (mr_includes.glsl does).
#ifndef FUSE_MR_LAYERED_GLSL
#define FUSE_MR_LAYERED_GLSL

#define ML_NO_PUSH_CONSTANTS
#define ML_BINDLESS_TEXTURES
vec4 ml_bindless_sample(uint image, uint smp, vec2 uv, vec2 dx, vec2 dy) { return fuse_mr_sample(image, smp, uv, dx, dy); }
#include "ml_common.glsl"

// Camera centre of the view-projection (columns): rows {x, y, w} . (C, 1) = 0 by Cramer's rule
// (resolve_kernel::camera_centre). False without a finite centre (orthographic).
bool fuse_mr_camera_centre(vec4 vp[4], out vec3 centre) {
    const vec3 a0 = vec3(vp[0].x, vp[1].x, vp[2].x);
    const vec3 a1 = vec3(vp[0].y, vp[1].y, vp[2].y);
    const vec3 a2 = vec3(vp[0].w, vp[1].w, vp[2].w);
    precise vec3 c12 = vec3(a1.y * a2.z - a1.z * a2.y, a1.z * a2.x - a1.x * a2.z, a1.x * a2.y - a1.y * a2.x);
    precise vec3 c20 = vec3(a2.y * a0.z - a2.z * a0.y, a2.z * a0.x - a2.x * a0.z, a2.x * a0.y - a2.y * a0.x);
    precise vec3 c01 = vec3(a0.y * a1.z - a0.z * a1.y, a0.z * a1.x - a0.x * a1.z, a0.x * a1.y - a0.y * a1.x);
    precise float det = a0.x * c12.x + a0.y * c12.y + a0.z * c12.z;
    centre = vec3(0.0);
    if (det == 0.0) {
        return false;
    }
    const float b0 = vp[3].x;
    const float b1 = vp[3].y;
    const float b2 = vp[3].w;
    precise vec3 c;
    c.x = -(b0 * c12.x + b1 * c20.x + b2 * c01.x) / det;
    c.y = -(b0 * c12.y + b1 * c20.y + b2 * c01.y) / det;
    c.z = -(b0 * c12.z + b1 * c20.z + b2 * c01.z) / det;
    centre = c;
    return true;
}

// The layered surface of a reconstructed pixel (material_resolve::layered_surface): vertex colour (1, 1, 1, 1),
// the meshlet streams have no colour stream.
MlSurface fuse_mr_layered_surface(FuseMrFrame f, FuseMrAttributes a, uint layeredIndex) {
    MlSurface s;
    s.position[0] = a.position.x;
    s.position[1] = a.position.y;
    s.position[2] = a.position.z;
    vec3 c;
    float dist = 0.0;
    if (fuse_mr_camera_centre(f.viewProj, c)) {
        precise float d0 = a.position.x - c.x;
        precise float d1 = a.position.y - c.y;
        precise float d2 = a.position.z - c.z;
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

// Layered-material G-buffer of a reconstructed pixel whose row has FUSE_GPU_MATERIAL_LAYERED: the layered result
// (albedo, roughness, metallic, AO, normal) + the row's emissive radiance and shading model, packed by write_gbuffer.
// Without a table (f.layered == 0) or with an out-of-range index, ml_evaluate's default surface.
FuseMrGBuffer fuse_mr_shade_layered(FuseGpuSceneHeaderRef scene, FuseMrFrame f, FuseMrAttributes a) {
    const FuseGpuMaterial m = fuse_mr_material(scene, a.material);
    MlView v;
    if (f.layered != 0u) {
        v = ml_resolve_view(fuse_buffer_address(f.layered), f.sampler_);
    } else {
        v.materials = 0ul;
        v.materialCount = 0u;
        v.textures = 0ul;
        v.textureCount = 0u;
        v.texels = 0ul;
        v.lut = 0ul;
        v.sampler_ = f.sampler_;
    }
    MlGrad g;
    g.duvdx = a.duvdx;
    g.duvdy = a.duvdy;
    g.dPdx = a.dPdx;
    g.dPdy = a.dPdy;
    const MlResult r = ml_evaluate_grad(v, fuse_mr_layered_surface(f, a, m.padding), g);
    FuseMrGBuffer o;
    write_gbuffer(vec3(r.normal[0], r.normal[1], r.normal[2]), vec3(r.albedo[0], r.albedo[1], r.albedo[2]), r.roughness,
                  r.metallic, fuse_material_emissive_radiance(m), r.ao, a.velocity, m.shading_model, o.rt0, o.rt1, o.rt2,
                  o.rt3, o.rt5);
    o.rt4 = vec4(a.depth, 0.0, 0.0, 0.0);
    o.material = a.material;
    return o;
}

// The resolve of one pixel for a pipeline evaluating `features` (fuse_mr_resolve_features of its bin): layered rows
// through fuse_mr_shade_layered when FUSE_MR_FEATURE_LAYERED is set, everything else through fuse_mr_shade exactly as
// before (so non-layered pixels are bit-identical in every bin and in the uber path).
FuseMrGBuffer fuse_mr_resolve_pixel(FuseGpuSceneHeaderRef scene, FuseMrFrame f, FuseMrAttributes a, uint features) {
    if ((features & FUSE_MR_FEATURE_LAYERED) != 0u && a.bin == FUSE_MR_BIN_LAYERED) {
        return fuse_mr_shade_layered(scene, f, a);
    }
    return fuse_mr_shade(scene, a.material, f.sampler_, features & (FUSE_MR_FEATURE_TEXTURES | FUSE_MR_FEATURE_NORMAL_MAP),
                         a.uv, a.duvdx, a.duvdy, a.normal, a.tangent, a.tangentSign, a.depth, a.velocity);
}

#endif // FUSE_MR_LAYERED_GLSL
