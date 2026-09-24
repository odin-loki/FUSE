#version 460
// WP-1.5 material resolve, fragment stage: one invocation per pixel (tile quads of one bin, or the
// full-screen triangle of the uber path). Reads the visibility sample, reconstructs the triangle's
// attributes with analytic derivatives (fuse_mr_attributes; no hardware derivatives are used),
// evaluates the material with textureGrad and writes the G-buffer (RT0..RT5, GBufferAttachment order,
// packed by write_gbuffer) + the material id. Specialisation constant 0 = the bin (4 = uber): the
// features it evaluates are fuse_mr_bin_features(bin), and every pixel of a tile in bin b needs at
// most those, so both paths produce the same bits. GLSL twin of mr_resolve_fs.slang.
#extension GL_GOOGLE_include_directive : require
#include "mr_includes.glsl"

layout(constant_id = 0) const uint FUSE_MR_BIN_ID = 4u;

FUSE_BINDLESS_STORAGE_IMAGE_LAYOUT(rg32ui) uniform readonly uimage2D fuse_vis_images[];

layout(location = 0) out vec4 outNormalAo;
layout(location = 1) out vec4 outAlbedoAlpha;
layout(location = 2) out vec4 outRoughMetal;
layout(location = 3) out vec4 outVelocity;
layout(location = 4) out vec4 outDepth;
layout(location = 5) out vec4 outEmissive;
layout(location = 6) out uint outMaterial;

void main() {
    const FuseMrFrame f = FuseMrFrameRef(pc.frame).f;
    const uvec2 p = uvec2(gl_FragCoord.xy);
    const uvec2 v = imageLoad(fuse_vis_images[fuse_handle_index(f.vis)], ivec2(p)).xy;
    FuseGpuSceneHeaderRef scene = fuse_gpu_scene(f.scene);
    const FuseMrAttributes a = fuse_mr_attributes(scene, f, p, v);
    FuseMrGBuffer g = fuse_mr_gbuffer_empty();
    if (a.flags == FUSE_MR_ATTR_OK) {
        g = fuse_mr_shade(scene, a.material, f.sampler_, fuse_mr_bin_features(FUSE_MR_BIN_ID), a.uv, a.duvdx, a.duvdy,
                          a.normal, a.tangent, a.tangentSign, a.depth, a.velocity);
    }
    outNormalAo = g.rt0;
    outAlbedoAlpha = g.rt1;
    outRoughMetal = g.rt2;
    outVelocity = g.rt3;
    outDepth = g.rt4;
    outEmissive = g.rt5;
    outMaterial = g.material;
}
