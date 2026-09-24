#version 460
// WP-1.5 forward G-buffer reference, fragment stage: rasteriser-interpolated attributes and hardware
// UV derivatives (dFdx / dFdy, taken in uniform control flow), the same material evaluation and
// packing as the resolve (fuse_mr_shade, every feature), + the material id and the (instance,
// triangle) ids. Depth test LESS on its own D32.
#extension GL_GOOGLE_include_directive : require
#include "mr_includes.glsl"

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inTangent;
layout(location = 3) flat in float inSign;
layout(location = 4) in vec4 inPrevClip;
layout(location = 5) flat in uint inInstance;

layout(location = 0) out vec4 outNormalAo;
layout(location = 1) out vec4 outAlbedoAlpha;
layout(location = 2) out vec4 outRoughMetal;
layout(location = 3) out vec4 outVelocity;
layout(location = 4) out vec4 outDepth;
layout(location = 5) out vec4 outEmissive;
layout(location = 6) out uint outMaterial;
layout(location = 7) out uvec2 outIds;

void main() {
    const vec2 dx = dFdx(inUv);
    const vec2 dy = dFdy(inUv);
    const FuseMrFrame f = FuseMrFrameRef(pc.frame).f;
    FuseGpuSceneHeaderRef scene = fuse_gpu_scene(f.scene);
    const FuseGpuInstance inst = fuse_gpu_scene_instances(scene).v[inInstance];
    const FuseGpuMesh mesh = fuse_gpu_scene_meshes(scene).v[inst.mesh];
    const uint material = fuse_mr_triangle_material(scene, inst, mesh, uint(gl_PrimitiveID));
    vec2 velocity = vec2(0.0);
    if (inPrevClip.w > 0.0) {
        precise vec2 prev;
        prev.x = (inPrevClip.x / inPrevClip.w * 0.5 + 0.5) * float(f.width);
        prev.y = (inPrevClip.y / inPrevClip.w * 0.5 + 0.5) * float(f.height);
        velocity = gl_FragCoord.xy - prev;
    }
    const FuseMrGBuffer g = fuse_mr_shade(scene, material, f.sampler_, FUSE_MR_FEATURE_TEXTURES | FUSE_MR_FEATURE_NORMAL_MAP,
                                          inUv, dx, dy, inNormal, inTangent, inSign, gl_FragCoord.z, velocity);
    outNormalAo = g.rt0;
    outAlbedoAlpha = g.rt1;
    outRoughMetal = g.rt2;
    outVelocity = g.rt3;
    outDepth = g.rt4;
    outEmissive = g.rt5;
    outMaterial = g.material;
    outIds = uvec2(inInstance, uint(gl_PrimitiveID));
}
